#include "alarm.h"

#include <stdio.h>
#include "../platform/format.h" // our printf (format.h: the library's is slow)

#include "../drivers/buzzer.h"

#define SECONDS_PER_DAY ((uint32_t)86400U)

static bool enabled;
static uint32_t time_of_day;
static bool switch_on;
static bool sounding;
static uint16_t sounding_epochs;
static bool have_last;
static uint32_t last_local; // the local second of the previous epoch
static bool fired;          // ... on fired_day, a local day number
static uint32_t fired_day;
static AlarmStats stats;

static void stop(uint32_t *counter)
{
    if (sounding)
    {
        sounding = false;
        buzzer_stop(BUZZER_PRIORITY_ALARM);
        (*counter)++;
    }
}


void alarm_init(void)
{
    enabled = false;
    time_of_day = 0;
    switch_on = false;
    sounding = false;
    sounding_epochs = 0;
    have_last = false;
    fired = false;
    stats.sounded = 0;
    stats.stopped_by_button = 0;
    stats.stopped_by_switch = 0;
    stats.timed_out = 0;
}


void alarm_set(bool on, uint32_t time_of_day_s)
{
    if (time_of_day_s >= SECONDS_PER_DAY)
    {
        return;
    }
    if (time_of_day_s != time_of_day)
    {
        fired = false; // a new time may go off today
    }
    enabled = on;
    time_of_day = time_of_day_s;
    if (!enabled)
    {
        stop(&stats.stopped_by_switch);
    }
}


void alarm_set_switch(bool on)
{
    switch_on = on;
    if (!on)
    {
        stop(&stats.stopped_by_switch);
    }
}


bool alarm_press(void)
{
    if (!sounding)
    {
        return false;
    }
    stop(&stats.stopped_by_button);
    return true;
}


// The alarm's moment passed between the previous second and this one: the
// latest alarm moment at or before now is after the previous second, on a
// day it has not yet gone off. Not if the clock went back, or jumped.
static bool reached(uint32_t local, uint32_t *moment_day)
{
    uint32_t moment;

    if (!have_last || local <= last_local || local - last_local > ALARM_CATCH_UP_S)
    {
        return false;
    }
    moment = local - local % SECONDS_PER_DAY + time_of_day;
    if (moment > local)
    {
        if (moment < SECONDS_PER_DAY)
        {
            return false;
        }
        moment -= SECONDS_PER_DAY;
    }
    *moment_day = moment / SECONDS_PER_DAY;
    return moment > last_local && !(fired && fired_day == *moment_day);
}


void alarm_epoch(bool known, uint32_t local_seconds)
{
    uint32_t day;

    if (sounding && ++sounding_epochs >= ALARM_SOUND_S)
    {
        stop(&stats.timed_out);
    }
    if (!known)
    {
        have_last = false;
        return;
    }
    if (enabled && switch_on && !sounding && reached(local_seconds, &day))
    {
        sounding = buzzer_play(&buzzer_alarm, buzzer_alarm.group, BUZZER_PRIORITY_ALARM);
        sounding_epochs = 0;
        fired = true;
        fired_day = day;
        stats.sounded++;
    }
    last_local = local_seconds;
    have_last = true;
}


AlarmState alarm_state(void)
{
    if (!enabled)
    {
        return ALARM_OFF;
    }
    if (sounding)
    {
        return ALARM_SOUNDING;
    }
    return switch_on ? ALARM_ARMED : ALARM_DISARMED;
}


bool alarm_enabled(void)
{
    return enabled;
}


uint32_t alarm_time(void)
{
    return time_of_day;
}


bool alarm_is_set(void)
{
    AlarmState state = alarm_state();
    return state == ALARM_ARMED || state == ALARM_SOUNDING;
}


const char *alarm_state_name(AlarmState state)
{
    static const char *const names[ALARM_STATE_COUNT] = {"OFF", "DISARMED", "ARMED", "SOUNDING"};
    return state < ALARM_STATE_COUNT ? names[state] : "?";
}


const AlarmStats *alarm_stats(void)
{
    return &stats;
}


void alarm_print(bool verbose)
{
    printf("Alarm: %02lu:%02lu:%02lu, %s", (unsigned long)(time_of_day / 3600U),
           (unsigned long)(time_of_day / 60U % 60U), (unsigned long)(time_of_day % 60U),
           alarm_state_name(alarm_state()));
    if (verbose)
    {
        printf("; sounded %lu (stopped: %lu button, %lu switch, %lu timed out)",
               (unsigned long)stats.sounded, (unsigned long)stats.stopped_by_button,
               (unsigned long)stats.stopped_by_switch, (unsigned long)stats.timed_out);
    }
    printf("\r\n");
}
