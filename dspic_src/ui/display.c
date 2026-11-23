#include "display.h"

#include <stddef.h>
#include <stdio.h>
#include "../platform/format.h" // our printf (format.h: the library's is slow)
#include <string.h>

#include "../drivers/tubes.h"
#include "frame.h"

typedef struct
{
    const char *name;
    void (*render)(const DisplayView *view, TubesFrame *frame);
} DisplayModeDefinition;

// A second to show: dashes until the time is known.
typedef struct
{
    bool known;
    DisplayView view;
} DisplaySecond;

static void render_hhmm(const DisplayView *view, TubesFrame *frame);
static void render_mmss(const DisplayView *view, TubesFrame *frame);
static void render_ssmm(const DisplayView *view, TubesFrame *frame);
static void render_yyyy(const DisplayView *view, TubesFrame *frame);
static void render_mmdd(const DisplayView *view, TubesFrame *frame);
static void render_delta(const DisplayView *view, TubesFrame *frame);
static void render_uncertainty(const DisplayView *view, TubesFrame *frame);

// One row per DisplayMode, in the same order.
static const DisplayModeDefinition modes[DISPLAY_MODE_COUNT] = {
    {"HHMM", render_hhmm},
    {"MMSS", render_mmss},
    {"SSMM", render_ssmm},
    {"YYYY", render_yyyy},
    {"MMDD", render_mmdd},
    {"DELTA", render_delta},
    {"UNCERTAINTY", render_uncertainty},
};

// The units of the uncertainty mode, as lower case as seven segments go.
static const uint8_t unit_glyphs[3] = {
    TUBES_SEGMENT_C | TUBES_SEGMENT_E | TUBES_SEGMENT_G, // n
    TUBES_SEGMENT_C | TUBES_SEGMENT_D | TUBES_SEGMENT_E, // u
    TUBES_SEGMENT_A | TUBES_SEGMENT_C | TUBES_SEGMENT_E, // m, as V2's M
};

static DisplayMode selected;
static DisplayMode shown_mode;
static bool previewing; // the menu shows preview_mode instead, for the moment
static DisplayMode preview_mode;
static bool overlay_on; // the menu's own frame, in place of the clock
static TubesFrame overlay;
static uint16_t cycle_epochs; // since the last press, while shown_mode != selected
static bool twelve_hours;
static bool alarm_dot; // the start dot, in every mode: the alarm is set
static uint32_t delta_epoch;
static bool delta_local; // delta_epoch is local wall-clock time

static DisplaySecond shown; // the second on the tubes (latched at the last epoch)
static DisplaySecond next;  // the second prepared for the next epoch
static int32_t offset_s;    // UTC to local, of the last frame prepared
static TubesFrame prepared;
static bool prepared_pending;
static DisplayStats stats;

// --------------------------------------------------------------------------
// Modes
// --------------------------------------------------------------------------

// The colon blinks with the seconds while synchronised. As V2: HH:MM keeps
// it on while running free, the other modes leave it off.
static void blink(const DisplayView *view, TubesFrame *frame, bool steady_when_free)
{
    if (view->synchronised ? view->local.second % 2U == 0U : steady_when_free)
    {
        frame->separators |= TUBES_MIDDLE_BOTH;
    }
}


// 12 hours: 12, 1 .. 11, 12, 1 .. 11, without a leading zero.
static void render_hhmm(const DisplayView *view, TubesFrame *frame)
{
    uint8_t hour = view->local.hour;

    if (twelve_hours)
    {
        hour = hour % 12U == 0U ? 12U : (uint8_t)(hour % 12U);
        frame_number_blanked(frame, 0, 2, hour);
    }
    else
    {
        frame_number(frame, 0, 2, hour);
    }
    frame_number(frame, 2, 2, view->local.minute);
    blink(view, frame, true);
}


static void render_mmss(const DisplayView *view, TubesFrame *frame)
{
    frame_number(frame, 0, 2, view->local.minute);
    frame_number(frame, 2, 2, view->local.second);
    blink(view, frame, false);
}


static void render_ssmm(const DisplayView *view, TubesFrame *frame)
{
    frame_number(frame, 0, 2, view->local.second);
    frame_number(frame, 2, 2, view->hundredths);
    blink(view, frame, false);
}


