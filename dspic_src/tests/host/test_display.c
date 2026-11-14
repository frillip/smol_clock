/*
 * Host tests of the tubes: the driver's wire format against the V2 code it
 * replaces, the SPI and latch sequences, brightness, and the display layer's
 * rendering and timing against the epoch.
 */
#include <stdio.h>
#include <string.h>
#include <xc.h>

#include "../../drivers/tubes.h"
#include "../../ui/display.h"
#include "../../ui/frame.h"

FakeLog fake_spi2buf, fake_latch_gpio;
volatile uint16_t SPI2STAT, SPI2CON1, SPI2CON2;
volatile uint16_t OC3CON1, OC3CON2, OC3R, OC3RS;
volatile uint16_t _LATB11, _RB10;
volatile SPI2STATBITS_t SPI2STATbits = {0, 1, 1, 0, 0};
volatile IFS2BITS_t IFS2bits;
volatile IEC2BITS_t IEC2bits;

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

// ---- V2 reference (drivers/tubes.c as it was) ----------------------------
#define TUBE_4_OFFSET         33
#define TUBE_3_OFFSET         20
#define TUBE_2_OFFSET         13
#define TUBE_1_OFFSET         0
#define SEG_G                 0x00000004ULL
#define SEG_F                 0x00000002ULL
#define SEG_E                 0x00000001ULL
#define SEG_D                 0x00000040ULL
#define SEG_C                 0x00000020ULL
#define SEG_B                 0x00000010ULL
#define SEG_A                 0x00000008ULL
#define START_SEPARATOR_DOT   0x080000000ULL
#define START_SEPARATOR_LINE  0x100000000ULL
#define MIDDLE_SEPARATOR_DOT  0x0800ULL
#define MIDDLE_SEPARATOR_LINE 0x1000ULL
static const uint64_t v2_characters[] = {
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,
    SEG_B | SEG_C,
    SEG_A | SEG_B | SEG_D | SEG_E | SEG_G,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_G,
    SEG_B | SEG_C | SEG_F | SEG_G,
    SEG_A | SEG_C | SEG_D | SEG_F | SEG_G,
    SEG_A | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_B | SEG_C,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G,
};
// V2's letters, A to Z.
static const uint64_t v2_letters[26] = {
    SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G,
    SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_D | SEG_E | SEG_F,
    SEG_B | SEG_C | SEG_D | SEG_E | SEG_G,
    SEG_A | SEG_D | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_C | SEG_D | SEG_E | SEG_F,
    SEG_B | SEG_C | SEG_E | SEG_F | SEG_G,
    SEG_E | SEG_F,
    SEG_B | SEG_C | SEG_D | SEG_E,
    SEG_A | SEG_C | SEG_E | SEG_F | SEG_G,
    SEG_D | SEG_E | SEG_F,
    SEG_A | SEG_C | SEG_E,
    SEG_A | SEG_B | SEG_C | SEG_E | SEG_F,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,
    SEG_A | SEG_B | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_B | SEG_C | SEG_F | SEG_G,
    SEG_A | SEG_B | SEG_E | SEG_F,
    SEG_A | SEG_C | SEG_D | SEG_F | SEG_G,
    SEG_D | SEG_E | SEG_F | SEG_G,
    SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,
    SEG_B | SEG_C | SEG_D | SEG_F,
    SEG_B | SEG_D | SEG_F,
    SEG_B | SEG_C | SEG_E | SEG_F | SEG_G,
    SEG_B | SEG_C | SEG_D | SEG_F | SEG_G,
    SEG_A | SEG_B | SEG_D | SEG_E | SEG_G,
};
static const int v2_offset[4] = {TUBE_4_OFFSET, TUBE_3_OFFSET, TUBE_2_OFFSET, TUBE_1_OFFSET};

// display_send_buffer(): the four words in the order V2 sent them.
static void v2_words(uint64_t buffer, uint16_t words[4])
{
    words[0] = (uint16_t)((buffer >> 24) & 0xFFFF);
    words[1] = (uint16_t)((buffer >> 32) & 0xFFFF);
    words[2] = (uint16_t)((buffer >> 16) & 0xFFFF);
    words[3] = (uint16_t)(buffer & 0xFFFF);
}


