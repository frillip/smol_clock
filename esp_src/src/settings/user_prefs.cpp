#include "user_prefs.h"

#include <Preferences.h>
#include <string.h>

#include "../util/time_text.h"

#define NAMESPACE "CLOCK_PREFS"
#define KEY       "PREF_STRUCT"
#define NTP_KEY   "NTP_SERVERS"

UserPrefs user_prefs;
UserPrefsNtp user_prefs_ntp;

static Preferences store;
static UserPrefs saved; // as last loaded or saved, to tell what is unsaved
static UserPrefsNtp saved_ntp;
static const char *const ntp_defaults[NTP_SERVERS] = NTP_SERVER_DEFAULTS;

void user_prefs_set_string(char *field, const char *value)
{
    memset(field, 0, USER_PREFS_STORED_STRING_SIZE);
    strlcpy(field, value != NULL ? value : "", USER_PREFS_MAX_STRING_SIZE + 1);
}


static bool string_ok(const char *field)
{
    return memchr(field, '\0', USER_PREFS_STORED_STRING_SIZE) != NULL;
}


char *user_prefs_ntp_server(int index)
{
    return index == 0 ? user_prefs.fields.ntp.server : user_prefs_ntp.server[index - 1];
}


void user_prefs_reset(void)
{
    UserPrefs *p = &user_prefs;

    memset(p->raw, 0, sizeof(p->raw));
    p->fields.header = USER_PREFS_HEADER_VALUE;
    p->fields.telnet.flags.enabled = TELNET_ENABLED_DEFAULT;
    p->fields.telnet.port = TELNET_PORT_DEFAULT;
    p->fields.ntp.interval = NTP_INTERVAL_DEFAULT;
    p->fields.rtzinfo.flags.enabled = TZINFO_ENABLED_DEFAULT;
    user_prefs_set_string(p->fields.rtzinfo.server, TZINFO_SERVER_DEFAULT);
    user_prefs_set_string(p->fields.rtzinfo.path, TZINFO_PATH_DEFAULT);
    p->fields.rtzinfo.interval = TZINFO_INTERVAL_DEFAULT;
    p->fields.rtzinfo.gnss_accuracy = TZINFO_GNSS_ACCURACY_DEFAULT;
    p->fields.updater.flags.auto_enabled = UPDATER_AUTO_ENABLED_DEFAULT;
    p->fields.updater.flags.auto_time = UPDATER_AUTO_HOUR_DEFAULT;
    user_prefs_set_string(p->fields.updater.server, UPDATER_SERVER_DEFAULT);
    user_prefs_set_string(p->fields.updater.path, UPDATER_PATH_DEFAULT);
    user_prefs_set_string(p->fields.updater.config, UPDATER_CONFIG_DEFAULT);
    memset(&user_prefs_ntp, 0, sizeof(user_prefs_ntp));
    user_prefs_ntp.header = USER_PREFS_HEADER_VALUE;
    for (int index = 0; index < NTP_SERVERS; index++)
    {
        user_prefs_set_string(user_prefs_ntp_server(index), ntp_defaults[index]);
    }
}


// Puts each bad field back to its default. Returns the number repaired.
static unsigned repair(Stream *output)
{
    UserPrefs *p = &user_prefs;
    unsigned repaired = 0;

#define REPAIR(condition, name, action)                                   \
    if (!(condition))                                                     \
    {                                                                     \
        output->printf("Settings: %s invalid, default restored\n", name); \
        action;                                                           \
        repaired++;                                                       \
    }

    REPAIR(p->fields.telnet.port != 0, "telnet.port", p->fields.telnet.port = TELNET_PORT_DEFAULT);
    REPAIR(string_ok(p->fields.ntp.server) && p->fields.ntp.server[0] != '\0', "ntp.server",
           user_prefs_set_string(p->fields.ntp.server, ntp_defaults[0]));
    REPAIR(p->fields.ntp.interval >= NTP_INTERVAL_MIN && p->fields.ntp.interval <= NTP_INTERVAL_MAX,
           "ntp.interval", p->fields.ntp.interval = NTP_INTERVAL_DEFAULT);
    REPAIR(string_ok(p->fields.rtzinfo.server) && p->fields.rtzinfo.server[0] != '\0',
           "rtzinfo.server",
           user_prefs_set_string(p->fields.rtzinfo.server, TZINFO_SERVER_DEFAULT));
    REPAIR(string_ok(p->fields.rtzinfo.path) && p->fields.rtzinfo.path[0] == '/', "rtzinfo.path",
           user_prefs_set_string(p->fields.rtzinfo.path, TZINFO_PATH_DEFAULT));
    REPAIR(p->fields.rtzinfo.interval >= TZINFO_INTERVAL_MIN &&
               p->fields.rtzinfo.interval <= TZINFO_INTERVAL_MAX,
           "rtzinfo.interval", p->fields.rtzinfo.interval = TZINFO_INTERVAL_DEFAULT);
    REPAIR(p->fields.rtzinfo.gnss_accuracy >= TZINFO_GNSS_ACCURACY_MIN &&
               p->fields.rtzinfo.gnss_accuracy <= TZINFO_GNSS_ACCURACY_MAX,
           "rtzinfo.gnss_accuracy", p->fields.rtzinfo.gnss_accuracy = TZINFO_GNSS_ACCURACY_DEFAULT);
    REPAIR(string_ok(p->fields.updater.server) && p->fields.updater.server[0] != '\0',
           "updater.server",
           user_prefs_set_string(p->fields.updater.server, UPDATER_SERVER_DEFAULT));
    REPAIR(string_ok(p->fields.updater.path) && p->fields.updater.path[0] == '/', "updater.path",
           user_prefs_set_string(p->fields.updater.path, UPDATER_PATH_DEFAULT));
    if (strcmp(p->fields.updater.path, UPDATER_PATH_FORMAT_1) == 0)
    {
        output->printf("Settings: updater.path %s, now %s (manifest format 2)\n",
                       UPDATER_PATH_FORMAT_1, UPDATER_PATH_DEFAULT);
        user_prefs_set_string(p->fields.updater.path, UPDATER_PATH_DEFAULT);
        repaired++;
    }
    REPAIR(string_ok(p->fields.updater.config), "updater.config",
           user_prefs_set_string(p->fields.updater.config, UPDATER_CONFIG_DEFAULT));
    REPAIR(p->fields.updater.flags.auto_time <= UPDATER_AUTO_HOUR_MAX, "updater.auto_time",
           p->fields.updater.flags.auto_time = UPDATER_AUTO_HOUR_DEFAULT);
    for (int index = 1; index < NTP_SERVERS; index++)
    {
        char *server = user_prefs_ntp_server(index);

        REPAIR(string_ok(server), "ntp.server", user_prefs_set_string(server, ""));
    }
#undef REPAIR
    return repaired;
}


