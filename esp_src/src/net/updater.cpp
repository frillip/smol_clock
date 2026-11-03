#include "updater.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <string.h>

#include "../console/console.h"
#include "../pic/pic_boot.h"
#include "../pic/pic_link.h"
#include "../pic/pic_report.h"
#include "../pic/pic_store.h"
#include "../settings/user_prefs.h"
#include "../version.h"
#include "manifest.h"
#include "net_connect.h"
#include "root_ca.h"

extern "C"
{
#include "../pic/sml/sml.h"
}

// What a check or an update came to; above 0, the HTTP status.
enum
{
    RESULT_UPDATE_AVAILABLE = -3,
    RESULT_NO_PROFILE = -2,
    RESULT_NO_UPDATE = -1,
    RESULT_UPDATE_OK = 0,
    RESULT_HTTP_FAILED = 1,
    RESULT_WRITE_ERROR = 2,
    RESULT_JSON_PROBLEM = 3,
    RESULT_OTA_FAILED = 4,
    RESULT_SIZE = 5,   // not the size the manifest gives
    RESULT_SHA256 = 6, // not the file the manifest names
};

#define HTTP_TIMEOUT_MS       15000U
#define DOWNLOAD_IDLE_MS      30000U
#define DOWNLOAD_BUFFER_BYTES 1024U

typedef bool (*Sink)(void *context, const uint8_t *data, size_t length);

static Stream *progress_output;
static bool available;
static bool pic_available;
static bool running;
static bool day_known;
static uint32_t checked_day; // local day number of the last automatic check
static char last_address[48]; // the server the latest request connected to
static char last_failure[64]; // why it failed, if it did

static String url(void)
{
    return String("https://") + net_url_host(user_prefs.fields.updater.server) +
           user_prefs.fields.updater.path;
}


void updater_set_server(const char *server)
{
    user_prefs_set_string(user_prefs.fields.updater.server, server);
}


void updater_set_path(const char *path)
{
    String value = path;

    if (!value.startsWith("/"))
    {
        value = "/" + value;
    }
    user_prefs_set_string(user_prefs.fields.updater.path, value.c_str());
}


void updater_set_config(const char *config)
{
    user_prefs_set_string(user_prefs.fields.updater.config,
                          strcmp(config, "default") == 0 ? UPDATER_CONFIG_DEFAULT : config);
}


static void print_version(Stream *output)
{
    output->printf("ESP version: %s", ESP_VERSION);
    if (user_prefs.fields.updater.config[0] != '\0')
    {
        output->printf("_%s", user_prefs.fields.updater.config);
    }
    output->printf("\n");
}


static const char *result_name(int code)
{
    switch (code)
    {
    case RESULT_UPDATE_AVAILABLE: return "update available, not installed";
    case RESULT_NO_PROFILE: return "no configuration for this clock";
    case RESULT_NO_UPDATE: return "no update for this version";
    case RESULT_UPDATE_OK: return "update installed, restart to use it";
    case RESULT_HTTP_FAILED: return "HTTP request failed";
    case RESULT_WRITE_ERROR: return "write error";
    case RESULT_JSON_PROBLEM: return "invalid manifest";
    case RESULT_OTA_FAILED: return "update failed (no OTA partition?)";
    case RESULT_SIZE: return "not the size the manifest gives";
    case RESULT_SHA256: return "SHA-256 wrong: not the file the manifest names";
    default: return code > 0 ? "unexpected HTTP status" : "unknown error";
    }
}


static void print_result(Stream *output, const char *what, int result)
{
    output->printf("%s: %d (%s", what, result, result_name(result));
    if ((result == RESULT_HTTP_FAILED || result == RESULT_JSON_PROBLEM ||
         result == RESULT_WRITE_ERROR) &&
        last_failure[0] != '\0')
    {
        output->printf(": %s", last_failure);
    }
    output->printf(")");
    if (last_address[0] != '\0')
    {
        output->printf(", %s", last_address);
    }
    output->printf("\n");
}

// ---- Requests --------------------------------------------------------------