static uint64_t v2_separators(uint8_t separators)
{
    return ((separators & TUBES_START_DOT) ? START_SEPARATOR_DOT : 0) |
           ((separators & TUBES_START_LINE) ? START_SEPARATOR_LINE : 0) |
           ((separators & TUBES_MIDDLE_DOT) ? MIDDLE_SEPARATOR_DOT : 0) |
           ((separators & TUBES_MIDDLE_LINE) ? MIDDLE_SEPARATOR_LINE : 0);
}


// ---- Tests -----------------------------------------------------------------
static void test_wire_format(void)
{
    long mismatches = 0;
    printf("wire format against V2\n");

    // Every four-digit number with every separator combination.
    for (unsigned value = 0; value < 10000U; value++)
    {
        for (uint8_t separators = 0; separators < 16U; separators++)
        {
            TubesFrame frame;
            uint16_t ours[4], theirs[4];
            uint64_t buffer = v2_separators(separators);
            unsigned digits[4] = {value / 1000U, value / 100U % 10U, value / 10U % 10U,
                                  value % 10U};

            frame_clear(&frame);
            frame_number(&frame, 0, 4, (uint16_t)value);
            frame.separators = separators;
            for (int tube = 0; tube < 4; tube++)
                buffer |= v2_characters[digits[tube]] << v2_offset[tube];
            tubes_encode(&frame, ours);
            v2_words(buffer, theirs);
            if (memcmp(ours, theirs, sizeof ours) != 0)
                mismatches++;
        }
    }
    CHECK(mismatches == 0);

    // Every letter on every tube.
    mismatches = 0;
    for (int letter = 0; letter < 26; letter++)
    {
        for (int tube = 0; tube < 4; tube++)
        {
            TubesFrame frame;
            uint16_t ours[4], theirs[4];
            frame_clear(&frame);
            frame.segments[tube] = frame_glyph((char)('A' + letter));
            tubes_encode(&frame, ours);
            v2_words(v2_letters[letter] << v2_offset[tube], theirs);
            if (memcmp(ours, theirs, sizeof ours) != 0)
                mismatches++;
        }
    }
    CHECK(mismatches == 0);
    CHECK(frame_glyph('m') == frame_glyph('M') && frame_glyph('?') == 0 && frame_glyph(' ') == 0);
}


static void test_frames(void)
{
    TubesFrame frame;
    printf("frames\n");

    frame_clear(&frame);
    frame_text(&frame, "AB");
    CHECK(frame.segments[0] == frame_glyph('A') && frame.segments[1] == frame_glyph('B') &&
          frame.segments[2] == 0 && frame.segments[3] == 0);
    frame_number(&frame, 1, 3, 1234); // too wide: the 1 is dropped
    CHECK(frame.segments[1] == frame_glyph('2') && frame.segments[3] == frame_glyph('4'));
    frame_number(&frame, 2, 2, 7);
    CHECK(frame.segments[2] == frame_glyph('0') && frame.segments[3] == frame_glyph('7'));
    frame_blank(&frame, 3, 5);
    CHECK(frame.segments[3] == 0 && frame.segments[2] != 0);
    frame_dashes(&frame);
    CHECK(frame.segments[0] == TUBES_SEGMENT_G && frame.segments[3] == TUBES_SEGMENT_G &&
          frame.separators == (TUBES_START_LINE | TUBES_MIDDLE_LINE));
}


static unsigned spi_mark;
static void spi_since(uint16_t words[], unsigned *count)
{
    *count = fake_spi2buf.count - spi_mark;
    for (unsigned i = 0; i < *count && i < 16U; i++)
        words[i] = fake_spi2buf.values[(spi_mark + i) % FAKE_LOG_LENGTH];
    spi_mark = fake_spi2buf.count;
}


