/*
 * The menu (menu/menu.c), played with the button against stand-ins for the
 * settings, the display, the brightness, the synchroniser, the RTC source
 * and the ESP: moving through it, editing (taps, the confirming hold, the
 * repeat and its take-back), flashing, the timeout, saving once on closing,
 * the display mode's name then preview, the brightness preview, tIME (only
 * while the RTC alone has the time), the resets and the ESP's commands.
 */
#include <stdio.h>
#include <string.h>

#include "../../menu/menu.h"
#include "../../settings/settings.h"
#include "../../sync/source_rtc.h"
#include "../../sync/sync.h"
#include "../../ui/beeps.h"
#include "../../ui/brightness.h"
#include "../../ui/display.h"
#include "../../ui/frame.h"
#include "../../ui/timezone.h"

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

// ---- Stand-ins -------------------------------------------------------------

static SML_SETTINGS_BODY settings;
static uint32_t revision;
static int saves, writes, restores;

void settings_body(SML_SETTINGS_BODY *body)
{
    *body = settings;
}


uint32_t settings_revision(void)
{
    return revision;
}


uint8_t settings_write(const SML_SETTINGS_BODY *body)
{
    writes++;
    if (memcmp(body, &settings, sizeof(settings)) != 0)
    {
        settings = *body;
        revision++;
    }
    return SML_ERROR_NONE;
}


static void defaults(SML_SETTINGS_BODY *body)
{
    memset(body, 0, sizeof(*body));
    body->schema_version = SML_SETTINGS_SCHEMA;
    body->flags = SML_SETTING_TZ_AUTO | SML_SETTING_DST_AUTO | SML_SETTING_BRI_AUTO;
    body->brightness = 2000;
    body->selected_display = SML_DISPLAY_HHMM;
}


void settings_restore_defaults(void)
{
    restores++;
    defaults(&settings);
    revision++;
}


void settings_save(void)
{
    saves++;
}


static TubesFrame overlay;
static bool overlay_on;
static DisplayMode previewing = DISPLAY_MODE_COUNT;

void display_set_overlay(const TubesFrame *frame)
{
    overlay_on = frame != NULL;
    if (frame != NULL)
    {
        overlay = *frame;
    }
}


void display_preview(DisplayMode mode)
{
    previewing = mode;
}


static uint16_t level;
static bool automatic = true;
void brightness_set_level(uint16_t value)
{
    level = value;
}


void brightness_set_auto(bool on)
{
    automatic = on;
}


static int clicks;
void beeps_button(void)
{
    clicks++;
}


static SyncStatus status;
const SyncStatus *sync_status(void)
{
    return &status;
}


bool sync_source_steers(uint8_t source)
{
    return source != SYNC_SOURCE_RTC;
}


static int time_sets;
static uint32_t set_utc, set_epoch;
static uint16_t set_slot;
bool sync_set_time(uint32_t utc_seconds, uint32_t epoch_sequence, uint16_t slot_ms)
{
    time_sets++;
    set_utc = utc_seconds;
    set_epoch = epoch_sequence;
    set_slot = slot_ms;
    return true;
}


static bool rtc_busy;
static int rtc_sets;
static uint32_t rtc_utc;
bool source_rtc_set(uint32_t utc_seconds)
{
    if (rtc_busy)
    {
        return false;
    }
    rtc_sets++;
    rtc_utc = utc_seconds;
    return true;
}


static int32_t zone_offset; // the offset in force, s
void timezone_offset_at(uint32_t utc_seconds, TimezoneOffset *zone)
{
    (void)utc_seconds;
    memset(zone, 0, sizeof(*zone));
    zone->offset_s = zone_offset;
}


static uint16_t requests[16];
static uint32_t request_arguments[16];
static int request_count;
static uint16_t request(uint16_t command, uint32_t argument)
{
    if (request_count < 16)
    {
        requests[request_count] = command;
        request_arguments[request_count] = argument;
    }
    request_count++;
    return (uint16_t)request_count;
}


