#include "commands.h"

#include <ctype.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <string.h>
#include <strings.h>

#include "../net/telnet.h"
#include "../net/updater.h"
#include "../net/wifi_link.h"
#include "../pic/pic_boot.h"
#include "../pic/pic_link.h"
#include "../pic/pic_report.h"
#include "../settings/user_prefs.h"
#include "../timing/ntp.h"
#include "../tz/leap.h"
#include "../tz/tzinfo.h"
#include "../util/parse.h"
#include "../util/time_text.h"
#include "../version.h"

// The PIC's limits, checked again there: offsets and the manual brightness.
#define OFFSET_LIMIT_S    86399
#define BRIGHTNESS_FULL   4000
#define DISPLAY_MODES     "hhmm mmss ssmm yyyy mmdd delta uncertainty" // display_codes[]
#define STAT_SETTINGS_KEY ((uint32_t)SML_CLASS_STAT << 8 | SML_STAT_SETTINGS)

// ---- The table ----------------------------------------------------------------

typedef enum
{
    VALUE_NONE,     // an action: nothing may follow
    VALUE_OPTION,   // an action: nothing, or one of the choices
    VALUE_CHOICE,   // one of the choices
    VALUE_BOOL,     // on/off, 1/0, yes/no, true/false
    VALUE_NUMBER,   // minimum..maximum
    VALUE_DURATION, // seconds, with s, m, h or d, minimum..maximum
    VALUE_OFFSET,   // +/-h[:mm[:ss]]
    VALUE_TIME,     // h:mm[:ss], or seconds past midnight
    VALUE_MOMENT,   // ISO 8601 or Unix seconds
    VALUE_TEXT,     // up to USER_PREFS_MAX_STRING_SIZE
    VALUE_WORDS,    // the rest of the line, as it is
} ValueKind;

typedef struct
{
    bool given;       // false: show the value instead
    int choice;       // the index among the entry's choices, or -1
    int64_t number;   // BOOL (0/1), NUMBER, DURATION, OFFSET, TIME, MOMENT
    const char *text; // the value as typed
    bool verbose;     // -v at the end of the line
    bool local;       // MOMENT: a wall-clock time, no zone given
} Value;

struct Entry;
typedef void (*Handler)(Stream *output, const struct Entry *entry, const Value *value);

// One thing a noun can be told. word NULL is the noun's own value
// ("brightness auto"): anything after the noun that is not another entry's
// word. Given no value, a setting shows itself.
typedef struct Entry
{
    const char *word;
    ValueKind kind;
    const char *choices; // words also accepted, space-separated, or NULL
    int64_t minimum;
    int64_t maximum;
    const char *usage; // the value, for help
    const char *help;
    Handler handler;
} Entry;

typedef struct
{
    const char *name;
    const char *help;
    void (*bare)(Stream *output, bool verbose); // the noun alone (and -v)
    const Entry *entries;
    size_t count;
} Noun;

// Information is printed briefly, as chronyc does, unless asked for with -v
// or the verbose setting is on. Not saved: off at restart.
static bool verbose_setting;

// ---- Helpers ------------------------------------------------------------------

static const char *entry_name(const Entry *entry)
{
    return entry->word != NULL ? entry->word : "";
}

// The PIC's settings, to change one: false (and said why) if not known yet.
static bool pic_settings(Stream *output, SML_SETTINGS_BODY *body)
{
    if (pic_link_settings(body))
    {
        return true;
    }
    output->printf("PIC settings not known yet; try again\n");
    return false;
}

static void write_pic_settings(Stream *output, const SML_SETTINGS_BODY *body)
{
    if (!pic_link_write_settings(body, output))
    {
        output->printf("PIC settings not known yet; try again\n");
    }
}

static void set_flag(SML_SETTINGS_BODY *body, uint16_t flag, bool on)
{
    body->flags = on ? (uint16_t)(body->flags | flag) : (uint16_t)(body->flags & ~flag);
}

static bool has_flag(const SML_SETTINGS_BODY *body, uint16_t flag)
{
    return (body->flags & flag) != 0;
}

static const char *on_off(bool on)
{
    return on ? "ON" : "OFF";
}

// The n-th space-separated word of words, into word.
static void nth_word(const char *words, int n, char *word, size_t size)
{
    while (n-- > 0)
    {
        words = strchr(words, ' ') + 1;
    }
    size_t length = strcspn(words, " ");
    if (length >= size)
    {
        length = size - 1;
    }
    memcpy(word, words, length);
    word[length] = '\0';
}

static int find_choice(const char *choices, const char *text)
{
    int index = 0;

    while (choices != NULL && *choices != '\0')
    {
        size_t length = strcspn(choices, " ");

        if (strlen(text) == length && strncasecmp(choices, text, length) == 0)
        {
            return index;
        }
        choices += length;
        while (*choices == ' ')
        {
            choices++;
        }
        index++;
    }
    return -1;
}

static void request_pic(Stream *output, uint16_t command, uint32_t argument, bool verbose)
{
    if (!pic_link_is_sml())
    {
        output->printf("PIC not in SML mode; sending anyway\n");
    }
    pic_link_request(command, argument, output, verbose);
}

// The PIC prints the topic: briefly, or everything. Its ACK is not news.
static void show_pic(Stream *output, uint32_t topic, bool verbose)
{
    request_pic(output, SML_USER_SHOW, topic | (verbose ? SML_SHOW_VERBOSE : 0U), false);
}

// ---- Debug names -------------------------------------------------------------

static const struct
{
    const char *name;
    uint16_t bit;
} debug_names[] = {
    {"timing", SML_DEBUG_TIMING},   {"gnss", SML_DEBUG_GNSS}, {"sync", SML_DEBUG_SYNC},
    {"sources", SML_DEBUG_SOURCES}, {"ntp", SML_DEBUG_NTP},   {"minute", SML_DEBUG_MINUTE},
    {"tuning", SML_DEBUG_TUNING},   {"rtc", SML_DEBUG_RTC},   {"events", SML_DEBUG_EVENTS},
    {"all", SML_DEBUG_ALL},         {"none", 0},
};

#define DEBUG_NAME_COUNT (sizeof(debug_names) / sizeof(debug_names[0]))

static void print_debug_names(Stream *output)
{
    size_t index;

    for (index = 0; index < DEBUG_NAME_COUNT; index++)
    {
        output->printf("%s%s", index == 0 ? "" : " ", debug_names[index].name);
    }
}

static bool debug_word(const char *word, uint16_t *bits)
{
    size_t index;
    uint32_t value;

    for (index = 0; index < DEBUG_NAME_COUNT; index++)
    {
        if (strcasecmp(word, debug_names[index].name) == 0)
        {
            *bits = debug_names[index].bit;
            return true;
        }
    }
    if (word[0] == '0' && (word[1] == 'x' || word[1] == 'X'))
    {
        char *end;
        unsigned long parsed = strtoul(word + 2, &end, 16);

        if (*end != '\0' || end == word + 2 || parsed > 0xFFFFUL)
        {
            return false;
        }
        *bits = (uint16_t)parsed;
        return true;
    }
    if (parse_u32(word, 0, 0xFFFF, &value))
    {
        *bits = (uint16_t)value;
        return true;
    }
    return false;
}