static void test_driver(void)
{
    TubesFrame a, b;
    uint16_t sent[16], expected[4];
    unsigned count, latch_mark;
    printf("driver\n");

    memset(&fake_spi2buf, 0, sizeof fake_spi2buf);
    memset(&fake_latch_gpio, 0, sizeof fake_latch_gpio);
    spi_mark = 0;
    _RB10 = 0;
    tubes_init();
    spi_since(sent, &count);
    CHECK(SPI2CON1 == 0x0463 && SPI2CON2 == 0x0001 && SPI2STAT == 0x800C);
    CHECK(count == 4 && sent[0] == 0 && sent[3] == 0); // blank, latched
    CHECK(fake_latch_gpio.count == 2 && fake_latch_gpio.values[0] == 1 &&
          fake_latch_gpio.values[1] == 0);
    CHECK(tubes_brightness() == TUBES_BRIGHTNESS_DEFAULT && OC3R == 2000 && OC3RS == 3999 &&
          OC3CON1 == 0x1C0D && OC3CON2 == 0x001F);

    // Loaded for the epoch: four words, no latch pulse.
    frame_clear(&a);
    frame_text(&a, "1234");
    latch_mark = fake_latch_gpio.count;
    tubes_load(&a);
    spi_since(sent, &count);
    tubes_encode(&a, expected);
    CHECK(count == 4 && memcmp(sent, expected, sizeof expected) == 0);
    CHECK(fake_latch_gpio.count == latch_mark);
    CHECK(tubes_stats()->loads == 1 && tubes_stats()->loads_too_late == 0);
    CHECK(tubes_shown()->segments[0] == 0); // not until the epoch
    tubes_epoch();
    CHECK(memcmp(tubes_shown(), &a, sizeof a) == 0);

    // Shown at once while another frame waits for the epoch: shifted and
    // latched, then the waiting frame is shifted back in.
    tubes_load(&a);
    spi_since(sent, &count);
    frame_clear(&b);
    frame_text(&b, "MENU");
    tubes_show(&b);
    spi_since(sent, &count);
    CHECK(count == 8);
    tubes_encode(&b, expected);
    CHECK(memcmp(sent, expected, sizeof expected) == 0);
    tubes_encode(&a, expected);
    CHECK(memcmp(sent + 4, expected, sizeof expected) == 0);
    CHECK(fake_latch_gpio.count == latch_mark + 2);
    CHECK(memcmp(tubes_shown(), &b, sizeof b) == 0 && memcmp(tubes_loaded(), &a, sizeof a) == 0);
    tubes_epoch();
    CHECK(memcmp(tubes_shown(), &a, sizeof a) == 0);

    // Nothing waiting: no reload, and the epoch keeps what was shown.
    tubes_show(&b);
    spi_since(sent, &count);
    CHECK(count == 4);
    tubes_epoch();
    CHECK(memcmp(tubes_shown(), &b, sizeof b) == 0);

    // A load while the epoch pulse is high is counted: it shows at once.
    _RB10 = 1;
    tubes_load(&a);
    _RB10 = 0;
    CHECK(tubes_stats()->loads_too_late == 1);

    // Brightness: blank below the minimum, lit continuously above the
    // maximum, PWM between.
    tubes_brightness_set(100);
    CHECK((OC3CON1 & 7U) == 0 && _LATB11 == 1 && tubes_brightness() == TUBES_BRIGHTNESS_OFF);
    tubes_brightness_set(3000);
    CHECK(OC3CON1 == 0x1C0D && OC3R == 3000 && tubes_brightness() == 3000);
    tubes_brightness_set(3900);
    CHECK((OC3CON1 & 7U) == 0 && _LATB11 == 0 && tubes_brightness() == TUBES_BRIGHTNESS_FULL);
    tubes_brightness_set(TUBES_BRIGHTNESS_DEFAULT);
    CHECK(OC3CON1 == 0x1C0D && OC3R == 2000);
}


