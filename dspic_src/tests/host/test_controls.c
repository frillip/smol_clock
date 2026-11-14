/*
 * The button and the switch (ui/controls.c): debouncing, short and long
 * presses as V2, the starting levels, and two readers of the presses.
 */
#include <stdio.h>
#include <string.h>

#include "../../ui/controls.h"

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

static uint32_t now;
static bool button;
static bool toggle;

static void run_ms(uint32_t ms)
{
    uint32_t end = now + ms;

    while (now < end)
    {
        now += CONTROLS_SERVICE_MS;
        controls_service(now, button, toggle);
    }
}


// The press a reader that has seen count would see now, or NONE.
static ControlsPress new_press(uint32_t *seen)
{
    uint32_t count;
    ControlsPress press = controls_press(&count);

    if (count == *seen)
    {
        return CONTROLS_NONE;
    }
    CHECK(count == *seen + 1U); // one at a time in these tests
    *seen = count;
    return press;
}


int main(void)
{
    uint32_t seen = 0;
    uint32_t other = 0;
    int bounce;

    // Starting levels: the switch on is not a change.
    now = 1000;
    toggle = true;
    controls_init();
    run_ms(100);
    CHECK(controls_switch_on() && controls_switch_changes() == 0 && !controls_button_down());

    // A short press: 100 ms, with contact bounce at both ends.
    for (bounce = 0; bounce < 3; bounce++)
    {
        button = !button;
        run_ms(CONTROLS_SERVICE_MS);
    }
    CHECK(new_press(&seen) == CONTROLS_NONE); // bouncing: nothing yet
    button = true;
    run_ms(100);
    CHECK(controls_button_down() && new_press(&seen) == CONTROLS_NONE);
    button = false;
    run_ms(CONTROLS_SERVICE_MS);
    button = true;
    run_ms(CONTROLS_SERVICE_MS);
    button = false;
    run_ms(100);
    CHECK(!controls_button_down() && new_press(&seen) == CONTROLS_SHORT);
    CHECK(controls_stats()->bounces > 0);

    // A glitch shorter than the debounce time is not a press.
    button = true;
    run_ms(CONTROLS_DEBOUNCE_MS - CONTROLS_SERVICE_MS);
    button = false;
    run_ms(100);
    CHECK(new_press(&seen) == CONTROLS_NONE);

    // A long press: reported once held CONTROLS_LONG_MS from the first
    // sample to see it, while still held; the release adds nothing.
    button = true;
    run_ms(CONTROLS_LONG_MS);
    CHECK(new_press(&seen) == CONTROLS_NONE);
    run_ms(CONTROLS_SERVICE_MS);
    CHECK(new_press(&seen) == CONTROLS_LONG);
    run_ms(3000);
    CHECK(new_press(&seen) == CONTROLS_NONE); // held on: no hold yet
    button = false;
    run_ms(100);
    CHECK(new_press(&seen) == CONTROLS_NONE);

    // Just short of long is short.
    button = true;
    run_ms(CONTROLS_LONG_MS - 2U * CONTROLS_SERVICE_MS);
    button = false;
    run_ms(100);
    CHECK(new_press(&seen) == CONTROLS_SHORT);

    // A second reader sees the same presses, whenever it looks.
    {
        uint32_t count;
        CHECK(controls_press(&count) == CONTROLS_SHORT && count == seen && count != other);
    }

    // The switch: off and on again, debounced.
    toggle = false;
    run_ms(CONTROLS_SERVICE_MS);
    toggle = true;
    run_ms(CONTROLS_SERVICE_MS);
    toggle = false;
    run_ms(100);
    CHECK(!controls_switch_on() && controls_switch_changes() == 1);
    toggle = true;
    run_ms(100);
    CHECK(controls_switch_on() && controls_switch_changes() == 2);

    // Late service (a busy pass): times still from the samples' clock.
    button = true;
    now += 40;
    controls_service(now, button, toggle);
    now += 600;
    controls_service(now, button, toggle);
    CHECK(new_press(&seen) == CONTROLS_LONG);
    button = false;
    run_ms(100);

    // Held through a reset: not a press until released and pressed again.
    button = true;
    controls_init();
    run_ms(2000);
    button = false;
    run_ms(100);
    seen = 0;
    CHECK(new_press(&seen) == CONTROLS_NONE && !controls_button_down());
    button = true;
    run_ms(100);
    button = false;
    run_ms(100);
    CHECK(new_press(&seen) == CONTROLS_SHORT);

    CHECK(controls_press_name(CONTROLS_LONG)[0] == 'L');
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