// A GET of url on a connection made with fallback (net_connect.h): the
// status, or 0 if no connection.
static int get(HTTPClient &http, NetworkClientSecure &secure, NetworkClient &plain, const char *url)
{
    IPAddress address;
    int status;

    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!net_http_connect(secure, plain, url, root_ca_pem, HTTP_TIMEOUT_MS, &address))
    {
        snprintf(last_failure, sizeof(last_failure), "no connection");
        return 0;
    }
    snprintf(last_address, sizeof(last_address), "%s", address.toString().c_str());
    if (!(strncmp(url, "https://", 8) == 0 ? http.begin(secure, url) : http.begin(plain, url)))
    {
        snprintf(last_failure, sizeof(last_failure), "bad URL");
        return 0;
    }
    status = http.GET();
    if (status < 0)
    {
        snprintf(last_failure, sizeof(last_failure), "%s",
                 HTTPClient::errorToString(status).c_str());
    }
    return status;
}


/* The file a component names, to sink, as it arrives: its size checked
 * against the manifest's before anything is written, its SHA-256 after.
 * A sink's false stops it (RESULT_WRITE_ERROR). The caller discards what
 * the sink took on any result but RESULT_UPDATE_OK. */
static int download(const ManifestComponent *component, Sink sink, void *context)
{
    NetworkClientSecure secure;
    NetworkClient plain;
    HTTPClient http;
    NetworkClient *stream;
    uint8_t buffer[DOWNLOAD_BUFFER_BYTES];
    uint8_t digest[SHA256_BYTES];
    Sha256 sha;
    uint32_t total = component->size;
    uint32_t offset = 0;
    uint32_t last_progress_ms;
    int previous_percent = -1;
    int status = get(http, secure, plain, component->url);

    if (status != HTTP_CODE_OK)
    {
        http.end();
        return status > 0 ? status : RESULT_HTTP_FAILED;
    }
    if (http.getSize() != (int)total)
    {
        http.end();
        return RESULT_SIZE;
    }
    stream = http.getStreamPtr();
    if (stream == NULL)
    {
        http.end();
        return RESULT_WRITE_ERROR;
    }
    sha256_start(&sha);
    last_progress_ms = millis();
    while (offset < total)
    {
        size_t waiting;
        size_t wanted;
        size_t got;
        int percent;

        if (!http.connected() && stream->available() == 0)
        {
            break;
        }
        // Drain the PIC link: a download holds up loop() for many seconds,
        // longer than its receive buffer lasts.
        pic_link_poll();
        waiting = (size_t)stream->available();
        if (waiting == 0)
        {
            if (millis() - last_progress_ms >= DOWNLOAD_IDLE_MS)
            {
                break;
            }
            delay(1);
            continue;
        }
        wanted = waiting < sizeof(buffer) ? waiting : sizeof(buffer);
        if (wanted > total - offset)
        {
            wanted = total - offset;
        }
        got = stream->readBytes(buffer, wanted);
        if (got == 0)
        {
            break;
        }
        sha256_update(&sha, buffer, got);
        if (!sink(context, buffer, got))
        {
            http.end();
            return RESULT_WRITE_ERROR;
        }
        offset += (uint32_t)got;
        last_progress_ms = millis();
        percent = (int)(100ULL * offset / total);
        if (progress_output != NULL && percent / 10 != previous_percent / 10)
        {
            progress_output->printf("Downloading: %lu of %lu bytes (%d%%)\n",
                                    (unsigned long)offset, (unsigned long)total, percent);
            previous_percent = percent;
        }
    }
    http.end();
    if (offset != total)
    {
        snprintf(last_failure, sizeof(last_failure), "%lu of %lu bytes", (unsigned long)offset,
                 (unsigned long)total);
        return RESULT_WRITE_ERROR;
    }
    sha256_finish(&sha, digest);
    return memcmp(digest, component->sha256, SHA256_BYTES) == 0 ? RESULT_UPDATE_OK : RESULT_SHA256;
}