static TubesFrame hhmm(const char *digits, bool colon)
{
    TubesFrame frame;
    frame_clear(&frame);
    frame_text(&frame, digits);
    if (colon)
        frame.separators = TUBES_MIDDLE_BOTH;
    return frame;
}


// One epoch of the main loop: prepare at slot 1, load at the load slot.
static void run_epoch(const DisplayClock *clock)
{
    _RB10 = 1; // the epoch pulse latches what was loaded
    tubes_epoch();
    display_prepare(clock);
    _RB10 = 0; // pulse over by DISPLAY_LOAD_SLOT
    display_service();
}


static void test_display(void)
{
    // 2026-10-18T12:34:56Z
    const uint32_t noon = 1792326896UL;
    DisplayClock clock = {true, true, noon, false, {0, 0}, false, 0};
    TubesFrame expected;
    unsigned loads;
    printf("display\n");

    memset(&fake_spi2buf, 0, sizeof fake_spi2buf);
    timezone_init();
    display_init();
    expected = hhmm("----", false);
    expected.separators = TUBES_START_LINE | TUBES_MIDDLE_LINE;
    CHECK(memcmp(tubes_shown(), &expected, sizeof expected) == 0);

    // The frame for the next second, never loaded while the pulse holds
    // the latch open, and loaded once.
    display_prepare(&clock);
    loads = tubes_stats()->loads;
    _RB10 = 1;
    display_service(); // pulse still high (a stretched epoch): wait
    CHECK(tubes_stats()->loads == loads);
    _RB10 = 0;
    display_service();
    CHECK(tubes_stats()->loads == loads + 1);
    display_service(); // once only
    CHECK(tubes_stats()->loads == loads + 1);
    expected = hhmm("1234", false); // :57 is odd: colon off
    CHECK(memcmp(tubes_loaded(), &expected, sizeof expected) == 0);

    // Through a minute: the colon blinks, the minute changes on the second.
    clock.utc_seconds = noon + 2; // :58 -> shows :59
    run_epoch(&clock);
    CHECK(memcmp(tubes_loaded(), &expected, sizeof expected) == 0);
    clock.utc_seconds = noon + 3; // :59 -> shows 12:35:00
    run_epoch(&clock);
    expected = hhmm("1235", true);
    CHECK(memcmp(tubes_loaded(), &expected, sizeof expected) == 0);
    tubes_epoch();
    CHECK(memcmp(tubes_shown(), &expected, sizeof expected) == 0);
    CHECK(display_stats()->missed == 0);

    // Not synchronised: the colon stays on.
    clock.synchronised = false;
    clock.utc_seconds = noon;
    run_epoch(&clock);
    expected = hhmm("1234", true);
    CHECK(memcmp(tubes_loaded(), &expected, sizeof expected) == 0);
    clock.synchronised = true;

    // Manual time zone: +1 h, then UTC-5 (the day before, for the date).
    timezone_set_auto(false);
    timezone_set_manual(3600, 0, false);
    run_epoch(&clock);
    expected = hhmm("1334", false);
    CHECK(memcmp(tubes_loaded(), &expected, sizeof expected) == 0);
    timezone_set_manual(-5L * 3600L, 3600, true); // EDT: -5 h + 1 h DST
    run_epoch(&clock);
    expected = hhmm("0834", false);
    CHECK(memcmp(tubes_loaded(), &expected, sizeof expected) == 0);

    // Automatic, from a TZ-STATE schedule: London leaves BST at
    // 2026-10-25T01:00:00Z, so local 01:59:59 BST is followed by 01:00:00.
    {
        const uint32_t transition = 1792890000UL;
        SML_TZ_STATE_PAYLOAD state;

        memset(&state, 0, sizeof state);
        state.flags = SML_TZ_VALID;
        state.source = SML_TZ_SOURCE_GNSS;
        state.count = 1;
        state.issued_utc = transition - 86400UL;
        state.horizon_end_utc = 1806195600UL;
        state.current.utc_seconds = 1774746000UL;
        state.current.dst_offset_s = 3600;
        state.current.flags = SML_TZ_ENTRY_DST_ACTIVE;
        state.next[0].utc_seconds = transition;
        state.next[0].dst_offset_s = 3600;
        timezone_set_auto(true);
        CHECK(timezone_apply(&state) == TIMEZONE_APPLIED);
        clock.utc_seconds = transition - 2UL; // :58 -> shows 01:59:59 BST
        run_epoch(&clock);
        expected = hhmm("0159", false);
        CHECK(memcmp(tubes_loaded(), &expected, sizeof expected) == 0);
        clock.utc_seconds = transition - 1UL; // -> 01:00:00 GMT
        run_epoch(&clock);
        expected = hhmm("0100", true);
        CHECK(memcmp(tubes_loaded(), &expected, sizeof expected) == 0);
        timezone_init(); // back to UTC
        clock.utc_seconds = noon;
    }

    // A leap second: 23:59:59, 23:59:60, 00:00:00.
    {
        const uint32_t midnight = 1798761600UL;
        clock.leap.event_utc = midnight;
        clock.leap.change = 1;
        clock.utc_seconds = midnight - 2; // :58 -> :59, odd
        run_epoch(&clock);
        expected = hhmm("2359", false);
        CHECK(memcmp(tubes_loaded(), &expected, sizeof expected) == 0);
        clock.utc_seconds = midnight - 1; // :59 -> :60, even
        run_epoch(&clock);
        expected = hhmm("2359", true);
        CHECK(memcmp(tubes_loaded(), &expected, sizeof expected) == 0);
        clock.leap_second = true; // :60 -> 00:00:00
        run_epoch(&clock);
        expected = hhmm("0000", true);
        CHECK(memcmp(tubes_loaded(), &expected, sizeof expected) == 0);
        clock.leap_second = false;
        clock.leap.change = 0;
    }

    // Time lost entirely: dashes.
    clock.known = false;
    run_epoch(&clock);
    expected = hhmm("----", false);
    expected.separators = TUBES_START_LINE | TUBES_MIDDLE_LINE;
    CHECK(memcmp(tubes_loaded(), &expected, sizeof expected) == 0);

    // A frame never loaded before the next is prepared is counted.
    display_prepare(&clock);
    display_prepare(&clock);
    CHECK(display_stats()->missed == 1);
    CHECK(tubes_stats()->loads_too_late == 0);
    display_print();
}


