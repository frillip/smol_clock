/*
 * The RTC source (sync/source_rtc.c) against a model of the PCF8563 and of
 * time: our epochs exactly on the true second, the main loop passing every
 * 300 us (slower where a test says so), the RTC counting its own seconds,
 * frozen while STOP is set and first incrementing a start delay after STOP
 * is released. The driver (drivers/pcf8563.c, tested in test_i2c.c) is
 * replaced by one that acts on the model: a read takes 250 us and returns
 * the seconds as they were when it started, stamped then.
 *
 *   - start-up: the RTC's second found, reported as an edge, nothing written
 *     while the clock is not synchronised;
 *   - synchronised: checked, set aligned, confirmed;
 *   - a clone whose start delay is half a second longer: learnt on the
 *     second attempt;
 *   - an RTC that lost power (VL): set without measuring;
 *   - a main loop too busy at the moment of release: given up after three
 *     attempts and written plainly, never left stopped; and if the clock
 *     stops being synchronised between attempts, no more writes, and the
 *     RTC, left loaded and stopped, is released once a read shows it;
 *   - a fast RTC: set again at the next check (built with a 10-minute check);
 *   - a setting by command: measured again, not overwritten until a check;
 *   - an RTC left stopped (a reset in the middle of an aligned write):
 *     released at once, then found.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../drivers/pcf8563.h"
#include "../../sync/source_rtc.h"

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

#define P     40000000ULL // ticks a second
#define US    40ULL
#define MS    40000ULL
#define L0    1792600000UL // our label of epoch 0
#define E0    (5ULL * P)   // epoch 0, ticks
#define LATCH (25ULL * US) // the chip takes its registers about a byte in

// ---- Time ------------------------------------------------------------------
static uint64_t now; // ticks

// Each call is a microsecond of the main loop spinning.
uint32_t pps_capture_timebase(void)
{
    now += US;
    return (uint32_t)now;
}


// ---- The RTC -------------------------------------------------------------------
// Running: it shows anchor_value from anchor_time, one more every period.
// Stopped: frozen_value.
static bool rtc_running;
static bool rtc_vl;
static int64_t anchor_time;
static int64_t frozen_until; // after a release: the loaded time until its first increment
static uint32_t anchor_value;
static uint32_t frozen_value;
static uint64_t rtc_period;
static uint64_t start_delay = 19687500ULL; // 63/128 s, as measured on V2

static int64_t floor_div(int64_t a, int64_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}


static uint32_t rtc_value_at(uint64_t t)
{
    if (!rtc_running)
    {
        return frozen_value;
    }
    if ((int64_t)t < frozen_until)
    {
        return anchor_value - 1U;
    }
    return anchor_value + (uint32_t)floor_div((int64_t)t - anchor_time, (int64_t)rtc_period);
}


// The first increment after t.
static uint64_t rtc_next_after(uint64_t t)
{
    int64_t n = floor_div((int64_t)t - anchor_time, (int64_t)rtc_period) + 1;
    return (uint64_t)(anchor_time + n * (int64_t)rtc_period);
}


// Running, seconds_ahead of our clock, its increments phase ticks after our
// epochs, ppm fast.
static void rtc_start(int32_t seconds_ahead, uint64_t phase, double ppm)
{
    rtc_running = true;
    rtc_vl = false;
    rtc_period = (uint64_t)llround((double)P / (1.0 + ppm * 1e-6));
    anchor_time = (int64_t)(E0 + phase);
    anchor_value = (uint32_t)((int64_t)L0 + seconds_ahead);
    frozen_until = INT64_MIN;
}


// ---- The driver, on the model ------------------------------------------------
typedef enum
{
    OP_NONE,
    OP_READ,
    OP_WRITE,
    OP_WRITE_STOPPED,
    OP_RELEASE
} Op;

static Pcf8563Status status;
static Op op;
static uint64_t op_start, op_done;
static uint32_t op_value;
static bool auto_read = true;
static uint64_t last_auto_read;
static int stop_writes, plain_writes_seen;

const Pcf8563Status *pcf8563_status(void)
{
    return &status;
}


bool pcf8563_busy(void)
{
    return op != OP_NONE;
}


void pcf8563_set_auto_read(bool enabled)
{
    auto_read = enabled;
}


static bool begin(Op kind, uint64_t duration, uint32_t value)
{
    if (op != OP_NONE)
    {
        return false;
    }
    op = kind;
    op_start = now;
    op_done = now + duration;
    op_value = value;
    status.request = kind == OP_READ ? PCF8563_READING : PCF8563_WRITING;
    return true;
}


bool pcf8563_read(void)
{
    return begin(OP_READ, 250 * US, 0);
}


bool pcf8563_write(uint32_t utc_seconds)
{
    return utc_seconds >= PCF8563_FIRST_UTC && begin(OP_WRITE, 300 * US, utc_seconds);
}


bool pcf8563_write_stopped(uint32_t utc_seconds)
{
    return utc_seconds >= PCF8563_FIRST_UTC && begin(OP_WRITE_STOPPED, 300 * US, utc_seconds);
}


bool pcf8563_release(void)
{
    return begin(OP_RELEASE, 80 * US, 0);
}


// pcf8563_service(): finishes the operation once its time has come, and
// reads every minute while allowed.
static void driver_service(void)
{
    if (op == OP_NONE && auto_read && now - last_auto_read >= 60 * 1000 * MS)
    {
        last_auto_read = now;
        (void)pcf8563_read();
    }
    if (op == OP_NONE || now < op_done)
    {
        return;
    }
    switch (op)
    {
        case OP_READ:
            status.utc_seconds = rtc_value_at(op_start + LATCH);
            status.calendar_valid = true;
            status.stopped = !rtc_running;
            status.voltage_low = rtc_vl;
            status.time_valid = rtc_running && !rtc_vl;
            status.read_stamp = (uint32_t)op_start;
            status.reads++;
            break;
        case OP_WRITE:
            rtc_running = true;
            rtc_vl = false;
            anchor_time = (int64_t)(op_done + P / 3U); // the prescaler is not reset: anywhere
            anchor_value = op_value + 1U;
            frozen_until = anchor_time;
            status.writes++;
            status.time_valid = true; // read back
            plain_writes_seen++;
            break;
        case OP_WRITE_STOPPED:
            frozen_value = op_value;
            rtc_running = false;
            rtc_vl = false;
            status.writes++;
            stop_writes++;
            break;
        case OP_RELEASE:
            rtc_running = true; // STOP clears with the data byte
            anchor_time = (int64_t)(op_start + 70 * US + start_delay);
            anchor_value = frozen_value + 1U;
            frozen_until = anchor_time;
            status.releases++;
            break;
        default: break;
    }
    op = OP_NONE;
    status.request = PCF8563_DONE;
}


// ---- The synchroniser, as far as the RTC source sees it: what main.c passes --------
static SyncTime clock_time;
static bool synchronised;
static uint32_t clock_bound = 100U; // ns, while synchronised

// ---- The main loop -------------------------------------------------------------
static uint32_t epoch;              // our latest epoch
static SyncSourceReport report;     // the RTC's report for it
static uint64_t slow_from, slow_to; // within each second: one pass across it all

static uint64_t epoch_time(uint32_t k)
{
    return E0 + (uint64_t)k * P;
}


static void reset(void)
{
    now = E0 + 2 * MS;
    epoch = 0;
    memset(&status, 0, sizeof status);
    status.present = true;
    op = OP_NONE;
    auto_read = true;
    last_auto_read = now;
    stop_writes = plain_writes_seen = 0;
    start_delay = 19687500ULL;
    memset(&clock_time, 0, sizeof clock_time);
    clock_time.known = true;
    clock_time.utc_seconds = L0;
    synchronised = false;
    slow_from = slow_to = 0;
    source_rtc_init();
    (void)pcf8563_read(); // pcf8563_init()
}


// Runs until our epoch `until`, each epoch's work 1 ms after it.
static void run_to(uint32_t until)
{
    while (epoch < until)
    {
        uint64_t phase = (now - E0) % P;
        bool slow = phase >= slow_from && phase < slow_to;
        TimingSnapshot snapshot;

        now += slow ? slow_to - phase : 300 * US;
        driver_service();
        source_rtc_poll();
        if (now >= epoch_time(epoch + 1U) + MS)
        {
            epoch++;
            memset(&snapshot, 0, sizeof snapshot);
            snapshot.timer.sequence = epoch;
            snapshot.capture.epoch_capture = (uint32_t)epoch_time(epoch);
            snapshot.capture.epoch_capture_valid = true;
            clock_time.utc_seconds = L0 + epoch;
            source_rtc_report(&snapshot, epoch * 1000U, &report);
            source_rtc_epoch(&snapshot, &clock_time, synchronised,
                             synchronised ? clock_bound : UINT32_MAX);
        }
    }
}


// Where the RTC's next increment is against our epochs, ticks (+ = after),
// and whether it then shows our label.
static int64_t rtc_phase(bool *label_right)
{
    uint64_t next = rtc_next_after(now);
    uint32_t k = (uint32_t)((next - E0 + P / 2U) / P);

    *label_right = rtc_value_at(next) == L0 + k;
    return (int64_t)(next - epoch_time(k));
}


// ---- Tests ---------------------------------------------------------------------
static void test_start_up(void)
{
    const SourceRtcState *s = source_rtc_state();
    bool label_right;

    printf("Start-up: the RTC 4.6 s ahead, the clock not synchronised\n");
    reset();
    rtc_start(5, 23 * MS + 0.37 * P, 0.0);
    run_to(1);
    // Searched for at once, as soon as the chip was read, not at the first
    // epoch's work: found before epoch 1 (its second falls 0.39 s in).
    CHECK(s->second_valid && s->searches == 1);
    run_to(10);
    printf("  second found to %lu us, %lu searches\n", (unsigned long)(s->resolution_ticks / US),
           (unsigned long)s->searches);
    CHECK(s->second_valid && s->searches == 1 && s->searches_failed == 0);
    CHECK(s->resolution_ticks <= 400 * US);
    // Its increment, against the model's: within the resolution.
    {
        uint64_t increment = rtc_next_after(now) - rtc_period; // the latest
        int64_t error = (int64_t)(int32_t)(s->second_stamp - (uint32_t)increment);
        // The source counts its second on from where it found it: compare
        // with the increment it now points at.
        error = floor_div(error + (int64_t)P / 2, (int64_t)P) * (int64_t)P - error;
        CHECK(llabs(error) <= 400 * (int64_t)US);
    }
    // Reported as an edge at its second, labelled with its time, not captured.
    CHECK(report.present && report.time_valid && report.edge && report.labelled &&
          !report.captured);
    CHECK(report.utc_seconds == rtc_value_at(epoch_time(epoch)));
    CHECK(report.uncertainty_ns == SOURCE_RTC_UNCERTAINTY_NS);
    CHECK(auto_read);
    // Not synchronised: nothing is ever written.
    run_to(400);
    CHECK(status.writes == 0 && s->writes == 0);
    (void)rtc_phase(&label_right);
    CHECK(!label_right);
}


static void test_set_aligned(void)
{
    const SourceRtcState *s = source_rtc_state();
    bool label_right;
    int64_t phase;

    printf("Synchronised: checked and set\n");
    reset();
    rtc_start(5, 23 * MS + 0.37 * P, 0.0);
    run_to(20);
    synchronised = true;
    run_to(20 + SOURCE_RTC_SETTLE_EPOCHS - 1U);
    CHECK(s->checks == 0 && s->writes == 0);
    run_to(20 + SOURCE_RTC_SETTLE_EPOCHS + 6U);
    phase = rtc_phase(&label_right);
    printf("  offset found %+ld ms; set: lands %+ld us from our epoch, measured %+ld us; "
           "correction %+ld us\n",
           (long)s->offset_ms, (long)(phase / (int64_t)US),
           (long)(s->last_write_error_ticks / (int32_t)US),
           (long)(s->release_correction_ticks / (int32_t)US));
    CHECK(s->checks == 1 && s->writes == 1 && s->writes_failed == 0 && stop_writes == 1);
    CHECK(label_right && llabs(phase) < 200 * (int64_t)US);
    CHECK(s->last_write_valid && labs(s->last_write_error_ticks) < 400 * (int32_t)US);
    CHECK(rtc_running && auto_read && s->activity == SOURCE_RTC_IDLE);
    // Measured again from the confirmation: on our second now.
    CHECK(s->offset_valid && labs(s->offset_ms) <= 1);
    CHECK(s->next_check_epoch == epoch - (epoch - s->next_check_epoch) &&
          (int32_t)(s->next_check_epoch - epoch) > (int32_t)SOURCE_RTC_CHECK_EPOCHS - 10);
    // Reported again, now on our second: the latest increment at or before
    // our epoch, which may be the one before if it was found just after.
    run_to(epoch + 2U);
    {
        int32_t raw = (int32_t)((uint32_t)epoch_time(epoch) - report.edge_stamp);
        uint32_t label = report.utc_seconds + (raw >= (int32_t)(P / 2U) ? 1U : 0U);
        int32_t off = raw >= (int32_t)(P / 2U) ? raw - (int32_t)P : raw;
        CHECK(report.edge && label == L0 + epoch && labs(off) < 400 * (int32_t)US);
    }
}


static void test_clone(void)
{
    const SourceRtcState *s = source_rtc_state();
    bool label_right;
    int64_t phase;

    printf("A clone whose first increment comes half a second later\n");
    reset();
    start_delay = 19687500ULL + P / 2U;
    rtc_start(-3, 0.8 * P, 0.0);
    synchronised = true;
    run_to(SOURCE_RTC_SETTLE_EPOCHS + 12U);
    phase = rtc_phase(&label_right);
    printf("  %lu set, %lu missed; correction learnt %+ld ms; lands %+ld us\n",
           (unsigned long)s->writes, (unsigned long)s->writes_failed,
           (long)(s->release_correction_ticks / (int32_t)MS), (long)(phase / (int64_t)US));
    CHECK(s->writes == 1 && s->writes_failed == 1);
    CHECK(labs(s->release_correction_ticks - (int32_t)(P / 2U)) < 600 * (int32_t)US);
    CHECK(label_right && llabs(phase) < 200 * (int64_t)US);
}


static void test_power_lost(void)
{
    const SourceRtcState *s = source_rtc_state();
    bool label_right;
    int64_t phase;

    printf("An RTC that lost power (VL)\n");
    reset();
    rtc_start(-800000000, 0.5 * P, 0.0);
    rtc_vl = true;
    run_to(30);
    CHECK(s->searches == 0 && !s->second_valid && !report.time_valid);
    synchronised = true;
    run_to(30 + SOURCE_RTC_SETTLE_EPOCHS + 6U);
    phase = rtc_phase(&label_right);
    CHECK(s->writes == 1 && !rtc_vl && label_right && llabs(phase) < 200 * (int64_t)US);
}


static void test_busy_loop(void)
{
    const SourceRtcState *s = source_rtc_state();
    bool label_right;

    printf("A main loop busy when the release is due\n");
    reset();
    rtc_start(2, 0.3 * P, 0.0);
    slow_from = 470 * MS; // the release falls at ~492 ms
    slow_to = 530 * MS;
    synchronised = true;
    run_to(SOURCE_RTC_SETTLE_EPOCHS + 12U);
    (void)rtc_phase(&label_right);
    printf("  %lu late, %lu missed, %lu plain writes\n", (unsigned long)s->late_releases,
           (unsigned long)s->writes_failed, (unsigned long)s->plain_writes);
    CHECK(s->late_releases == SOURCE_RTC_SYNC_ATTEMPTS && s->writes == 0);
    CHECK(s->plain_writes == 1 && plain_writes_seen == 1);
    // Never left stopped; a plain write cannot place its second, but the
    // time is right to within it.
    CHECK(rtc_running && auto_read);
    CHECK(rtc_value_at(now) + 1U >= L0 + epoch && rtc_value_at(now) <= L0 + epoch + 1U);
    (void)label_right;
}


static void test_sync_lost(void)
{
    const SourceRtcState *s = source_rtc_state();

    printf("Synchronisation lost between attempts\n");
    reset();
    rtc_start(2, 0.3 * P, 0.0);
    slow_from = 470 * MS;
    slow_to = 530 * MS;
    synchronised = true;
    int loads;

    while (s->late_releases == 0)
        run_to(epoch + 1U);
    // Lost from here: the attempt already loaded may still go out (and miss),
    // but none is begun after it.
    synchronised = false;
    loads = stop_writes;
    run_to(epoch + 2U); // the attempt in flight misses too
    slow_from = slow_to = 0;
    run_to(epoch + 90U);
    printf("  %d loaded when lost, %d in all, %lu late; released %lu\n", loads, stop_writes,
           (unsigned long)s->late_releases, (unsigned long)status.releases);
    // At most the one in flight after it; none begun once it was lost.
    CHECK(stop_writes <= loads + 1 && s->late_releases == (uint32_t)stop_writes);
    CHECK(s->writes == 0 && s->plain_writes == 0 && status.writes == (uint32_t)stop_writes);
    CHECK(rtc_running && status.releases == 1); // released after the minute's read
    CHECK(s->second_valid && report.time_valid);
}


// A fast RTC: its rate learnt from the search 10 minutes after it was set,
// its second counted on at that rate, and checked often enough that it
// stays within SOURCE_RTC_DRIFT_LIMIT_MS (here every 3333 s, not 6 h).
static void test_fast_rtc(double ppm)
{
    const SourceRtcState *s = source_rtc_state();
    bool label_right;
    int64_t phase;
    uint32_t set_at;
    uint32_t interval;
    int64_t worst_predicted = 0;
    uint32_t k;

    printf("An RTC %.0f ppm %s\n", fabs(ppm), ppm > 0.0 ? "fast" : "slow");
    reset();
    rtc_start(0, 0.9 * P, ppm);
    synchronised = true;
    run_to(SOURCE_RTC_SETTLE_EPOCHS + 10U);
    CHECK(s->writes == 1 && !s->rate_valid);
    set_at = s->last_write_epoch;
    run_to(set_at + SOURCE_RTC_RATE_EPOCHS + 5U);
    interval = source_rtc_check_interval();
    printf("  rate learnt %+.1f ppm (%lu updates); checks every %lu s\n",
           source_rtc_rate_ppm10() / 10.0, (unsigned long)s->rate_updates, (unsigned long)interval);
    CHECK(s->rate_valid && s->rate_updates == 1);
    CHECK(labs(source_rtc_rate_ppm10() - (int32_t)lround(ppm * 10.0)) <= 20); // 2 ppm
    CHECK(llabs((int64_t)interval - (int64_t)(500.0 / (fabs(ppm) * 1e-6) / 1000.0)) < 60);
    CHECK(s->writes == 1); // the rate search sets nothing
    // Counted on at its rate: the reported second stays on the RTC's.
    for (k = epoch; k < set_at + interval - 5U; k += 100U)
    {
        int64_t error;

        run_to(k);
        error = (int64_t)(int32_t)(report.edge_stamp -
                                   (uint32_t)(rtc_next_after(epoch_time(epoch)) - rtc_period));
        if (llabs(error) > worst_predicted)
            worst_predicted = llabs(error);
    }
    // Checked and set again at the interval, before it is 500 ms out.
    run_to(set_at + interval + 10U);
    phase = rtc_phase(&label_right);
    printf("  counted on to within %lld us; checked %lu times, set %lu times; now %+lld us\n",
           (long long)(worst_predicted / (int64_t)US), (unsigned long)s->checks,
           (unsigned long)s->writes, (long long)(phase / (int64_t)US));
    CHECK(worst_predicted < 2 * (int64_t)MS);
    CHECK(s->checks == 2 && s->writes == 2 && s->offset_ms < 520 && s->offset_ms > -520);
    CHECK(label_right && llabs(phase) < 5 * (int64_t)MS);
    // The rate is kept over a setting.
    CHECK(s->rate_valid && labs(source_rtc_rate_ppm10() - (int32_t)lround(ppm * 10.0)) <= 20);
}


// Where the start of the UTC second the RTC's report predicts is, against
// that true second, ticks.
static int64_t predicted_error(void)
{
    uint32_t start = report.edge_stamp - (uint32_t)(report.edge_offset_ns / 25);
    uint32_t truth = (uint32_t)(E0 + (uint64_t)(report.utc_seconds - L0) * P);
    return (int64_t)(int32_t)(start - truth);
}


static void test_command(void)
{
    const SourceRtcState *s = source_rtc_state();

    printf("Set by command while synchronised\n");
    reset();
    rtc_start(0, 0.2 * P, 0.0);
    synchronised = true;
    run_to(SOURCE_RTC_SETTLE_EPOCHS + 10U);
    CHECK(s->writes == 1);
    CHECK(!source_rtc_take_set()); // its own writes are not reported unasked
    CHECK(source_rtc_set(L0 + epoch + 1000U));
    CHECK(source_rtc_take_set() && !source_rtc_take_set());
    run_to(epoch + 10U);
    CHECK(s->plain_writes == 1 && s->second_valid && s->searches >= 3);
    CHECK(s->second_label >= L0 + epoch + 1000U && s->second_label <= L0 + epoch + 1001U);
    // Found again while synchronised: measured 1000 s ahead, so reported
    // as UTC, not as it shows.
    CHECK(s->anchor.valid && s->anchor.aligned && s->anchor.offset_ns < -999000000000LL &&
          s->anchor.offset_ns > -1001000000000LL);
    CHECK(llabs(predicted_error()) < 1 * (int64_t)MS);
    CHECK(s->writes == 1); // left alone until the next check
}


/* The bound and the drift, over a power cycle: an RTC 150 ppm fast, set,
 * measured and its rate learnt; what is kept taken; a day off (it gains
 * 13 s); started again from what was kept. Its time is corrected for the
 * drift and the bound covers what is left. Then the same without the rate,
 * and with nothing kept. */