/*
 * SET_DEBUG's argument, clear << 16 | set, from the words: none shows the
 * mask (0); plain names set it exactly ("gnss sync"); names each with + or -
 * add and remove ("+ntp -sync"). Separated by spaces or commas.
 */
static bool debug_argument(const char *text, uint32_t *argument)
{
    uint16_t set = 0;
    uint16_t clear = 0;
    int signed_words = 0;
    int plain_words = 0;

    while (*text != '\0')
    {
        char word[16];
        size_t length;
        char sign = 0;
        uint16_t bits;

        while (*text == ' ' || *text == ',')
        {
            text++;
        }
        length = strcspn(text, " ,");
        if (length == 0)
        {
            break;
        }
        if (length >= sizeof(word))
        {
            return false;
        }
        memcpy(word, text, length);
        word[length] = '\0';
        text += length;
        if (word[0] == '+' || word[0] == '-')
        {
            sign = word[0];
            memmove(word, word + 1, length);
        }
        if (!debug_word(word, &bits))
        {
            return false;
        }
        if (sign == '-')
        {
            clear |= bits;
            signed_words++;
        }
        else if (sign == '+')
        {
            set |= bits;
            signed_words++;
        }
        else
        {
            set |= bits;
            plain_words++;
        }
    }
    if (signed_words != 0 && plain_words != 0)
    {
        return false;
    }
    if (plain_words != 0)
    {
        clear = 0xFFFF; // exactly these
    }
    *argument = (uint32_t)clear << 16 | set;
    return true;
}

// ---- Top level -----------------------------------------------------------------

const char *commands_reset_reason(void)
{
    esp_reset_reason_t reason = esp_reset_reason();

    switch (reason)
    {
        // The EN pin (the USB-UART's DTR/RTS reset) reports as power-on.
        case ESP_RST_POWERON: return "power-on or EN pin";
        case ESP_RST_EXT: return "external pin";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "SDIO";
        default: return "unknown";
    }
}

void commands_print_status(Stream *output, bool verbose)
{
    uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);

    if (!verbose)
    {
        output->printf("\nESP:      %s %s, up ", ESP_VERSION, wifi_link_hostname());
        time_print_duration(output, up_s);
        output->printf("\n");
        ntp_print(output, false);
        tzinfo_print(output, false);
        leap_print(output, false);
        wifi_link_print(output, false);
        telnet_print(output, false);
        output->printf("\n");
        pic_report_print(output, false);
        return;
    }
    output->printf("\n=== ESP %s, %s ===\n", ESP_VERSION, wifi_link_hostname());
    output->printf("Up:        ");
    time_print_duration(output, up_s);
    output->printf(", last reset: %s\n", commands_reset_reason());
    ntp_print(output, true);
    output->printf("\n=== Time zone ===\n");
    tzinfo_print(output, true);
    leap_print(output, true);
    output->printf("\n=== Wi-Fi ===\n");
    wifi_link_print(output, true);
    telnet_print(output, true);
    pic_report_print(output, true);
}

static void esp_restart(Stream *output)
{
    output->printf("Restarting the ESP\n");
    delay(100);
    ESP.restart();
}

static void esp_defaults(Stream *output)
{
    bool telnet_was_enabled = user_prefs.fields.telnet.flags.enabled;

    user_prefs_reset();
    output->printf("ESP settings: DEFAULTS, not saved\n");
    if (telnet_was_enabled && !user_prefs.fields.telnet.flags.enabled)
    {
        output->printf("Telnet start-up now OFF\n");
    }
}

static void esp_save(Stream *output)
{
    user_prefs_save();
    output->printf("ESP settings saved\n");
}

static void bare_status(Stream *output, bool verbose)
{
    commands_print_status(output, verbose);
}

static void bare_save(Stream *output, bool verbose)
{
    (void)verbose;
    esp_save(output);
    request_pic(output, SML_USER_SAVE, 0, false);
}

static void bare_defaults(Stream *output, bool verbose)
{
    (void)verbose;
    esp_defaults(output);
    request_pic(output, SML_USER_DEFAULTS, 0, false);
}

static void bare_reset(Stream *output, bool verbose)
{
    (void)verbose;
    output->printf("Resetting the PIC and the ESP\n");
    pic_link_request(SML_USER_RESET, 0, NULL);
    pic_link_uart()->flush();
    esp_restart(output);
}

static void bare_verbose(Stream *output, bool verbose)
{
    (void)verbose;
    output->printf("verbose: %s\n", on_off(verbose_setting));
}

static void set_verbose(Stream *output, const Entry *entry, const Value *value)
{
    (void)entry;
    if (value->given)
    {
        verbose_setting = value->number != 0;
    }
    bare_verbose(output, false);
}

static void print_help(Stream *output, const char *name);

static void bare_help(Stream *output, bool verbose)
{
    (void)verbose;
    print_help(output, NULL);
}

static void help_noun(Stream *output, const Entry *entry, const Value *value)
{
    (void)entry;
    print_help(output, value->given ? value->text : NULL);
}

static const Entry verbose_entries[] = {
    {NULL, VALUE_BOOL, NULL, 0, 1, "on|off", "Everything without -v, until restart", set_verbose},
};

static const Entry help_entries[] = {
    {NULL, VALUE_WORDS, NULL, 0, 0, "<command>", "That command's settings and actions", help_noun},
};

// ---- The time zone ---------------------------------------------------------------

// The ESP's lookups, then the PIC: what is in force, and why.
static void show_tz(Stream *output, bool verbose)
{
    tzinfo_print(output, verbose);
    leap_print(output, verbose);
    show_pic(output, SML_SHOW_TZ, verbose);
}

static void tz_offset(Stream *output, const Entry *entry, const Value *value)
{
    SML_SETTINGS_BODY body;

    (void)entry;
    if (!pic_settings(output, &body))
    {
        return;
    }
    if (!value->given)
    {
        output->printf("tz offset: %s (", has_flag(&body, SML_SETTING_TZ_AUTO) ? "AUTO" : "MANUAL");
        time_print_offset(output, body.base_offset_s);
        output->printf(")\n");
        return;
    }
    if (value->choice == 0) // auto
    {
        set_flag(&body, SML_SETTING_TZ_AUTO, true);
    }
    else
    {
        body.base_offset_s = (int32_t)value->number;
        set_flag(&body, SML_SETTING_TZ_AUTO, false);
    }
    write_pic_settings(output, &body);
}

static void tz_dst(Stream *output, const Entry *entry, const Value *value)
{
    SML_SETTINGS_BODY body;

    (void)entry;
    if (!pic_settings(output, &body))
    {
        return;
    }
    if (!value->given)
    {
        output->printf("tz dst: %s\n", has_flag(&body, SML_SETTING_DST_AUTO)     ? "AUTO"
                                       : has_flag(&body, SML_SETTING_DST_ACTIVE) ? "ON"
                                                                                 : "OFF");
        return;
    }
    // auto: from the zone, the manual state kept for when it is turned off.
    set_flag(&body, SML_SETTING_DST_AUTO, value->choice == 0);
    if (value->choice != 0)
    {
        set_flag(&body, SML_SETTING_DST_ACTIVE, value->choice == 1);
    }
    write_pic_settings(output, &body);
}

