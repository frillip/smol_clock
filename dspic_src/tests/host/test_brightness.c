/*
 * Brightness (ui/brightness.c): the level from the light, the manual
 * level, and the fade between them, across off and fully lit, against a
 * stand-in tube driver and light sensor.
 */
#include <stdio.h>
#include <string.h>

#include "../../drivers/tubes.h"
#include "../../drivers/veml6040.h"
#include "../../ui/brightness.h"

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

// The tube driver: what it shows for a level, as tubes.c does.
static uint16_t shown;
static int sets;
void tubes_brightness_set(uint16_t level)
{
    sets++;
    shown = level < TUBES_BRIGHTNESS_MIN   ? TUBES_BRIGHTNESS_OFF
            : level > TUBES_BRIGHTNESS_MAX ? TUBES_BRIGHTNESS_FULL
                                           : level;
}


uint16_t tubes_brightness(void)
{
    return shown;
}


static Veml6040Status light;
const Veml6040Status *veml6040_status(void)
{
    return &light;
}


// Services until the tubes stop changing; false if they never do. Checks
// each step is a fade, not a jump (except into and out of off and full).
static bool settle(int limit)
{
    while (limit-- > 0)
    {
        uint16_t before = shown;
        int before_sets = sets;

        brightness_service();
        if (sets == before_sets)
        {
            return true;
        }
        if (before >= TUBES_BRIGHTNESS_MIN && before <= TUBES_BRIGHTNESS_MAX &&
            shown >= TUBES_BRIGHTNESS_MIN && shown <= TUBES_BRIGHTNESS_MAX)
        {
            int step = (int)shown - (int)before;
            CHECK(step >= -(int)BRIGHTNESS_STEP && step <= (int)BRIGHTNESS_STEP);
        }
    }
    return false;
}


int main(void)
{
    // The light to a level.
    CHECK(brightness_from_lux(0) == TUBES_BRIGHTNESS_MIN);
    CHECK(brightness_from_lux(66) == 200);    // 198, rounded to the step
    CHECK(brightness_from_lux(1000) == 3000); // 100 lux
    CHECK(brightness_from_lux(1267) == TUBES_BRIGHTNESS_MAX);
    CHECK(brightness_from_lux(65535) == TUBES_BRIGHTNESS_MAX); // no overflow

    brightness_init();
    CHECK(brightness_is_auto() && shown == TUBES_BRIGHTNESS_DEFAULT);

    // Auto with no sensor: the manual level stands in.
    brightness_set_level(1500);
    CHECK(settle(1000) && shown == 1500 && brightness_target() == 1500);

    // A reading: fades to its level, 10 a step.
    light.present = true;
    light.reads = 1;
    light.lux_valid = true;
    light.lux_x10 = 1000;
    CHECK(settle(1000) && shown == 3000 && brightness_target() == 3000);
    // Too dark, and saturated.
    light.lux_valid = false;
    light.too_dark = true;
    CHECK(settle(1000) && shown == TUBES_BRIGHTNESS_MIN);
    light.too_dark = false;
    light.saturated = true;
    CHECK(settle(1000) && shown == TUBES_BRIGHTNESS_MAX);
    light.saturated = false;
    // Gone: the manual level again.
    light.present = false;
    CHECK(settle(1000) && shown == 1500);

    // Manual: a step, straight to the level, off and fully lit included.
    brightness_set_auto(false);
    brightness_set_level(TUBES_BRIGHTNESS_OFF);
    brightness_service();
    CHECK(shown == TUBES_BRIGHTNESS_OFF);
    brightness_set_level(3000);
    brightness_service();
    CHECK(shown == 3000);
    brightness_set_level(TUBES_BRIGHTNESS_FULL);
    brightness_service();
    CHECK(shown == TUBES_BRIGHTNESS_FULL);
    // Past full is full.
    brightness_set_level(60000U);
    brightness_service();
    CHECK(shown == TUBES_BRIGHTNESS_FULL && brightness_target() == 4000);

    // Auto from fully lit: the fade starts at MAX (not stuck above the PWM
    // range) ...
    light.present = true;
    light.lux_valid = true;
    light.lux_x10 = 1000;
    brightness_set_auto(true);
    brightness_service();
    CHECK(shown == TUBES_BRIGHTNESS_MAX);
    CHECK(settle(1000) && shown == 3000);
    // ... and from off at MIN (not stuck below it).
    brightness_set_auto(false);
    brightness_set_level(TUBES_BRIGHTNESS_OFF);
    brightness_service();
    CHECK(shown == TUBES_BRIGHTNESS_OFF);
    brightness_set_auto(true);
    brightness_service();
    CHECK(shown == TUBES_BRIGHTNESS_MIN);
    CHECK(settle(1000) && shown == 3000);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