// The frame text with separators.
static TubesFrame text_frame(const char *text, uint8_t separators)
{
    TubesFrame frame;
    frame_clear(&frame);
    frame_text(&frame, text);
    frame.separators = separators;
    return frame;
}


static bool loaded_is(const char *text, uint8_t separators)
{
    TubesFrame expected = text_frame(text, separators);
    return memcmp(tubes_loaded(), &expected, sizeof expected) == 0;
}


static bool shown_is(const char *text, uint8_t separators)
{
    TubesFrame expected = text_frame(text, separators);
    return memcmp(tubes_shown(), &expected, sizeof expected) == 0;
}


// The uncertainty mode for a bound: the three figures, the decimal point
// and the unit (n, u, m).
static bool uncertainty_is(DisplayClock *clock, bool valid, uint32_t ns, const char *figures,
                           bool point, char unit)
{
    static const uint8_t units[3] = {
        TUBES_SEGMENT_C | TUBES_SEGMENT_E | TUBES_SEGMENT_G,
        TUBES_SEGMENT_C | TUBES_SEGMENT_D | TUBES_SEGMENT_E,
        TUBES_SEGMENT_A | TUBES_SEGMENT_C | TUBES_SEGMENT_E,
    };
    TubesFrame expected = text_frame(figures, point ? TUBES_MIDDLE_DOT : 0);
    bool ok;

    expected.segments[3] = units[unit == 'n' ? 0 : unit == 'u' ? 1 : 2];
    clock->uncertainty_valid = valid;
    clock->uncertainty_ns = ns;
    run_epoch(clock);
    ok = memcmp(tubes_loaded(), &expected, sizeof expected) == 0;
    if (!ok)
    {
        printf("  uncertainty %lu ns: expected \"%s\"%s%c\n", (unsigned long)ns, figures,
               point ? " ." : " ", unit);
    }
    return ok;
}