// The manifest's configuration for this clock.
static int fetch(ManifestEntry *entry)
{
    NetworkClientSecure secure;
    NetworkClient plain;
    HTTPClient http;
    JsonDocument doc;
    DeserializationError error;
    String manifest_url = url();
    int status = get(http, secure, plain, manifest_url.c_str());
    int length;
    ManifestResult result;

    if (status != HTTP_CODE_OK)
    {
        http.end();
        return status > 0 ? status : RESULT_HTTP_FAILED;
    }
    length = http.getSize();
    if (length <= 0 || length > MANIFEST_MAX_BYTES)
    {
        http.end();
        snprintf(last_failure, sizeof(last_failure), "%d bytes", length);
        return RESULT_JSON_PROBLEM;
    }
    error = deserializeJson(doc, http.getStream());
    http.end();
    if (error)
    {
        snprintf(last_failure, sizeof(last_failure), "%s", error.c_str());
        return RESULT_JSON_PROBLEM;
    }
    result = manifest_select(doc, UPDATER_HARDWARE, user_prefs.fields.updater.config,
                             WiFi.macAddress().c_str(), entry, last_failure, sizeof(last_failure));
    if (result == MANIFEST_INVALID)
    {
        return RESULT_JSON_PROBLEM;
    }
    return result == MANIFEST_NO_ENTRY ? RESULT_NO_PROFILE : RESULT_UPDATE_OK;
}

// ---- What is due ------------------------------------------------------------

// The ESP's image offered is newer, or, with any_version, offered at all:
// a forced install takes it whatever its version, the same one included
// (a rebuild under the same version, or a damaged image to be replaced).
static bool esp_due(const ManifestEntry *entry, bool any_version)
{
    int order = 0;

    return entry->esp.present && manifest_compare_versions(entry->esp.version, ESP_VERSION, &order) &&
           (order > 0 || any_version);
}


// The PIC's application as the PIC reports it: STAT-SYS from the running
// application, or BOOT-STATUS from the SPL (0: none there). False if the
// PIC has not been heard from.
// The PIC as heard just before the manifest's request (note_pic): a TLS
// request holds up loop() for seconds, by which time the PIC's latest
// STAT-SYS is too old for pic_link_get().
static bool pic_heard;
static uint32_t pic_version_heard; // 0: in its SPL without a valid application

static bool pic_running_version(uint32_t *version)
{
    SML_STAT_SYS_PAYLOAD sys;
    SML_BOOT_STATUS_PAYLOAD status;

    if (pic_link_in_spl() &&
        pic_link_get(SML_CLASS_BOOT, SML_BOOT_STATUS, &status, PIC_LINK_STATE_TIMEOUT_MS))
    {
        *version = (status.flags & SML_BOOT_FLAG_IMAGE_OK) != 0U ? status.app_version : 0U;
        return true;
    }
    if (!pic_link_in_spl() &&
        pic_link_get(SML_CLASS_STAT, SML_STAT_SYS, &sys, PIC_LINK_STATE_TIMEOUT_MS))
    {
        *version = sys.pic_version;
        return true;
    }
    return false;
}


static void note_pic(void)
{
    pic_heard = pic_running_version(&pic_version_heard);
    if (!pic_heard)
    {
        pic_version_heard = 0;
    }
}


typedef enum
{
    PIC_NOT_OFFERED,
    PIC_NOT_HEARD, // the PIC's version is not known
    PIC_CURRENT,
    PIC_WAITS_FOR_ESP, // newer, but the ESP's update comes first
    PIC_DUE,
} PicDue;

static PicDue pic_due(const ManifestEntry *entry, bool force, uint32_t *running)
{
    uint32_t offered;
    int order = 0;

    *running = 0;
    if (!entry->pic.present || !pic_file_version(entry->pic.version, &offered))
    {
        return PIC_NOT_OFFERED;
    }
    if (!pic_heard)
    {
        return force ? PIC_DUE : PIC_NOT_HEARD;
    }
    *running = pic_version_heard;
    if (!force && offered <= *running)
    {
        return PIC_CURRENT;
    }
    if (entry->esp.present && manifest_compare_versions(entry->esp.version, ESP_VERSION, &order) &&
        order > 0)
    {
        return PIC_WAITS_FOR_ESP;
    }
    return PIC_DUE;
}

// ---- Installing ----------------------------------------------------------------

static bool esp_sink(void *context, const uint8_t *data, size_t length)
{
    (void)context;
    return Update.write((uint8_t *)data, length) == length;
}


