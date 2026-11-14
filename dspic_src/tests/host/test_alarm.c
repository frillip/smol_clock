/*
 * The alarm (ui/alarm.c): armed by the switch, sounding once a local day at
 * its time (DST changes and time steps included), and stopped by a press,
 * the switch or the timeout, against a stand-in buzzer.
 */
#include <stdio.h>

#include "../../drivers/buzzer.h"
#include "../../ui/alarm.h"

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

static int plays, stops;
static bool playing;
bool buzzer_play(const BuzzerPattern *pattern, uint8_t count, BuzzerPriority priority)
{
    if (pattern == &buzzer_alarm && count == 4U && priority == BUZZER_PRIORITY_ALARM)
    {
        plays++;
        playing = true;
    }
    return true;
}


void buzzer_stop(BuzzerPriority priority)
{
    if (priority == BUZZER_PRIORITY_ALARM)
    {
        stops++;
        playing = false;
    }
}


#define DAY 86400UL
// 2026-10-28T00:00:00 local, as a scalar.
#define MIDNIGHT 1793145600UL

static uint32_t now;

// Seconds of the clock, one epoch each.
static void run(uint32_t seconds)
{
    while (seconds-- > 0U)
    {
        now++;
        alarm_epoch(true, now);
    }
}


static void start_at(uint32_t local)
{
    now = local;
    alarm_epoch(true, now);
}


int main(void)
{
    alarm_init();
    CHECK(alarm_state() == ALARM_OFF && !alarm_is_set());

    // Off: nothing at the time, switch or not.
    alarm_set(false, 7U * 3600U);
    alarm_set_switch(true);
    start_at(MIDNIGHT + 7U * 3600U - 10U);
    run(20);
    CHECK(plays == 0 && alarm_state() == ALARM_OFF);

    // On, switch off: disarmed, silent.
    alarm_set(true, 7U * 3600U);
    alarm_set_switch(false);
    CHECK(alarm_state() == ALARM_DISARMED && !alarm_is_set());
    start_at(MIDNIGHT + 7U * 3600U - 10U);
    run(20);
    CHECK(plays == 0);

    // Armed: sounds at the second, once, and stops after ALARM_SOUND_S.
    alarm_set_switch(true);
    CHECK(alarm_state() == ALARM_ARMED && alarm_is_set());
    start_at(MIDNIGHT + DAY + 7U * 3600U - 5U);
    run(4);
    CHECK(plays == 0);
    run(1); // 07:00:00
    CHECK(plays == 1 && playing && alarm_state() == ALARM_SOUNDING && alarm_is_set());
    run(ALARM_SOUND_S - 1U);
    CHECK(playing);
    run(1);
    CHECK(!playing && alarm_state() == ALARM_ARMED && alarm_stats()->timed_out == 1);
    run(3600);
    CHECK(plays == 1); // once a day

    // The next day: a press stops it, and is spent.
    start_at(MIDNIGHT + 2U * DAY + 7U * 3600U - 1U);
    run(1);
    CHECK(plays == 2 && playing);
    run(10);
    CHECK(alarm_press() && !playing && alarm_state() == ALARM_ARMED);
    CHECK(!alarm_press()); // not sounding: an ordinary press

    // The next: the switch stops it, and disarms.
    start_at(MIDNIGHT + 3U * DAY + 7U * 3600U - 1U);
    run(1);
    CHECK(plays == 3);
    alarm_set_switch(false);
    CHECK(!playing && alarm_state() == ALARM_DISARMED && alarm_stats()->stopped_by_switch == 1);
    alarm_set_switch(true);
    run(5);
    CHECK(plays == 3);

    // Clocks forward from 01:00 to 02:00 with the alarm at 01:30: at the
    // change.
    alarm_set(true, 5400U);
    start_at(MIDNIGHT + 4U * DAY + 3599U);
    now += 3600U; // 01:00:00 is 02:00:00
    run(1);
    CHECK(plays == 4 && playing);
    CHECK(alarm_press());

    // Clocks back from 02:00 to 01:00: 01:30 happens twice, sounds once.
    start_at(MIDNIGHT + 5U * DAY + 5400U - 1U);
    run(1);
    CHECK(plays == 5);
    CHECK(alarm_press());
    run(1800U - 1U); // 01:59:59
    now -= 3600U;    // 02:00:00 is 01:00:00
    run(3600);
    CHECK(plays == 5);

    // A step of the clock (set, not passed) past the alarm: nothing; a step
    // within ALARM_CATCH_UP_S: sounds.
    start_at(MIDNIGHT + 6U * DAY + 5400U - 3U * 3600U);
    now += 3U * 3600U;
    run(1);
    CHECK(plays == 5);
    start_at(MIDNIGHT + 7U * DAY + 5400U - 60U);
    now += 120U;
    run(1);
    CHECK(plays == 6);
    CHECK(alarm_press());

    // Time unknown in between: no catching up.
    start_at(MIDNIGHT + 8U * DAY + 5400U - 60U);
    alarm_epoch(false, 0);
    start_at(MIDNIGHT + 8U * DAY + 5400U + 1U);
    CHECK(plays == 6);

    // A new time may go off the same day; setting off stops it.
    alarm_set(true, 6000U);
    run(600);
    CHECK(plays == 7 && playing);
    alarm_set(false, 6000U);
    CHECK(!playing && alarm_state() == ALARM_OFF && !alarm_is_set());

    // Midnight itself.
    alarm_set(true, 0U);
    start_at(MIDNIGHT + 10U * DAY - 1U);
    run(1);
    CHECK(plays == 8);
    CHECK(alarm_press());

    alarm_print(true);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
