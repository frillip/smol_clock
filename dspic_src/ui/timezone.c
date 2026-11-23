#include "timezone.h"

#include <stddef.h>
#include <stdio.h>
#include "../platform/format.h" // our printf (format.h: the library's is slow)
#include <string.h>

#include "../../common/sml.h"
#include "../platform/utc.h"

#define OPEN_ENDED_UTC 0xFFFFFFFFUL
#define NS_PER_SECOND  1000000000L

static bool automatic;
static bool dst_automatic;
static int32_t manual_base_s;
static int32_t manual_dst_s;
static bool manual_dst_active;
static TimezoneSchedule schedule;
static bool changed;
static TimezoneStats stats;

void timezone_init(void)
{
    automatic = true;
    dst_automatic = true;
    manual_base_s = 0;
    manual_dst_s = 0;
    manual_dst_active = false;
    memset(&schedule, 0, sizeof(schedule));
    changed = false;
    memset(&stats, 0, sizeof(stats));
}


void timezone_set_auto(bool on)
{
    automatic = on;
}


bool timezone_is_auto(void)
{
    return automatic;
}


void timezone_set_dst_auto(bool on)
{
    dst_automatic = on;
}


bool timezone_is_dst_auto(void)
{
    return dst_automatic;
}


void timezone_set_manual(int32_t base_offset_s, int32_t dst_offset_s, bool dst_active)
{
    manual_base_s = base_offset_s;
    manual_dst_s = dst_offset_s;
    manual_dst_active = dst_active;
}


void timezone_manual(TimezoneOffset *offset)
{
    memset(offset, 0, sizeof(*offset));
    offset->base_offset_s = manual_base_s;
    offset->dst_offset_s = manual_dst_s;
    offset->dst_active = manual_dst_active;
    offset->offset_s = manual_base_s + (manual_dst_active ? manual_dst_s : 0);
}


static bool offset_ok(int32_t offset_s)
{
    return offset_s >= -TIMEZONE_OFFSET_LIMIT_S && offset_s <= TIMEZONE_OFFSET_LIMIT_S;
}


static void copy_entry(const SML_TZ_ENTRY *wire, TimezoneEntry *entry)
{
    entry->utc_seconds = wire->utc_seconds;
    entry->base_offset_s = wire->base_offset_s;
    entry->dst_offset_s = wire->dst_offset_s;
    entry->dst_active = (wire->flags & SML_TZ_ENTRY_DST_ACTIVE) != 0U;
}


// Count in range, future entries in ascending order after the current one,
// offsets that are time zones, and a horizon not before the last entry.
static bool well_formed(const SML_TZ_STATE_PAYLOAD *state)
{
    uint32_t previous = state->current.utc_seconds;
    uint8_t index;

    if (state->count > SML_TZ_TRANSITION_MAX || !offset_ok(state->current.base_offset_s) ||
        state->subsecond_offset_ns <= -NS_PER_SECOND || state->subsecond_offset_ns >= NS_PER_SECOND)
    {
        return false;
    }
    for (index = 0; index < state->count; index++)
    {
        const SML_TZ_ENTRY *entry = &state->next[index];
        if (entry->utc_seconds <= previous || !offset_ok(entry->base_offset_s))
        {
            return false;
        }
        previous = entry->utc_seconds;
    }
    return state->horizon_end_utc > previous;
}


// The schedule's identity for persistence: the wire payload with issued_utc
// left out, so a periodic resend of the same schedule is not a change.
static uint16_t schedule_crc(const SML_TZ_STATE_PAYLOAD *state)
{
    const SmlMessage *message = sml_message(SML_CLASS_TZ, SML_TZ_STATE);
    uint8_t wire[SML_TZ_STATE_BYTES];
    uint16_t crc;

    (void)sml_encode(message->layout, state, wire);
    crc = sml_crc(SML_CRC_INIT, wire, 4U);
    return sml_crc(crc, &wire[8], (uint16_t)(SML_TZ_STATE_BYTES - 8U));
}


