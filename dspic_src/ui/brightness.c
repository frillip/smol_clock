#include "brightness.h"

#include <stdio.h>
#include "../platform/format.h" // our printf (format.h: the library's is slow)

#include "../drivers/tubes.h"
#include "../drivers/veml6040.h"

static bool automatic;
static uint16_t manual_level;
static uint16_t target;
static uint16_t level; // where the tubes were taken; tubes_brightness() is what that shows

void brightness_init(void)
{
    automatic = true;
    manual_level = TUBES_BRIGHTNESS_DEFAULT;
    target = TUBES_BRIGHTNESS_DEFAULT;
    level = TUBES_BRIGHTNESS_DEFAULT;
    tubes_brightness_set(level);
}


void brightness_set_auto(bool on)
{
    automatic = on;
}


bool brightness_is_auto(void)
{
    return automatic;
}


void brightness_set_level(uint16_t new_level)
{
    manual_level = new_level > TUBES_BRIGHTNESS_FULL ? TUBES_BRIGHTNESS_FULL : new_level;
}


uint16_t brightness_target(void)
{
    return target;
}


uint16_t brightness_from_lux(uint16_t lux_x10)
{
    uint32_t value = BRIGHTNESS_LUX_CONST + BRIGHTNESS_LUX_LINEAR * lux_x10 +
                     BRIGHTNESS_LUX_SQUARE * ((uint32_t)lux_x10 * lux_x10);

    value = (value + BRIGHTNESS_STEP / 2U) / BRIGHTNESS_STEP * BRIGHTNESS_STEP;
    if (value < TUBES_BRIGHTNESS_MIN)
    {
        return TUBES_BRIGHTNESS_MIN;
    }
    return value > TUBES_BRIGHTNESS_MAX ? TUBES_BRIGHTNESS_MAX : (uint16_t)value;
}


static uint16_t auto_target(void)
{
    const Veml6040Status *sensor = veml6040_status();

    if (!sensor->present || sensor->reads == 0U)
    {
        return manual_level;
    }
    if (sensor->saturated)
    {
        return TUBES_BRIGHTNESS_MAX;
    }
    if (sensor->too_dark)
    {
        return TUBES_BRIGHTNESS_MIN;
    }
    return sensor->lux_valid ? brightness_from_lux(sensor->lux_x10) : manual_level;
}


// One step from level towards target. Off and fully lit are outside the PWM
// range, so a step out of either lands on its end of the range.
static uint16_t step_towards(uint16_t from, uint16_t to)
{
    uint16_t next;

    if (from < to)
    {
        next =
            from < TUBES_BRIGHTNESS_MIN ? TUBES_BRIGHTNESS_MIN : (uint16_t)(from + BRIGHTNESS_STEP);
        return next > to ? to : next;
    }
    if (from > to)
    {
        next = from > TUBES_BRIGHTNESS_MAX
                   ? TUBES_BRIGHTNESS_MAX
                   : (from < BRIGHTNESS_STEP ? 0U : (uint16_t)(from - BRIGHTNESS_STEP));
        return next < to ? to : next;
    }
    return from;
}


void brightness_service(void)
{
    target = automatic ? auto_target() : manual_level;
    if (level != target)
    {
        // Auto fades; a manual level is a step response.
        level = automatic ? step_towards(level, target) : target;
        tubes_brightness_set(level);
    }
}


// A level as it looks: OFF below the PWM range, FULL above it.
static void print_level(uint16_t value)
{
    if (value < TUBES_BRIGHTNESS_MIN)
    {
        printf("OFF");
    }
    else if (value > TUBES_BRIGHTNESS_MAX)
    {
        printf("FULL");
    }
    else
    {
        printf("%u", value);
    }
}


void brightness_print(bool verbose)
{
    const Veml6040Status *sensor = veml6040_status();
    bool reading = sensor->present && sensor->reads != 0U;

    printf("Brightness: %s ", automatic ? "AUTO" : "MANUAL");
    print_level(tubes_brightness());
    if (tubes_brightness() != target)
    {
        printf(" -> ");
        print_level(target);
    }
    if (automatic && !reading)
    {
        printf(", NO READING: manual level");
    }
    else if (automatic && sensor->saturated)
    {
        printf(", SATURATED");
    }
    else if (automatic && sensor->too_dark)
    {
        printf(", TOO DARK");
    }
    else if (automatic && sensor->lux_valid)
    {
        printf(", %u.%u lux", sensor->lux_x10 / 10U, sensor->lux_x10 % 10U);
    }
    if (verbose && automatic)
    {
        printf("; manual ");
        print_level(manual_level);
    }
    printf("\r\n");
}