static void render_yyyy(const DisplayView *view, TubesFrame *frame)
{
    frame_number(frame, 0, 4, view->local.year);
    blink(view, frame, false);
}


static void render_mmdd(const DisplayView *view, TubesFrame *frame)
{
    frame_number(frame, 0, 2, view->local.month);
    frame_number(frame, 2, 2, view->local.day);
    blink(view, frame, false);
}


static uint8_t days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (month == 2U && year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U))
    {
        return 29U;
    }
    return days[month - 1U];
}


// x's day of the month and time of day come before y's.
static bool before_in_month(const UtcCivil *x, const UtcCivil *y)
{
    uint32_t x_key = ((uint32_t)x->day << 17) | ((uint32_t)x->hour << 12) |
                     ((uint32_t)x->minute << 6) | x->second;
    uint32_t y_key = ((uint32_t)y->day << 17) | ((uint32_t)y->hour << 12) |
                     ((uint32_t)y->minute << 6) | y->second;
    return x_key < y_key;
}


// Calendar time between the delta epoch and now, the two largest
// units: years (alone from 100), years and months, months and days, days and
// hours, hours and minutes, or minutes and seconds. The start line is lit
// while the epoch is still to come. Whole months are counted from the
// earlier moment, a day of the month that does not exist (31 February)
// taken as the month's last. A UTC epoch is counted against UTC; a local one
// against local time, as the display shows it, so a DST change between the
// two moves the delta by the change, as it moves the clock.
static void render_delta(const DisplayView *view, TubesFrame *frame)
{
    uint32_t now =
        delta_local ? view->utc_seconds + (uint32_t)view->zone.offset_s : view->utc_seconds;
    bool future = now < delta_epoch;
    uint32_t from = future ? now : delta_epoch;
    uint32_t to = future ? delta_epoch : now;
    UtcCivil a, b, anchor;
    uint32_t months;
    uint32_t rest;
    uint16_t first, second;

    utc_to_civil(from, false, &a);
    utc_to_civil(to, false, &b);
    months = (uint32_t)(b.year - a.year) * 12U + b.month - a.month;
    if (months > 0U && before_in_month(&b, &a)) // the last month is not whole yet
    {
        months--;
    }
    anchor = a;
    anchor.year = (uint16_t)(a.year + (a.month - 1U + months) / 12U);
    anchor.month = (uint8_t)((a.month - 1U + months) % 12U + 1U);
    if (anchor.day > days_in_month(anchor.year, anchor.month))
    {
        anchor.day = days_in_month(anchor.year, anchor.month);
    }
    rest = to - utc_from_civil(&anchor);

    if (months >= 1200U)
    {
        frame_number_blanked(frame, 0, 4, (uint16_t)(months / 12U));
    }
    else
    {
        if (months >= 12U)
        {
            first = (uint16_t)(months / 12U);
            second = (uint16_t)(months % 12U);
        }
        else if (months > 0U)
        {
            first = (uint16_t)months;
            second = (uint16_t)(rest / 86400UL);
        }
        else if (rest >= 86400UL)
        {
            first = (uint16_t)(rest / 86400UL);
            second = (uint16_t)(rest / 3600UL % 24U);
        }
        else if (rest >= 3600UL)
        {
            first = (uint16_t)(rest / 3600UL);
            second = (uint16_t)(rest / 60U % 60U);
        }
        else
        {
            first = (uint16_t)(rest / 60U);
            second = (uint16_t)(rest % 60U);
        }
        frame_number(frame, 0, 2, first);
        frame_number(frame, 2, 2, second);
    }
    blink(view, frame, false);
    if (future)
    {
        frame->separators |= TUBES_START_LINE;
    }
}


static uint32_t ceiling_div(uint32_t value, uint32_t divisor)
{
    return value / divisor + (value % divisor != 0U ? 1U : 0U);
}