// The ESP's image into the other OTA slot; restart if asked.
static int install_esp(const ManifestEntry *entry, bool restart)
{
    int result;

    if (!Update.begin(entry->esp.size))
    {
        return RESULT_OTA_FAILED;
    }
    result = download(&entry->esp, esp_sink, NULL);
    if (result != RESULT_UPDATE_OK)
    {
        Update.abort();
        return result;
    }
    if (!Update.end())
    {
        Update.abort();
        return RESULT_OTA_FAILED;
    }
    if (restart)
    {
        delay(1000);
        ESP.restart();
    }
    return RESULT_UPDATE_OK;
}


static bool pic_sink(void *context, const uint8_t *data, size_t length)
{
    PicStoreWriter *writer = (PicStoreWriter *)context;

    return pic_store_write(writer, data, (uint32_t)length) == PIC_STORE_OK;
}


// The manifest's PIC application downloaded into a slot, checked. what
// prefixes what is printed. False, said why, if it failed.
static bool stage_pic(const ManifestEntry *entry, uint8_t slot, PicStoredFile *file,
                      const char *what, Stream *output)
{
    static PicStoreWriter writer; // big: not on the stack
    PicStoreResult stored;
    int result;

    stored = pic_store_begin(&writer, PIC_FILE_APP, slot, entry->pic.size);
    if (stored != PIC_STORE_OK)
    {
        output->printf("%s: staging: %s\n", what, pic_store_result_name(stored));
        return false;
    }
    progress_output = output;
    result = download(&entry->pic, pic_sink, &writer);
    progress_output = NULL;
    if (result != RESULT_UPDATE_OK)
    {
        char text[48];

        snprintf(text, sizeof(text), "%s: download", what);
        print_result(output, text, result);
        return false;
    }
    stored = pic_store_finish(&writer, entry->pic.sha256, file);
    if (stored != PIC_STORE_OK)
    {
        output->printf("%s: staging: %s\n", what, pic_store_result_name(stored));
        return false;
    }
    return true;
}


// A staged file the PIC can take: the SML major this ESP speaks.
static bool pic_compatible(const PicStoredFile *file, Stream *output)
{
    if (file->header.sml_version >> 8 != SML_PROTOCOL_MAJOR)
    {
        output->printf("PIC update: the application speaks SML %u, this ESP %u\n",
                       file->header.sml_version >> 8, (unsigned)SML_PROTOCOL_MAJOR);
        return false;
    }
    return true;
}


// The PIC's application into picfw, unless a slot holds it already; then
// its install from there, run by pic_boot. False, said why, if not started.
static bool install_pic(const ManifestEntry *entry, uint32_t running, bool force, Stream *output)
{
    PicStoredFile file;
    uint32_t offered = 0;

    (void)pic_file_version(entry->pic.version, &offered);
    if (!pic_store_present())
    {
        output->printf("PIC update: no picfw partition: reflash the ESP over USB\n");
        return false;
    }
    if (pic_boot_updating())
    {
        output->printf("PIC update: one is running already\n");
        return false;
    }
    if (pic_store_find(PIC_FILE_APP, offered, entry->pic.sha256, &file))
    {
        output->printf("PIC update: %s already staged in slot %u\n", entry->pic.version,
                       file.slot);
    }
    else
    {
        uint8_t slot = pic_store_choose(PIC_FILE_APP, running);

        output->printf("PIC update: downloading %s into slot %u\n", entry->pic.version, slot);
        if (!stage_pic(entry, slot, &file, "PIC update", output))
        {
            return false;
        }
    }
    if (file.header.version != offered)
    {
        (void)pic_store_erase(PIC_FILE_APP, file.slot);
        output->printf("PIC update: the file holds another version than the manifest says: slot "
                       "%u emptied\n",
                       file.slot);
        return false;
    }
    if (!pic_compatible(&file, output))
    {
        return false;
    }
    return pic_boot_update(&file, force, output);
}


