/*
 * Local time (ui/timezone.c): applying SML-TZ-STATE schedules, moving through
 * their entries, the stale horizon, manual mode and the persistence CRC.
 */
#include <stdio.h>
#include <string.h>

#include "../../ui/timezone.h"

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

static const uint32_t london_next[8] = {1792890000UL, 1806195600UL, 1824944400UL, 1837645200UL,
                                        1856394000UL, 1869094800UL, 1887843600UL, 1901149200UL};
#define LONDON_HORIZON 1919293200UL

static SML_TZ_STATE_PAYLOAD london(uint32_t issued)
{
    SML_TZ_STATE_PAYLOAD state;
    memset(&state, 0, sizeof state);
    state.flags = SML_TZ_VALID;
    state.source = SML_TZ_SOURCE_GNSS;
    state.count = 8;
    state.issued_utc = issued;
    state.horizon_end_utc = LONDON_HORIZON;
    state.current.utc_seconds = 1774746000UL;
    state.current.dst_offset_s = 3600;
    state.current.flags = SML_TZ_ENTRY_DST_ACTIVE;
    for (int i = 0; i < 8; i++)
    {
        state.next[i].utc_seconds = london_next[i];
        state.next[i].dst_offset_s = 3600;
        state.next[i].flags = (uint8_t)(i % 2 ? SML_TZ_ENTRY_DST_ACTIVE : 0);
    }
    return state;
}


static int32_t offset_at(uint32_t utc)
{
    TimezoneOffset offset;
    timezone_offset_at(utc, &offset);
    return offset.offset_s;
}


static void test_london(void)
{
    SML_TZ_STATE_PAYLOAD state = london(1791911413UL);
    TimezoneOffset offset;

    timezone_init();
    timezone_offset_at(1791911413UL, &offset);
    CHECK(offset.offset_s == 0 && !offset.base_automatic && !offset.dst_automatic); // UTC

    CHECK(timezone_apply(&state) == TIMEZONE_APPLIED);
    CHECK(timezone_take_changed() && !timezone_take_changed());
    timezone_offset_at(1791911413UL, &offset);
    CHECK(offset.offset_s == 3600 && offset.dst_active && offset.base_automatic &&
          offset.dst_automatic && !offset.stale);
    // Every transition, to the second.
    for (int i = 0; i < 8; i++)
    {
        int32_t after = i % 2 ? 3600 : 0;
        CHECK(offset_at(london_next[i] - 1U) == 3600 - after);
        CHECK(offset_at(london_next[i]) == after);
    }
    // Past the horizon: the last entry is held, flagged stale.
    timezone_offset_at(LONDON_HORIZON - 1U, &offset);
    CHECK(offset.offset_s == 3600 && !offset.stale);
    timezone_offset_at(LONDON_HORIZON, &offset);
    CHECK(offset.offset_s == 3600 && offset.stale);
    // Before the current entry (should not happen): the current entry.
    CHECK(offset_at(1000000000UL) == 3600);
}