static bool remote_known = true;
static bool remote_on = true;
static bool remote(uint32_t now_ms, bool *enabled)
{
    (void)now_ms;
    *enabled = remote_on;
    return remote_known;
}


static bool linked(void)
{
    return true;
}


static const MenuEsp esp = {request, remote, linked};

// ---- The button --------------------------------------------------------------

static uint32_t now = 1000;
static uint32_t epoch = 100;

// ms of the clock, in 10 ms passes, the button down or up.
static void run(uint32_t ms, bool down)
{
    while (ms >= 10U)
    {
        now += 10U;
        if (now % 1000U == 0U)
        {
            epoch++;
        }
        menu_service(now, down, epoch, (uint16_t)(now % 1000U));
        ms -= 10U;
    }
}


static void press(uint32_t ms)
{
    run(ms, true);
    run(20, false);
}


static void tap(void)
{
    press(100);
}


static void hold(void) // to confirm
{
    press(800);
}


static TubesFrame text_frame(const char *text, uint8_t separators)
{
    TubesFrame frame;
    frame_clear(&frame);
    frame_text(&frame, text);
    frame.separators = separators;
    return frame;
}


static bool shows(const char *text, uint8_t separators)
{
    TubesFrame expected = text_frame(text, separators);
    bool same = overlay_on && memcmp(&overlay, &expected, sizeof(expected)) == 0;

    if (!same)
    {
        printf("  expected \"%s\" (%02X), overlay %s:", text, separators,
               overlay_on ? "on" : "off");
        for (unsigned i = 0; i < TUBES_COUNT; i++)
        {
            for (char c = ' '; c <= 'z'; c++)
            {
                char one[2] = {c, 0};
                TubesFrame f;
                frame_clear(&f);
                frame_text(&f, one);
                if (f.segments[0] == overlay.segments[i])
                {
                    printf("%c", c);
                    break;
                }
            }
        }
        printf(" (%02X)\n", overlay.separators);
    }
    return same;
}


static void open_menu(void)
{
    menu_open(now);
    run(20, true); // the long press that opened it, still held: ignored
    run(20, false);
}


// From MENU, short presses to the nth item of the top level.
static void go_top(int n)
{
    while (n-- > 0)
    {
        tap();
    }
}


// ---- Tests -------------------------------------------------------------------

static void test_moving(void)
{
    static const char *const top[] = {"MENU", "  tZ", " dSt", "ALrM", "bEEP",
                                      "dISP", "tIME", " rSt", "EXIt"};
    printf("moving through\n");

    open_menu();
    CHECK(menu_is_open() && shows("MENU", 0) && menu_state() == SML_MENU_TOP);
    for (int i = 1; i <= 9; i++)
    {
        tap();
        CHECK(shows(top[i % 9], 0));
    }
    CHECK(clicks >= 9);
    // A long press on MENU closes; nothing changed, nothing saved.
    press(600);
    CHECK(!menu_is_open() && !overlay_on && saves == 0);

    // tZ: AUtO (the offset hidden while automatic), the remote setting, bACK.
    open_menu();
    go_top(1);
    press(600);
    CHECK(shows("AUtO", 0) && menu_state() == SML_MENU_TZ);
    tap();
    CHECK(shows("r ON", 0) && menu_state() == (SML_MENU_TZ | 2U));
    tap();
    CHECK(shows("bACK", 0));
    press(600); // back to tZ
    CHECK(shows("  tZ", 0) && menu_state() == (SML_MENU_TOP | 1U));
    // EXIt closes.
    go_top(7);
    CHECK(shows("EXIt", 0));
    press(600);
    CHECK(!menu_is_open());
}


