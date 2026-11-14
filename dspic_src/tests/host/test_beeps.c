/*
 * The chimes and clicks (ui/beeps.c): when they sound and how many beeps,
 * against a stand-in buzzer.
 */
#include <stdio.h>

#include "../../drivers/buzzer.h"
#include "../../ui/beeps.h"

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

const BuzzerPattern buzzer_button = {60U, 0U, 0U, 0U, false};
const BuzzerPattern buzzer_chime = {100U, 100U, 4U, 300U, false};
const BuzzerPattern buzzer_alarm = {100U, 100U, 4U, 500U, true};

static const BuzzerPattern *played;
static uint8_t played_count;
static int plays;
bool buzzer_play(const BuzzerPattern *pattern, uint8_t count, BuzzerPriority priority)
{
    (void)priority;
    played = pattern;
    played_count = count;
    plays++;
    return true;
}


#define DAY 86400UL

int main(void)
{
    uint32_t base = 20000UL * DAY; // a local midnight

    CHECK(beeps_chime_count(0) == 12);
    CHECK(beeps_chime_count(3600) == 1);
    CHECK(beeps_chime_count(12UL * 3600) == 12); // noon
    CHECK(beeps_chime_count(15UL * 3600) == 3);
    CHECK(beeps_chime_count(15UL * 3600 + 900) == 1);
    CHECK(beeps_chime_count(15UL * 3600 + 1) == 0);
    CHECK(beeps_chime_count(23UL * 3600 + 2700) == 1);

    beeps_init();
    beeps_epoch(true, false, base);
    beeps_button();
    CHECK(plays == 0); // off by default

    beeps_set_enabled(true);
    beeps_epoch(true, false, base + 15UL * 3600);
    CHECK(plays == 1 && played == &buzzer_chime && played_count == 3);
    beeps_epoch(true, false, base + 15UL * 3600 + 1);
    CHECK(plays == 1);
    beeps_epoch(false, false, base + 16UL * 3600); // time unknown
    beeps_epoch(true, true, base + 16UL * 3600);   // a leap second
    CHECK(plays == 1);
    beeps_button();
    CHECK(plays == 2 && played == &buzzer_button && played_count == 1);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
