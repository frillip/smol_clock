#include "controls.h"

#include <stdio.h>
#include "../platform/format.h" // our printf (format.h: the library's is slow)
#include <string.h>

#include "../platform/time_ms.h"

// One debounced input.
typedef struct
{
    bool raw;           // as last sampled
    uint32_t raw_since; // when it last changed
    bool level;         // debounced
    uint32_t level_since;
} Debounced;

static Debounced button;
static Debounced toggle;
static bool started;       // the first sample is the starting level, not a change
static bool long_reported; // this press has been reported as long
static ControlsPress last_press;
static uint32_t press_count;
static ControlsStats stats;

static void debounce_init(Debounced *input)
{
    memset(input, 0, sizeof(*input));
}


// True when the level changes this sample. A raw change that reverts
// before the debounce time is a bounce.
static bool debounce(Debounced *input, bool raw, uint32_t now_ms)
{
    if (raw != input->raw)
    {
        if (input->raw != input->level)
        {
            stats.bounces++; // went back before it counted
        }
        input->raw = raw;
        input->raw_since = now_ms;
    }
    if (input->raw != input->level &&
        time_ms_has_elapsed(now_ms, input->raw_since, CONTROLS_DEBOUNCE_MS))
    {
        input->level = input->raw;
        input->level_since = input->raw_since; // from the first edge
        return true;
    }
    return false;
}


static void report(ControlsPress press)
{
    last_press = press;
    press_count++;
    stats.presses[press]++;
}


void controls_init(void)
{
    debounce_init(&button);
    debounce_init(&toggle);
    started = false;
    long_reported = false;
    last_press = CONTROLS_NONE;
    press_count = 0;
    memset(&stats, 0, sizeof(stats));
}


void controls_service(uint32_t now_ms, bool button_pressed, bool switch_on)
{
    bool button_changed;

    if (!started)
    {
        // As found at start-up: a switch left on is on, not a change, and a
        // button held through the reset is not a press (long_reported).
        started = true;
        button.raw = button.level = button_pressed;
        toggle.raw = toggle.level = switch_on;
        button.raw_since = button.level_since = now_ms;
        toggle.raw_since = toggle.level_since = now_ms;
        long_reported = button_pressed;
        return;
    }
    button_changed = debounce(&button, button_pressed, now_ms);

    if (button_changed && button.level)
    {
        long_reported = false;
    }
    else if (button_changed && !long_reported)
    {
        report(CONTROLS_SHORT); // released before it was long
    }
    // Not while a release is being debounced: that press was short.
    if (button.level && button.raw && !long_reported &&
        time_ms_has_elapsed(now_ms, button.level_since, CONTROLS_LONG_MS))
    {
        long_reported = true;
        report(CONTROLS_LONG);
    }
    if (debounce(&toggle, switch_on, now_ms))
    {
        stats.switch_changes++;
    }
}


bool controls_button_down(void)
{
    return button.level;
}


bool controls_switch_on(void)
{
    return toggle.level;
}


ControlsPress controls_press(uint32_t *count)
{
    *count = press_count;
    return last_press;
}


uint32_t controls_switch_changes(void)
{
    return stats.switch_changes;
}


const ControlsStats *controls_stats(void)
{
    return &stats;
}


const char *controls_press_name(ControlsPress press)
{
    static const char *const names[CONTROLS_PRESS_COUNT] = {"NONE", "SHORT", "LONG", "HOLD"};
    return press < CONTROLS_PRESS_COUNT ? names[press] : "?";
}


void controls_print(bool verbose)
{
    printf("Controls: button %s, switch %s", button.level ? "DOWN" : "UP",
           toggle.level ? "ON" : "OFF");
    if (verbose)
    {
        printf("; %lu short, %lu long presses, %lu switch changes, %lu bounces",
               (unsigned long)stats.presses[CONTROLS_SHORT],
               (unsigned long)stats.presses[CONTROLS_LONG], (unsigned long)stats.switch_changes,
               (unsigned long)stats.bounces);
    }
    printf("\r\n");
}