static void tz_dst_offset(Stream *output, const Entry *entry, const Value *value)
{
    SML_SETTINGS_BODY body;

    (void)entry;
    if (!pic_settings(output, &body))
    {
        return;
    }
    if (!value->given)
    {
        output->printf("tz dst-offset: ");
        time_print_offset(output, body.dst_offset_s);
        output->printf("\n");
        return;
    }
    body.dst_offset_s = (int32_t)value->number;
    write_pic_settings(output, &body);
}

static void tz_lookup(Stream *output, const Entry *entry, const Value *value)
{
    (void)entry;
    if (value->choice == 0) // check
    {
        tzinfo_check(output);
    }
    else
    {
        tzinfo_update(output);
    }
}

static void tz_remote(Stream *output, const Entry *entry, const Value *value)
{
    (void)entry;
    if (value->given)
    {
        tzinfo_set_enabled(value->number != 0);
    }
    output->printf("tz remote: %s\n", on_off(user_prefs.fields.rtzinfo.flags.enabled));
}

static void tz_server(Stream *output, const Entry *entry, const Value *value)
{
    (void)entry;
    if (value->given)
    {
        tzinfo_set_server(value->text);
    }
    output->printf("tz server: %s\n", user_prefs.fields.rtzinfo.server);
}

static void tz_path(Stream *output, const Entry *entry, const Value *value)
{
    (void)entry;
    if (value->given)
    {
        tzinfo_set_path(value->text);
    }
    output->printf("tz path: %s\n", user_prefs.fields.rtzinfo.path);
}

static void tz_interval(Stream *output, const Entry *entry, const Value *value)
{
    (void)entry;
    if (value->given)
    {
        user_prefs.fields.rtzinfo.interval = (uint32_t)value->number;
    }
    output->printf("tz interval: ");
    time_print_duration(output, user_prefs.fields.rtzinfo.interval);
    output->printf("\n");
}

static void tz_precision(Stream *output, const Entry *entry, const Value *value)
{
    (void)entry;
    if (value->given)
    {
        user_prefs.fields.rtzinfo.gnss_accuracy = value->choice == 0 ? -1 : (int32_t)value->number;
    }
    if (user_prefs.fields.rtzinfo.gnss_accuracy < 0)
    {
        output->printf("tz precision: NONE (GeoIP)\n");
    }
    else
    {
        output->printf("tz precision: %ld dp\n", (long)user_prefs.fields.rtzinfo.gnss_accuracy);
    }
}

static const Entry tz_entries[] = {
    {"offset", VALUE_OFFSET, "auto", -OFFSET_LIMIT_S, OFFSET_LIMIT_S, "auto|+/-hh:mm",
     "Base offset: from the zone, or manual", tz_offset},
    {"dst", VALUE_CHOICE, "auto on off", 0, 0, "auto|on|off", "DST: from the zone, or manual",
     tz_dst},
    {"dst-offset", VALUE_OFFSET, NULL, -OFFSET_LIMIT_S, OFFSET_LIMIT_S, "+/-hh:mm",
     "Manual DST amount", tz_dst_offset},
    {"lookup", VALUE_OPTION, "check", 0, 0, "[check]",
     "Look the zone up now and use it (check: only print it)", tz_lookup},
    {"remote", VALUE_BOOL, NULL, 0, 1, "on|off", "Look the zone up from the tzinfo server",
     tz_remote},
    {"server", VALUE_TEXT, NULL, 0, 0, "<host>", "tzinfo server", tz_server},
    {"path", VALUE_TEXT, NULL, 0, 0, "<path>", "tzinfo path", tz_path},
    {"interval", VALUE_DURATION, NULL, TZINFO_INTERVAL_MIN, TZINFO_INTERVAL_MAX, "<time>",
     "Between lookups (s, or 15m, 1d)", tz_interval},
    {"precision", VALUE_NUMBER, "none", 0, TZINFO_GNSS_ACCURACY_MAX, "none|0..6",
     "Decimal places of position sent; none: GeoIP", tz_precision},
};

// ---- The display ----------------------------------------------------------------

// The SML_DISPLAY_* value of each word of DISPLAY_MODES.
static const uint8_t display_codes[] = {
    SML_DISPLAY_HHMM, SML_DISPLAY_MMSS,  SML_DISPLAY_SSMM,        SML_DISPLAY_YYYY,
    SML_DISPLAY_MMDD, SML_DISPLAY_DELTA, SML_DISPLAY_UNCERTAINTY,
};

// A display mode as the display shows it: HHMM.
static void print_mode(Stream *output, uint8_t selected)
{
    char mode[16] = "?";
    char *letter;
    size_t index;

    for (index = 0; index < sizeof(display_codes); index++)
    {
        if (display_codes[index] == selected)
        {
            nth_word(DISPLAY_MODES, (int)index, mode, sizeof(mode));
        }
    }
    for (letter = mode; *letter != '\0'; letter++)
    {
        *letter = (char)toupper((unsigned char)*letter);
    }
    output->printf("%s", mode);
}

// A UTC instant (2027-01-01T00:00:00Z) or a local wall-clock time
// (2027-01-01T00:00:00 local).
static void print_epoch(Stream *output, uint32_t epoch, bool local)
{
    if (epoch == 0 && !local)
    {
        output->printf("NONE");
    }
    else if (local)
    {
        time_print_wall(output, epoch);
        output->printf(" local");
    }
    else
    {
        time_print_utc(output, epoch);
    }
}

// The PIC prints the display as running, the brightness and the controls.
static void show_display(Stream *output, bool verbose)
{
    show_pic(output, SML_SHOW_DISPLAY, verbose);
}

static void display_mode(Stream *output, const Entry *entry, const Value *value)
{
    SML_SETTINGS_BODY body;

    (void)entry;
    if (!pic_settings(output, &body))
    {
        return;
    }
    if (!value->given)
    {
        output->printf("display: ");
        print_mode(output, body.selected_display);
        output->printf("\n");
        return;
    }
    body.selected_display = display_codes[value->choice];
    write_pic_settings(output, &body);
}

static void display_hours(Stream *output, const Entry *entry, const Value *value)
{
    SML_SETTINGS_BODY body;

    (void)entry;
    if (!pic_settings(output, &body))
    {
        return;
    }
    if (!value->given)
    {
        output->printf("display hours: %s\n",
                       has_flag(&body, SML_SETTING_FORMAT_12H) ? "12" : "24");
        return;
    }
    set_flag(&body, SML_SETTING_FORMAT_12H, value->choice == 0);
    write_pic_settings(output, &body);
}

static void display_epoch(Stream *output, const Entry *entry, const Value *value)
{
    SML_SETTINGS_BODY body;

    (void)entry;
    if (!pic_settings(output, &body))
    {
        return;
    }
    if (!value->given)
    {
        output->printf("display epoch: ");
        print_epoch(output, body.delta_epoch_utc, has_flag(&body, SML_SETTING_DELTA_LOCAL));
        output->printf("\n");
        return;
    }
    // A date and time without a zone is local, counted against the clock's
    // local time: no time zone data needed for the past or the far future.
    body.delta_epoch_utc = (uint32_t)value->number;
    set_flag(&body, SML_SETTING_DELTA_LOCAL, value->local);
    write_pic_settings(output, &body);
}

