/*
 * Closed-loop host test of sync/: source judging, the state machine and the
 * time of day, driving the real epoch control loop against the same model
 * of the hardware as test_acquire.c (a crystal 1.9 ppm slow, the reference
 * edge captured to the tick below, the epoch edge exactly, a requested
 * period loaded at the next epoch).
 *
 * The GNSS source is scripted: its adapter is replaced by one that reports
 * what the script says the receiver is doing each epoch. The script runs
 * through a cold start, an unlocked receiver whose PPS is off, poor
 * accuracy, a time jump, a leap second during holdover, and a holdover that
 * outlasts its limit (built with a synchronised limit of 4 us, about 95 s
 * of holdover after GNSS). NTP never appears: see test_sources.c.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../console/print_queue.h"
#include "../../sync/source_gnss.h"
#include "../../sync/source_ntp.h"
#include "../../sync/sync.h"
#include "../../sync/sync_report.h"
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

// What the synchroniser's report prints, in one go or queued a part per step
// (console/print_queue.h): the same text.
#define SNAPSHOT_EPOCH 300 // locked on GNSS, with history

static char captured[3][16384];
static int saved_stdout;

// stdout into file, until restore_stdout().
static void into(FILE *file)
{
    fflush(stdout);
    saved_stdout = dup(1);
    dup2(fileno(file), 1);
}


static void restore_stdout(void)
{
    fflush(stdout);
    dup2(saved_stdout, 1);
    close(saved_stdout);
}


static size_t read_back(FILE *file, int which)
{
    size_t length;

    rewind(file);
    length = fread(captured[which], 1, sizeof(captured[which]) - 1U, file);
    captured[which][length] = '\0';
    fclose(file);
    return length;
}


static size_t capture(int which, bool queued, bool verbose)
{
    FILE *file = tmpfile();

    into(file);
    if (queued)
    {
        print_queue_reset();
        (void)sync_report_queue(verbose);
        while (print_queue_service(0, true))
        {
        }
    }
    else
    {
        sync_report_status(verbose);
    }
    restore_stdout();
    return read_back(file, which);
}


// ---- Epoch timer stand-in ----------------------------------------------
static uint32_t requested_period;
static bool request_pending;

void epoch_timer_set_period(uint32_t period_ticks)
{
    requested_period = period_ticks;
    request_pending = true;
}


// ---- The script ------------------------------------------------------------
#define TRUE_SECOND 39999923.75
#define EPOCHS      1000L
#define MIDNIGHT    1798761600LL       // 2027-01-01T00:00:00Z
#define UTC_ORIGIN  (MIDNIGHT - 612LL) // label of reference edge 0

// What the receiver does at reference edge i (by our epoch k).
typedef struct
{
    bool present;
    bool time_valid;
    bool pps;             // outputs its PPS
    double pps_offset;    // ticks, while unlocked
    uint32_t uncertainty; // tAcc, ns
    bool announces_leap;
} Receiver;

// LEAP_FROM_LINK: the receiver never announces the leap second; the ESP's
// TIME-LEAP does (sync_set_leap), and the result must be the same.
#ifndef LEAP_FROM_LINK
#define LEAP_FROM_LINK 0
#endif

static int64_t leap_event; // set in main()

static Receiver receiver_at(long k)
{
    Receiver r = {true, true, true, 0.0, 30U, false};

    if (k < 10)
    {
        r.present = false;
        r.time_valid = false;
        r.pps = false;
    }
    else if (k < 20)
    {
        r.time_valid = false; // cold: answering, no fix, no PPS
        r.pps = false;
    }
    else if (k >= 20 && k < 25)
    {
        r.uncertainty = 50000U; // first fix: too coarse to qualify, taken as a fallback
    }
    else if (k >= 25 && k < 40)
    {
        r.uncertainty = 1014U; // settling: coarse, but good enough to lock
    }
    else if (k >= 300 && k < 360)
    {
        r.time_valid = false; // lost lock: PPS free-running 2 us off
        r.pps_offset = 80.0;
    }
    else if (k >= 450 && k < 455)
    {
        r.uncertainty = 200000U; // poor solution: beyond even the hold limit
    }
    else if (k >= 470 && k < 480)
    {
        r.uncertainty = 50000U; // coarse, but within the hold limit: kept
    }
    else if (k >= 600 && k < 620)
    {
        r.present = false; // antenna unplugged across the leap second
        r.time_valid = false;
        r.pps = false;
    }
    else if (k >= 700 && k < 820)
    {
        r.present = false; // longer than the holdover limit
        r.time_valid = false;
        r.pps = false;
    }
    r.announces_leap = !LEAP_FROM_LINK && r.present && k >= 560 && k < 700;
    return r;
}


// The receiver's label for reference edge i: from edge 500 on it has
// corrected its time by a second, and a leap second is inserted before
// leap_event (edge 611 is 23:59:60).
static int64_t label_of(long i, bool *leap)
{
    int64_t base = UTC_ORIGIN + i + (i >= 500 ? 1 : 0);
    *leap = base == leap_event;
    return base >= leap_event ? base - 1 : base;
}


// ---- The scripted adapters -----------------------------------------------
static Receiver now_receiver;
static long now_edge;
static int gnss_selections, ntp_selections;

void source_gnss_report(const TimingSnapshot *snapshot, uint32_t now_ms, SyncSourceReport *report)
{
    (void)now_ms;
    memset(report, 0, sizeof(*report));
    report->present = now_receiver.present;
    report->time_valid = now_receiver.time_valid;
    report->uncertainty_ns = now_receiver.time_valid ? now_receiver.uncertainty : 0U;
    report->edge = snapshot->capture.reference_capture_valid;
    report->captured = report->edge;
    report->edge_stamp = snapshot->capture.reference_capture;
    report->edge_sequence = snapshot->capture.reference_edge_sequence;
    report->labelled = report->edge && now_receiver.time_valid;
    if (report->labelled)
        report->utc_seconds = label_of(now_edge, &report->leap_second);
    if (now_receiver.announces_leap)
    {
        report->leap.event_utc = leap_event;
        report->leap.change = 1;
    }
}


void source_gnss_select(void)
{
    gnss_selections++;
}


// No ESP here: NTP is never present.
void source_ntp_report(const TimingSnapshot *snapshot, uint32_t now_ms, SyncSourceReport *report)
{
    (void)snapshot;
    (void)now_ms;
    memset(report, 0, sizeof(*report));
}


void source_ntp_select(void)
{
    ntp_selections++;
}


// No RTC either: see test_sources.c.
void source_rtc_report(const TimingSnapshot *snapshot, uint32_t now_ms, SyncSourceReport *report)
{
    (void)snapshot;
    (void)now_ms;
    memset(report, 0, sizeof(*report));
}


// ---- The run -------------------------------------------------------------
typedef struct
{
    double worst_locked;   // largest |true error| while LOCKED, after settling
    double worst_unlocked; // ... during the unlocked-PPS spell
    double worst_holdover;
    long wrong_labels; // LOCKED/HOLDOVER epochs with the wrong UTC second
    long steered_while_unusable;
    long first_locked;
    bool leap_seen;
    long bounded_locked; // LOCKED epochs with an uncertainty bound covering the error
    long locked;
    long bounded_free_run;   // FREE_RUN epochs claiming a bound: must be none
    uint32_t holdover_bound; // largest bound given in holdover
} Result;

int main(void)
{
    const double pps_origin = 23754758.3;
    double epoch = 40167998.0;
    uint32_t running = TIMING_NOMINAL_TICKS;
    uint32_t sequence = 0;
    uint32_t edges = 0;
    int64_t last_cap = 0;
    long last_index = -100;
    Result r;
    SyncState expected_order[] = {SYNC_ACQUIRE,  SYNC_LOCKED, SYNC_HOLDOVER, SYNC_LOCKED,
                                  SYNC_HOLDOVER, SYNC_LOCKED, SYNC_HOLDOVER, SYNC_LOCKED,
                                  SYNC_HOLDOVER, SYNC_LOCKED, SYNC_HOLDOVER, SYNC_FREE_RUN,
                                  SYNC_ACQUIRE,  SYNC_LOCKED};
    SyncState seen[32];
    int transitions = 0;
    const SyncStatus *status;
    FILE *parts_file = NULL;

    leap_event = MIDNIGHT;
    memset(&r, 0, sizeof r);
    r.first_locked = -1;
    sync_init();

    for (long k = 0; k < EPOCHS; k++)
    {
        TimingSnapshot snap;
        SyncUpdate update;
        long pps_index, nearest;
        double reference, edge_time, error;
        int64_t cap;
        bool truth_leap;
        int64_t truth;

        if (request_pending)
        {
            running = requested_period;
            request_pending = false;
        }
        sequence++;

        // The reference edge in this epoch's capture window, and the one
        // nearest the epoch (whose UTC second the epoch should carry).
        pps_index = (long)floor((epoch + 40000.0 - pps_origin) / TRUE_SECOND);
        nearest = lround((epoch - pps_origin) / TRUE_SECOND);
        reference = pps_origin + pps_index * TRUE_SECOND;
        error = epoch - (pps_origin + nearest * TRUE_SECOND);
        now_receiver = receiver_at(k);
        now_edge = pps_index;
        edge_time = reference + (now_receiver.time_valid ? 0.0 : now_receiver.pps_offset);
        cap = (int64_t)floor(edge_time);

        memset(&snap, 0, sizeof snap);
        snap.timer.sequence = sequence;
        snap.timer.running_period = running;
        snap.capture.reference_capture_valid = now_receiver.pps;
        if (now_receiver.pps)
        {
            edges++;
            snap.capture.reference_capture = (uint32_t)cap;
            snap.capture.reference_edge_sequence = edges;
            snap.capture.reference_period_valid = pps_index == last_index + 1;
            snap.capture.reference_period = (uint32_t)(cap - last_cap);
            snap.capture.reference_period_sequence = edges;
            last_cap = cap;
            last_index = pps_index;
        }
        else
        {
            last_index = -100;
        }
        snap.capture.epoch_capture = (uint32_t)(int64_t)epoch;
        snap.capture.epoch_capture_valid = true;

        if (LEAP_FROM_LINK && k == 560)
        {
            UtcLeap announced = {(uint32_t)leap_event, 1};
            sync_set_leap(&announced);
        }
        update = sync_update(&snap, (uint32_t)k * 1000U);
        status = sync_status();

        // Printed in parts with epochs between them, the report is still
        // the synchroniser as it stood at its first part.
        if (k == SNAPSHOT_EPOCH)
        {
            (void)capture(0, false, true);
            parts_file = tmpfile();
            print_queue_reset();
            (void)sync_report_queue(true);
            into(parts_file);
            (void)print_queue_service(0, true); // part 0: the copy
            restore_stdout();
        }
        if (k == SNAPSHOT_EPOCH + 30)
        {
            (void)capture(2, false, true);
            into(parts_file);
            while (print_queue_service(0, true))
            {
            }
            restore_stdout();
            (void)read_back(parts_file, 1);
            CHECK(strcmp(captured[0], captured[1]) == 0 && strcmp(captured[0], captured[2]) != 0);
        }
        if (update.transitioned && transitions < 32)
        {
            const SyncTransition *t = sync_last_transition();
            seen[transitions++] = (SyncState)t->to;
            printf("  ");
            sync_report_transition(&update);
            printf("  ");
            sync_report_sources();
        }

        // The steering decision follows the receiver's state.
        if (update.usable && !(now_receiver.time_valid && now_receiver.pps &&
                               now_receiver.uncertainty <= SYNC_GNSS_HOLD_NS))
            r.steered_while_unusable++;

        // The epoch the receiver corrects its time by a second is held over
        // on the old count: whether that or the new label is right is not
        // knowable from one source.
        truth = label_of(nearest, &truth_leap);
        if (sync_is_synchronised() && status->state != SYNC_ACQUIRE && k != 500 &&
            (status->time.utc_seconds != truth || status->time.leap_second != truth_leap))
        {
            if (r.wrong_labels < 5)
                printf("  epoch %ld: label %lld%s, truth %lld%s\n", k,
                       (long long)status->time.utc_seconds, status->time.leap_second ? "(60)" : "",
                       (long long)truth, truth_leap ? "(60)" : "");
            r.wrong_labels++;
        }
        if (truth_leap && status->time.leap_second && status->state == SYNC_HOLDOVER)
            r.leap_seen = true;

        if (status->state == SYNC_LOCKED && r.first_locked < 0)
            r.first_locked = k;
        if (status->state == SYNC_LOCKED && r.first_locked >= 0 && k > r.first_locked + 15 &&
            !(k >= 820 && k < 860) && fabs(error) > r.worst_locked)
            r.worst_locked = fabs(error);
        if (k >= 300 && k < 360 && fabs(error) > r.worst_unlocked)
            r.worst_unlocked = fabs(error);
        if (status->state == SYNC_HOLDOVER && fabs(error) > r.worst_holdover)
            r.worst_holdover = fabs(error);
        {
            uint32_t bound;
            bool bounded = sync_uncertainty(&bound);
            if (status->state == SYNC_LOCKED)
            {
                r.locked++;
                // The bound is in ns; the error in ticks of 25 ns.
                if (bounded && bound >= 55U && fabs(error) * 25.0 <= (double)bound)
                    r.bounded_locked++;
            }
            if (status->state == SYNC_FREE_RUN && bounded)
                r.bounded_free_run++;
            if (status->state == SYNC_HOLDOVER && bounded && bound > r.holdover_bound)
                r.holdover_bound = bound;
        }

        epoch += (double)running;
    }

    sync_report_status(true);
    for (int verbose = 0; verbose < 2; verbose++)
    {
        size_t whole = capture(0, false, verbose != 0);
        size_t parts = capture(1, true, verbose != 0);

        CHECK(whole > 40 && whole == parts && strcmp(captured[0], captured[1]) == 0);
        CHECK(print_queue_stats()->most == 1 &&
              print_queue_stats()->steps == (verbose ? SYNC_REPORT_PARTS : 1U));
    }
    status = sync_status();
    printf("sync state machine\n");
    printf("  first locked at epoch %ld; worst error locked %.1f, while the receiver was unlocked "
           "%.1f, in holdover %.1f ticks\n",
           r.first_locked, r.worst_locked, r.worst_unlocked, r.worst_holdover);
    printf(
        "  %d transitions, %ld wrong labels, %lu label corrections, steered while unusable %ld\n",
        transitions, r.wrong_labels, (unsigned long)status->time.corrections,
        r.steered_while_unusable);

    // Cold start: the first fix at 50 us is beyond the qualify limit, but
    // with nothing else it is taken as a fallback on its third epoch (22),
    // and locked a few later. Once within the limit (from 25) it is an
    // ordinary source: the poor solution at 450 is not kept.
    CHECK(r.first_locked > 22 && r.first_locked < 32);
    // The expected sequence of states, no more and no fewer.
    CHECK(transitions == (int)(sizeof expected_order / sizeof expected_order[0]));
    for (int i = 0; i < transitions && i < (int)(sizeof expected_order / sizeof expected_order[0]);
         i++)
        CHECK(seen[i] == expected_order[i]);
    // Never steered to a source that was not good.
    CHECK(r.steered_while_unusable == 0);
    // The free-running PPS of an unlocked receiver, 2 us off, moved nothing.
    CHECK(r.worst_unlocked < 3.0);
    CHECK(r.worst_locked < 3.0);
    CHECK(r.worst_holdover < 3.0);
    // The time of day was right throughout, through the receiver's one
    // second correction and the leap second in holdover.
    CHECK(r.wrong_labels == 0);
    CHECK(r.leap_seen);
    CHECK(status->time.corrections == 1);
    CHECK(status->sources[SYNC_SOURCE_GNSS].label_jumps == 1);
    // 20-22 before it was taken (then within the hold limit), and 450-454.
    CHECK(status->sources[SYNC_SOURCE_GNSS].verdicts[SYNC_CHECK_INACCURATE] == 8);
    CHECK(status->sources[SYNC_SOURCE_GNSS].verdicts[SYNC_CHECK_INCONSISTENT] == 1);
    CHECK(status->state == SYNC_LOCKED);
    // The uncertainty bound: always given and honest when locked, never in
    // free run, growing in holdover up to the synchronised limit.
    printf("  uncertainty bound covered the error in %ld of %ld locked epochs; up to %lu ns in "
           "holdover\n",
           r.bounded_locked, r.locked, (unsigned long)r.holdover_bound);
    CHECK(r.bounded_locked == r.locked && r.bounded_free_run == 0);
    // (In the epoch it passes the limit it is still HOLDOVER, not counted
    // as synchronised, and goes to free run on the next: one epoch's growth.)
    CHECK(r.holdover_bound > 1000U && r.holdover_bound <= SYNC_SYNCHRONISED_LIMIT_NS + 100U);
    // GNSS was selected once at each acquisition, NTP never.
    CHECK(gnss_selections == 2 && ntp_selections == 0);
    CHECK(LEAP_FROM_LINK == (status->link_leap.change != 0));

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