static void test_editing(void)
{
    uint32_t start;
    printf("editing\n");

    open_menu();
    go_top(1);
    press(600); // tZ
    press(600); // edit AUtO
    CHECK(shows("AUtO", 0) && (menu_state() & SML_MENU_EDITING) != 0U);
    // Flashing: lit for 600 ms of each second from the last change.
    run(600, false);
    CHECK(shows("", 0));
    run(400, false);
    CHECK(shows("AUtO", 0));
    tap();
    CHECK(shows(" MAN", 0));
    // Held: a blink at 0.5 s, then steady (let go to confirm).
    run(MENU_LONG_MS + 50U, true);
    CHECK(shows("", 0));
    run(MENU_CUE_MS, true);
    CHECK(shows(" MAN", 0));
    run(500, true); // steady, not flashing, to 1.2 s
    CHECK(shows(" MAN", 0));
    run(20, false); // confirmed, applied, not saved
    CHECK((settings.flags & SML_SETTING_TZ_AUTO) == 0 && saves == 0 &&
          (menu_state() & SML_MENU_EDITING) == 0);
    CHECK(shows(" MAN", 0));

    // The offset now shows: +5:30 by taps, hours then minutes.
    tap();
    CHECK(shows("0000", TUBES_MIDDLE_BOTH));
    press(600);
    for (int i = 0; i < 5; i++)
        tap();
    CHECK(shows("0500", TUBES_MIDDLE_BOTH));
    hold();
    tap();
    tap();
    hold();
    CHECK(settings.base_offset_s == 19800 && shows("0530", TUBES_MIDDLE_BOTH));

    // Held past 1.2 s: +1 every 200 ms; let go just after a step, and it is
    // taken back. 5 -> 6 at 1.2 s, 7, 8, 9 at 1.8 s, then let go at 1.85 s.
    press(600);
    start = writes;
    run(1850, true);
    CHECK(shows("0930", TUBES_MIDDLE_BOTH));
    run(20, false);
    CHECK(shows("0830", TUBES_MIDDLE_BOTH) && (menu_state() & SML_MENU_EDITING) != 0U);
    // Let go well after a step: kept. 8 -> 9 at 1.2 s, held to 1.35 s.
    run(1350, true);
    run(20, false);
    CHECK(shows("0930", TUBES_MIDDLE_BOTH));
    // Through the wrap: 9 + 6 is past +14, so -12; the start line for minus.
    for (int i = 0; i < 6; i++)
        tap();
    CHECK(shows("1230", TUBES_MIDDLE_BOTH | TUBES_START_LINE)); // -12:30
    hold();
    hold();
    CHECK(settings.base_offset_s == -(12 * 3600 + 30 * 60) && writes > (int)start);

    // The timeout: closes, saves once.
    run(MENU_TIMEOUT_MS + 100U, false);
    CHECK(!menu_is_open() && saves == 1 && menu_stats()->timeouts == 1);
}


// A long press acted on while held goes no further, however long it is held:
// it neither steps through the section it opened nor repeats in the value it
// started editing.
static void test_long_hold(void)
{
    printf("a long press held on\n");

    open_menu();
    go_top(4);   // bEEP
    press(3000); // opens it
    CHECK(shows(" OFF", 0) && menu_state() == SML_MENU_BEEP);
    press(3000); // starts editing: OFF, not stepped
    CHECK(shows(" OFF", 0) && (menu_state() & SML_MENU_EDITING) != 0U);
    tap();
    CHECK(shows("  ON", 0));
    tap();
    CHECK(shows(" OFF", 0));
    hold(); // OFF, as it was
    CHECK((menu_state() & SML_MENU_EDITING) == 0U);
    tap();
    CHECK(shows("bACK", 0));
    press(3000); // back to bEEP, then out
    CHECK(shows("bEEP", 0));
    run(MENU_TIMEOUT_MS + 100U, false);
    CHECK(!menu_is_open());
}


