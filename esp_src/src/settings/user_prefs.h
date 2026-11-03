/*
 * The ESP's own settings, kept in NVS as one blob (CLOCK_PREFS/PREF_STRUCT).
 *
 * NTP servers 2 to 4 are kept apart, under a key of their own (NTP_SERVERS),
 * taking their defaults without it. On load, each field is checked on its
 * own: a field out of range is put back to its default and reported, and
 * the rest are kept. A blob of the wrong size or with the wrong header
 * (anything saved by V2, or by V2+ before 0.3.1) resets everything.
 *
 * Changes made from the console apply at once; "save" (or "esp save") writes
 * them.
 */
#ifndef USER_PREFS_H
#define USER_PREFS_H

#include <Arduino.h>
#include <stdint.h>

// "SMOL". Settings saved under any other header, including V2's and up to
// 0.3.0's (0xC0FFEEEE), are not taken: the clock starts from its defaults.
#define USER_PREFS_HEADER_VALUE       0x534D4F4CUL
#define USER_PREFS_MAX_STRING_SIZE    100
#define USER_PREFS_STORED_STRING_SIZE (USER_PREFS_MAX_STRING_SIZE + 4)

// Telnet
#define TELNET_ENABLED_DEFAULT 0
#define TELNET_PORT_DEFAULT    23

// NTP: interval is the longest poll interval; the ESP polls more often
// while its corrections are large (ntp.h). 512 s: the frequency window's
// eight samples then span about an hour, long enough to measure the
// crystal to a fraction of a ppm through a few ms of jitter, short enough
// to follow its wander with temperature. Four servers at 512 s are a
// 48-byte request every 32 s on average. Server 1 is always set; 2 to 4
// are optional (empty). The defaults, one per server, "" for none: three
// independent sources, so that one going wrong is outvoted.
#define NTP_SERVERS 4
#define NTP_SERVER_DEFAULTS                                                \
    {                                                                      \
        "rubidium.darksky.io", "time.cloudflare.com", "ntp1.npl.co.uk", "" \
    }
#define NTP_INTERVAL_DEFAULT 512
#define NTP_INTERVAL_MIN     128
#define NTP_INTERVAL_MAX     86400

// Remote tzinfo (protocol v1). gnss_accuracy is the number of decimal places
// of the coordinates sent (0: 111 km ... 6: 11 cm), or -1 to send none, in
// which case the server locates the clock by its IP address.
#define TZINFO_ENABLED_DEFAULT       1
#define TZINFO_SERVER_DEFAULT        "rubidium.darksky.io"
#define TZINFO_PATH_DEFAULT          "/smol_clock/api/v1/tzinfo"
#define TZINFO_INTERVAL_DEFAULT      86400
#define TZINFO_INTERVAL_MIN          900
#define TZINFO_INTERVAL_MAX          604800
#define TZINFO_GNSS_ACCURACY_DEFAULT 1
#define TZINFO_GNSS_ACCURACY_MIN     (-1)
#define TZINFO_GNSS_ACCURACY_MAX     6

// OTA updates
#define UPDATER_SERVER_DEFAULT       "rubidium.darksky.io"
#define UPDATER_PATH_DEFAULT         "/smol_clock/v2plus/manifest.json"
// The format 1 manifest, removed: a stored path equal to it becomes the default.
#define UPDATER_PATH_FORMAT_1        "/smol_clock/v2plus/update.json"
#define UPDATER_CONFIG_DEFAULT       ""
#define UPDATER_AUTO_ENABLED_DEFAULT 1
#define UPDATER_AUTO_HOUR_DEFAULT    4
#define UPDATER_AUTO_HOUR_MAX        23

typedef union
{
    struct __attribute__((packed)) UserPrefsFields
    {
        uint32_t header;
        struct __attribute__((packed))
        {
            struct __attribute__((packed))
            {
                bool enabled : 1;
                uint16_t padding : 15;
            } flags;
            uint16_t port;
        } telnet;
        struct __attribute__((packed))
        {
            char server[USER_PREFS_STORED_STRING_SIZE];
            uint32_t interval;
        } ntp;
        struct __attribute__((packed))
        {
            struct __attribute__((packed))
            {
                bool enabled : 1;
                uint16_t padding : 15;
            } flags;
            char server[USER_PREFS_STORED_STRING_SIZE];
            char path[USER_PREFS_STORED_STRING_SIZE];
            uint32_t interval;
            int32_t gnss_accuracy;
        } rtzinfo;
        struct __attribute__((packed))
        {
            struct __attribute__((packed))
            {
                uint16_t auto_enabled : 1;
                uint16_t auto_time : 8; // local hour of the automatic check
                uint16_t padding : 7;
            } flags;
            char server[USER_PREFS_STORED_STRING_SIZE];
            char path[USER_PREFS_STORED_STRING_SIZE];
            char config[USER_PREFS_STORED_STRING_SIZE];
        } updater;
        uint32_t checksum; // unused
    } fields;
    uint8_t raw[sizeof(struct UserPrefsFields)];
} UserPrefs;

static_assert(sizeof(UserPrefs) == 652, "the stored layout must not change");

// NTP servers 2 to 4.
typedef struct __attribute__((packed))
{
    uint32_t header;
    char server[NTP_SERVERS - 1][USER_PREFS_STORED_STRING_SIZE];
} UserPrefsNtp;

extern UserPrefs user_prefs;
extern UserPrefsNtp user_prefs_ntp;

// Loads, checks and repairs; saves if anything was repaired.
void user_prefs_init(Stream *output);
void user_prefs_reset(void);
void user_prefs_save(void);
// The settings differ from those last loaded or saved.
bool user_prefs_unsaved(void);
// Every setting, by what it does, and whether they are saved.
void user_prefs_print(Stream *output);

// Copies a string setting, NUL-terminated and truncated to fit.
void user_prefs_set_string(char *field, const char *value);
// NTP server index (0 to NTP_SERVERS - 1): server 1 is user_prefs' own.
// Empty if not set.
char *user_prefs_ntp_server(int index);

#endif // USER_PREFS_H
