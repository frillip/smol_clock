/*
 * sync_set_time (the menu's tIME) against the real sync and epoch control,
 * with no source giving the time: refused unless free running, taken at the
 * next sync_update, the epoch moved to the second boundary of the moment
 * given (later by the slot, or earlier by the rest of the second past the
 * half), and the epochs labelled from that moment on.
 */
#include <stdio.h>
#include <string.h>

#include "../../sync/source_gnss.h"
#include "../../sync/source_ntp.h"
#include "../../sync/sync.h"
#include "../../timing/epoch_timer.h"

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

static uint32_t requested_period;
static int requests;

void epoch_timer_set_period(uint32_t period_ticks)
{
    requested_period = period_ticks;
    requests++;
}


// No source has the time.
static void nothing(const TimingSnapshot *snapshot, uint32_t now_ms, SyncSourceReport *report)
{
    (void)snapshot;
    (void)now_ms;
    memset(report, 0, sizeof(*report));
}


void source_gnss_report(const TimingSnapshot *snapshot, uint32_t now_ms, SyncSourceReport *report)
{
    nothing(snapshot, now_ms, report);
}


void source_ntp_report(const TimingSnapshot *snapshot, uint32_t now_ms, SyncSourceReport *report)
{
    nothing(snapshot, now_ms, report);
}


void source_rtc_report(const TimingSnapshot *snapshot, uint32_t now_ms, SyncSourceReport *report)
{
    nothing(snapshot, now_ms, report);
}


void source_gnss_select(void)
{
}


void source_ntp_select(void)
{
}


static uint32_t sequence;

// One epoch: the period it asked for, if any, beyond nominal.
static int32_t epoch(void)
{
    TimingSnapshot snap;
    memset(&snap, 0, sizeof snap);
    snap.timer.sequence = ++sequence;
    snap.timer.running_period = TIMING_NOMINAL_TICKS;
    requests = 0;
    requested_period = TIMING_NOMINAL_TICKS;
    (void)sync_update(&snap, sequence * 1000U);
    return (int32_t)(requested_period - TIMING_NOMINAL_TICKS);
}


#define MOMENT 1825102800UL // 2027-11-01T21:00:00Z

int main(void)
{
    const SyncStatus *status = sync_status();
    int32_t step;

    sync_init();
    for (int i = 0; i < 5; i++)
        (void)epoch();
    CHECK(status->state == SYNC_FREE_RUN && !status->time.known);

    printf("early in the second: the epochs later by the slot\n");
    CHECK(!sync_set_time(MOMENT, sequence, 1000U));
    CHECK(sync_set_time(MOMENT, sequence, 200U)); // pressed 200 ms after this epoch
    CHECK(!status->time.known);                   // not until the next update
    step = epoch();
    CHECK(status->time.known && status->time.source == SYNC_SOURCE_NONE && status->aligned);
    CHECK(status->time.utc_seconds == MOMENT + 1U); // the next second boundary
    CHECK(step == (int32_t)(200U * TIMING_TICKS_PER_MS));
    (void)epoch();
    CHECK(status->time.utc_seconds == MOMENT + 2U && status->time.corrections == 0U);

    printf("late in the second, a second ago: earlier, and counted on\n");
    CHECK(sync_set_time(MOMENT + 60U, sequence - 1U, 700U));
    step = epoch();
    // Pressed 1.3 s before this epoch: it is moved 300 ms earlier, to 1 s on.
    CHECK(status->time.utc_seconds == MOMENT + 61U && status->time.corrections == 1U);
    CHECK(step == -(int32_t)(300U * TIMING_TICKS_PER_MS));
    step = epoch(); // once
    CHECK(step == 0 && status->time.utc_seconds == MOMENT + 62U);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