static void test_timeout_drops_edit(void)
{
    printf("an edit not confirmed\n");

    saves = 0;
    open_menu();
    go_top(4); // bEEP
    press(600);
    press(600); // edit OFF
    tap();      // ON, not confirmed
    run(MENU_TIMEOUT_MS + 100U, false);
    CHECK(!menu_is_open() && (settings.flags & SML_SETTING_BEEP_ENABLED) == 0 && saves == 0);
}


static void test_display(void)
{
    printf("dISP: the mode, the brightness\n");

    saves = 0;
    open_menu();
    go_top(5);
    press(600); // dISP: 24 h
    CHECK(shows("  24", 0));
    tap(); // the mode
    CHECK(shows("HHMM", 0));
    press(600); // edit
    tap();
    CHECK(shows("MMSS", 0) && previewing == DISPLAY_MODE_COUNT);
    run(MENU_NAME_MS, false); // then the mode itself
    CHECK(!overlay_on && previewing == DISPLAY_MODE_MMSS);
    tap(); // SSMM: its name again
    CHECK(overlay_on && previewing == DISPLAY_MODE_COUNT && shows("SSMM", 0));
    hold();
    CHECK(settings.selected_display == SML_DISPLAY_SSMM && previewing == DISPLAY_MODE_COUNT);

    // Brightness: AUtO, then b 10 .. b100, shown on the tubes as chosen.
    tap();
    CHECK(shows("AUtO", 0));
    press(600);
    tap();
    CHECK(shows("b 10", 0) && !automatic && level == 400);
    for (int i = 0; i < 9; i++)
        tap();
    CHECK(shows("b100", 0) && level == 4000);
    tap(); // wraps to AUtO
    CHECK(shows("AUtO", 0) && automatic);
    tap();
    tap();
    tap(); // b 30
    hold();
    CHECK((settings.flags & SML_SETTING_BRI_AUTO) == 0 && settings.brightness == 1200 &&
          level == 1200 && !automatic);
    // Edited, then dropped by the timeout: put back.
    press(600);
    tap();
    CHECK(level == 1600);
    run(MENU_TIMEOUT_MS + 100U, false);
    CHECK(level == 1200 && !automatic && saves == 1);
}


static void test_alarm(void)
{
    printf("ALrM: 1 min steps, and the time hidden while off\n");

    open_menu();
    go_top(3);
    press(600);
    CHECK(shows(" OFF", 0));
    tap(); // the time hidden: bACK
    CHECK(shows("bACK", 0));
    tap();
    press(600);
    tap();
    hold(); // ON
    CHECK((settings.flags & SML_SETTING_ALARM_ENABLED) != 0);
    tap();
    CHECK(shows("0000", TUBES_MIDDLE_BOTH));
    press(600);
    for (int i = 0; i < 7; i++)
        tap();
    hold();
    run(1200 + 200 * 4 + 150, true); // 0 -> 5 minutes held, the last kept
    run(20, false);
    hold();
    CHECK(settings.alarm_offset_s == 7U * 3600U + 5U * 60U);
    run(MENU_TIMEOUT_MS + 100U, false);
}