static const Entry display_entries[] = {
    {NULL, VALUE_CHOICE, DISPLAY_MODES, 0, 0, "hhmm|mmss|ssmm|yyyy|mmdd|delta|uncertainty",
     "Display mode", display_mode},
    {"hours", VALUE_CHOICE, "12 24", 0, 0, "12|24", "12- or 24-hour clock", display_hours},
    {"epoch", VALUE_MOMENT, NULL, 0, 0, "<moment>", "The delta mode's reference (no zone: local)",
     display_epoch},
};

// A level as it looks: off below the PWM range, full above it (the PIC's
// TUBES_BRIGHTNESS_MIN and MAX).
static void print_level(Stream *output, uint16_t level)
{
    if (level < 200)
    {
        output->printf("OFF");
    }
    else if (level > 3800)
    {
        output->printf("FULL");
    }
    else
    {
        output->printf("%u", level);
    }
}

// Auto or manual, the tubes now (STAT-UI) and the light they follow
// (STAT-ENV), and the manual level.
static void print_brightness(Stream *output, const SML_SETTINGS_BODY *body)
{
    SML_STAT_UI_PAYLOAD ui;
    SML_STAT_ENV_PAYLOAD env;
    bool automatic = has_flag(body, SML_SETTING_BRI_AUTO);

    output->printf("brightness: %s", automatic ? "AUTO" : "MANUAL");
    if (pic_link_get(SML_CLASS_STAT, SML_STAT_UI, &ui, PIC_LINK_STATE_TIMEOUT_MS))
    {
        output->printf(" ");
        print_level(output, ui.brightness);
        if (ui.brightness != ui.brightness_target)
        {
            output->printf(" -> ");
            print_level(output, ui.brightness_target);
        }
        if (automatic && pic_link_get(SML_CLASS_STAT, SML_STAT_ENV, &env, 15000U) &&
            (env.flags & SML_ENV_LUX_VALID) != 0)
        {
            output->printf(", %u.%u lux", env.lux / 10U, env.lux % 10U);
        }
    }
    output->printf("; manual %u\n", body->brightness);
}

static void brightness_value(Stream *output, const Entry *entry, const Value *value)
{
    SML_SETTINGS_BODY body;

    (void)entry;
    if (!pic_settings(output, &body))
    {
        return;
    }
    if (!value->given)
    {
        print_brightness(output, &body);
        return;
    }
    if (value->choice == 0) // auto, the level kept for when there is no sensor
    {
        set_flag(&body, SML_SETTING_BRI_AUTO, true);
    }
    else
    {
        body.brightness = (uint16_t)value->number;
        set_flag(&body, SML_SETTING_BRI_AUTO, false);
    }
    write_pic_settings(output, &body);
}

static void bare_brightness(Stream *output, bool verbose)
{
    Value none = {false, -1, 0, "", false, false};

    (void)verbose;
    brightness_value(output, NULL, &none);
}

static const Entry brightness_entries[] = {
    {NULL, VALUE_NUMBER, "auto", 0, BRIGHTNESS_FULL, "auto|0..4000",
     "The light sensor, or a level (under 200 off, over 3800 full)", brightness_value},
};

static void beeps_value(Stream *output, const Entry *entry, const Value *value)
{
    SML_SETTINGS_BODY body;

    (void)entry;
    if (!pic_settings(output, &body))
    {
        return;
    }
    if (!value->given)
    {
        output->printf("beeps: %s\n", on_off(has_flag(&body, SML_SETTING_BEEP_ENABLED)));
        return;
    }
    set_flag(&body, SML_SETTING_BEEP_ENABLED, value->number != 0);
    write_pic_settings(output, &body);
}

static void bare_beeps(Stream *output, bool verbose)
{
    Value none = {false, -1, 0, "", false, false};

    (void)verbose;
    beeps_value(output, NULL, &none);
}

static const Entry beeps_entries[] = {
    {NULL, VALUE_BOOL, NULL, 0, 1, "on|off", "Quarter-hour chimes and button clicks", beeps_value},
};

// As the PIC has it (STAT-UI): the switch arms it.
static void print_alarm_state(Stream *output, const SML_SETTINGS_BODY *body)
{
    SML_STAT_UI_PAYLOAD ui;

    if (!has_flag(body, SML_SETTING_ALARM_ENABLED) ||
        !pic_link_get(SML_CLASS_STAT, SML_STAT_UI, &ui, PIC_LINK_STATE_TIMEOUT_MS))
    {
        return;
    }
    output->printf(", %s", (ui.flags & SML_UI_ALARM_ACTIVE) != 0U ? "SOUNDING"
                           : (ui.flags & SML_UI_SWITCH) != 0U     ? "ARMED"
                                                                  : "DISARMED (switch OFF)");
}

static void alarm_value(Stream *output, const Entry *entry, const Value *value)
{
    SML_SETTINGS_BODY body;

    (void)entry;
    if (!pic_settings(output, &body))
    {
        return;
    }
    if (!value->given)
    {
        output->printf("alarm: %s, %02lu:%02lu:%02lu",
                       on_off(has_flag(&body, SML_SETTING_ALARM_ENABLED)),
                       (unsigned long)(body.alarm_offset_s / 3600U),
                       (unsigned long)(body.alarm_offset_s / 60U % 60U),
                       (unsigned long)(body.alarm_offset_s % 60U));
        print_alarm_state(output, &body);
        output->printf("\n");
        return;
    }
    if (value->choice < 0) // a time: set, and on
    {
        body.alarm_offset_s = (uint32_t)value->number;
    }
    set_flag(&body, SML_SETTING_ALARM_ENABLED, value->choice != 0);
    write_pic_settings(output, &body);
}

static void bare_alarm(Stream *output, bool verbose)
{
    Value none = {false, -1, 0, "", false, false};

    (void)verbose;
    alarm_value(output, NULL, &none);
}

static const Entry alarm_entries[] = {
    {NULL, VALUE_TIME, "off on", 0, 0, "off|on|hh:mm[:ss]|<seconds>",
     "Off, on, or a local time (and on); the switch arms it", alarm_value},
};

// ---- The RTC, the oscillator, GNSS, sources, debug ---------------------------------

static void bare_rtc(Stream *output, bool verbose)
{
    show_pic(output, SML_SHOW_RTC, verbose);
}

static void rtc_set(Stream *output, const Entry *entry, const Value *value)
{
    uint32_t utc_seconds = (uint32_t)value->number;

    if (!value->given)
    {
        output->printf("rtc set: when? %s\n", entry->usage);
        return;
    }
    if (value->choice == 0 && !ntp_utc_now(&utc_seconds)) // now
    {
        output->printf("rtc set now: the ESP has no time yet\n");
        return;
    }
    request_pic(output, SML_USER_SET_RTC, utc_seconds, false);
}

static const Entry rtc_entries[] = {
    // Unix seconds only, not a date: setting it wrongly takes a conversion first.
    // From 2000 (the PCF8563's century) to the end of uint32_t.
    {"set", VALUE_NUMBER, "now", 946684800LL, 4294967295LL, "now|<Unix seconds>",
     "Set the RTC (now: the ESP's NTP time)", rtc_set},
};