TimezoneResult timezone_apply(const SML_TZ_STATE_PAYLOAD *state)
{
    TimezoneResult result;
    uint16_t crc;
    uint8_t index;

    if ((state->flags & SML_TZ_VALID) == 0U)
    {
        result = TIMEZONE_NOT_VALID;
    }
    else if (!well_formed(state))
    {
        result = TIMEZONE_MALFORMED;
    }
    else if (schedule.held && state->source == schedule.source &&
             state->issued_utc < schedule.issued_utc)
    {
        result = TIMEZONE_OLDER;
    }
    else
    {
        crc = schedule_crc(state);
        result = schedule.held && crc == schedule.crc ? TIMEZONE_UNCHANGED : TIMEZONE_APPLIED;

        // Replaced as a whole: nothing reads it in between (foreground only).
        schedule.held = true;
        schedule.source = state->source;
        schedule.ambiguous = (state->flags & SML_TZ_AMBIGUOUS) != 0U;
        schedule.issued_utc = state->issued_utc;
        schedule.horizon_end_utc = state->horizon_end_utc;
        schedule.subsecond_offset_ns = state->subsecond_offset_ns;
        schedule.count = (uint8_t)(state->count + 1U);
        copy_entry(&state->current, &schedule.entries[0]);
        for (index = 0; index < state->count; index++)
        {
            copy_entry(&state->next[index], &schedule.entries[index + 1U]);
        }
        schedule.crc = crc;
        changed = changed || result == TIMEZONE_APPLIED;
    }
    stats.results[result]++;
    return result;
}


// Nearest whole second of a sub-second offset.
static int32_t rounded_seconds(int32_t nanoseconds)
{
    if (nanoseconds >= NS_PER_SECOND / 2)
    {
        return 1;
    }
    return nanoseconds < -(NS_PER_SECOND / 2) ? -1 : 0;
}


void timezone_offset_at(uint32_t utc_seconds, TimezoneOffset *offset)
{
    const TimezoneEntry *entry = &schedule.entries[0];
    uint8_t index;

    memset(offset, 0, sizeof(*offset));
    offset->base_offset_s = manual_base_s;
    offset->dst_offset_s = manual_dst_s;
    offset->dst_active = manual_dst_active;
    if (schedule.held)
    {
        for (index = 1; index < schedule.count; index++)
        {
            if (utc_seconds < schedule.entries[index].utc_seconds)
            {
                break;
            }
            entry = &schedule.entries[index];
        }
        offset->base_automatic = automatic;
        offset->dst_automatic = dst_automatic;
    }
    if (offset->base_automatic)
    {
        offset->base_offset_s = entry->base_offset_s;
        offset->offset_s = rounded_seconds(schedule.subsecond_offset_ns);
    }
    if (offset->dst_automatic)
    {
        offset->dst_offset_s = entry->dst_offset_s;
        offset->dst_active = entry->dst_active;
    }
    if (offset->base_automatic || offset->dst_automatic)
    {
        offset->stale =
            schedule.horizon_end_utc != OPEN_ENDED_UTC && utc_seconds >= schedule.horizon_end_utc;
        offset->ambiguous = schedule.ambiguous;
    }
    offset->offset_s += offset->base_offset_s + (offset->dst_active ? offset->dst_offset_s : 0);
}


const TimezoneSchedule *timezone_schedule(void)
{
    return &schedule;
}


bool timezone_state(SML_TZ_STATE_PAYLOAD *state)
{
    uint8_t index;

    memset(state, 0, sizeof(*state));
    if (!schedule.held)
    {
        return false;
    }
    state->flags = (uint16_t)(SML_TZ_VALID | (schedule.ambiguous ? SML_TZ_AMBIGUOUS : 0U));
    state->source = schedule.source;
    state->count = (uint8_t)(schedule.count - 1U);
    state->issued_utc = schedule.issued_utc;
    state->horizon_end_utc = schedule.horizon_end_utc;
    state->subsecond_offset_ns = schedule.subsecond_offset_ns;
    for (index = 0; index < schedule.count; index++)
    {
        const TimezoneEntry *entry = &schedule.entries[index];
        SML_TZ_ENTRY *wire = index == 0U ? &state->current : &state->next[index - 1U];

        wire->utc_seconds = entry->utc_seconds;
        wire->base_offset_s = entry->base_offset_s;
        wire->dst_offset_s = entry->dst_offset_s;
        wire->flags = entry->dst_active ? SML_TZ_ENTRY_DST_ACTIVE : 0U;
    }
    return true;
}