static void test_power_cycle(void)
{
    const SourceRtcState *s = source_rtc_state();
    SourceRtcKept kept;
    uint32_t bound;
    int64_t error;
    uint32_t changes;

    printf("Over a power cycle: 150 ppm fast, a day off\n");
    reset();
    rtc_start(0, 0.9 * P, 150.0);
    synchronised = true;
    CHECK(!source_rtc_bound(&bound) || bound == SOURCE_RTC_UNCERTAINTY_NS);
    run_to(SOURCE_RTC_SETTLE_EPOCHS + 10U);
    // Set aligned: the RTC's changes count, and the confirmation is the
    // anchor, to the clock's bound and the search's resolution.
    CHECK(s->writes == 1 && s->rtc_changes == 1 && s->anchor.valid && s->anchor.aligned);
    CHECK(s->anchor.bound_ns >= clock_bound && s->anchor.bound_ns < clock_bound + 300000U);
    CHECK(llabs(s->anchor.offset_ns) < 1000000LL && s->last_write_aligned);
    changes = s->kept_changes;
    run_to(s->last_write_epoch + SOURCE_RTC_RATE_EPOCHS + 5U);
    CHECK(s->rate_valid && s->kept_changes > changes && s->rtc_changes == 1);
    // Reported as UTC's second, and the bound grows from the anchor.
    error = predicted_error();
    CHECK(llabs(error) < 1 * (int64_t)MS);
    CHECK(source_rtc_bound(&bound) && bound > s->anchor.bound_ns && bound < 10000000U);
    source_rtc_kept(&kept);

    // Off for a day: our epochs go on from where they were (the model's
    // time does), the RTC 13 s further ahead.
    epoch += 86400U;
    now = epoch_time(epoch) + 2 * MS;
    source_rtc_init();
    source_rtc_restore(&kept);
    synchronised = false;
    clock_time.known = false;
    (void)pcf8563_read();
    run_to(epoch + 8U);
    error = predicted_error();
    printf("  RTC %+.3f s from UTC; predicted to %+.1f ms, bound %.1f ms\n",
           (double)(int32_t)(s->second_label - report.utc_seconds), (double)error / (double)MS,
           report.uncertainty_ns / 1e6);
    CHECK(s->restored && s->second_valid && s->second_label - report.utc_seconds == 13U);
    CHECK(report.uncertainty_ns > 860000000U && report.uncertainty_ns < 870000000U);
    CHECK(llabs(error) * 25 < (int64_t)report.uncertainty_ns && llabs(error) < 50 * (int64_t)MS);
    CHECK(source_rtc_bound(&bound) && bound == report.uncertainty_ns);
    CHECK(s->last_write_valid && s->last_write_aligned && s->writes == 0);

    // The PIC's oscillator 1 ppm fast (40 ticks a second more than nominal):
    // its rate on the capture timebase is 1 ppm slower than against true
    // seconds, so the prediction moves 1 us a second since the anchor.
    {
        int64_t before, after, expected;
        uint32_t elapsed;

        before = (int64_t)report.edge_offset_ns +
                 (int64_t)(int32_t)(report.utc_seconds - s->second_label) * 1000000000LL;
        source_rtc_set_oscillator(40 * 65536);
        run_to(epoch + 1U);
        after = (int64_t)report.edge_offset_ns +
                (int64_t)(int32_t)(report.utc_seconds - s->second_label) * 1000000000LL;
        elapsed = s->second_label - s->anchor.utc_seconds;
        expected = (int64_t)s->rate_q8 * 25 / 256 - (int64_t)elapsed * 1000;
        printf("  oscillator 1 ppm fast: prediction moved %+.3f ms\n",
               (double)(after - before) / 1e6);
        CHECK(llabs((after - before) - expected) < 1000);
        source_rtc_set_oscillator(0);
    }

    // Without the rate: no correction, and a bound of the rate limit, past
    // what the bound can say.
    kept.rate_valid = false;
    source_rtc_init();
    source_rtc_restore(&kept);
    (void)pcf8563_read();
    run_to(epoch + 8U);
    CHECK(s->second_valid && report.utc_seconds == s->second_label);
    CHECK(report.uncertainty_ns == SOURCE_RTC_BOUND_MAX_NS);

    // Nothing kept: as it shows, to a second.
    source_rtc_init();
    (void)pcf8563_read();
    run_to(epoch + 8U);
    CHECK(report.utc_seconds == s->second_label && report.edge_offset_ns == 0);
    CHECK(report.uncertainty_ns == SOURCE_RTC_UNCERTAINTY_NS && !s->restored);

    // A plain setting while not synchronised (the menu's tIME): its own
    // anchor, to a second, kept as such.
    CHECK(source_rtc_set(L0 + epoch + 1U));
    CHECK(s->anchor.valid && !s->anchor.aligned &&
          s->anchor.bound_ns == SOURCE_RTC_UNCERTAINTY_NS && s->rtc_changes == 1);
    CHECK(s->last_write_valid && !s->last_write_aligned && s->last_write_utc == L0 + epoch + 1U);
}


static void test_left_stopped(void)
{
    const SourceRtcState *s = source_rtc_state();

    printf("An RTC left stopped\n");
    reset();
    rtc_start(0, 0.4 * P, 0.0);
    rtc_running = false;
    frozen_value = L0 - 30U;
    run_to(8);
    CHECK(status.releases == 1 && rtc_running);
    CHECK(s->second_valid && s->searches == 1 && report.time_valid);
    CHECK(status.writes == 0); // not synchronised: left at its stale time
}


int main(void)
{
    test_start_up();
    test_set_aligned();
    test_clone();
    test_power_lost();
    test_busy_loop();
    test_sync_lost();
    test_fast_rtc(150.0);
    test_fast_rtc(-40.0);
    test_command();
    test_power_cycle();
    test_left_stopped();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