// The bound, rounded up (it is a bound), in three figures and a unit: whole
// nanoseconds, then tenths and whole microseconds and milliseconds, the
// middle lower dot as the decimal point. " --m" without one.
static void render_uncertainty(const DisplayView *view, TubesFrame *frame)
{
    uint32_t ns = view->uncertainty_ns;
    uint32_t step = 100U; // ns in a tenth of the unit
    uint8_t unit = 0;
    uint32_t value;
    bool point = false;

    if (!view->uncertainty_valid || ns > DISPLAY_UNCERTAINTY_MAX_NS)
    {
        frame_text(frame, " --");
        frame->segments[3] = unit_glyphs[2];
        return;
    }
    value = ns;
    if (ns >= 1000U)
    {
        for (unit = 1; unit < 3U; unit++)
        {
            value = ceiling_div(ns, step);
            if (value < 1000U)
            {
                point = true;
                break;
            }
            value = ceiling_div(ns, step * 10U);
            if (value < 1000U)
            {
                break;
            }
            step *= 1000U;
        }
    }
    if (point)
    {
        frame_number(frame, 0, 3, (uint16_t)value);
        if (value < 100U)
        {
            frame_blank(frame, 0, 1);
        }
        frame->separators |= TUBES_MIDDLE_DOT;
    }
    else
    {
        frame_number_blanked(frame, 0, 3, (uint16_t)value);
    }
    frame->segments[3] = unit_glyphs[unit];
}


// --------------------------------------------------------------------------
// Drawing
// --------------------------------------------------------------------------

// The mode drawn: shown_mode, unless the menu is previewing another.
static DisplayMode drawn_mode(void)
{
    return previewing ? preview_mode : shown_mode;
}


static void render(const DisplaySecond *second, TubesFrame *frame)
{
    DisplayMode mode = drawn_mode();

    if (overlay_on)
    {
        *frame = overlay;
        return;
    }
    frame_clear(frame);
    if (second->known)
    {
        modes[mode].render(&second->view, frame);
        if (alarm_dot)
        {
            frame->separators |= TUBES_START_DOT;
        }
    }
    else
    {
        frame_dashes(frame);
    }
}


// The mode or its settings changed: the second on the tubes is drawn again
// now, and the one prepared for the next epoch, loaded again if it already
// was. Nothing before the first epoch has been prepared (dashes).
static void redraw(void)
{
    TubesFrame frame;

    if (!overlay_on && !shown.known && !next.known)
    {
        return;
    }
    render(&next, &prepared);
    render(&shown, &frame);
    tubes_show(&frame);
    stats.redraws++;
    if (!prepared_pending && !tubes_latch_open())
    {
        tubes_load(&prepared);
    }
}


// --------------------------------------------------------------------------
// Interface
// --------------------------------------------------------------------------

void display_init(void)
{
    TubesFrame frame;

    selected = DISPLAY_MODE_HHMM;
    shown_mode = DISPLAY_MODE_HHMM;
    previewing = false;
    preview_mode = DISPLAY_MODE_HHMM;
    overlay_on = false;
    cycle_epochs = 0;
    twelve_hours = false;
    alarm_dot = false;
    delta_epoch = 0;
    delta_local = false;
    shown.known = false;
    next.known = false;
    offset_s = 0;
    prepared_pending = false;
    stats.prepared = 0;
    stats.loaded = 0;
    stats.missed = 0;
    stats.redraws = 0;

    tubes_init();
    frame_clear(&frame);
    frame_dashes(&frame);
    tubes_show(&frame);
}


void display_set_mode(DisplayMode mode)
{
    if (mode >= DISPLAY_MODE_COUNT || mode == selected)
    {
        return;
    }
    selected = mode;
    shown_mode = mode;
    redraw();
}


void display_set_alarm_dot(bool on)
{
    if (on != alarm_dot)
    {
        alarm_dot = on;
        redraw();
    }
}


void display_set_12h(bool twelve)
{
    if (twelve != twelve_hours)
    {
        twelve_hours = twelve;
        redraw();
    }
}


void display_set_delta_epoch(uint32_t seconds, bool local)
{
    if (seconds != delta_epoch || local != delta_local)
    {
        delta_epoch = seconds;
        delta_local = local;
        redraw();
    }
}


DisplayMode display_mode(void)
{
    return shown_mode;
}


DisplayMode display_selected(void)
{
    return selected;
}


bool display_is_12h(void)
{
    return twelve_hours;
}


uint32_t display_delta_epoch(void)
{
    return delta_epoch;
}


bool display_delta_is_local(void)
{
    return delta_local;
}