static void test_rules(void)
{
    SML_TZ_STATE_PAYLOAD state = london(2000);
    SML_TZ_STATE_PAYLOAD other;

    timezone_init();
    CHECK(timezone_apply(&state) == TIMEZONE_APPLIED);
    (void)timezone_take_changed();

    // A resend: same contents, newer issue time. Applied, not a change.
    state.issued_utc = 2600;
    CHECK(timezone_apply(&state) == TIMEZONE_UNCHANGED && !timezone_take_changed());
    CHECK(timezone_schedule()->issued_utc == 2600);
    // Same issue time is not older.
    CHECK(timezone_apply(&state) == TIMEZONE_UNCHANGED);
    // Older, same source: ignored.
    other = london(1000);
    other.next[0].utc_seconds += 3600U;
    CHECK(timezone_apply(&other) == TIMEZONE_OLDER);
    // Older, but from another source: applied.
    other.source = SML_TZ_SOURCE_PINNED;
    CHECK(timezone_apply(&other) == TIMEZONE_APPLIED && timezone_take_changed());
    CHECK(timezone_schedule()->source == SML_TZ_SOURCE_PINNED);
    // VALID clear: ignored, the schedule held is kept.
    state.flags = 0;
    state.issued_utc = 9999;
    CHECK(timezone_apply(&state) == TIMEZONE_NOT_VALID);
    CHECK(timezone_schedule()->source == SML_TZ_SOURCE_PINNED);

    // Malformed: out of order, too many, offsets beyond a day, horizon
    // before the last entry, sub-second offset of a second or more.
    state = london(3000);
    state.next[3].utc_seconds = state.next[2].utc_seconds;
    CHECK(timezone_apply(&state) == TIMEZONE_MALFORMED);
    state = london(3000);
    state.count = 9;
    CHECK(timezone_apply(&state) == TIMEZONE_MALFORMED);
    state = london(3000);
    state.next[5].base_offset_s = 90000;
    CHECK(timezone_apply(&state) == TIMEZONE_MALFORMED);
    state = london(3000);
    state.horizon_end_utc = london_next[7];
    CHECK(timezone_apply(&state) == TIMEZONE_MALFORMED);
    state = london(3000);
    state.subsecond_offset_ns = 1000000000L;
    CHECK(timezone_apply(&state) == TIMEZONE_MALFORMED);
    CHECK(timezone_stats()->results[TIMEZONE_MALFORMED] == 5);

    // TZ auto and DST auto choose independently; the schedule is always held.
    timezone_set_auto(false);
    timezone_set_manual(-18000, 3600, true);
    state = london(4000);
    CHECK(timezone_apply(&state) == TIMEZONE_APPLIED);
    // Manual base, DST from the schedule (BST then GMT).
    CHECK(offset_at(london_next[0] - 1U) == -14400);
    CHECK(offset_at(london_next[0]) == -18000);
    // Both manual.
    timezone_set_dst_auto(false);
    CHECK(offset_at(london_next[0]) == -14400);
    timezone_set_manual(-18000, 3600, false);
    CHECK(offset_at(london_next[0]) == -18000);
    // Base from the schedule, DST manual.
    timezone_set_auto(true);
    CHECK(offset_at(london_next[0] - 1U) == 0);
    timezone_set_manual(-18000, 1800, true);
    CHECK(offset_at(london_next[0]) == 1800);
    // Both from the schedule again.
    timezone_set_dst_auto(true);
    CHECK(offset_at(london_next[0] + 7200U) == 0 && offset_at(london_next[1]) == 3600);
    // No schedule: auto settings fall back to the manual values.
    timezone_init();
    timezone_set_manual(-18000, 3600, true);
    {
        TimezoneOffset offset;
        timezone_offset_at(london_next[0], &offset);
        CHECK(offset.offset_s == -14400 && !offset.base_automatic && !offset.dst_automatic);
    }
}


static void test_special(void)
{
    SML_TZ_STATE_PAYLOAD state;
    TimezoneOffset offset;

    timezone_init();
    // Dublin: standard time is summer (+1 h base), winter "DST" is -1 h.
    memset(&state, 0, sizeof state);
    state.flags = SML_TZ_VALID | SML_TZ_AMBIGUOUS;
    state.source = SML_TZ_SOURCE_GNSS;
    state.count = 1;
    state.issued_utc = 1;
    state.horizon_end_utc = 0xFFFFFFFFUL;
    state.current.base_offset_s = 3600;
    state.current.dst_offset_s = -3600;
    state.current.flags = SML_TZ_ENTRY_DST_ACTIVE;
    state.next[0].utc_seconds = 1806195600UL;
    state.next[0].base_offset_s = 3600;
    state.next[0].dst_offset_s = -3600;
    CHECK(timezone_apply(&state) == TIMEZONE_APPLIED);
    timezone_offset_at(1806195599UL, &offset);
    CHECK(offset.offset_s == 0 && offset.dst_active && offset.dst_offset_s == -3600);
    CHECK(offset.ambiguous);
    timezone_offset_at(0xFFFFFFF0UL, &offset);
    CHECK(offset.offset_s == 3600 && !offset.stale); // open-ended: never stale

    // Mecca time: +02:39:18.3, rounded to the second.
    memset(&state, 0, sizeof state);
    state.flags = SML_TZ_VALID;
    state.source = SML_TZ_SOURCE_SPECIAL;
    state.issued_utc = 2;
    state.horizon_end_utc = 0xFFFFFFFFUL;
    state.subsecond_offset_ns = 300000000L;
    state.current.base_offset_s = 9558;
    CHECK(timezone_apply(&state) == TIMEZONE_APPLIED);
    CHECK(offset_at(1800000000UL) == 9558);
    state.subsecond_offset_ns = 700000000L;
    state.issued_utc = 3;
    CHECK(timezone_apply(&state) == TIMEZONE_APPLIED);
    CHECK(offset_at(1800000000UL) == 9559);
}


int main(void)
{
    test_london();
    test_rules();
    test_special();
    timezone_print(1792890000UL, false);
    timezone_print(1792890000UL, true);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
