/*
 * The console commands (src/console/commands.cpp) and the PIC link
 * (src/pic/pic_link.cpp) on the host, against a PIC played by the test: what
 * each line sends the PIC, and how its answers are handled. The value
 * parsers (src/util/parse.cpp) too. The ESP's other modules are stand-ins
 * that record what they were asked.
 */
#include <Arduino.h>
#include <esp_system.h>

#include "../../src/console/commands.h"
#include "../../src/net/telnet.h"
#include "../../src/net/telnet_auth.h"
#include "../../src/net/updater.h"
#include "../../src/net/wifi_link.h"
#include "../../src/pic/pic_boot.h"
#include "../../src/pic/pic_link.h"
#include "../../src/settings/user_prefs.h"
#include "../../src/timing/ntp.h"
#include "../../src/timing/pps.h"
#include "../../src/tz/leap.h"
#include "../../src/tz/tzinfo.h"
#include "../../src/util/parse.h"
#include "../../src/util/time_text.h"

static int failures, checks;
#define CHECK(cond)                                          \
    do                                                       \
    {                                                        \
        checks++;                                            \
        if (!(cond))                                         \
        {                                                    \
            failures++;                                      \
            printf("  FAIL line %d: %s\n", __LINE__, #cond); \
        }                                                    \
    } while (0)

// ---- The Arduino core and the ESP ------------------------------------------------

HardwareSerial Serial(0);
uint32_t fake_millis = 1000;
EspClass ESP;
int64_t esp_timer_get_time(void)
{
    return (int64_t)fake_millis * 1000;
}
esp_reset_reason_t esp_reset_reason(void)
{
    return ESP_RST_POWERON;
}

// ---- Stand-ins for the ESP's modules, recording what they were asked ---------------

static std::string calls;
static void called(const char *name)
{
    calls += name;
    calls += ' ';
}

UserPrefs user_prefs;
UserPrefsNtp user_prefs_ntp;
static int prefs_saves;
char *user_prefs_ntp_server(int index)
{
    return index == 0 ? user_prefs.fields.ntp.server : user_prefs_ntp.server[index - 1];
}
void user_prefs_reset(void)
{
    called("prefs-reset");
    memset(&user_prefs, 0, sizeof user_prefs);
    memset(&user_prefs_ntp, 0, sizeof user_prefs_ntp);
    user_prefs.fields.ntp.interval = NTP_INTERVAL_DEFAULT;
    user_prefs.fields.telnet.port = TELNET_PORT_DEFAULT;
}
void user_prefs_save(void)
{
    prefs_saves++;
}
void user_prefs_print(Stream *output)
{
    output->printf("(prefs)\n");
}
void user_prefs_set_string(char *field, const char *value)
{
    memset(field, 0, USER_PREFS_STORED_STRING_SIZE);
    strncpy(field, value, USER_PREFS_MAX_STRING_SIZE);
}

void telnet_start(Stream *)
{
    called("telnet-start");
}
void telnet_stop(Stream *)
{
    called("telnet-stop");
}
void telnet_enable(Stream *)
{
    called("telnet-enable");
}
void telnet_disable(Stream *)
{
    called("telnet-disable");
}
void telnet_set_port(Stream *, uint16_t port)
{
    user_prefs.fields.telnet.port = port;
}
bool telnet_is_running(void)
{
    return false;
}
void ntp_gnss_print(Stream *output, bool verbose)
{
    output->printf(verbose ? "(ntp gnss -v)\n" : "(ntp gnss)\n");
}
void ntp_gnss_reset(void)
{
    called("ntp-gnss-reset");
}
// A telnet connection: its stream, and whether it has logged in.
static Stream telnet_stream;
static bool telnet_connected;
static bool telnet_logged_in;
Stream *telnet_client(void)
{
    return telnet_connected ? &telnet_stream : NULL;
}
bool telnet_authorised(void)
{
    return telnet_logged_in;
}
void telnet_set_authorised(bool on)
{
    telnet_logged_in = on;
}
// telnet_auth's platform: a fake NVS record, and a counter for salts.
static TelnetAuthRecord stored_auth;
static bool auth_stored;
bool telnet_auth_load(TelnetAuthRecord *record)
{
    *record = stored_auth;
    return auth_stored;
}
void telnet_auth_save(const TelnetAuthRecord *record)
{
    stored_auth = *record;
    auth_stored = true;
}
void telnet_auth_random(uint8_t *data, uint32_t length)
{
    static uint8_t next = 1;
    for (uint32_t index = 0; index < length; index++)
        data[index] = next++;
}
void telnet_print(Stream *output, bool)
{
    output->printf("(telnet)\n");
}

void updater_set_server(const char *server)
{
    user_prefs_set_string(user_prefs.fields.updater.server, server);
}
void updater_set_path(const char *path)
{
    user_prefs_set_string(user_prefs.fields.updater.path, path);
}
void updater_set_config(const char *config)
{
    user_prefs_set_string(user_prefs.fields.updater.config, config);
}
bool updater_check(Stream *)
{
    called("update-check");
    return true;
}
void updater_pull(Stream *)
{
    called("update-pull");
}
void updater_esp(Stream *, bool force)
{
    called(force ? "update-esp-force" : "update-esp");
}
void updater_spl(Stream *, bool force)
{
    called(force ? "update-spl-force" : "update-spl");
}
void pic_boot_print_staged(Stream *, bool verbose)
{
    called(verbose ? "staged-verbose" : "staged");
}
void pic_boot_erase(Stream *, bool application, bool spl)
{
    called(application && spl ? "erase-all" : application ? "erase-pic" : spl ? "erase-spl" : "erase-none");
}
void updater_pic(Stream *, bool force)
{
    called(force ? "update-pic-force" : "update-pic");
}
bool updater_is_available(void)
{
    return false;
}
bool updater_pic_is_available(void)
{
    return false;
}
void pic_boot_print(Stream *, bool verbose)
{
    called(verbose ? "boot-print-verbose" : "boot-print");
}
void pic_boot_command(Stream *, uint8_t command)
{
    called(command == SML_BOOT_CMD_RUN ? "boot-run" : command == SML_BOOT_CMD_HOLD ? "boot-hold" : "boot-other");
}
bool updater_is_running(void)
{
    return false;
}

void wifi_link_forget(void)
{
    called("wifi-forget");
}
void wifi_link_clear(Stream *)
{
    called("wifi-clear");
}
void wifi_link_setup(Stream *)
{
    called("wifi-setup");
}
void wifi_link_reconnect(void)
{
    called("wifi-connect");
}
void wifi_link_disconnect(void)
{
    called("wifi-disconnect");
}
void wifi_link_scan(Stream *)
{
    called("wifi-scan");
}
void wifi_link_print(Stream *output, bool)
{
    output->printf("(wifi)\n");
}
const char *wifi_link_hostname(void)
{
    return "smol";
}

int32_t user_prefs_ntp_asymmetry_us;
int32_t user_prefs_ntp_dhcp = NTP_DHCP_ENABLED;
const char *user_prefs_ntp_dhcp_name(int32_t mode)
{
    return mode == NTP_DHCP_DISABLED ? "disable" : mode == NTP_DHCP_PREFERRED ? "prefer" : "enable";
}
int32_t user_prefs_ntp_offset_us[NTP_SERVERS + 1];
bool ntp_set_offset(int server, int32_t us)
{
    called("ntp-offset");
    user_prefs_ntp_offset_us[server] = us;
    return true;
}
bool ntp_set_asymmetry(int32_t us)
{
    called("ntp-asymmetry");
    user_prefs_ntp_asymmetry_us = us;
    return true;
}

static bool ntp_known = true;
void ntp_resync(void)
{
    called("ntp-sync");
}
bool ntp_utc_now(uint32_t *utc_seconds)
{
    *utc_seconds = 1800000000UL;
    return ntp_known;
}
bool ntp_utc_at(int64_t, int64_t *)
{
    return false;
}
void ntp_print(Stream *output, bool verbose)
{
    output->printf(verbose ? "(ntp -v)\n" : "(ntp)\n");
}
void ntp_print_sources(Stream *output, bool)
{
    output->printf("(ntp sources)\n");
}
void ntp_print_tracking(Stream *output)
{
    output->printf("(ntp tracking)\n");
}

bool tzinfo_update(Stream *)
{
    called("tz-lookup");
    return true;
}
bool tzinfo_check(Stream *)
{
    called("tz-check");
    return true;
}
void tzinfo_set_enabled(bool enabled)
{
    user_prefs.fields.rtzinfo.flags.enabled = enabled;
}
void tzinfo_set_server(const char *server)
{
    user_prefs_set_string(user_prefs.fields.rtzinfo.server, server);
}
void tzinfo_set_path(const char *path)
{
    user_prefs_set_string(user_prefs.fields.rtzinfo.path, path);
}
void tzinfo_print(Stream *output, bool)
{
    output->printf("(tzinfo)\n");
}
void leap_print(Stream *output, bool)
{
    output->printf("(leap)\n");
}

bool pps_pic_latest(PpsEdge *)
{
    return false;
}
bool pps_gnss_latest(PpsEdge *)
{
    return false;
}
static std::string pic_text; // the PIC's console text, as printed
void console_write_text(const uint8_t *text, size_t length)
{
    pic_text.append((const char *)text, length);
}
void console_printf(const char *, ...)
{
}

// ---- The PIC, played by the test -----------------------------------------------------

typedef struct
{
    uint8_t class_id, message_id;
    uint8_t payload[SML_MAX_PAYLOAD_BYTES];
} Frame;

static std::vector<Frame> sent;

// Everything the ESP has sent since the last call, decoded into sent.
static HardwareSerial *link_uart;
static void collect(void)
{
    static SmlParser parser;
    static bool started;
    SmlFrame frame;

    if (!started)
    {
        sml_parser_init(&parser);
        started = true;
    }
    sent.clear();
    for (uint8_t byte : link_uart->tx)
    {
        if (sml_parser_push(&parser, byte, &frame))
        {
            do
            {
                Frame decoded;
                decoded.class_id = frame.class_id;
                decoded.message_id = frame.message_id;
                sml_decode(sml_message(frame.class_id, frame.message_id)->layout, frame.payload,
                           decoded.payload);
                sent.push_back(decoded);
            } while (sml_parser_next(&parser, &frame));
        }
    }
    link_uart->tx.clear();
}

static void from_pic(uint8_t class_id, uint8_t message_id, const void *native)
{
    uint8_t frame[SML_MAX_FRAME_BYTES];
    uint16_t length = sml_frame_message(class_id, message_id, native, frame);
    link_uart->rx.insert(link_uart->rx.end(), frame, frame + length);
    pic_link_service();
}

static SML_STAT_SETTINGS_PAYLOAD pic_settings;

static void send_stat_settings(void)
{
    from_pic(SML_CLASS_STAT, SML_STAT_SETTINGS, &pic_settings);
}

static void answer(uint16_t token, uint16_t command, uint16_t status, uint16_t detail)
{
    SML_CMD_RESPONSE_PAYLOAD response = {status, detail, token, command, pic_settings.revision};
    from_pic(SML_CLASS_CMD, SML_CMD_RESPONSE, &response);
}

// Runs a line; what it printed, and what it sent, decoded.
static Stream console;
static std::string run(const char *text)
{
    char line[128];

    snprintf(line, sizeof line, "%s", text);
    console.out.clear();
    calls.clear();
    commands_execute(&console, line);
    collect();
    return console.out;
}

// The same, typed at the telnet connection.
static std::string run_telnet(const char *text)
{
    char line[128];

    snprintf(line, sizeof line, "%s", text);
    telnet_stream.out.clear();
    calls.clear();
    commands_execute(&telnet_stream, line);
    collect();
    return telnet_stream.out;
}

static const SML_CMD_USER_PAYLOAD *user(size_t index)
{
    if (index >= sent.size() || sent[index].class_id != SML_CLASS_CMD ||
        sent[index].message_id != SML_CMD_USER)
        return NULL;
    return (const SML_CMD_USER_PAYLOAD *)sent[index].payload;
}

static const SML_CMD_SETTINGS_PAYLOAD *settings_write(size_t index)
{
    if (index >= sent.size() || sent[index].class_id != SML_CLASS_CMD ||
        sent[index].message_id != SML_CMD_SETTINGS)
        return NULL;
    return (const SML_CMD_SETTINGS_PAYLOAD *)sent[index].payload;
}

// The PIC applying a write as it would: ACK, the revision up.
static void apply(const SML_CMD_SETTINGS_PAYLOAD *write)
{
    pic_settings.settings = write->settings;
    pic_settings.revision++;
    answer(write->token, SML_OPERATION_SETTINGS_WRITE, SML_RESULT_ACK, SML_ERROR_NONE);
}

static bool contains(const std::string &text, const char *part)
{
    return text.find(part) != std::string::npos;
}

// ---- Tests ---------------------------------------------------------------------------

static void test_parsers(void)
{
    int32_t offset;
    uint32_t value;

    printf("Parsers\n");
    CHECK(parse_offset("+5:30", 86399, &offset) && offset == 19800);
    CHECK(parse_offset("-5", 86399, &offset) && offset == -18000);
    CHECK(parse_offset("1", 86399, &offset) && offset == 3600);
    CHECK(parse_offset("+05:45", 86399, &offset) && offset == 20700);
    CHECK(parse_offset("-0:25:21", 86399, &offset) && offset == -1521);
    CHECK(parse_offset("0", 86399, &offset) && offset == 0);
    CHECK(parse_offset("-18000s", 86399, &offset) && offset == -18000);
    CHECK(!parse_offset("+24", 86399, &offset) && !parse_offset("+5:3", 86399, &offset) &&
          !parse_offset("+5:60", 86399, &offset) && !parse_offset("", 86399, &offset) &&
          !parse_offset("+1h", 86399, &offset) && !parse_offset("90000s", 86399, &offset));

    CHECK(parse_time_of_day("07:30", &value) && value == 27000);
    CHECK(parse_time_of_day("7:30:15", &value) && value == 27015);
    CHECK(parse_time_of_day("27000", &value) && value == 27000);
    CHECK(parse_time_of_day("27015s", &value) && value == 27015);
    CHECK(parse_time_of_day("0", &value) && value == 0);
    CHECK(!parse_time_of_day("24:00", &value) && !parse_time_of_day("86400", &value) &&
          !parse_time_of_day("07:30pm", &value) && !parse_time_of_day("-5", &value) &&
          !parse_time_of_day("s", &value) && !parse_time_of_day("7:5", &value));

    CHECK(parse_moment("2026-10-18T12:34:56Z", &value) && value == 1792326896UL);
    {
        bool local = true;
        CHECK(parse_moment("2026-10-18T12:34:56Z", &value, &local) && !local);
        CHECK(parse_moment("2026-10-18 13:34:56+01:00", &value, &local) && !local);
        CHECK(parse_moment("1792326896", &value, &local) && !local);
        CHECK(parse_moment("2026-10-18 12:34:56", &value, &local) && local &&
              value == 1792326896UL);
        CHECK(parse_moment("2026-10-18", &value, &local) && local);
    }
    CHECK(parse_moment("2026-10-18 13:34:56+01:00", &value) && value == 1792326896UL);
    CHECK(parse_moment("2026-10-18t12:34:56", &value) && value == 1792326896UL);
    CHECK(parse_moment("1792326896", &value) && value == 1792326896UL);
    CHECK(parse_moment("2026-10-18", &value) && value == 1792281600UL);
    CHECK(parse_moment("1970-01-01T00:00Z", &value) && value == 0);
    CHECK(parse_moment("2024-02-29", &value) && !parse_moment("2025-02-29", &value));
    // Loosely: spaces, basic forms, fractions, compact and short offsets.
    CHECK(parse_moment("2026-10-18 12:34:56 Z", &value) && value == 1792326896UL);
    CHECK(parse_moment("2026-10-18  13:34:56 +01:00", &value) && value == 1792326896UL);
    CHECK(parse_moment("20261018T123456Z", &value) && value == 1792326896UL);
    CHECK(parse_moment("20261018T1334+0100", &value) && value == 1792326840UL);
    CHECK(parse_moment("2026-10-18T12:34:56.789Z", &value) && value == 1792326896UL);
    CHECK(parse_moment("2026-10-18T07:34:56-5", &value) && value == 1792326896UL);
    CHECK(parse_moment("2026-10-18T18:04:56+5:30", &value) && value == 1792326896UL);
    CHECK(!parse_moment("2026-10-18_12:34", &value) && !parse_moment("20261018T12345Z", &value) &&
          !parse_moment("2026-10-18T12:34+123", &value) &&
          !parse_moment("2026-10-18T12:34+24:00", &value) &&
          !parse_moment("2026-10-18T12:34:56.", &value));
    CHECK(!parse_moment("2026-13-01", &value) && !parse_moment("2026-10-18T25:00", &value) &&
          !parse_moment("1969-12-31T23:59:59Z", &value) && !parse_moment("-5", &value) &&
          !parse_moment("2026-1-18", &value) && !parse_moment("2026-10-18T12:34Zx", &value));

    CHECK(parse_duration("1024", 128, 86400, &value) && value == 1024);
    CHECK(parse_duration("17m", 128, 86400, &value) && value == 1020);
    CHECK(parse_duration("1h", 128, 86400, &value) && value == 3600);
    CHECK(parse_duration("1d", 128, 86400, &value) && value == 86400);
    CHECK(!parse_duration("2d", 128, 86400, &value) && !parse_duration("1m", 128, 86400, &value) &&
          !parse_duration("m", 0, 10, &value) && !parse_duration("1w", 0, 1000000, &value));
}


// Times to four significant figures, in the largest unit with a whole part.
static void test_time_text(void)
{
    static const struct
    {
        int64_t ns;
        bool plus;
        const char *text;
    } cases[] = {
        {106, false, "106 ns"},         {999, false, "999 ns"},         {1000, false, "1.000 us"},
        {5184800, false, "5.185 ms"},   {-2460800, true, "-2.461 ms"},  {474000, false, "474.0 us"},
        {75933000, true, "+75.93 ms"},  {-151419000, true, "-151.4 ms"}, {9999600, false, "10.00 ms"},
        {999960, false, "1.000 ms"},    {11428000, false, "11.43 ms"},  {1234567890123LL, false, "1235 s"},
        {999999999, false, "1.000 s"},  {13000000000LL, true, "+13.00 s"}, {0, false, "0 ns"},
    };
    char text[TIME_NS_TEXT];

    printf("Times as text\n");
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
    {
        time_format_ns(text, cases[i].ns, cases[i].plus);
        CHECK(strcmp(text, cases[i].text) == 0);
    }
}


static void test_settings(void)
{
    std::string out;
    const SML_CMD_SETTINGS_PAYLOAD *write;
    SML_CMD_SETTINGS_PAYLOAD first;

    printf("PIC settings, written whole\n");
    // Before the PIC has sent its settings: nothing is sent, they are asked for.
    out = run("brightness 1200");
    CHECK(contains(out, "not known yet") && sent.size() == 1 && user(0) != NULL &&
          user(0)->command == SML_USER_REQUEST_MESSAGE &&
          user(0)->argument == ((uint32_t)SML_CLASS_STAT << 8 | SML_STAT_SETTINGS));

    send_stat_settings();
    out = run("tz offset +5:30");
    write = settings_write(0);
    CHECK(sent.size() == 1 && write != NULL && write->expected_revision == 5 &&
          write->settings.base_offset_s == 19800 &&
          (write->settings.flags & SML_SETTING_TZ_AUTO) == 0 && write->flags == 0);
    first = *write;
    // A second change before the first is answered waits, then goes with both.
    out = run("brightness 1200");
    CHECK(sent.empty() && out.empty());
    out = run("tz dst on");
    CHECK(sent.empty());
    apply(&first);
    collect();
    write = settings_write(0);
    CHECK(sent.size() == 1 && write != NULL && write->expected_revision == 6 &&
          write->settings.base_offset_s == 19800 && write->settings.brightness == 1200 &&
          (write->settings.flags & SML_SETTING_BRI_AUTO) == 0 &&
          (write->settings.flags & SML_SETTING_DST_AUTO) == 0 &&
          (write->settings.flags & SML_SETTING_DST_ACTIVE) != 0);
    first = *write;
    console.out.clear();
    apply(&first);
    CHECK(contains(console.out, "PIC settings: ACK"));
    // A STAT-SETTINGS afterwards changes nothing that matters.
    send_stat_settings();

    // Showing a value: from what is known, nothing sent.
    out = run("tz offset");
    CHECK(sent.empty() && contains(out, "tz offset: MANUAL (+05:30)"));
    out = run("brightness");
    CHECK(sent.empty() && contains(out, "brightness: MANUAL") && contains(out, "manual 1200"));
    out = run("tz dst");
    CHECK(contains(out, "tz dst: ON"));

    // Each setting, as it lands in the body.
    struct
    {
        const char *line;
        uint16_t set, clear;
    } flags[] = {
        {"tz offset auto", SML_SETTING_TZ_AUTO, 0},
        {"tz dst auto", SML_SETTING_DST_AUTO, 0},
        {"tz dst off", 0, SML_SETTING_DST_AUTO | SML_SETTING_DST_ACTIVE},
        {"brightness auto", SML_SETTING_BRI_AUTO, 0},
        {"beeps on", SML_SETTING_BEEP_ENABLED, 0},
        {"beeps off", 0, SML_SETTING_BEEP_ENABLED},
        {"alarm 07:30", SML_SETTING_ALARM_ENABLED, 0},
        {"alarm off", 0, SML_SETTING_ALARM_ENABLED},
        {"alarm on", SML_SETTING_ALARM_ENABLED, 0},
        {"display hours 12", SML_SETTING_FORMAT_12H, 0},
        {"display hours 24", 0, SML_SETTING_FORMAT_12H},
    };
    for (auto &flag : flags)
    {
        run(flag.line);
        write = settings_write(0);
        CHECK(write != NULL && (write->settings.flags & flag.set) == flag.set &&
              (write->settings.flags & flag.clear) == 0);
        if (write == NULL)
        {
            printf("  %s\n", flag.line);
            continue;
        }
        first = *write;
        apply(&first);
    }
    CHECK(pic_settings.settings.alarm_offset_s == 27000 &&
          pic_settings.settings.brightness == 1200);
    run("tz dst-offset +0:30");
    CHECK(settings_write(0) != NULL && settings_write(0)->settings.dst_offset_s == 1800);
    first = *settings_write(0);
    apply(&first);
    run("display mmdd");
    CHECK(settings_write(0) != NULL &&
          settings_write(0)->settings.selected_display == SML_DISPLAY_MMDD);
    first = *settings_write(0);
    // Refused: printed, the settings asked for again, a change waiting dropped.
    out = run("osc baseline 40000123");
    CHECK(sent.empty());
    console.out.clear();
    answer(first.token, SML_OPERATION_SETTINGS_WRITE, SML_RESULT_NACK, SML_ERROR_UNSUPPORTED);
    collect();
    CHECK(contains(console.out, "PIC settings: NACK (") &&
          contains(console.out, "not sent, the change before was refused"));
    CHECK(sent.size() == 1 && user(0) != NULL && user(0)->command == SML_USER_REQUEST_MESSAGE);
    out = run("beeps on");
    CHECK(contains(out, "not known yet"));
    send_stat_settings();
    run("display epoch 2026-10-18T12:34:56Z");
    CHECK(settings_write(0) != NULL &&
          settings_write(0)->settings.delta_epoch_utc == 1792326896UL &&
          settings_write(0)->settings.selected_display == pic_settings.settings.selected_display &&
          (settings_write(0)->settings.flags & SML_SETTING_DELTA_LOCAL) == 0);
    first = *settings_write(0);
    apply(&first);
    CHECK(contains(run("display epoch"), "display epoch: 2026-10-18T12:34:56Z\n"));
    // Without a zone: local wall-clock time, flagged; Unix seconds are UTC.
    run("display epoch 2027-01-01 00:00");
    CHECK(settings_write(0) != NULL && settings_write(0)->settings.delta_epoch_utc == 1798761600UL &&
          (settings_write(0)->settings.flags & SML_SETTING_DELTA_LOCAL) != 0);
    first = *settings_write(0);
    apply(&first);
    CHECK(contains(run("display epoch"), "display epoch: 2027-01-01T00:00:00 local\n"));
    run("display epoch 1798761600");
    CHECK(settings_write(0) != NULL &&
          (settings_write(0)->settings.flags & SML_SETTING_DELTA_LOCAL) == 0);
    first = *settings_write(0);
    apply(&first);
    run("display uncertainty");
    CHECK(settings_write(0) != NULL &&
          settings_write(0)->settings.selected_display == SML_DISPLAY_UNCERTAINTY);
    first = *settings_write(0);
    apply(&first);
    run("display ssmm");
    CHECK(settings_write(0) != NULL &&
          settings_write(0)->settings.selected_display == SML_DISPLAY_SSMM);
    first = *settings_write(0);
    apply(&first);
    run("osc baseline 40000123");
    CHECK(settings_write(0) != NULL &&
          settings_write(0)->settings.calibration_fosc_hz == 40000123UL);
    first = *settings_write(0);
    // No answer: the same as a refusal, once it times out.
    fake_millis += 6000;
    console.out.clear();
    pic_link_service();
    collect();
    CHECK(contains(console.out, "PIC settings: no response") && sent.size() == 1 &&
          user(0)->command == SML_USER_REQUEST_MESSAGE);
    send_stat_settings();

    // Bad values: nothing sent.
    out = run("brightness 5000");
    CHECK(sent.empty() && contains(out, "bad value '5000'") && contains(out, "0..4000"));
    out = run("tz offset +25");
    CHECK(sent.empty() && contains(out, "bad value"));
    out = run("display hhmmss");
    CHECK(sent.empty() && contains(out, "expected hhmm|mmss"));
    out = run("alarm 86400");
    CHECK(sent.empty() && contains(out, "bad value"));
}


static void test_pic_actions(void)
{
    std::string out;

    printf("PIC actions\n");
    struct
    {
        const char *line;
        uint16_t command;
        uint32_t argument;
    } actions[] = {
        {"pic", SML_USER_SHOW, SML_SHOW_ALL},
        {"pic stored", SML_USER_SHOW, SML_SHOW_STORED},
        {"tz", SML_USER_SHOW, SML_SHOW_TZ},
        {"display", SML_USER_SHOW, SML_SHOW_DISPLAY},
        {"rtc", SML_USER_SHOW, SML_SHOW_RTC},
        {"osc", SML_USER_SHOW, SML_SHOW_OSC},
        {"gnss", SML_USER_SHOW, SML_SHOW_GNSS},
        {"sources", SML_USER_REQUEST_MESSAGE, (uint32_t)SML_CLASS_STAT << 8 | SML_STAT_SOURCES},
        {"pic save", SML_USER_SAVE, 0},
        {"pic defaults", SML_USER_DEFAULTS, 0},
        {"pic reset", SML_USER_RESET, 0},
        {"pic resync", SML_USER_RESYNC, 0},
        {"pic bootloader", SML_USER_ENTER_BOOTLOADER, 0},
        {"rtc set 1792326896", SML_USER_SET_RTC, 1792326896UL},
        {"rtc set now", SML_USER_SET_RTC, 1800000000UL},
        {"debug", SML_USER_SET_DEBUG, 0},
        {"debug gnss sync", SML_USER_SET_DEBUG, 0xFFFF0000UL | SML_DEBUG_GNSS | SML_DEBUG_SYNC},
        {"debug +ntp -sync", SML_USER_SET_DEBUG, (uint32_t)SML_DEBUG_SYNC << 16 | SML_DEBUG_NTP},
        {"debug -all", SML_USER_SET_DEBUG, (uint32_t)SML_DEBUG_ALL << 16},
        {"debug none", SML_USER_SET_DEBUG, 0xFFFF0000UL},
        {"debug 0x21", SML_USER_SET_DEBUG, 0xFFFF0021UL},
        {"debug rtc events", SML_USER_SET_DEBUG, 0xFFFF0000UL | SML_DEBUG_RTC | SML_DEBUG_EVENTS},
        {"debug +events", SML_USER_SET_DEBUG, SML_DEBUG_EVENTS},
    };
    for (auto &action : actions)
    {
        run(action.line);
        bool found = false;
        for (size_t index = 0; index < sent.size(); index++)
        {
            const SML_CMD_USER_PAYLOAD *command = user(index);
            if (command != NULL && command->command == action.command &&
                command->argument == action.argument)
            {
                found = true;
            }
        }
        CHECK(found);
        if (!found)
        {
            printf("  %s\n", action.line);
        }
    }
    out = run("debug gnss +ntp");
    CHECK(sent.empty() && contains(out, "debug: bad"));
    out = run("rtc set");
    CHECK(sent.empty() && contains(out, "rtc set: when?"));
    // Unix seconds or now, never a date; nothing before 2000.
    out = run("rtc set 2026-10-18T12:34:56Z");
    CHECK(sent.empty() && contains(out, "bad value"));
    out = run("rtc set 946684799");
    CHECK(sent.empty() && contains(out, "bad value"));
    ntp_known = false;
    out = run("rtc set now");
    CHECK(sent.empty() && contains(out, "no time yet"));
    ntp_known = true;

    // SHOW: what was asked for is the answer, never the ACK; -v anywhere
    // asks the PIC for everything.
    run("pic stored");
    console.out.clear();
    answer(user(0)->token, SML_USER_SHOW, SML_RESULT_ACK, SML_ERROR_NONE);
    CHECK(console.out.empty());
    run("pic stored -v");
    CHECK(user(0) != NULL && user(0)->argument == (SML_SHOW_STORED | SML_SHOW_VERBOSE));
    run("pic -v");
    CHECK(user(0) != NULL && user(0)->argument == (SML_SHOW_ALL | SML_SHOW_VERBOSE));
    console.out.clear();
    answer(user(0)->token, SML_USER_SHOW, SML_RESULT_ACK, SML_ERROR_NONE);
    CHECK(console.out.empty());
    run("rtc   -v");
    CHECK(user(0) != NULL && user(0)->argument == (SML_SHOW_RTC | SML_SHOW_VERBOSE));
    run("tz -v");
    CHECK(user(0) != NULL && user(0)->argument == (SML_SHOW_TZ | SML_SHOW_VERBOSE));
    out = run("wifi scan -v");
    CHECK(calls == "wifi-scan ");
    run("pic save");
    console.out.clear();
    answer(user(0)->token, SML_USER_SAVE, SML_RESULT_ACCEPTED, SML_ERROR_NONE);
    answer(user(0)->token, SML_USER_SAVE, SML_RESULT_ACK, SML_ERROR_NONE);
    CHECK(contains(console.out, "PIC SAVE: ACCEPTED") && contains(console.out, "PIC SAVE: ACK"));
}


static void test_esp(void)
{
    std::string out;

    printf("ESP settings and actions\n");
    user_prefs_reset();
    out = run("ntp asymmetry -120");
    CHECK(user_prefs_ntp_asymmetry_us == -120 && contains(out, "ntp asymmetry: -120 us"));
    out = run("ntp asymmetry 20000");
    CHECK(user_prefs_ntp_asymmetry_us == -120 && contains(out, "bad value"));
    out = run("ntp asymmetry");
    CHECK(contains(out, "ntp asymmetry: -120 us"));
    out = run("ntp server 2 offset 150");
    CHECK(user_prefs_ntp_offset_us[2] == 150 && contains(out, "ntp server 2 offset: +150 us"));
    out = run("ntp server dhcp offset -50");
    CHECK(user_prefs_ntp_offset_us[0] == -50 && contains(out, "ntp server 0 offset: -50 us"));
    out = run("ntp server 0 offset");
    CHECK(contains(out, "ntp server 0 offset: -50 us"));
    out = run("ntp server 2 offset 20000");
    CHECK(user_prefs_ntp_offset_us[2] == 150 && contains(out, "-10000 to 10000"));
    out = run("ntp server 2 offset 1x");
    CHECK(user_prefs_ntp_offset_us[2] == 150 && contains(out, "-10000 to 10000"));
    out = run("ntp server");
    CHECK(contains(out, "dhcp, enable (the server DHCP offers), offset -50 us") &&
          contains(out, ", offset +150 us\n"));
    out = run("ntp server 2 other.example");
    CHECK(user_prefs_ntp_offset_us[2] == 0 && contains(out, "ntp server 2 offset: +150 us cleared"));
    run("ntp server 2 none");
    out = run("ntp server dhcp offset 0");
    CHECK(user_prefs_ntp_offset_us[0] == 0);
    out = run("ntp server dhcp prefer");
    CHECK(user_prefs_ntp_dhcp == NTP_DHCP_PREFERRED && contains(out, "ntp server 0: dhcp, prefer"));
    out = run("ntp server 0 disable");
    CHECK(user_prefs_ntp_dhcp == NTP_DHCP_DISABLED && contains(out, "dhcp, disable"));
    out = run("ntp server dhcp sometimes");
    CHECK(user_prefs_ntp_dhcp == NTP_DHCP_DISABLED && contains(out, "disable, enable, prefer or offset <us>"));
    out = run("ntp server 2 dhcp");
    CHECK(contains(out, "server 0: ntp server dhcp enable"));
    out = run("ntp server");
    CHECK(contains(out, "ntp server 0: dhcp, disable") && contains(out, "ntp server 1:"));
    user_prefs_ntp_dhcp = NTP_DHCP_ENABLED;
    out = run("ntp interval 17m");
    CHECK(user_prefs.fields.ntp.interval == 1020 && contains(out, "ntp interval: 1020 s"));
    out = run("ntp interval 10");
    CHECK(user_prefs.fields.ntp.interval == 1020 && contains(out, "(128..86400)"));
    user_prefs_set_string(user_prefs.fields.ntp.server, "rubidium.example");
    out = run("ntp server pool.ntp.org");
    CHECK(contains(out, "which one, 1..4") && strcmp(user_prefs.fields.ntp.server, "rubidium.example") == 0);
    out = run("ntp server 1 pool.ntp.org");
    CHECK(strcmp(user_prefs.fields.ntp.server, "pool.ntp.org") == 0 &&
          contains(out, "ntp server 1: pool.ntp.org, polling"));
    out = run("ntp server 3   0.pool.ntp.org");
    CHECK(strcmp(user_prefs_ntp.server[1], "0.pool.ntp.org") == 0);
    out = run("ntp server");
    CHECK(contains(out, "ntp server 1: pool.ntp.org\nntp server 2: NONE\n"
                        "ntp server 3: 0.pool.ntp.org\nntp server 4: NONE\n"));
    out = run("ntp server 3");
    CHECK(out == "ntp server 3: 0.pool.ntp.org\n");
    out = run("ntp server 3 NONE");
    CHECK(user_prefs_ntp.server[1][0] == '\0' && contains(out, "ntp server 3: NONE"));
    out = run("ntp server 1 none");
    CHECK(contains(out, "server 1 is needed") && strcmp(user_prefs.fields.ntp.server, "pool.ntp.org") == 0);
    out = run("ntp server 5 x.example");
    CHECK(contains(out, "which one"));
    out = run("ntp server 2 a.example b.example");
    CHECK(contains(out, "bad host") && user_prefs_ntp.server[0][0] == '\0');
    out = run("ntp");
    CHECK(out == "(ntp)\n(ntp sources)\n");
    out = run("ntp -v");
    CHECK(out == "(ntp -v)\n(ntp gnss)\n");
    out = run("ntp tracking");
    CHECK(out == "(ntp tracking)\n(ntp gnss)\n");
    run("ntp sync");
    CHECK(calls == "ntp-sync ");
    run("tz remote on");
    CHECK(user_prefs.fields.rtzinfo.flags.enabled);
    run("tz precision none");
    CHECK(user_prefs.fields.rtzinfo.gnss_accuracy == -1);
    run("tz precision 3");
    CHECK(user_prefs.fields.rtzinfo.gnss_accuracy == 3);
    out = run("tz precision 7");
    CHECK(user_prefs.fields.rtzinfo.gnss_accuracy == 3 && contains(out, "bad value"));
    run("tz interval 1d");
    CHECK(user_prefs.fields.rtzinfo.interval == 86400);
    run("tz lookup");
    CHECK(calls == "tz-lookup ");
    run("tz lookup check");
    CHECK(calls == "tz-check ");
    run("telnet on");
    CHECK(calls == "telnet-enable ");
    run("telnet off");
    CHECK(calls == "telnet-disable ");
    run("telnet start");
    CHECK(calls == "telnet-start ");
    run("telnet port 2323");
    CHECK(user_prefs.fields.telnet.port == 2323);
    run("update auto 3");
    CHECK(user_prefs.fields.updater.flags.auto_enabled &&
          user_prefs.fields.updater.flags.auto_time == 3);
    run("update auto off");
    CHECK(!user_prefs.fields.updater.flags.auto_enabled &&
          user_prefs.fields.updater.flags.auto_time == 3);
    run("update install");
    CHECK(calls == "update-pull ");
    out = run("update install force");
    CHECK(calls.empty() && contains(out, "expected nothing"));
    run("update esp");
    CHECK(calls == "update-esp ");
    run("update esp force");
    CHECK(calls == "update-esp-force ");
    run("update spl");
    CHECK(calls == "update-spl ");
    run("update erase pic");
    CHECK(calls == "erase-pic ");
    run("update erase spl");
    CHECK(calls == "erase-spl ");
    run("update erase all");
    CHECK(calls == "erase-all ");
    out = run("update erase");
    CHECK(calls.empty() && contains(out, "pic, spl or all"));
    run("update");
    CHECK(calls == "staged ");
    run("update -v");
    CHECK(calls == "staged-verbose ");
    run("update pic");
    CHECK(calls == "update-pic ");
    run("update pic force");
    CHECK(calls == "update-pic-force ");
    run("pic boot");
    CHECK(calls == "boot-print ");
    run("pic boot run");
    CHECK(calls == "boot-run ");
    run("pic boot hold");
    CHECK(calls == "boot-hold ");
    run("update config development");
    CHECK(strcmp(user_prefs.fields.updater.config, "development") == 0);
    run("wifi forget");
    CHECK(calls == "wifi-clear ");
    out = run("wifi scan now");
    CHECK(calls.empty() && contains(out, "expected nothing"));

    // save, defaults, reset: both chips; esp ... the ESP only.
    prefs_saves = 0;
    run("save");
    CHECK(prefs_saves == 1 && user(0) != NULL && user(0)->command == SML_USER_SAVE);
    run("esp save");
    CHECK(prefs_saves == 2 && sent.empty());
    run("defaults");
    CHECK(calls == "prefs-reset " && prefs_saves == 2 && user(0) != NULL &&
          user(0)->command == SML_USER_DEFAULTS);
    run("esp defaults");
    CHECK(calls == "prefs-reset " && prefs_saves == 2 && sent.empty());
    run("esp factory");
    CHECK(calls == "prefs-reset wifi-clear " && prefs_saves == 3);
    ESP.restarts = 0;
    run("reset");
    CHECK(ESP.restarts == 1 && user(0) != NULL && user(0)->command == SML_USER_RESET);
    run("esp reset");
    CHECK(ESP.restarts == 2 && sent.empty());

    // The PIC's commands for the ESP.
    {
        uint16_t detail;
        bool restart = false;
        prefs_saves = 0;
        calls.clear();
        CHECK(commands_from_pic(SML_USER_ESP_DEFAULTS, 0, &detail, &restart) == SML_RESULT_ACK &&
              calls == "prefs-reset " && prefs_saves == 0);
        CHECK(commands_from_pic(SML_USER_ESP_SAVE, 0, &detail, &restart) == SML_RESULT_ACK &&
              prefs_saves == 1);
        CHECK(commands_from_pic(SML_USER_ESP_TZ_REMOTE, 0, &detail, &restart) == SML_RESULT_ACK &&
              !user_prefs.fields.rtzinfo.flags.enabled && prefs_saves == 1);
        CHECK(commands_from_pic(SML_USER_ESP_TZ_REMOTE, 2, &detail, &restart) == SML_RESULT_NACK &&
              detail == SML_ERROR_ARGUMENT);
        CHECK(commands_from_pic(SML_USER_ESP_WIFI_FORGET, 0, &detail, &restart) == SML_RESULT_ACK &&
              restart);
        CHECK(commands_from_pic(SML_USER_SAVE, 0, &detail, &restart) == SML_RESULT_NACK &&
              detail == SML_ERROR_UNSUPPORTED);
    }
}


static void test_grammar(void)
{
    std::string out;

    printf("Grammar and help\n");
    out = run("frobnicate");
    CHECK(contains(out, "Unknown command 'frobnicate'"));
    out = run("tz frobnicate");
    CHECK(contains(out, "tz: nothing called 'frobnicate'"));
    out = run("BRIGHTNESS Auto");
    CHECK(settings_write(0) != NULL);
    if (settings_write(0) != NULL)
    {
        SML_CMD_SETTINGS_PAYLOAD first = *settings_write(0);
        apply(&first);
    }
    // Brightness as it is: the tubes now and the light, from STAT-UI and STAT-ENV.
    {
        SML_STAT_UI_PAYLOAD ui;
        SML_STAT_ENV_PAYLOAD env;

        memset(&ui, 0, sizeof ui);
        memset(&env, 0, sizeof env);
        ui.brightness = 1520;
        ui.brightness_target = 1600;
        env.flags = SML_ENV_LUX_PRESENT | SML_ENV_LUX_VALID;
        env.lux = 506;
        from_pic(SML_CLASS_STAT, SML_STAT_UI, &ui);
        from_pic(SML_CLASS_STAT, SML_STAT_ENV, &env);
        out = run("brightness");
        CHECK(contains(out, "brightness: AUTO 1520 -> 1600, 50.6 lux; manual 1200"));
        ui.brightness = ui.brightness_target = 100;
        from_pic(SML_CLASS_STAT, SML_STAT_UI, &ui);
        out = run("brightness");
        CHECK(contains(out, "brightness: AUTO OFF, 50.6 lux"));
    }
    out = run("verbose on");
    CHECK(contains(out, "verbose: ON"));
    out = run("verbose");
    CHECK(contains(out, "verbose: ON"));
    run("verbose off");
    out = run("help");
    CHECK(contains(out, "  tz ") && contains(out, "  esp ") && contains(out, "Times:"));
    out = run("help tz");
    CHECK(contains(out, "tz offset auto|+/-hh:mm") && contains(out, "tz lookup [check]"));
    out = run("help debug");
    CHECK(contains(out, "names: timing gnss"));
    out = run("help nothing");
    CHECK(contains(out, "no command 'nothing'"));
    out = run("");
    CHECK(contains(out, "ESP:"));
    // Every line of help is under 100 characters.
    out = run("help");
    for (const char *noun : {"tz", "display", "brightness", "beeps", "alarm", "rtc", "osc", "debug",
                             "pic", "ntp", "wifi", "telnet", "update", "esp"})
    {
        char line[32];
        snprintf(line, sizeof line, "help %s", noun);
        out += run(line);
    }
    size_t start = 0;
    size_t longest = 0;
    while (start < out.size())
    {
        size_t end = out.find('\n', start);
        if (end == std::string::npos)
            end = out.size();
        if (end - start > longest)
            longest = end - start;
        if (end - start > 100)
            printf("  too long: %s\n", out.substr(start, end - start).c_str());
        start = end + 1;
    }
    CHECK(longest <= 100);
}


// The alarm as the PIC has it: armed by the switch, sounding.
static void test_alarm(void)
{
    SML_STAT_UI_PAYLOAD ui;
    std::string out;

    printf("alarm\n");
    pic_settings.settings.flags |= SML_SETTING_ALARM_ENABLED;
    pic_settings.settings.alarm_offset_s = 27000;
    send_stat_settings();
    memset(&ui, 0, sizeof ui);
    out = run("alarm"); // no STAT-UI yet: the setting alone
    CHECK(contains(out, "alarm: ON, 07:30:00\n"));
    ui.flags = SML_UI_SWITCH | SML_UI_ALARM_ENABLED;
    from_pic(SML_CLASS_STAT, SML_STAT_UI, &ui);
    out = run("alarm");
    CHECK(contains(out, "alarm: ON, 07:30:00, ARMED\n"));
    ui.flags |= SML_UI_ALARM_ACTIVE;
    from_pic(SML_CLASS_STAT, SML_STAT_UI, &ui);
    CHECK(contains(run("alarm"), ", SOUNDING\n"));
    ui.flags = SML_UI_ALARM_ENABLED;
    from_pic(SML_CLASS_STAT, SML_STAT_UI, &ui);
    CHECK(contains(run("alarm"), ", DISARMED (switch OFF)\n"));
    // Seconds past midnight as well as a time.
    run("alarm 27900");
    CHECK(settings_write(0) != NULL && settings_write(0)->settings.alarm_offset_s == 27900 &&
          (settings_write(0)->settings.flags & SML_SETTING_ALARM_ENABLED) != 0);
    {
        SML_CMD_SETTINGS_PAYLOAD write = *settings_write(0);
        apply(&write);
    }
    CHECK(contains(run("alarm"), "alarm: ON, 07:45:00, DISARMED"));
    pic_settings.settings.flags &= (uint16_t)~SML_SETTING_ALARM_ENABLED;
    send_stat_settings();
    out = run("alarm");
    CHECK(contains(out, "alarm: OFF, 07:45:00\n"));
}


// Raw text and the link polled from blocking work.
static uint16_t handled;
static uint16_t esp_command(uint16_t, uint32_t, uint16_t *, bool *)
{
    handled++;
    return SML_RESULT_ACK;
}

static void test_text_and_poll(void)
{
    static const char raw[] = "\x1b[2J\x1b[1;1HPIC 0.3.2, up 00:00:00\r\n\x1b[0mready\x01\x80\r\n";
    SML_CMD_USER_PAYLOAD command;
    const SML_CMD_RESPONSE_PAYLOAD *response;

    printf("Raw text and polling\n");
    pic_text.clear();
    // Split in the middle of a control sequence, as the UART may deliver it.
    link_uart->rx.insert(link_uart->rx.end(), raw, raw + 6);
    pic_link_service();
    link_uart->rx.insert(link_uart->rx.end(), raw + 6, raw + sizeof(raw) - 1);
    pic_link_service();
    // The line with binary bytes in it is dropped whole.
    CHECK(pic_text == "PIC 0.3.2, up 00:00:00\r\n");

    // A frame with bytes lost from its middle, then a line of text: only the
    // line. Then an unfinished line, dropped once the link is quiet.
    {
        SML_STAT_SETTINGS_PAYLOAD body = pic_settings;
        uint8_t frame[SML_MAX_FRAME_BYTES];
        uint16_t length = sml_frame_message(SML_CLASS_STAT, SML_STAT_SETTINGS, &body, frame);
        static const char line[] = "ok\r\n";

        pic_text.clear();
        link_uart->rx.insert(link_uart->rx.end(), frame, frame + 9);
        link_uart->rx.insert(link_uart->rx.end(), frame + 20, frame + length);
        link_uart->rx.insert(link_uart->rx.end(), line, line + 4);
        pic_link_service();
        fake_millis += 1000;
        pic_link_service();
        CHECK(pic_text.empty()); // the debris and the line it ran into
        link_uart->rx.insert(link_uart->rx.end(), line, line + 4);
        pic_link_service();
        CHECK(pic_text.find("ok\r\n") != std::string::npos);
        // A line that pauses part-way, as the PIC's start-up banner does:
        // printed whole when it ends.
        pic_text.clear();
        link_uart->rx.insert(link_uart->rx.end(), raw, raw + 16);
        pic_link_service();
        fake_millis += 1000;
        pic_link_service();
        CHECK(pic_text.empty());
        link_uart->rx.insert(link_uart->rx.end(), raw + 16, raw + 38);
        pic_link_service();
        CHECK(pic_text == "PIC 0.3.2, up 00:00:00\r\n");
        // One left unfinished by a whole frame: debris, dropped.
        pic_text.clear();
        link_uart->rx.insert(link_uart->rx.end(), line, line + 2);
        pic_link_service();
        send_stat_settings();
        link_uart->rx.insert(link_uart->rx.end(), line, line + 4);
        pic_link_service();
        CHECK(pic_text == "ok\r\n");
    }

    pic_link_set_command_handler(esp_command);
    memset(&command, 0, sizeof(command));
    command.command = SML_USER_ESP_FIRST;
    command.token = 0x1234;
    collect();
    {
        uint8_t frame[SML_MAX_FRAME_BYTES];
        uint16_t length = sml_frame_message(SML_CLASS_CMD, SML_CMD_USER, &command, frame);
        link_uart->rx.insert(link_uart->rx.end(), frame, frame + length);
    }
    pic_link_poll();
    collect();
    response = sent.size() == 1 && sent[0].class_id == SML_CLASS_CMD &&
                       sent[0].message_id == SML_CMD_RESPONSE
                   ? (const SML_CMD_RESPONSE_PAYLOAD *)sent[0].payload
                   : NULL;
    CHECK(response != NULL && response->token == 0x1234 && response->status == SML_RESULT_NACK &&
          response->detail == SML_ERROR_BUSY && handled == 0);
    from_pic(SML_CLASS_CMD, SML_CMD_USER, &command);
    collect();
    response = sent.size() == 1 ? (const SML_CMD_RESPONSE_PAYLOAD *)sent[0].payload : NULL;
    CHECK(response != NULL && response->status == SML_RESULT_ACK && handled == 1);
    pic_link_set_command_handler(NULL);

    // The SPL's boot window after a reset: the link keeps going (the SPL
    // hands over by itself). Held: quiet, until STAT-SYS.
    {
        SML_BOOT_STATUS_PAYLOAD status;
        SML_STAT_SYS_PAYLOAD sys;

        memset(&status, 0, sizeof(status));
        status.mode = SML_BOOT_MODE_WINDOW;
        from_pic(SML_CLASS_BOOT, SML_BOOT_STATUS, &status);
        CHECK(!pic_link_in_spl());
        status.mode = SML_BOOT_MODE_HELD;
        from_pic(SML_CLASS_BOOT, SML_BOOT_STATUS, &status);
        CHECK(pic_link_in_spl());
        memset(&sys, 0, sizeof(sys));
        sys.uptime_s = 1;
        from_pic(SML_CLASS_STAT, SML_STAT_SYS, &sys);
        CHECK(!pic_link_in_spl());
    }

    // A fault the PIC recorded, in place of the reset cause (SML 2.6).
    {
        SML_STAT_SYS_PAYLOAD sys;
        std::string out;

        memset(&sys, 0, sizeof(sys));
        sys.reset_cause = SML_RESET_SWR;
        sys.uptime_s = 5;
        sys.pic_version = 0x000308;
        sys.flags = SML_SYS_FAULT;
        sys.fault = SML_FAULT_TRAP << 8 | SML_TRAP_ADDRESS;
        from_pic(SML_CLASS_STAT, SML_STAT_SYS, &sys);
        out = run("status");
        CHECK(contains(out, "last reset: ADDRESS trap"));
        sys.reset_cause = SML_RESET_WDTO;
        sys.fault = SML_FAULT_WATCHDOG << 8;
        from_pic(SML_CLASS_STAT, SML_STAT_SYS, &sys);
        out = run("status");
        CHECK(contains(out, "last reset: watchdog"));
        sys.flags = 0;
        sys.fault = 0;
        sys.reset_cause = SML_RESET_SWR;
        from_pic(SML_CLASS_STAT, SML_STAT_SYS, &sys);
        out = run("status");
        CHECK(contains(out, "last reset: software"));
    }
}

static void test_telnet_login(void)
{
    std::string out;

    printf("Telnet login\n");
    telnet_auth_init();
    telnet_connected = true;
    telnet_logged_in = false;
    // Looking is open; changing is not.
    out = run_telnet("tz");
    CHECK(!contains(out, "log in first"));
    out = run_telnet("ntp tracking");
    CHECK(!contains(out, "log in first"));
    out = run_telnet("update check");
    CHECK(!contains(out, "log in first") && calls == "update-check ");
    out = run_telnet("debug +sync");
    CHECK(!contains(out, "log in first"));
    out = run_telnet("pic boot");
    CHECK(!contains(out, "log in first"));
    out = run_telnet("ntp gnss");
    CHECK(contains(out, "(ntp gnss)"));
    out = run_telnet("ntp gnss -v");
    CHECK(contains(out, "(ntp gnss -v)"));
    out = run_telnet("ntp gnss reset");
    CHECK(contains(out, "log in first") && calls.empty());
    out = run("ntp gnss reset");
    CHECK(contains(out, "started again") && calls == "ntp-gnss-reset ");
    for (const char *line : {"reset", "save", "defaults", "esp reset", "esp factory", "pic reset",
                             "pic boot run", "pic bootloader", "update install", "update pic force",
                             "update erase all", "update server x.example", "wifi forget",
                             "wifi disconnect", "telnet off", "telnet port 2323",
                             "telnet passwd hunter22", "ntp server 2 x.example", "tz server x.example",
                             "tz offset +1", "brightness 100", "alarm off", "rtc set now",
                             "osc baseline 40000000", "pic resync"})
    {
        out = run_telnet(line);
        if (!contains(out, "telnet: log in first"))
            printf("  not refused: %s\n", line);
        CHECK(contains(out, "telnet: log in first") && calls.empty() && sent.empty());
    }
    // A setting shown, not changed: open.
    out = run_telnet("tz offset");
    CHECK(!contains(out, "log in first"));
    // The serial console needs no login.
    out = run("telnet login smolclock");
    CHECK(contains(out, "this console needs none"));

    // Wrong passwords: five, then a 30 s lockout; commands still run.
    for (unsigned attempt = 1; attempt < TELNET_LOGIN_GRACE; attempt++)
    {
        out = run_telnet("telnet login wrong");
        CHECK(contains(out, "wrong password (") && !telnet_logged_in);
    }
    out = run_telnet("telnet login wrong");
    CHECK(contains(out, "logins locked for 30 s"));
    out = run_telnet("telnet login smolclock");
    CHECK(contains(out, "locked for another 30 s") && !telnet_logged_in);
    out = run_telnet("tz");
    CHECK(!contains(out, "log in first") && !contains(out, "locked"));
    fake_millis += 29000;
    out = run_telnet("telnet login smolclock");
    CHECK(contains(out, "locked for another 1 s"));
    fake_millis += 1000;
    out = run_telnet("telnet login wrong"); // past the grace: locked again at once
    CHECK(contains(out, "logins locked for 30 s"));
    // A password change from the serial console ends it.
    out = run("telnet passwd abc");
    CHECK(contains(out, "4 to 64 characters"));
    out = run("telnet passwd hunter22");
    CHECK(contains(out, "password changed") && auth_stored && telnet_auth_failures() == 0);
    out = run_telnet("telnet login smolclock");
    CHECK(contains(out, "wrong password (4 tries"));
    out = run_telnet("telnet login hunter22");
    CHECK(contains(out, "telnet: logged in") && !contains(out, "default") && telnet_logged_in);
    out = run_telnet("tz offset +1");
    CHECK(!contains(out, "log in first"));
    run("tz offset auto");
    // Stored: kept over a restart, the default no longer.
    telnet_auth_init();
    CHECK(!telnet_auth_is_default());
    // help marks what needs it.
    out = run_telnet("help update");
    CHECK(contains(out, "* update install") && contains(out, "  update check"));
    out = run_telnet("help");
    CHECK(contains(out, "* reset") && contains(out, "  tz "));
    telnet_connected = false;
    telnet_logged_in = false;
}


int main(void)
{
    test_time_text();
    pic_link_init();
    link_uart = pic_link_uart();
    collect();
    // At start: SHOW everything (token 0) and a request for STAT-SETTINGS.
    CHECK(sent.size() == 2 && user(0) != NULL && user(0)->command == SML_USER_SHOW &&
          user(0)->argument == SML_SHOW_ALL && user(1) != NULL &&
          user(1)->command == SML_USER_REQUEST_MESSAGE);
    pic_settings.flags = SML_SETTINGS_VALID;
    pic_settings.revision = 5;
    pic_settings.settings.schema_version = SML_SETTINGS_SCHEMA;
    pic_settings.settings.flags = SML_SETTING_TZ_AUTO | SML_SETTING_DST_AUTO | SML_SETTING_BRI_AUTO;
    pic_settings.settings.calibration_fosc_hz = 40000000UL;
    pic_settings.settings.brightness = 2000;
    pic_settings.settings.selected_display = SML_DISPLAY_HHMM;

    test_parsers();
    test_settings();
    test_pic_actions();
    test_alarm();
    test_esp();
    test_grammar();
    test_text_and_poll();
    test_telnet_login();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