// The manifest's PIC application into picfw, if no slot holds it: the file
// for its install, fetched ahead, or, when it is the one the PIC runs, a copy
// to reinstall without the network (update pic force offline, and pic_boot's
// recovery of a PIC left without an application). The slot holding the
// version the PIC runs is kept (pic_store_choose()).
static void stage_offered(const ManifestEntry *entry, Stream *output)
{
    PicStoredFile file;
    uint32_t offered = 0;
    uint32_t running = pic_heard ? pic_version_heard : 0U;
    uint8_t slot;

    if (!pic_store_present() || pic_boot_updating() || !entry->pic.present ||
        !pic_file_version(entry->pic.version, &offered) ||
        pic_store_find(PIC_FILE_APP, offered, entry->pic.sha256, &file))
    {
        return;
    }
    slot = pic_store_choose(PIC_FILE_APP, running);
    output->printf("PIC: staging %s%s into slot %u\n", entry->pic.version,
                   offered == running ? ", the application it runs," : "", slot);
    if (!stage_pic(entry, slot, &file, "PIC staging", output))
    {
        return;
    }
    if (file.header.version != offered)
    {
        (void)pic_store_erase(PIC_FILE_APP, slot);
        output->printf("PIC staging: the file holds another version than the manifest says: "
                       "slot %u emptied\n",
                       slot);
        return;
    }
    output->printf("PIC: %s staged in slot %u\n", entry->pic.version, slot);
}


// No manifest: a staged file instead, when forced or when the PIC has no
// application. The one the PIC runs (or last ran), else the newest.
static void install_offline(Stream *output, bool force)
{
    PicStoredFile file;
    char text[16];
    bool no_application = pic_heard && pic_version_heard == 0U;

    if (!pic_store_present() || pic_boot_updating() ||
        !pic_store_pick(PIC_FILE_APP, pic_heard ? pic_version_heard : 0U, &file))
    {
        output->printf("PIC: nothing staged to install without the update server\n");
        return;
    }
    pic_report_version(text, sizeof(text), file.header.version);
    if (!force && !no_application)
    {
        output->printf("PIC: \"update pic force\" installs the staged %s without the update "
                       "server\n",
                       text);
        return;
    }
    if (!pic_compatible(&file, output))
    {
        return;
    }
    output->printf("PIC update: without the update server, the staged %s from slot %u\n", text,
                   file.slot);
    (void)pic_boot_update(&file, force, output);
}

// ---- Commands ------------------------------------------------------------------

static void print_offers(Stream *output, const ManifestEntry *entry)
{
    uint32_t running = 0;
    char text[16];

    if (entry->esp.present)
    {
        output->printf("ESP: %s offered, %s\n", entry->esp.version,
                       esp_due(entry, false) ? "UPDATE AVAILABLE" : "up to date");
    }
    else
    {
        output->printf("ESP: nothing offered\n");
    }
    switch (pic_due(entry, false, &running))
    {
    case PIC_NOT_OFFERED: output->printf("PIC: nothing offered\n"); break;
    case PIC_NOT_HEARD: output->printf("PIC: %s offered, the PIC not heard from\n", entry->pic.version); break;
    case PIC_CURRENT:
        pic_report_version(text, sizeof(text), running);
        output->printf("PIC: %s offered, up to date (%s)\n", entry->pic.version, text);
        break;
    case PIC_WAITS_FOR_ESP:
        output->printf("PIC: %s offered, after the ESP's update\n", entry->pic.version);
        break;
    case PIC_DUE:
        pic_report_version(text, sizeof(text), running);
        output->printf("PIC: %s offered, UPDATE AVAILABLE (runs %s)\n", entry->pic.version,
                       running != 0U ? text : "nothing");
        break;
    }
    if (entry->pic_spl.present)
    {
        output->printf("PIC SPL: %s offered, not installed: SPL updates are not supported yet\n",
                       entry->pic_spl.version);
    }
}


// The manifest; its result printed. True with the entry.
static bool start(Stream *output, ManifestEntry *entry)
{
    int result;

    last_address[0] = '\0';
    last_failure[0] = '\0';
    note_pic();
    running = true;
    result = fetch(entry);
    running = false;
    if (result != RESULT_UPDATE_OK)
    {
        print_result(output, "Update server", result);
        return false;
    }
    return true;
}