static void osc_baseline(Stream *output, const Entry *entry, const Value *value)
{
    SML_SETTINGS_BODY body;

    (void)entry;
    if (!pic_settings(output, &body))
    {
        return;
    }
    if (!value->given)
    {
        output->printf("osc baseline: %lu Hz (unused)\n", (unsigned long)body.calibration_fosc_hz);
        return;
    }
    body.calibration_fosc_hz = (uint32_t)value->number;
    write_pic_settings(output, &body);
}

static void bare_osc(Stream *output, bool verbose)
{
    show_pic(output, SML_SHOW_OSC, verbose);
}

static const Entry osc_entries[] = {
    {"baseline", VALUE_NUMBER, NULL, 1, UINT32_MAX, "<Hz>",
     "Oscillator baseline (FCY), within 1000 ppm of 40 MHz", osc_baseline},
};

static void bare_gnss(Stream *output, bool verbose)
{
    show_pic(output, SML_SHOW_GNSS, verbose);
}

static void bare_sources(Stream *output, bool verbose)
{
    request_pic(output, SML_USER_REQUEST_MESSAGE, (uint32_t)SML_CLASS_STAT << 8 | SML_STAT_SOURCES,
                verbose);
}

static void bare_debug(Stream *output, bool verbose)
{
    (void)verbose;
    request_pic(output, SML_USER_SET_DEBUG, 0, false); // 0: only shows it
}

static void debug_value(Stream *output, const Entry *entry, const Value *value)
{
    uint32_t argument;

    (void)entry;
    if (!debug_argument(value->text, &argument))
    {
        output->printf("debug: bad '%s': names (", value->text);
        print_debug_names(output);
        output->printf(", or a mask), all plain or all with + or -\n");
        return;
    }
    request_pic(output, SML_USER_SET_DEBUG, argument, false);
}

static const Entry debug_entries[] = {
    {NULL, VALUE_WORDS, NULL, 0, 0, "names|+name -name",
     "The PIC's periodic output: exactly these, or +add -remove", debug_value},
};

// ---- The PIC --------------------------------------------------------------------

// The PIC's own account of every module; with -v the ESP's view of the link
// and its measurements of the PIC too.
static void bare_pic(Stream *output, bool verbose)
{
    show_pic(output, SML_SHOW_ALL, verbose);
    if (verbose)
    {
        pic_report_print(output, true);
    }
}

static void pic_action(Stream *output, const Entry *entry, const Value *value)
{
    static const struct
    {
        const char *word;
        uint16_t command;
        uint32_t argument;
    } actions[] = {
        {"stored", SML_USER_SHOW, SML_SHOW_STORED},
        {"save", SML_USER_SAVE, 0},
        {"defaults", SML_USER_DEFAULTS, 0},
        {"reset", SML_USER_RESET, 0},
        {"resync", SML_USER_RESYNC, 0},
        {"bootloader", SML_USER_ENTER_BOOTLOADER, 0},
    };
    size_t index;

    for (index = 0; index < sizeof(actions) / sizeof(actions[0]); index++)
    {
        if (strcmp(entry->word, actions[index].word) == 0 &&
            actions[index].command == SML_USER_SHOW)
        {
            show_pic(output, actions[index].argument, value->verbose);
            return;
        }
        if (strcmp(entry->word, actions[index].word) == 0)
        {
            request_pic(output, actions[index].command, actions[index].argument, false);
            return;
        }
    }
}

// "pic boot": the SPL's report, or the application's version; run or hold.
static void pic_boot_action(Stream *output, const Entry *entry, const Value *value)
{
    (void)entry;
    if (value->choice == 0)
    {
        pic_boot_command(output, SML_BOOT_CMD_RUN);
    }
    else if (value->choice == 1)
    {
        pic_boot_command(output, SML_BOOT_CMD_HOLD);
    }
    else
    {
        pic_boot_print(output, value->verbose || verbose_setting);
    }
}

static const Entry pic_entries[] = {
    {"stored", VALUE_NONE, NULL, 0, 0, "", "What the PIC has in flash", pic_action},
    {"save", VALUE_NONE, NULL, 0, 0, "", "Save the PIC's settings", pic_action},
    {"defaults", VALUE_NONE, NULL, 0, 0, "", "The PIC's default settings, not saved", pic_action},
    {"reset", VALUE_NONE, NULL, 0, 0, "", "Reset the PIC", pic_action},
    {"resync", VALUE_NONE, NULL, 0, 0, "", "Judge every source again and choose again", pic_action},
    {"bootloader", VALUE_NONE, NULL, 0, 0, "", "Enter the bootloader (the SPL)", pic_action},
    {"boot", VALUE_OPTION, "run hold", 0, 0, "[run|hold]",
     "The SPL's report; run: start the app; hold: stay in it",
     pic_boot_action},
};

// ---- NTP, Wi-Fi, Telnet, updates ---------------------------------------------------

// The clock and every server, as chronyc sources; everything with -v.
static void bare_ntp(Stream *output, bool verbose)
{
    if (verbose)
    {
        ntp_print(output, true);
        return;
    }
    ntp_print(output, false);
    ntp_print_sources(output, false);
}

static void print_ntp_server(Stream *output, int index)
{
    const char *host = user_prefs_ntp_server(index);

    output->printf("ntp server %d: %s\n", index + 1, host[0] != '\0' ? host : "NONE");
}

// ntp server <n> [<host>|none]: server n (1 to 4) shown or set; with no n,
// all four. Server 1 cannot be none. A new server is polled at once.
static void ntp_server(Stream *output, const Entry *entry, const Value *value)
{
    char text[USER_PREFS_MAX_STRING_SIZE + 8];
    char *host;
    char *end;
    long number;
    int index;

    (void)entry;
    if (!value->given)
    {
        for (index = 0; index < NTP_SERVERS; index++)
        {
            print_ntp_server(output, index);
        }
        return;
    }
    snprintf(text, sizeof(text), "%s", value->text);
    number = strtol(text, &end, 10);
    if (end == text || (*end != '\0' && *end != ' ') || number < 1 || number > NTP_SERVERS)
    {
        output->printf("ntp server: which one, 1..%d, then the host\n", NTP_SERVERS);
        return;
    }
    index = (int)number - 1;
    host = end;
    while (*host == ' ')
    {
        host++;
    }
    if (*host == '\0')
    {
        print_ntp_server(output, index);
        return;
    }
    if (strchr(host, ' ') != NULL || strlen(host) > USER_PREFS_MAX_STRING_SIZE)
    {
        output->printf("ntp server: bad host '%s'\n", host);
        return;
    }
    if (strcasecmp(host, "none") == 0)
    {
        if (index == 0)
        {
            output->printf("ntp server: server 1 is needed\n");
            return;
        }
        host[0] = '\0';
    }
    user_prefs_set_string(user_prefs_ntp_server(index), host);
    output->printf("ntp server %d: %s%s\n", index + 1, host[0] != '\0' ? host : "NONE",
                   host[0] != '\0' ? ", polling" : "");
}

static void ntp_tracking(Stream *output, const Entry *entry, const Value *value)
{
    (void)entry;
    (void)value;
    ntp_print_tracking(output);
}