void user_prefs_init(Stream *output)
{
    size_t stored;
    size_t stored_ntp;
    unsigned repaired = 0;

    store.begin(NAMESPACE, true);
    stored = store.getBytesLength(KEY);
    if (stored == sizeof(user_prefs.raw))
    {
        store.getBytes(KEY, user_prefs.raw, sizeof(user_prefs.raw));
    }
    stored_ntp = store.getBytesLength(NTP_KEY);
    if (stored_ntp == sizeof(user_prefs_ntp))
    {
        store.getBytes(NTP_KEY, &user_prefs_ntp, sizeof(user_prefs_ntp));
    }
    store.end();
    // Servers 2 to 4 not saved: from the defaults.
    if (stored_ntp != sizeof(user_prefs_ntp) || user_prefs_ntp.header != USER_PREFS_HEADER_VALUE)
    {
        memset(&user_prefs_ntp, 0, sizeof(user_prefs_ntp));
        user_prefs_ntp.header = USER_PREFS_HEADER_VALUE;
        for (int index = 1; index < NTP_SERVERS; index++)
        {
            user_prefs_set_string(user_prefs_ntp_server(index), ntp_defaults[index]);
        }
        repaired++;
    }

    if (stored != sizeof(user_prefs.raw) || user_prefs.fields.header != USER_PREFS_HEADER_VALUE)
    {
        if (stored == sizeof(user_prefs.raw))
        {
            output->printf("Settings: from other firmware, defaults saved\n");
        }
        else
        {
            output->printf("Settings: none stored (%u bytes), defaults saved\n", (unsigned)stored);
        }
        user_prefs_reset();
        user_prefs_save();
        return;
    }
    if (repair(output) + repaired != 0)
    {
        user_prefs_save();
    }
    saved = user_prefs;
    saved_ntp = user_prefs_ntp;
}


bool user_prefs_unsaved(void)
{
    return memcmp(saved.raw, user_prefs.raw, sizeof(saved.raw)) != 0 ||
           memcmp(&saved_ntp, &user_prefs_ntp, sizeof(saved_ntp)) != 0;
}


void user_prefs_save(void)
{
    store.begin(NAMESPACE, false);
    store.putBytes(KEY, user_prefs.raw, sizeof(user_prefs.raw));
    store.putBytes(NTP_KEY, &user_prefs_ntp, sizeof(user_prefs_ntp));
    store.end();
    saved = user_prefs;
    saved_ntp = user_prefs_ntp;
}


void user_prefs_print(Stream *output)
{
    const UserPrefs *p = &user_prefs;

    output->printf("Settings: %s\n", user_prefs_unsaved() ? "NOT SAVED" : "saved");
    output->printf("  NTP: 1: %s", p->fields.ntp.server);
    for (int index = 1; index < NTP_SERVERS; index++)
    {
        if (user_prefs_ntp_server(index)[0] != '\0')
        {
            output->printf(", %d: %s", index + 1, user_prefs_ntp_server(index));
        }
    }
    output->printf("; interval %lu s\n", (unsigned long)p->fields.ntp.interval);
    output->printf("  TZ lookups: %s, https://%s%s, every ",
                   p->fields.rtzinfo.flags.enabled ? "ON" : "OFF", p->fields.rtzinfo.server,
                   p->fields.rtzinfo.path);
    time_print_duration(output, p->fields.rtzinfo.interval);
    if (p->fields.rtzinfo.gnss_accuracy < 0)
    {
        output->printf(", position NONE\n");
    }
    else
    {
        output->printf(", position %ld dp\n", (long)p->fields.rtzinfo.gnss_accuracy);
    }
    output->printf("  Updates: https://%s%s, profile %s, auto ", p->fields.updater.server,
                   p->fields.updater.path,
                   p->fields.updater.config[0] != '\0' ? p->fields.updater.config : "DEFAULT");
    if (p->fields.updater.flags.auto_enabled)
    {
        output->printf("%02u:00\n", p->fields.updater.flags.auto_time);
    }
    else
    {
        output->printf("OFF\n");
    }
    output->printf("  Telnet: start-up %s, port %u\n",
                   p->fields.telnet.flags.enabled ? "ON" : "OFF", p->fields.telnet.port);
}