// The delta mode at now (the second shown is now + 1) against an epoch.
static bool delta_is(DisplayClock *clock, uint32_t shown_utc, uint32_t epoch, const char *text,
                     uint8_t separators)
{
    display_set_delta_epoch(epoch, false);
    clock->utc_seconds = shown_utc - 1U;
    run_epoch(clock);
    return loaded_is(text, separators);
}


static void test_modes(void)
{
    // 2026-10-18T09:05:56Z: the second shown is 09:05:57.
    const uint32_t morning = 1792314356UL;
    DisplayClock clock = {true, true, morning, false, {0, 0}, true, 61};
    uint16_t loads;
    printf("modes\n");

    timezone_init();
    display_init();
    CHECK(display_mode() == DISPLAY_MODE_HHMM && display_selected() == DISPLAY_MODE_HHMM);
    display_cycle(); // nothing to cycle while the time is unknown
    CHECK(display_mode() == DISPLAY_MODE_HHMM);
    run_epoch(&clock);
    CHECK(loaded_is("0905", 0));

    // 12 hours: no leading zero; 00 and 12 are 12.
    display_set_12h(true);
    CHECK(display_is_12h() && loaded_is(" 905", 0));
    clock.utc_seconds = morning - 9U * 3600U - 5U * 60U; // 00:00:57
    run_epoch(&clock);
    CHECK(loaded_is("1200", 0));
    clock.utc_seconds = morning + 4U * 3600U; // 13:05:57
    run_epoch(&clock);
    CHECK(loaded_is(" 105", 0));
    clock.utc_seconds = morning + 3U * 3600U; // 12:05:57
    run_epoch(&clock);
    CHECK(loaded_is("1205", 0));
    display_set_12h(false);
    clock.utc_seconds = morning;

    // MM:SS, the colon blinking only while synchronised (as V2).
    display_set_mode(DISPLAY_MODE_MMSS);
    CHECK(display_mode() == DISPLAY_MODE_MMSS && display_selected() == DISPLAY_MODE_MMSS);
    run_epoch(&clock);
    CHECK(loaded_is("0557", 0));
    clock.utc_seconds = morning + 1U;
    run_epoch(&clock);
    CHECK(loaded_is("0558", TUBES_MIDDLE_BOTH));
    clock.synchronised = false;
    run_epoch(&clock);
    CHECK(loaded_is("0558", 0));
    clock.synchronised = true;
    clock.utc_seconds = morning;

    // YYYY and MMDD.
    display_set_mode(DISPLAY_MODE_YYYY);
    run_epoch(&clock);
    CHECK(loaded_is("2026", 0));
    display_set_mode(DISPLAY_MODE_MMDD);
    run_epoch(&clock);
    CHECK(loaded_is("1018", 0));

    // SS:hh: the epoch's frame is :00, then the hundredths are shown at once
    // as they change, from 10 ms; the second waiting for the epoch is shifted
    // back in after each.
    display_set_mode(DISPLAY_MODE_SSMM);
    run_epoch(&clock);
    CHECK(loaded_is("5700", 0));
    _RB10 = 1;
    tubes_epoch();
    clock.utc_seconds = morning + 1U;
    display_prepare(&clock); // 57 is now on the tubes, 58 prepared
    display_tick(1);
    CHECK(shown_is("5700", 0) || memcmp(tubes_shown(), tubes_loaded(), sizeof(TubesFrame)) == 0);
    loads = (uint16_t)tubes_stats()->shows;
    display_tick(9); // still :00
    CHECK(tubes_stats()->shows == loads);
    display_tick(10); // latch open: shows at once
    CHECK(shown_is("5701", 0));
    display_tick(15); // same hundredth: nothing
    CHECK(tubes_stats()->shows == (uint32_t)loads + 1U);
    _RB10 = 0;
    display_service(); // 58 loaded for the epoch
    CHECK(loaded_is("5800", TUBES_MIDDLE_BOTH));
    display_tick(737);
    CHECK(shown_is("5773", 0) && loaded_is("5800", TUBES_MIDDLE_BOTH));
    display_tick(999);
    CHECK(shown_is("5799", 0));
    display_tick(1003); // a stretched epoch: no more than 99
    CHECK(shown_is("5799", 0));
    CHECK(display_stats()->redraws >= 4);
    clock.utc_seconds = morning;

    // Uncertainty: rounded up, three figures, the lower middle dot as the
    // decimal point, the unit last.
    display_set_mode(DISPLAY_MODE_UNCERTAINTY);
    CHECK(uncertainty_is(&clock, true, 0, "  0", false, 'n'));
    CHECK(uncertainty_is(&clock, true, 89, " 89", false, 'n'));
    CHECK(uncertainty_is(&clock, true, 999, "999", false, 'n'));
    CHECK(uncertainty_is(&clock, true, 1000, " 10", true, 'u'));
    CHECK(uncertainty_is(&clock, true, 1500, " 15", true, 'u'));
    CHECK(uncertainty_is(&clock, true, 1501, " 16", true, 'u'));
    CHECK(uncertainty_is(&clock, true, 12345, "124", true, 'u'));
    CHECK(uncertainty_is(&clock, true, 99950, "100", false, 'u'));
    CHECK(uncertainty_is(&clock, true, 130000, "130", false, 'u'));
    CHECK(uncertainty_is(&clock, true, 999001, " 10", true, 'm'));
    CHECK(uncertainty_is(&clock, true, 12600000UL, "126", true, 'm'));
    CHECK(uncertainty_is(&clock, true, 13700001UL, "138", true, 'm'));
    CHECK(uncertainty_is(&clock, true, 999000000UL, "999", false, 'm'));
    CHECK(uncertainty_is(&clock, true, 999000001UL, " --", false, 'm'));
    CHECK(uncertainty_is(&clock, false, 61, " --", false, 'm'));
    clock.uncertainty_valid = true;
    clock.uncertainty_ns = 61;

    // Delta: the two largest units, the start line while the epoch is to
    // come.
    display_set_mode(DISPLAY_MODE_DELTA);
    clock.synchronised = false; // no blinking colon to allow for
    {
        const uint32_t jan31 = 1769817600UL; // 2026-01-31T00:00:00Z
        const uint32_t mar1 = 1772323200UL;  // 2026-03-01T00:00:00Z
        CHECK(delta_is(&clock, morning + 1U, morning + 1U + 3599U, "5959", TUBES_START_LINE));
        CHECK(delta_is(&clock, morning + 1U, morning + 1U - 2U * 3600U - 61U, "0201", 0));
        CHECK(delta_is(&clock, morning + 1U, morning + 1U + 86400U + 7200U, "0102",
                       TUBES_START_LINE));
        CHECK(delta_is(&clock, mar1, jan31, "0101", 0)); // 31 Feb is 28 Feb
        CHECK(delta_is(&clock, jan31, mar1, "0101", TUBES_START_LINE));
        CHECK(delta_is(&clock, mar1 - 1U, jan31 + 1U, "2823", 0)); // the month not yet whole
        CHECK(delta_is(&clock, morning + 1U, 0, "5609", 0));       // since 1970: 56 y 9 m
        CHECK(delta_is(&clock, 4102444800UL, 0, " 130", 0));       // 2100: years alone
        CHECK(delta_is(&clock, morning + 1U, morning + 1U, "0000", 0));
    }
    clock.synchronised = true;
    clock.utc_seconds = morning;

    // A local epoch is counted against local time: New Year at midnight in
    // UTC+1 is 1 h nearer than UTC's.
    {
        const uint32_t new_year = 1798761600UL; // 2027-01-01T00:00:00 (as a scalar)
        display_set_delta_epoch(new_year, true);
        CHECK(display_delta_is_local());
        clock.synchronised = false;
        clock.utc_seconds = new_year - 3600U - 61U; // shown: 22:59:00 UTC, 23:59:00 local
        timezone_set_auto(false);
        timezone_set_manual(3600, 0, false);
        run_epoch(&clock);
        CHECK(loaded_is("0100", TUBES_START_LINE)); // 1 min to midnight local
        timezone_set_manual(0, 0, false);
        run_epoch(&clock);
        CHECK(loaded_is("0100", TUBES_START_LINE) == false); // 1 h 1 min in UTC
        CHECK(loaded_is("0101", TUBES_START_LINE));
        display_set_delta_epoch(new_year, false);
        timezone_init();
        clock.synchronised = true;
        clock.utc_seconds = morning;
    }

    // The button: the next mode at once, back to the one selected after
    // DISPLAY_CYCLE_EPOCHS without another press.
    display_set_mode(DISPLAY_MODE_HHMM);
    run_epoch(&clock);
    run_epoch(&clock);
    display_cycle();
    CHECK(display_mode() == DISPLAY_MODE_MMSS && display_selected() == DISPLAY_MODE_HHMM);
    CHECK(shown_is("0557", 0) && loaded_is("0557", 0)); // redrawn now, and the next too
    for (unsigned i = 1; i < DISPLAY_CYCLE_EPOCHS; i++)
    {
        run_epoch(&clock);
    }
    CHECK(display_mode() == DISPLAY_MODE_MMSS);
    run_epoch(&clock);
    CHECK(display_mode() == DISPLAY_MODE_HHMM && loaded_is("0905", 0));
    for (unsigned i = 0; i < DISPLAY_MODE_COUNT; i++)
    {
        display_cycle();
    }
    CHECK(display_mode() == DISPLAY_MODE_HHMM); // all the way round
    display_cycle();
    display_set_mode(DISPLAY_MODE_YYYY); // a new selection ends the cycle
    CHECK(display_mode() == DISPLAY_MODE_YYYY && loaded_is("2026", 0) && shown_is("2026", 0));
    CHECK(strcmp(display_mode_name(DISPLAY_MODE_UNCERTAINTY), "UNCERTAINTY") == 0);

    // The alarm dot, at once and in every mode; never with the dashes.
    display_set_alarm_dot(true);
    CHECK(loaded_is("2026", TUBES_START_DOT) && shown_is("2026", TUBES_START_DOT));
    display_set_mode(DISPLAY_MODE_UNCERTAINTY);
    run_epoch(&clock);
    CHECK((tubes_loaded()->separators & TUBES_START_DOT) != 0);
    clock.known = false;
    run_epoch(&clock);
    CHECK(loaded_is("----", TUBES_START_LINE | TUBES_MIDDLE_LINE));
    clock.known = true;
    display_set_alarm_dot(false);
    run_epoch(&clock);
    CHECK((tubes_loaded()->separators & TUBES_START_DOT) == 0);

    // The menu's overlay: at once and at each epoch, until cleared; the
    // preview: another mode for the moment, the mode shown unchanged.
    {
        TubesFrame menu = text_frame("MENU", 0);
        display_set_mode(DISPLAY_MODE_HHMM);
        run_epoch(&clock);
        display_set_overlay(&menu);
        CHECK(display_overlay_active() && shown_is("MENU", 0));
        run_epoch(&clock);
        CHECK(loaded_is("MENU", 0));
        display_tick(500); // nothing of the clock's
        CHECK(shown_is("MENU", 0));
        display_set_overlay(NULL);
        CHECK(!display_overlay_active() && shown_is("0905", 0) && loaded_is("0905", 0));
        display_preview(DISPLAY_MODE_YYYY);
        CHECK(shown_is("2026", 0) && display_mode() == DISPLAY_MODE_HHMM);
        run_epoch(&clock);
        CHECK(loaded_is("2026", 0));
        display_preview(DISPLAY_MODE_COUNT);
        CHECK(shown_is("0905", 0));
    }
    display_print();
}


int main(void)
{
    test_wire_format();
    test_frames();
    test_driver();
    test_display();
    test_modes();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