static void test_time(void)
{
    printf("tIME: only with the RTC alone; set at the press\n");

    // The time now 2026-10-28 21:59:xx local (UTC+1).
    status.time.known = true;
    status.time.utc_seconds = 1793221140UL; // 2026-10-28T20:59:00Z
    zone_offset = 3600;
    open_menu();
    go_top(6);
    CHECK(shows("tIME", 0));
    press(600);
    CHECK(shows("2026", 0));
    tap(); // 2027
    hold();
    CHECK(shows("1028", TUBES_MIDDLE_LINE));
    tap(); // November
    hold();
    for (int i = 0; i < 2; i++)
        tap(); // 30, the last of November
    tap();     // wraps: 1
    hold();
    CHECK(shows("2159", TUBES_MIDDLE_BOTH));
    tap(); // 22
    hold();
    tap(); // 22:00
    hold();
    CHECK(shows("  OK", 0) && time_sets == 0);
    // OK: a tap does nothing; the long press sets it at once, at the moment
    // the button went down.
    tap();
    CHECK(time_sets == 0);
    run(10, true);
    {
        uint32_t down_epoch = epoch;
        uint16_t down_slot = (uint16_t)(now % 1000U);
        run(MENU_LONG_MS, true);
        CHECK(time_sets == 1 && set_epoch == down_epoch && set_slot == down_slot);
    }
    // 2027-11-01T22:00:00+01:00 = 2027-11-01T21:00:00Z.
    CHECK(set_utc == 1825102800UL);
    run(20, false);
    CHECK(shows("tIME", 0) && (menu_state() & SML_MENU_EDITING) == 0);

    // The RTC: once the new time is in force, retried while busy.
    rtc_busy = true;
    status.time.source = SYNC_SOURCE_NONE;
    status.time.utc_seconds = 1825102801UL;
    run(100, false);
    CHECK(rtc_sets == 0);
    rtc_busy = false;
    run(10, false);
    CHECK(rtc_sets == 1 && (rtc_utc == 1825102801UL || rtc_utc == 1825102802UL));
    run(100, false);
    CHECK(rtc_sets == 1);

    // NTP gives the time: tIME goes, even from under the menu.
    status.sources[SYNC_SOURCE_NTP].report.time_valid = true;
    menu_epoch();
    CHECK(!menu_time_settable());
    run(30, false);
    CHECK(shows(" rSt", 0));
    go_top(1);
    CHECK(shows("EXIt", 0));
    press(600);
}


static void test_resets(void)
{
    printf("rSt and the ESP\n");

    request_count = 0;
    saves = 0;
    settings.flags = 0;
    // rSt: WIFI NO by default; ALL YES: the PIC's defaults saved, the ESP's
    // defaults, saved, then its network forgotten.
    open_menu();
    go_top(6); // tIME gone: rSt is sixth
    CHECK(shows(" rSt", 0));
    press(600);
    CHECK(shows("WIFI", 0));
    press(600);
    CHECK(shows("  NO", 0));
    hold(); // NO: nothing
    CHECK(request_count == 0);
    tap();
    CHECK(shows("SEt ", 0));
    tap();
    CHECK(shows(" ALL", 0));
    press(600);
    tap();
    CHECK(shows(" YES", 0));
    hold();
    CHECK(restores == 1 && saves == 1 &&
          settings.flags == (SML_SETTING_TZ_AUTO | SML_SETTING_DST_AUTO | SML_SETTING_BRI_AUTO));
    CHECK(request_count == 3 && requests[0] == SML_USER_ESP_DEFAULTS &&
          requests[1] == SML_USER_ESP_SAVE && requests[2] == SML_USER_ESP_WIFI_FORGET);

    // The remote time zone: sent at once, the ESP saved on closing.
    request_count = 0;
    tap();
    press(600); // bACK
    go_top(3);  // to tZ: EXIt MENU tZ
    CHECK(shows("  tZ", 0));
    press(600);
    tap();
    CHECK(shows("r ON", 0));
    press(600);
    tap();
    hold();
    CHECK(request_count == 1 && requests[0] == SML_USER_ESP_TZ_REMOTE &&
          request_arguments[0] == 0U);
    tap();
    press(600); // bACK to tZ
    run(MENU_TIMEOUT_MS + 100U, false);
    CHECK(request_count == 2 && requests[1] == SML_USER_ESP_SAVE);
    // Unknown remote setting: hidden.
    remote_known = false;
    open_menu();
    go_top(1);
    press(600);
    tap();
    CHECK(shows("bACK", 0));
    run(MENU_TIMEOUT_MS + 100U, false);
}


int main(void)
{
    defaults(&settings);
    menu_init(&esp);
    CHECK(!menu_is_open() && menu_state() == SML_MENU_NONE && menu_time_settable());
    test_moving();
    test_editing();
    test_long_hold();
    test_timeout_drops_edit();
    test_display();
    test_alarm();
    test_time();
    test_resets();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