static void ntp_interval(Stream *output, const Entry *entry, const Value *value)
{
    (void)entry;
    if (value->given)
    {
        user_prefs.fields.ntp.interval = (uint32_t)value->number;
    }
    output->printf("ntp interval: %lu s\n", (unsigned long)user_prefs.fields.ntp.interval);
}

static void ntp_sync(Stream *output, const Entry *entry, const Value *value)
{
    (void)entry;
    (void)value;
    output->printf("Polling NTP now\n");
    ntp_resync();
}

static const Entry ntp_entries[] = {
    {"server", VALUE_WORDS, NULL, 0, 0, "1..4 <host>|none",
     "NTP server 1 to 4 (2 to 4 optional), then poll it", ntp_server},
    {"tracking", VALUE_NONE, NULL, 0, 0, "", "The server followed and the clock", ntp_tracking},
    {"interval", VALUE_DURATION, NULL, NTP_INTERVAL_MIN, NTP_INTERVAL_MAX, "<time>",
     "Longest poll interval (s, or 17m, 1h)", ntp_interval},
    {"sync", VALUE_NONE, NULL, 0, 0, "", "Poll now", ntp_sync},
};

static void bare_wifi(Stream *output, bool verbose)
{
    wifi_link_print(output, verbose);
}

static void wifi_action(Stream *output, const Entry *entry, const Value *value)
{
    (void)value;
    if (strcmp(entry->word, "scan") == 0)
    {
        wifi_link_scan(output);
    }
    else if (strcmp(entry->word, "connect") == 0)
    {
        output->printf("Reconnecting to Wi-Fi\n");
        wifi_link_reconnect();
    }
    else if (strcmp(entry->word, "disconnect") == 0)
    {
        output->printf("Disconnecting from Wi-Fi\n");
        wifi_link_disconnect();
    }
    else if (strcmp(entry->word, "setup") == 0)
    {
        wifi_link_setup(output);
    }
    else if (strcmp(entry->word, "forget") == 0)
    {
        wifi_link_clear(output); // restarts
    }
}

static const Entry wifi_entries[] = {
    {"scan", VALUE_NONE, NULL, 0, 0, "", "Scan for networks", wifi_action},
    {"connect", VALUE_NONE, NULL, 0, 0, "", "Reconnect", wifi_action},
    {"disconnect", VALUE_NONE, NULL, 0, 0, "", "Disconnect", wifi_action},
    {"setup", VALUE_NONE, NULL, 0, 0, "", "Open the setup access point", wifi_action},
    {"forget", VALUE_NONE, NULL, 0, 0, "", "Forget the network, and restart", wifi_action},
};

static void bare_telnet(Stream *output, bool verbose)
{
    telnet_print(output, verbose);
}

static void telnet_value(Stream *output, const Entry *entry, const Value *value)
{
    (void)entry;
    if (!value->given)
    {
        telnet_print(output, false);
    }
    else if (value->number != 0)
    {
        telnet_enable(output);
    }
    else
    {
        telnet_disable(output);
    }
}

static void telnet_action(Stream *output, const Entry *entry, const Value *value)
{
    (void)value;
    if (strcmp(entry->word, "start") == 0)
    {
        telnet_start(output);
    }
    else
    {
        telnet_stop(output);
    }
}

static void telnet_port(Stream *output, const Entry *entry, const Value *value)
{
    (void)entry;
    if (value->given)
    {
        telnet_set_port(output, (uint16_t)value->number);
    }
    output->printf("telnet port: %u\n", user_prefs.fields.telnet.port);
}

static const Entry telnet_entries[] = {
    {NULL, VALUE_BOOL, NULL, 0, 1, "on|off", "Run at start-up (on also starts it now)",
     telnet_value},
    {"start", VALUE_NONE, NULL, 0, 0, "", "Start now, this time only", telnet_action},
    {"stop", VALUE_NONE, NULL, 0, 0, "", "Stop now; this session stays open", telnet_action},
    {"port", VALUE_NUMBER, NULL, 1, 65535, "<n>", "Port (restarts the listener)", telnet_port},
};

static void print_update_auto(Stream *output)
{
    if (user_prefs.fields.updater.flags.auto_enabled)
    {
        output->printf("update auto: %02u:00\n", user_prefs.fields.updater.flags.auto_time);
    }
    else
    {
        output->printf("update auto: OFF\n");
    }
}

static void bare_update(Stream *output, bool verbose)
{
    output->printf("ESP %s%s%s\n", ESP_VERSION, updater_is_available() ? ", UPDATE AVAILABLE" : "",
                   updater_is_running() ? ", UPDATING" : "");
    if (updater_pic_is_available())
    {
        output->printf("PIC: UPDATE AVAILABLE\n");
    }
    pic_boot_print_staged(output, verbose || verbose_setting);
    output->printf("update: https://%s%s, profile %s\n", user_prefs.fields.updater.server,
                   user_prefs.fields.updater.path,
                   user_prefs.fields.updater.config[0] != '\0' ? user_prefs.fields.updater.config
                                                               : "DEFAULT");
    print_update_auto(output);
}

static void update_action(Stream *output, const Entry *entry, const Value *value)
{
    (void)value;
    if (strcmp(entry->word, "check") == 0)
    {
        updater_check(output);
    }
    else
    {
        updater_pull(output);
    }
}

// "update esp|pic|spl [force]": one part.
static void update_part(Stream *output, const Entry *entry, const Value *value)
{
    bool force = value->choice == 0;

    if (strcmp(entry->word, "esp") == 0)
    {
        updater_esp(output, force);
    }
    else if (strcmp(entry->word, "pic") == 0)
    {
        updater_pic(output, force);
    }
    else
    {
        updater_spl(output, force);
    }
}

// "update erase pic|spl|all": the staged files.
static void update_erase(Stream *output, const Entry *entry, const Value *value)
{
    (void)entry;
    if (!value->given)
    {
        output->printf("update erase: pic, spl or all\n");
        return;
    }
    pic_boot_erase(output, value->choice != 1, value->choice != 0);
}


static void update_text(Stream *output, const Entry *entry, const Value *value)
{
    if (strcmp(entry->word, "server") == 0)
    {
        if (value->given)
        {
            updater_set_server(value->text);
        }
        output->printf("update server: %s\n", user_prefs.fields.updater.server);
    }
    else if (strcmp(entry->word, "path") == 0)
    {
        if (value->given)
        {
            updater_set_path(value->text);
        }
        output->printf("update path: %s\n", user_prefs.fields.updater.path);
    }
    else
    {
        if (value->given)
        {
            updater_set_config(value->text);
        }
        output->printf("update config: %s\n", user_prefs.fields.updater.config[0] != '\0'
                                                  ? user_prefs.fields.updater.config
                                                  : "DEFAULT");
    }
}

static void update_auto(Stream *output, const Entry *entry, const Value *value)
{
    (void)entry;
    if (value->choice == 0) // off
    {
        user_prefs.fields.updater.flags.auto_enabled = 0;
    }
    else if (value->given)
    {
        user_prefs.fields.updater.flags.auto_enabled = 1;
        user_prefs.fields.updater.flags.auto_time = (uint16_t)value->number;
    }
    print_update_auto(output);
}