void display_print_epoch(uint32_t seconds, bool local)
{
    if (local)
    {
        utc_print_local(seconds);
        printf(" local");
    }
    else
    {
        utc_print(seconds);
    }
}


void display_cycle(void)
{
    if (!shown.known)
    {
        return;
    }
    shown_mode = (DisplayMode)((shown_mode + 1U) % DISPLAY_MODE_COUNT);
    cycle_epochs = 0;
    redraw();
}


void display_set_overlay(const TubesFrame *frame)
{
    if (frame == NULL)
    {
        if (overlay_on)
        {
            overlay_on = false;
            redraw();
        }
        return;
    }
    if (overlay_on && memcmp(frame, &overlay, sizeof(overlay)) == 0)
    {
        return;
    }
    overlay = *frame;
    overlay_on = true;
    redraw();
}


bool display_overlay_active(void)
{
    return overlay_on;
}


void display_preview(DisplayMode mode)
{
    bool on = mode < DISPLAY_MODE_COUNT;

    if (on == previewing && (!on || mode == preview_mode))
    {
        return;
    }
    previewing = on;
    if (on)
    {
        preview_mode = mode;
    }
    redraw();
}


void display_prepare(const DisplayClock *clock)
{
    // The epoch that has just passed latched the last frame loaded.
    tubes_epoch();
    if (prepared_pending)
    {
        stats.missed++;
    }
    shown = next;

    if (shown_mode != selected && ++cycle_epochs >= DISPLAY_CYCLE_EPOCHS)
    {
        shown_mode = selected; // from the next second
    }

    next.known = clock != NULL && clock->known;
    if (next.known)
    {
        DisplayView *view = &next.view;
        bool next_leap;
        uint32_t utc = utc_advance(clock->utc_seconds, clock->leap_second, 1U,
                                   clock->leap.change != 0 ? &clock->leap : NULL, &next_leap);

        timezone_offset_at(utc, &view->zone);
        offset_s = view->zone.offset_s;
        utc_to_civil(utc + (uint32_t)offset_s, next_leap, &view->local);
        view->utc_seconds = utc;
        view->synchronised = clock->synchronised;
        view->uncertainty_valid = clock->uncertainty_valid;
        view->uncertainty_ns = clock->uncertainty_ns;
        view->hundredths = 0;
    }
    render(&next, &prepared);
    prepared_pending = true;
    stats.prepared++;
}


void display_service(void)
{
    if (!prepared_pending || tubes_latch_open())
    {
        return;
    }
    tubes_load(&prepared);
    prepared_pending = false;
    stats.loaded++;
}


void display_tick(uint16_t slot)
{
    uint8_t hundredths = (uint8_t)(slot >= 990U ? 99U : slot / 10U);
    TubesFrame frame;

    // Until 10 ms the epoch's own frame (SS:00) is right, and the second
    // may not have been prepared yet.
    if (overlay_on || drawn_mode() != DISPLAY_MODE_SSMM || !shown.known || hundredths == 0U ||
        hundredths == shown.view.hundredths)
    {
        return;
    }
    shown.view.hundredths = hundredths;
    render(&shown, &frame);
    tubes_show(&frame);
    stats.redraws++;
}


const DisplayStats *display_stats(void)
{
    return &stats;
}


const char *display_mode_name(DisplayMode which)
{
    return which < DISPLAY_MODE_COUNT ? modes[which].name : "?";
}


void display_print(void)
{
    const TubesStats *tubes = tubes_stats();

    printf("Display: %s", display_mode_name(shown_mode));
    if (shown_mode != selected)
    {
        printf(" (selected %s)", display_mode_name(selected));
    }
    printf(", UTC");
    utc_print_offset(offset_s);
    printf("; frames: %lu prepared, %lu loaded, %lu missed, %lu redrawn; tubes: %lu loads "
           "(%lu too late), %lu shows, %lu SPI stalls\r\n",
           (unsigned long)stats.prepared, (unsigned long)stats.loaded, (unsigned long)stats.missed,
           (unsigned long)stats.redraws, (unsigned long)tubes->loads,
           (unsigned long)tubes->loads_too_late, (unsigned long)tubes->shows,
           (unsigned long)tubes->spi_stalls);
}