bool timezone_take_changed(void)
{
    bool was = changed;
    changed = false;
    return was;
}


const TimezoneStats *timezone_stats(void)
{
    return &stats;
}


const char *timezone_result_name(TimezoneResult result)
{
    static const char *const names[TIMEZONE_RESULT_COUNT] = {"applied", "unchanged", "not valid",
                                                             "older", "malformed"};
    return result < TIMEZONE_RESULT_COUNT ? names[result] : "?";
}


const char *timezone_source_name(uint8_t source)
{
    static const char *const names[] = {"NONE",   "CACHE",  "GEOIP",  "GNSS",
                                        "MANUAL", "PINNED", "SPECIAL"};

    return source < sizeof(names) / sizeof(names[0]) ? names[source] : "?";
}


// AUTO from the schedule, MANUAL, or AUTO falling back to manual (none held).
static const char *origin(bool from_schedule, bool automatic_setting)
{
    return from_schedule ? "AUTO" : automatic_setting ? "AUTO (none held: manual)" : "MANUAL";
}


static void print_entry_offset(const TimezoneEntry *entry)
{
    printf("UTC");
    utc_print_offset(entry->base_offset_s + (entry->dst_active ? entry->dst_offset_s : 0));
    if (entry->dst_active)
    {
        printf(" DST");
    }
}


void timezone_print(uint32_t utc_seconds, bool verbose)
{
    TimezoneOffset offset;
    uint8_t index;
    uint8_t next;

    timezone_offset_at(utc_seconds, &offset);
    printf("Time zone: UTC");
    utc_print_offset(offset.offset_s);
    printf("; base ");
    utc_print_offset(offset.base_offset_s);
    printf(" %s; DST ", origin(offset.base_automatic, automatic));
    utc_print_offset(offset.dst_offset_s);
    printf(" %s %s%s%s\r\n", offset.dst_active ? "ON" : "OFF",
           origin(offset.dst_automatic, dst_automatic), offset.stale ? ", STALE" : "",
           offset.ambiguous ? ", AMBIGUOUS" : "");
    if (!schedule.held)
    {
        printf("  Schedule: none held\r\n");
        return;
    }
    printf("  Schedule: %s, issued ", timezone_source_name(schedule.source));
    utc_print(schedule.issued_utc);
    if (schedule.horizon_end_utc != UTC_NEVER)
    {
        printf(", until ");
        utc_print(schedule.horizon_end_utc);
    }
    for (next = 0; next < schedule.count; next++)
    {
        if (schedule.entries[next].utc_seconds > utc_seconds)
        {
            break;
        }
    }
    if (next < schedule.count)
    {
        printf(", next ");
        utc_print(schedule.entries[next].utc_seconds);
        printf(" ");
        print_entry_offset(&schedule.entries[next]);
    }
    if (schedule.subsecond_offset_ns != 0)
    {
        printf(", %+ld ms (not shown)", (long)(schedule.subsecond_offset_ns / 1000000L));
    }
    printf("\r\n");
    if (!verbose)
    {
        return;
    }
    for (index = 0; index < schedule.count; index++)
    {
        const TimezoneEntry *entry = &schedule.entries[index];
        bool in_force = utc_seconds >= entry->utc_seconds &&
                        (index + 1U == schedule.count ||
                         utc_seconds < schedule.entries[index + 1U].utc_seconds);

        printf("  %c ", in_force ? '>' : ' ');
        if (entry->utc_seconds == 0U)
        {
            printf("%-20s", "(before the data)");
        }
        else
        {
            utc_print(entry->utc_seconds);
        }
        printf("  ");
        print_entry_offset(entry);
        printf("\r\n");
    }
    for (index = 0; index < TIMEZONE_RESULT_COUNT; index++)
    {
        printf("%s%s %lu", index == 0U ? "  Received: " : ", ",
               timezone_result_name((TimezoneResult)index), (unsigned long)stats.results[index]);
    }
    printf("\r\n");
}