bool updater_check(Stream *output)
{
    ManifestEntry entry;
    uint32_t running_version;

    print_version(output);
    if (!start(output, &entry))
    {
        return false;
    }
    available = esp_due(&entry, false);
    pic_available = pic_due(&entry, false, &running_version) == PIC_DUE;
    print_offers(output, &entry);
    stage_offered(&entry, output);
    return available || pic_available;
}


static void install_esp_reported(Stream *output, const ManifestEntry *entry, bool restart)
{
    int result;

    output->printf("ESP: installing %s%s\n", entry->esp.version, restart ? ", then restarting" : "");
    running = true;
    progress_output = output;
    result = install_esp(entry, restart);
    progress_output = NULL;
    running = false;
    print_result(output, "ESP update", result);
    if (result == RESULT_UPDATE_OK)
    {
        available = false;
        output->printf("Use \"esp reset\" to finish the update\n");
    }
}


void updater_pull(Stream *output)
{
    ManifestEntry entry;
    uint32_t running_version;

    print_version(output);
    if (!start(output, &entry))
    {
        return;
    }
    if (esp_due(&entry, false))
    {
        install_esp_reported(output, &entry, false);
        return;
    }
    if (pic_due(&entry, false, &running_version) == PIC_DUE)
    {
        (void)install_pic(&entry, running_version, false, output);
        return;
    }
    print_offers(output, &entry);
    stage_offered(&entry, output);
}


void updater_esp(Stream *output, bool force)
{
    ManifestEntry entry;

    print_version(output);
    if (!start(output, &entry))
    {
        return;
    }
    if (!entry.esp.present)
    {
        output->printf("ESP: nothing offered\n");
        return;
    }
    if (!esp_due(&entry, force))
    {
        output->printf("ESP: %s offered, up to date (force to install it anyway)\n",
                       entry.esp.version);
        return;
    }
    install_esp_reported(output, &entry, force);
}


void updater_spl(Stream *output, bool force)
{
    (void)force;
    output->printf("PIC SPL: updates are not supported yet\n");
}


void updater_pic(Stream *output, bool force)
{
    ManifestEntry entry;
    uint32_t running_version;

    if (!start(output, &entry))
    {
        install_offline(output, force);
        return;
    }
    switch (pic_due(&entry, force, &running_version))
    {
    case PIC_DUE: (void)install_pic(&entry, running_version, force, output); break;
    case PIC_NOT_OFFERED: output->printf("PIC: nothing offered\n"); break;
    case PIC_NOT_HEARD: output->printf("PIC: not heard from: its version is not known\n"); break;
    case PIC_CURRENT:
        output->printf("PIC: up to date (force to install it again)\n");
        stage_offered(&entry, output);
        break;
    case PIC_WAITS_FOR_ESP:
        output->printf("PIC: the ESP's update first (%s)\n", entry.esp.version);
        break;
    }
}


void updater_service(bool have_local, uint32_t local_seconds)
{
    uint32_t day = local_seconds / 86400;
    uint32_t hour = local_seconds / 3600 % 24;
    ManifestEntry entry;
    uint32_t running_version;
    Stream *output = &Serial;

    if (!have_local)
    {
        return;
    }
    if (!day_known)
    {
        // Started inside the update hour: skip today, it may be the update.
        day_known = true;
        checked_day = hour == user_prefs.fields.updater.flags.auto_time ? day : day - 1;
    }
    if (!user_prefs.fields.updater.flags.auto_enabled || checked_day == day ||
        hour != user_prefs.fields.updater.flags.auto_time || pic_boot_updating())
    {
        return;
    }
    checked_day = day;
    console_printf("Automatic update check\n");
    if (!start(output, &entry))
    {
        return;
    }
    if (esp_due(&entry, false))
    {
        install_esp_reported(output, &entry, true);
    }
    else if (pic_due(&entry, false, &running_version) == PIC_DUE)
    {
        (void)install_pic(&entry, running_version, false, output);
    }
    else
    {
        stage_offered(&entry, output);
    }
}


bool updater_is_available(void)
{
    return available;
}


bool updater_pic_is_available(void)
{
    return pic_available;
}


bool updater_is_running(void)
{
    return running || pic_boot_updating();
}