static const Entry update_entries[] = {
    {"check", VALUE_NONE, NULL, 0, 0, "", "What is offered; stages the PIC's application",
     update_action},
    {"install", VALUE_NONE, NULL, 0, 0, "", "The next due: ESP (esp reset), else PIC", update_action},
    {"esp", VALUE_OPTION, "force", 0, 0, "[force]",
     "The ESP if newer (esp reset); force: any version, restarts", update_part},
    {"pic", VALUE_OPTION, "force", 0, 0, "[force]",
     "The PIC's application if newer; force: any, or staged", update_part},
    {"spl", VALUE_OPTION, "force", 0, 0, "[force]", "The PIC's SPL (not yet)", update_part},
    {"erase", VALUE_CHOICE, "pic spl all", 0, 0, "pic|spl|all", "Empty the staged files' slots",
     update_erase},
    {"server", VALUE_TEXT, NULL, 0, 0, "<host>", "Update server", update_text},
    {"path", VALUE_TEXT, NULL, 0, 0, "<path>", "Update path", update_text},
    {"config", VALUE_TEXT, NULL, 0, 0, "<name>", "Update profile ('default' for none)",
     update_text},
    {"auto", VALUE_NUMBER, "off", 0, UPDATER_AUTO_HOUR_MAX, "off|<hour>",
     "Daily automatic updates, in that local hour", update_auto},
};

// ---- The ESP ----------------------------------------------------------------------

static void bare_esp(Stream *output, bool verbose)
{
    (void)verbose;
    output->printf("ESP %s (%s), up ", ESP_VERSION, wifi_link_hostname());
    time_print_duration(output, (uint32_t)(esp_timer_get_time() / 1000000));
    output->printf(", last reset: %s\n", commands_reset_reason());
    user_prefs_print(output);
}

static void esp_action(Stream *output, const Entry *entry, const Value *value)
{
    (void)value;
    if (strcmp(entry->word, "save") == 0)
    {
        esp_save(output);
    }
    else if (strcmp(entry->word, "defaults") == 0)
    {
        esp_defaults(output);
    }
    else if (strcmp(entry->word, "reset") == 0)
    {
        esp_restart(output);
    }
    else if (strcmp(entry->word, "factory") == 0)
    {
        user_prefs_reset();
        user_prefs_save();
        output->printf("ESP settings: DEFAULTS, saved\n");
        wifi_link_clear(output); // restarts
    }
}

static const Entry esp_entries[] = {
    {"save", VALUE_NONE, NULL, 0, 0, "", "Save the ESP's settings", esp_action},
    {"defaults", VALUE_NONE, NULL, 0, 0, "", "The ESP's default settings, not saved", esp_action},
    {"reset", VALUE_NONE, NULL, 0, 0, "", "Restart the ESP", esp_action},
    {"factory", VALUE_NONE, NULL, 0, 0, "", "Default settings saved, Wi-Fi forgotten, and restart",
     esp_action},
};

// ---- The nouns ----------------------------------------------------------------------

#define ENTRIES(table) table, sizeof(table) / sizeof(table[0])
#define NO_ENTRIES     NULL, 0

static const Noun nouns[] = {
    {"status", "Time, sources and links; same as an empty line", bare_status, NO_ENTRIES},
    {"help", "Commands; help <command> for its settings and actions", bare_help,
     ENTRIES(help_entries)},
    {"save", "Save both chips' settings", bare_save, NO_ENTRIES},
    {"defaults", "Default settings on both chips, not saved", bare_defaults, NO_ENTRIES},
    {"reset", "Reset the PIC and the ESP", bare_reset, NO_ENTRIES},
    {"verbose", "Everything without -v, until restart", bare_verbose, ENTRIES(verbose_entries)},

    {"tz", "The time zone: offsets, DST, lookups", show_tz, ENTRIES(tz_entries)},
    {"display", "Display mode, 12/24 hours, the delta epoch", show_display,
     ENTRIES(display_entries)},
    {"brightness", "Tube brightness", bare_brightness, ENTRIES(brightness_entries)},
    {"beeps", "Chimes and clicks", bare_beeps, ENTRIES(beeps_entries)},
    {"alarm", "The alarm", bare_alarm, ENTRIES(alarm_entries)},
    {"rtc", "The RTC", bare_rtc, ENTRIES(rtc_entries)},
    {"osc", "The PIC's oscillator", bare_osc, ENTRIES(osc_entries)},
    {"gnss", "The GNSS receiver", bare_gnss, NO_ENTRIES},
    {"sources", "The PIC's time sources, ranked", bare_sources, NO_ENTRIES},
    {"debug", "The PIC's debug output", bare_debug, ENTRIES(debug_entries)},
    {"pic", "The PIC: everything it reports", bare_pic, ENTRIES(pic_entries)},

    {"ntp", "The ESP's NTP clock and servers", bare_ntp, ENTRIES(ntp_entries)},
    {"wifi", "Wi-Fi (never the passphrase)", bare_wifi, ENTRIES(wifi_entries)},
    {"telnet", "The telnet console", bare_telnet, ENTRIES(telnet_entries)},
    {"update", "Firmware updates: the ESP and the PIC", bare_update, ENTRIES(update_entries)},
    {"esp", "The ESP: version, uptime, settings", bare_esp, ENTRIES(esp_entries)},
};

#define NOUN_COUNT (sizeof(nouns) / sizeof(nouns[0]))

static const Noun *find_noun(const char *name)
{
    size_t index;

    for (index = 0; index < NOUN_COUNT; index++)
    {
        if (strcasecmp(nouns[index].name, name) == 0)
        {
            return &nouns[index];
        }
    }
    return NULL;
}

static void print_entry(Stream *output, const Noun *noun, const Entry *entry)
{
    char usage[64];

    snprintf(usage, sizeof(usage), "%s%s%s%s%s", noun->name, entry->word != NULL ? " " : "",
             entry_name(entry), entry->usage[0] != '\0' ? " " : "", entry->usage);
    output->printf("  %-36s %s\n", usage, entry->help);
}

static void print_help(Stream *output, const char *name)
{
    size_t index;

    if (name != NULL)
    {
        const Noun *noun = find_noun(name);

        if (noun == NULL)
        {
            output->printf("help: no command '%s'\n", name);
            return;
        }
        output->printf("  %-36s %s\n", noun->name, noun->help);
        for (index = 0; index < noun->count; index++)
        {
            print_entry(output, noun, &noun->entries[index]);
        }
        if (noun == find_noun("debug"))
        {
            output->printf("  names: ");
            print_debug_names(output);
            output->printf(", or a mask such as 0x1f\n");
        }
        return;
    }
    for (index = 0; index < NOUN_COUNT; index++)
    {
        output->printf("  %-12s %s\n", nouns[index].name, nouns[index].help);
    }
    output->printf("<command> shows it (-v: everything); help <command> for what it takes.\n"
                   "Settings change at once, and are kept once saved (save).\n"
                   "Times: offsets +5:30, times of day 07:30, intervals 1024, 17m, 1h, 1d;\n"
                   "moments 2026-10-27T15:00Z, 2026-10-27 16:00+01:00 or Unix seconds.\n");
}

// ---- Running a line -----------------------------------------------------------------

static void print_bad(Stream *output, const Noun *noun, const Entry *entry, const char *text)
{
    output->printf("%s%s%s: bad value '%s', expected %s", noun->name,
                   entry->word != NULL ? " " : "", entry_name(entry), text,
                   entry->usage[0] != '\0' ? entry->usage : "nothing");
    if (entry->kind == VALUE_NUMBER || entry->kind == VALUE_DURATION)
    {
        output->printf(" (%lld..%lld)", (long long)entry->minimum, (long long)entry->maximum);
    }
    output->printf("\n");
}

static bool parse_value(const Entry *entry, const char *text, Value *value)
{
    uint32_t unsigned_value;
    int32_t signed_value;
    bool flag;

    value->given = text[0] != '\0';
    value->choice = value->given ? find_choice(entry->choices, text) : -1;
    value->number = 0;
    value->text = text;
    value->local = false;
    if (!value->given)
    {
        return true; // an action, or a setting to show
    }
    if (value->choice >= 0)
    {
        return true;
    }
    switch (entry->kind)
    {
        case VALUE_NONE:
        case VALUE_OPTION:
        case VALUE_CHOICE: return false;
        case VALUE_BOOL:
            if (!parse_bool(text, &flag))
            {
                return false;
            }
            value->number = flag ? 1 : 0;
            return true;
        case VALUE_NUMBER:
            if (entry->minimum < 0)
            {
                if (!parse_i32(text, (int32_t)entry->minimum, (int32_t)entry->maximum,
                               &signed_value))
                {
                    return false;
                }
                value->number = signed_value;
                return true;
            }
            if (!parse_u32(text, (uint32_t)entry->minimum, (uint32_t)entry->maximum,
                           &unsigned_value))
            {
                return false;
            }
            value->number = unsigned_value;
            return true;
        case VALUE_DURATION:
            if (!parse_duration(text, (uint32_t)entry->minimum, (uint32_t)entry->maximum,
                                &unsigned_value))
            {
                return false;
            }
            value->number = unsigned_value;
            return true;
        case VALUE_OFFSET:
            if (!parse_offset(text, (int32_t)entry->maximum, &signed_value))
            {
                return false;
            }
            value->number = signed_value;
            return true;
        case VALUE_TIME:
            if (!parse_time_of_day(text, &unsigned_value))
            {
                return false;
            }
            value->number = unsigned_value;
            return true;
        case VALUE_MOMENT:
            if (!parse_moment(text, &unsigned_value, &value->local))
            {
                return false;
            }
            value->number = unsigned_value;
            return true;
        case VALUE_TEXT: return strlen(text) <= USER_PREFS_MAX_STRING_SIZE;
        case VALUE_WORDS: return true;
    }
    return false;
}

// The next word of *text, ended with a NUL, and *text moved past the spaces
// after it.
static char *next_word(char **text)
{
    char *word = *text;
    char *end = word;

    while (*end != '\0' && !isspace((unsigned char)*end))
    {
        end++;
    }
    if (*end != '\0')
    {
        *end++ = '\0';
        while (isspace((unsigned char)*end))
        {
            end++;
        }
    }
    *text = end;
    return word;
}

static bool is_verbose_flag(const char *text)
{
    return strcmp(text, "-v") == 0 || strcasecmp(text, "verbose") == 0;
}

void commands_execute(Stream *output, char *line)
{
    char *end;
    char *rest;
    char *word;
    const Noun *noun;
    const Entry *entry = NULL;
    const char *value_text;
    Value value;
    size_t index;
    bool verbose = verbose_setting;

    while (isspace((unsigned char)*line))
    {
        line++;
    }
    end = line + strlen(line);
    while (end > line && isspace((unsigned char)end[-1]))
    {
        *--end = '\0';
    }
    // -v at the end, after anything: everything there is to show.
    if (end - line > 3 && strcmp(end - 3, " -v") == 0)
    {
        end -= 3;
        *end = '\0';
        while (end > line && isspace((unsigned char)end[-1]))
        {
            *--end = '\0';
        }
        verbose = true;
    }
    if (*line == '\0')
    {
        commands_print_status(output, verbose);
        return;
    }
    rest = line;
    word = next_word(&rest);
    noun = find_noun(word);
    if (noun == NULL)
    {
        output->printf("Unknown command '%s': try help\n", word);
        return;
    }
    if (*rest == '\0' || is_verbose_flag(rest))
    {
        noun->bare(output, verbose || *rest != '\0');
        return;
    }
    // A property or an action, else the noun's own value.
    value_text = rest;
    for (index = 0; index < noun->count; index++)
    {
        const Entry *candidate = &noun->entries[index];
        size_t length = candidate->word != NULL ? strlen(candidate->word) : 0;

        if (candidate->word != NULL && strncasecmp(rest, candidate->word, length) == 0 &&
            (rest[length] == '\0' || isspace((unsigned char)rest[length])))
        {
            entry = candidate;
            value_text = rest + length;
            while (isspace((unsigned char)*value_text))
            {
                value_text++;
            }
            break;
        }
        if (candidate->word == NULL)
        {
            entry = candidate; // unless a word matches later
        }
    }
    if (entry == NULL)
    {
        output->printf("%s: nothing called '%s'; help %s\n", noun->name, rest, noun->name);
        return;
    }
    if (!parse_value(entry, value_text, &value))
    {
        print_bad(output, noun, entry, value_text);
        return;
    }
    value.verbose = verbose;
    entry->handler(output, entry, &value);
}

// ---- Commands from the PIC ------------------------------------------------------------

uint16_t commands_from_pic(uint16_t command, uint32_t argument, uint16_t *detail, bool *restart)
{
    Stream *output = &Serial;

    *detail = SML_ERROR_NONE;
    switch (command)
    {
        case SML_USER_ESP_SAVE: user_prefs_save(); return SML_RESULT_ACK;
        case SML_USER_ESP_DEFAULTS: user_prefs_reset(); return SML_RESULT_ACK;
        case SML_USER_ESP_RESET: *restart = true; return SML_RESULT_ACK;
        case SML_USER_ESP_NTP_SYNC: ntp_resync(); return SML_RESULT_ACK;
        case SML_USER_ESP_TZ_REMOTE:
            if (argument > 1)
            {
                *detail = SML_ERROR_ARGUMENT;
                return SML_RESULT_NACK;
            }
            tzinfo_set_enabled(argument != 0);
            return SML_RESULT_ACK;
        case SML_USER_ESP_TZ_LOOKUP:
            if (tzinfo_update(output))
            {
                return SML_RESULT_ACK;
            }
            *detail = SML_ERROR_STATE;
            return SML_RESULT_NACK;
        case SML_USER_ESP_WIFI_FORGET:
            wifi_link_forget();
            *restart = true; // after the answer
            return SML_RESULT_ACK;
        case SML_USER_ESP_WIFI_SETUP: wifi_link_setup(output); return SML_RESULT_ACK;
        case SML_USER_ESP_UPDATE_CHECK: updater_check(output); return SML_RESULT_ACK;
        default: *detail = SML_ERROR_UNSUPPORTED; return SML_RESULT_NACK;
    }
}
