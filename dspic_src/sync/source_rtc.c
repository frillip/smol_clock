/*
 * The RTC as a time source, and keeping it set. See source_rtc.h.
 *
 * Who runs what: source_rtc_report() from sync_update() each epoch (counts
 * the RTC's second on); source_rtc_epoch() after it (decides searches,
 * checks and writes, acts on their results, with our epoch's time to hand);
 * source_rtc_poll() every pass of the main loop (keeps a search's reads
 * going, and times the release). Everything is foreground.
 */
#include "source_rtc.h"

#include <stdio.h>
#include "../platform/format.h" // our printf (format.h: the library's is slow)
#include <string.h>

#include "../console/debug.h"
#include "../drivers/pcf8563.h"
#include "../platform/utc.h"
#include "../timing/timing_config.h"

#define SECOND_TICKS ((uint32_t)TIMING_NOMINAL_TICKS)
// ns to ticks: 25 ns a tick.
#define START_DELAY_TICKS ((int32_t)(PCF8563_START_DELAY_NS / (1000UL / SOURCE_RTC_TICKS_PER_US)))
#define TICKS_PER_MS      ((int32_t)TIMING_TICKS_PER_MS)
#define NS_PER_TICK       (1000L / (int32_t)SOURCE_RTC_TICKS_PER_US)
#define NS_PER_SECOND     1000000000LL
#define BOUND_NONE        UINT32_MAX // the clock has no bound (sync_uncertainty)

typedef enum
{
    PURPOSE_MEASURE = 0, // find the RTC's second: at start-up, after a setting, when stale
    PURPOSE_CHECK,       // ... and compare it with ours, perhaps to set it
    PURPOSE_CONFIRM      // ... where an aligned write landed
} SearchPurpose;

static SourceRtcState state;
static int32_t oscillator_q16;     // source_rtc_set_oscillator
static int32_t second_fraction_q8; // counting its second on at its rate

// Our latest epoch (source_rtc_epoch).
static uint32_t epoch_sequence;
static bool epoch_valid; // its capture
static uint32_t epoch_stamp;

// A search.
static SearchPurpose purpose;
static uint32_t search_deadline;
static bool sample_valid;
static uint32_t sample_utc;
static uint32_t sample_stamp;
static uint32_t reads_seen;
static bool search_done; // ... ended: found (second_valid) or not, acted on at the epoch
static bool search_found;
static uint32_t measure_due_epoch; // the next start-up or stale-second search

// An aligned write.
static bool write_wanted; // begin one (or the next attempt) at the next epoch
static bool plain_wanted; // give up: write plainly at the next epoch
static uint32_t release_target;
static uint32_t expect_stamp; // the epoch its first increment should land on
static uint32_t expect_label; // ... showing this second
static uint32_t writes_seen;
static uint32_t releases_seen;
static uint32_t activity_epoch; // the epoch the current step began

// An RTC found stopped: released, then to be read back.
static bool stopped_released;
static bool set_by_command; // source_rtc_set's write: report its read-back

// When it was last read, by our clock.
static uint32_t reads_dated;

// Checks.
static bool was_synchronised;
static uint32_t corrections_seen;

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static bool epoch_reached(uint32_t epoch)
{
    return (int32_t)(epoch_sequence - epoch) >= 0;
}


// The next check no later than epoch.
static void check_by(uint32_t epoch)
{
    if ((int32_t)(epoch - state.next_check_epoch) < 0)
    {
        state.next_check_epoch = epoch;
    }
}


static bool writing(void)
{
    return state.activity == SOURCE_RTC_LOADING || state.activity == SOURCE_RTC_WAITING ||
           state.activity == SOURCE_RTC_RELEASING || state.activity == SOURCE_RTC_CONFIRMING;
}


static void set_activity(SourceRtcActivity activity)
{
    state.activity = activity;
    activity_epoch = epoch_sequence;
    // The periodic reads would only get in the way.
    pcf8563_set_auto_read(activity == SOURCE_RTC_IDLE);
}


static int32_t floor_div(int32_t a, int32_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}


// The next of its seconds, counted on at its rate, ticks.
static uint32_t next_second(int32_t *fraction_q8)
{
    int32_t q = *fraction_q8 + state.rate_q8;
    int32_t whole = floor_div(q, 256);

    *fraction_q8 = q - whole * 256;
    return (uint32_t)((int32_t)SECOND_TICKS + whole);
}


/* A second found again, with no setting since the baseline: how far it is
 * from where the baseline, counted on at the rate in use, puts it corrects
 * the rate. By the RTC's own count of seconds, so a jump of our epoch or a
 * gap in the search does not matter. */
static void learn_rate(uint32_t stamp, uint32_t label)
{
    uint32_t seconds = label - state.baseline_label;
    int64_t limit = (int64_t)SOURCE_RTC_RATE_LIMIT_PPM * (int64_t)SOURCE_RTC_TICKS_PER_US * 256;
    uint32_t predicted;
    int32_t error;
    int64_t rate;

    if (!state.baseline_valid || seconds < SOURCE_RTC_RATE_MIN_SECONDS || seconds > 200000UL)
    {
        return;
    }
    predicted = state.baseline_stamp + seconds * SECOND_TICKS +
                (uint32_t)(int32_t)(((int64_t)seconds * state.rate_q8) / 256);
    error = (int32_t)(stamp - predicted);
    rate = state.rate_q8 + ((int64_t)error * 256) / (int64_t)seconds;
    if (rate > limit || rate < -limit)
    {
        return;
    }
    state.rate_q8 = (int32_t)rate;
    state.rate_valid = true;
    state.rate_updates++;
    state.kept_changes++;
}


uint32_t source_rtc_check_interval(void)
{
    uint32_t ticks_per_second =
        (uint32_t)(state.rate_q8 < 0 ? -state.rate_q8 : state.rate_q8) / 256U;
    uint64_t seconds;

    if (!state.rate_valid || ticks_per_second == 0U)
    {
        return SOURCE_RTC_CHECK_EPOCHS;
    }
    seconds = (uint64_t)SOURCE_RTC_DRIFT_LIMIT_MS * (uint64_t)TICKS_PER_MS / ticks_per_second;
    if (seconds > SOURCE_RTC_CHECK_EPOCHS)
    {
        return SOURCE_RTC_CHECK_EPOCHS;
    }
    return seconds < SOURCE_RTC_CHECK_MIN_EPOCHS ? SOURCE_RTC_CHECK_MIN_EPOCHS : (uint32_t)seconds;
}


int32_t source_rtc_rate_ppm10(void)
{
    // rate_q8 / 256 ticks a second of 40 000 000: ppm = -rate_q8 / 10240.
    return state.rate_valid ? -(state.rate_q8 / 1024) : 0;
}


static void print_rate(void)
{
    int32_t ppm10 = source_rtc_rate_ppm10();
    int32_t size = ppm10 < 0 ? -ppm10 : ppm10;

    printf("%c%ld.%ld ppm", ppm10 < 0 ? '-' : '+', (long)(size / 10), (long)(size % 10));
}


static int32_t clamp_correction(int32_t ticks)
{
    if (ticks > SOURCE_RTC_CORRECTION_LIMIT_TICKS)
    {
        return SOURCE_RTC_CORRECTION_LIMIT_TICKS;
    }
    if (ticks < -SOURCE_RTC_CORRECTION_LIMIT_TICKS)
    {
        return -SOURCE_RTC_CORRECTION_LIMIT_TICKS;
    }
    return ticks;
}


// Ticks of the capture timebase, signed: +144.0 us.
static void print_ticks(int32_t ticks)
{
    utc_print_ns((int64_t)ticks * NS_PER_TICK, true);
}


static uint32_t saturate_bound(uint64_t ns)
{
    return ns > SOURCE_RTC_BOUND_MAX_NS ? SOURCE_RTC_BOUND_MAX_NS : (uint32_t)ns;
}


static int64_t floor_div64(int64_t a, int64_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}


// The RTC's time no longer what the anchor describes: it has been, or is
// being, written.
static void rtc_changed(void)
{
    state.anchor.valid = false;
    state.rtc_changes++;
    state.kept_changes++;
}


// Written plainly, to the second: where its second starts is not known.
static void wrote_plainly(uint32_t utc_seconds)
{
    rtc_changed();
    state.anchor.valid = true;
    state.anchor.aligned = false;
    state.anchor.utc_seconds = utc_seconds;
    state.anchor.offset_ns = 0;
    state.anchor.bound_ns = SOURCE_RTC_UNCERTAINTY_NS;
    state.last_write_valid = true;
    state.last_write_aligned = false;
    state.last_write_utc = utc_seconds;
    state.last_write_epoch = epoch_sequence;
}


// Its second found while the clock is synchronised: where it started
// against UTC's, from our epoch (time) and the stamps, to within the
// clock's bound and half the search's resolution.
static void anchor_found(const SyncTime *time, uint32_t clock_bound_ns)
{
    if (!epoch_valid || clock_bound_ns == BOUND_NONE)
    {
        return;
    }
    state.anchor.valid = true;
    state.anchor.aligned = true;
    state.anchor.utc_seconds = state.second_label;
    state.anchor.offset_ns =
        (int64_t)(int32_t)(time->utc_seconds - state.second_label) * NS_PER_SECOND +
        (int64_t)(int32_t)(state.second_stamp - epoch_stamp) * NS_PER_TICK;
    state.anchor.bound_ns = saturate_bound(
        (uint64_t)clock_bound_ns + (uint64_t)state.resolution_ticks * (uint64_t)NS_PER_TICK / 2U);
    state.kept_changes++;
}


// Its second labelled label, against UTC, from the anchor: where it starts
// minus UTC's (+ = the RTC late), and the bound. False without an anchor
// that came before it.
static bool predict(uint32_t label, int64_t *offset_ns, uint32_t *bound_ns)
{
    int32_t elapsed = (int32_t)(label - state.anchor.utc_seconds);
    uint64_t allowance =
        (uint64_t)(state.rate_valid ? SOURCE_RTC_ALLOWANCE_PPM : SOURCE_RTC_RATE_LIMIT_PPM);

    if (!state.anchor.valid || elapsed < 0)
    {
        return false;
    }
    *offset_ns = state.anchor.offset_ns;
    if (state.rate_valid)
    {
        // Its second is rate_q8 / 256 ticks longer than nominal, ours (a true
        // second) oscillator_q16 / 65536: + = slow, its seconds later.
        int64_t rate_q8 = (int64_t)state.rate_q8 - (int64_t)oscillator_q16 / 256;
        *offset_ns += (int64_t)elapsed * rate_q8 * NS_PER_TICK / 256;
    }
    *bound_ns = saturate_bound((uint64_t)state.anchor.bound_ns +
                               (uint64_t)(uint32_t)elapsed * allowance * 1000U);
    return true;
}


// Its label, as counted, else as read.
static bool current_label(uint32_t *label)
{
    const Pcf8563Status *rtc = pcf8563_status();

    if (!rtc->time_valid)
    {
        return false;
    }
    *label = state.second_valid ? state.second_label : rtc->utc_seconds;
    return true;
}


// The bound on its second labelled label.
static uint32_t bound_at(uint32_t label)
{
    int64_t offset_ns;
    uint32_t bound_ns;

    if (!predict(label, &offset_ns, &bound_ns))
    {
        return SOURCE_RTC_UNCERTAINTY_NS;
    }
    if (state.second_valid)
    {
        bound_ns = saturate_bound((uint64_t)bound_ns +
                                  (uint64_t)state.resolution_ticks * (uint64_t)NS_PER_TICK / 2U);
    }
    return bound_ns;
}


// --------------------------------------------------------------------------
// Searching for the RTC's second
// --------------------------------------------------------------------------

static void start_search(SearchPurpose why, uint32_t epochs)
{
    purpose = why;
    search_deadline = epoch_sequence + epochs;
    sample_valid = false;
    reads_seen = pcf8563_status()->reads;
    search_done = false;
    state.searches++;
    set_activity(why == PURPOSE_CONFIRM ? SOURCE_RTC_CONFIRMING : SOURCE_RTC_SEARCHING);
    (void)pcf8563_read();
}


static void end_search(bool found)
{
    search_done = true;
    search_found = found;
    if (!found)
    {
        state.searches_failed++;
    }
    set_activity(SOURCE_RTC_IDLE);
}


// The increment lies between the last read before it and the first after.
static void found(uint32_t stamp, uint32_t label, uint32_t resolution)
{
    if (state.second_valid)
    {
        // Counted from the second it replaces, so the edges run on.
        int32_t apart = (int32_t)(stamp - state.second_stamp);
        int32_t seconds = (apart + (int32_t)(SECOND_TICKS / 2U)) / (int32_t)SECOND_TICKS;
        state.second_edges += (uint32_t)(seconds > 0 ? seconds : 0);
    }
    else
    {
        state.second_edges++;
    }
    if (purpose != PURPOSE_CONFIRM)
    {
        learn_rate(stamp, label);
    }
    // The baseline for the next: after a setting, the confirmation.
    state.baseline_valid = true;
    state.baseline_stamp = stamp;
    state.baseline_label = label;
    second_fraction_q8 = 0;
    state.second_valid = true;
    state.second_stamp = stamp;
    state.second_label = label;
    state.measured_epoch = epoch_sequence;
    state.resolution_ticks = resolution;
    end_search(true);
}


static void take_sample(const Pcf8563Status *rtc)
{
    if (!rtc->calendar_valid || rtc->stopped)
    {
        sample_valid = false;
        return;
    }
    if (sample_valid && rtc->utc_seconds == sample_utc + 1U)
    {
        uint32_t gap = rtc->read_stamp - sample_stamp;

        if (gap <= SOURCE_RTC_SEARCH_RESOLUTION_TICKS)
        {
            found(sample_stamp + gap / 2U + SOURCE_RTC_READ_LATCH_TICKS, rtc->utc_seconds, gap);
            return;
        }
    }
    sample_valid = true;
    sample_utc = rtc->utc_seconds;
    sample_stamp = rtc->read_stamp;
}


static void keep_searching(void)
{
    const Pcf8563Status *rtc = pcf8563_status();

    if (rtc->reads != reads_seen)
    {
        reads_seen = rtc->reads;
        take_sample(rtc);
    }
    if (state.activity != SOURCE_RTC_SEARCHING && state.activity != SOURCE_RTC_CONFIRMING)
    {
        return; // found
    }
    if (!pcf8563_busy())
    {
        if (!rtc->present)
        {
            end_search(false);
            return;
        }
        (void)pcf8563_read();
    }
}


// --------------------------------------------------------------------------
// Aligned writing
// --------------------------------------------------------------------------

static void abort_write(void)
{
    state.writes_failed++;
    state.attempts++;
    set_activity(SOURCE_RTC_IDLE);
    if (state.attempts < SOURCE_RTC_SYNC_ATTEMPTS)
    {
        write_wanted = true;
    }
    else
    {
        plain_wanted = true; // never leave it stopped
    }
}


// Loads the time that the RTC should show until its first increment, which
// is to land on our epoch n seconds on: released n seconds less the start
// delay after this epoch, at least SOURCE_RTC_RELEASE_EARLIEST_TICKS in.
static void begin_write(const SyncTime *time)
{
    int32_t delay =
        START_DELAY_TICKS + state.release_correction_ticks + SOURCE_RTC_RELEASE_LATENCY_TICKS;
    uint32_t n = 1;
    uint32_t label = time->utc_seconds;

    write_wanted = false;
    while ((int32_t)(n * SECOND_TICKS) - delay < (int32_t)SOURCE_RTC_RELEASE_EARLIEST_TICKS)
    {
        n++;
    }
    // The RTC cannot hold a leap second: not across one.
    if (time->leap.change != 0 && time->leap.event_utc > label &&
        time->leap.event_utc - label <= n + 1U)
    {
        write_wanted = true; // after it
        return;
    }
    state.baseline_valid = false; // its second will be wherever we put it
    rtc_changed();
    release_target = epoch_stamp + n * SECOND_TICKS - (uint32_t)delay;
    expect_stamp = epoch_stamp + n * SECOND_TICKS;
    expect_label = label + n;
    writes_seen = pcf8563_status()->writes;
    set_activity(SOURCE_RTC_LOADING);
    if (!pcf8563_write_stopped(expect_label - 1U))
    {
        abort_write();
    }
}


// Spins for the last SOURCE_RTC_SPIN_TICKS before the release, so it goes
// out within microseconds of its moment; too late, it waits for another
// attempt.
static void wait_release(void)
{
    int32_t remaining = (int32_t)(release_target - pps_capture_timebase());

    if (remaining > SOURCE_RTC_SPIN_TICKS)
    {
        return;
    }
    if (remaining < -SOURCE_RTC_LATE_TICKS)
    {
        state.late_releases++;
        abort_write();
        return;
    }
    while ((int32_t)(release_target - pps_capture_timebase()) > 0)
    {
    }
    releases_seen = pcf8563_status()->releases;
    if (pcf8563_release())
    {
        set_activity(SOURCE_RTC_RELEASING);
    }
    else
    {
        abort_write();
    }
}


// --------------------------------------------------------------------------
// Results, at the epoch
// --------------------------------------------------------------------------

// RTC minus our clock at the RTC's latest increment, from our epoch now:
// whole seconds apart, less where its increment falls after our epoch.
// Saturated well before the arithmetic would overflow (24 days).
#define OFFSET_LIMIT_S 2000000L

static void measure_offset(const SyncTime *time)
{
    int32_t seconds;
    int32_t delta_ms;

    state.offset_valid = false;
    if (!time->known || !epoch_valid || !state.second_valid)
    {
        return;
    }
    seconds = (int32_t)(state.second_label - time->utc_seconds);
    delta_ms = (int32_t)(state.second_stamp - epoch_stamp) / TICKS_PER_MS;
    if (seconds > OFFSET_LIMIT_S)
    {
        state.offset_ms = OFFSET_LIMIT_S * 1000L;
    }
    else if (seconds < -OFFSET_LIMIT_S)
    {
        state.offset_ms = -OFFSET_LIMIT_S * 1000L;
    }
    else
    {
        state.offset_ms = seconds * 1000L - delta_ms;
    }
    state.offset_valid = true;
    state.offset_epoch = epoch_sequence;
}


static void confirmed(void)
{
    int32_t error = (int32_t)(state.second_stamp - expect_stamp);
    bool label_ok = state.second_label == expect_label;

    bool within =
        error <= SOURCE_RTC_ALIGN_TOLERANCE_TICKS && error >= -SOURCE_RTC_ALIGN_TOLERANCE_TICKS;

    if (label_ok)
    {
        // Learnt either way, so the next release allows for it: all of a
        // miss, half of what is left within tolerance, where the search's
        // own resolution (a read's length) is most of it.
        state.release_correction_ticks =
            clamp_correction(state.release_correction_ticks + (within ? error / 2 : error));
        state.last_write_error_ticks = error;
    }
    if (label_ok && within)
    {
        state.writes++;
        state.attempts = 0;
        state.last_write_valid = true;
        state.last_write_aligned = true;
        state.last_write_utc = expect_label;
        state.last_write_epoch = epoch_sequence;
        state.kept_changes++;
        state.next_check_epoch = epoch_sequence + source_rtc_check_interval();
        state.rate_due = true; // how far it has gone by then shows its rate
        state.rate_due_epoch = epoch_sequence + SOURCE_RTC_RATE_EPOCHS;
        if (debug_enabled(DEBUG_RTC))
        {
            printf("RTC: set, its second ");
            print_ticks(error);
            printf(" from the epoch (start delay correction ");
            print_ticks(state.release_correction_ticks);
            printf(")\n");
        }
        return;
    }
    if (debug_enabled(DEBUG_RTC))
    {
        printf("RTC: aligned write landed ");
        if (label_ok)
        {
            print_ticks(error);
            printf(" from the epoch");
        }
        else
        {
            printf("on the wrong second (%lu, not %lu)", (unsigned long)state.second_label,
                   (unsigned long)expect_label);
        }
        printf(", attempt %u\n", (unsigned)(state.attempts + 1U));
    }
    state.writes_failed++;
    state.attempts++;
    if (state.attempts < SOURCE_RTC_SYNC_ATTEMPTS)
    {
        write_wanted = true;
    }
    else if (!label_ok)
    {
        plain_wanted = true;
    }
    else
    {
        state.attempts = 0;
        state.next_check_epoch = epoch_sequence + source_rtc_check_interval();
    }
}


static void search_result(const SyncTime *time, bool synchronised, uint32_t clock_bound_ns)
{
    const Pcf8563Status *rtc = pcf8563_status();

    search_done = false;
    if (!search_found)
    {
        if (debug_enabled(DEBUG_RTC))
        {
            printf("RTC: second not found\n");
        }
        if (purpose == PURPOSE_CONFIRM)
        {
            abort_write();
        }
        else
        {
            measure_due_epoch = epoch_sequence + SOURCE_RTC_RETRY_EPOCHS;
            if (purpose == PURPOSE_CHECK)
            {
                state.next_check_epoch = epoch_sequence + SOURCE_RTC_RETRY_EPOCHS;
            }
        }
        return;
    }
    measure_offset(time);
    if (synchronised)
    {
        anchor_found(time, clock_bound_ns);
    }
    if (purpose == PURPOSE_CONFIRM)
    {
        confirmed();
        return;
    }
    if (debug_enabled(DEBUG_RTC))
    {
        printf("RTC: second found to ");
        utc_print_ns((int64_t)state.resolution_ticks * NS_PER_TICK, false);
        if (state.offset_valid)
        {
            printf(", ");
            utc_print_ns((int64_t)state.offset_ms * 1000000LL, true);
            printf(" against the clock");
        }
        if (state.rate_valid)
        {
            printf(", rate ");
            print_rate();
        }
        printf("\n");
    }
    if (purpose != PURPOSE_CHECK)
    {
        // A rate learnt may bring the next check forward.
        if (state.rate_valid && state.last_write_valid)
        {
            check_by(state.last_write_epoch + source_rtc_check_interval());
        }
        return;
    }
    state.next_check_epoch = epoch_sequence + source_rtc_check_interval();
    if (synchronised && state.offset_valid &&
        (!rtc->time_valid || state.offset_ms > SOURCE_RTC_RESYNC_MS ||
         state.offset_ms < -SOURCE_RTC_RESYNC_MS))
    {
        state.attempts = 0;
        write_wanted = true;
    }
}


// --------------------------------------------------------------------------
// Interface
// --------------------------------------------------------------------------

void source_rtc_init(void)
{
    memset(&state, 0, sizeof(state));
    second_fraction_q8 = 0;
    oscillator_q16 = 0;
    state.next_check_epoch = SOURCE_RTC_CHECK_EPOCHS;
    epoch_sequence = 0;
    epoch_valid = false;
    search_done = false;
    write_wanted = false;
    plain_wanted = false;
    measure_due_epoch = 0;
    was_synchronised = false;
    corrections_seen = 0;
    stopped_released = false;
    set_by_command = false;
    reads_dated = 0;
    set_activity(SOURCE_RTC_IDLE);
}


void source_rtc_set_oscillator(int32_t offset_q16)
{
    oscillator_q16 = offset_q16;
}


void source_rtc_kept(SourceRtcKept *kept)
{
    memset(kept, 0, sizeof(*kept));
    kept->anchor = state.anchor;
    kept->rate_valid = state.rate_valid;
    kept->rate_q8 = state.rate_q8;
    kept->last_write_valid = state.last_write_valid;
    kept->last_write_aligned = state.last_write_aligned;
    kept->last_write_utc = state.last_write_utc;
}


void source_rtc_restore(const SourceRtcKept *kept)
{
    int64_t limit = (int64_t)SOURCE_RTC_RATE_LIMIT_PPM * (int64_t)SOURCE_RTC_TICKS_PER_US * 256;

    state.anchor = kept->anchor;
    state.rate_valid = kept->rate_valid && kept->rate_q8 <= limit && kept->rate_q8 >= -limit;
    state.rate_q8 = state.rate_valid ? kept->rate_q8 : 0;
    state.last_write_valid = kept->last_write_valid;
    state.last_write_aligned = kept->last_write_aligned;
    state.last_write_utc = kept->last_write_utc;
    state.restored = true;
}


bool source_rtc_reckoning(uint32_t *utc_seconds)
{
    return current_label(utc_seconds);
}


bool source_rtc_bound(uint32_t *bound_ns)
{
    uint32_t label;

    if (!current_label(&label))
    {
        return false;
    }
    *bound_ns = bound_at(label);
    return true;
}


void source_rtc_report(const TimingSnapshot *snapshot, uint32_t now_ms, SyncSourceReport *report)
{
    const Pcf8563Status *rtc = pcf8563_status();
    const PpsCaptureSample *capture = &snapshot->capture;
    uint8_t steps = 0;

    int64_t offset_ns;
    uint32_t bound_ns;

    (void)now_ms;
    memset(report, 0, sizeof(*report));
    report->present = rtc->present;
    report->time_valid = rtc->time_valid && !writing();
    report->uncertainty_ns = SOURCE_RTC_UNCERTAINTY_NS;
    if (!state.second_valid || !capture->epoch_capture_valid)
    {
        uint32_t label;
        if (current_label(&label))
        {
            report->uncertainty_ns = bound_at(label);
        }
        return;
    }
    // Counted on, at its rate, to the latest increment at or before our epoch.
    for (;;)
    {
        int32_t fraction = second_fraction_q8;
        uint32_t length = next_second(&fraction);

        if ((int32_t)(capture->epoch_capture - (state.second_stamp + length)) < 0)
        {
            break;
        }
        second_fraction_q8 = fraction;
        state.second_stamp += length;
        state.second_label++;
        state.second_edges++;
        if (++steps > 3U)
        {
            state.second_valid = false; // lost track: find it again
            return;
        }
    }
    if (writing())
    {
        return;
    }
    report->edge = true;
    report->edge_sequence = state.second_edges;
    report->edge_stamp = state.second_stamp;
    report->labelled = true;
    report->utc_seconds = state.second_label;
    report->uncertainty_ns = bound_at(state.second_label);
    if (predict(state.second_label, &offset_ns, &bound_ns))
    {
        // The UTC second nearest its start, and how far from it.
        int64_t seconds = floor_div64(offset_ns + NS_PER_SECOND / 2, NS_PER_SECOND);

        report->utc_seconds = state.second_label + (uint32_t)(int32_t)seconds;
        report->edge_offset_ns = (int32_t)(offset_ns - seconds * NS_PER_SECOND);
    }
}


void source_rtc_epoch(const TimingSnapshot *snapshot, const SyncTime *time, bool synchronised,
                      uint32_t bound_ns)
{
    const Pcf8563Status *rtc = pcf8563_status();

    synchronised = synchronised && time->known && !time->leap_second;

    epoch_sequence = snapshot->timer.sequence;
    epoch_valid = snapshot->capture.epoch_capture_valid;
    epoch_stamp = snapshot->capture.epoch_capture;

    if (rtc->reads != reads_dated)
    {
        reads_dated = rtc->reads;
        if (time->known)
        {
            state.last_read_valid = true;
            state.last_read_utc = time->utc_seconds;
        }
    }

    if (state.second_valid &&
        epoch_sequence - state.measured_epoch > SOURCE_RTC_SECOND_MAX_AGE_EPOCHS)
    {
        state.second_valid = false;
    }

    // A search that ran out of time; a write step that never finished.
    if ((state.activity == SOURCE_RTC_SEARCHING || state.activity == SOURCE_RTC_CONFIRMING) &&
        epoch_reached(search_deadline))
    {
        end_search(false);
    }
    else if ((state.activity == SOURCE_RTC_LOADING || state.activity == SOURCE_RTC_WAITING ||
              state.activity == SOURCE_RTC_RELEASING) &&
             epoch_reached(activity_epoch + 4U))
    {
        abort_write();
    }
    if (search_done)
    {
        search_result(time, synchronised, bound_ns);
    }

    // Check soon after the clock is synchronised, or its time corrected.
    if (synchronised && (!was_synchronised || time->corrections != corrections_seen))
    {
        check_by(epoch_sequence + SOURCE_RTC_SETTLE_EPOCHS);
    }
    was_synchronised = synchronised;
    corrections_seen = time->corrections;

    if (state.activity != SOURCE_RTC_IDLE || !rtc->present || pcf8563_busy())
    {
        return;
    }
    if (plain_wanted)
    {
        plain_wanted = false;
        state.attempts = 0;
        state.next_check_epoch = epoch_sequence + SOURCE_RTC_CHECK_EPOCHS;
        if (synchronised && pcf8563_write(time->utc_seconds))
        {
            wrote_plainly(time->utc_seconds);
            state.plain_writes++;
            state.second_valid = false;
            state.baseline_valid = false;
            if (debug_enabled(DEBUG_RTC))
            {
                printf("RTC: aligned write failed: set to the nearest second\n");
            }
        }
        return;
    }
    if (write_wanted)
    {
        if (synchronised && epoch_valid)
        {
            begin_write(time);
        }
        else if (!synchronised)
        {
            write_wanted = false; // no time to give it: the next check decides
            state.attempts = 0;
        }
        return;
    }
    // Stopped, with no write of ours to follow: an aligned write cut short
    // (by a reset, or by losing synchronisation between attempts).
    // Let it run; its time is then stale by how long it was stopped, and
    // it is set at the next check once there is a time to give it.
    if (rtc->calendar_valid && rtc->stopped)
    {
        if (!stopped_released)
        {
            stopped_released = pcf8563_release();
            state.second_valid = false;
            if (state.anchor.valid)
            {
                rtc_changed(); // stopped for who knows how long
            }
            if (stopped_released && debug_enabled(DEBUG_RTC))
            {
                printf("RTC: found stopped: released\n");
            }
        }
        else
        {
            stopped_released = !pcf8563_read(); // see whether it runs now
        }
        return;
    }
    if (synchronised && epoch_reached(state.next_check_epoch))
    {
        state.checks++;
        if (!rtc->time_valid)
        {
            state.attempts = 0;
            write_wanted = true; // nothing to measure: set it
            return;
        }
        start_search(PURPOSE_CHECK, SOURCE_RTC_SEARCH_EPOCHS);
        return;
    }
    if (state.rate_due && epoch_reached(state.rate_due_epoch) && rtc->time_valid)
    {
        state.rate_due = false;
        start_search(PURPOSE_MEASURE, SOURCE_RTC_SEARCH_EPOCHS);
        return;
    }
    if (!state.second_valid && rtc->time_valid && epoch_reached(measure_due_epoch))
    {
        start_search(PURPOSE_MEASURE, SOURCE_RTC_SEARCH_EPOCHS);
    }
}


void source_rtc_poll(void)
{
    const Pcf8563Status *rtc = pcf8563_status();

    switch (state.activity)
    {
        case SOURCE_RTC_IDLE:
            // At start-up, as soon as the chip has been read: first.
            if (state.searches == 0U && !state.second_valid && rtc->reads != 0U &&
                rtc->time_valid && !pcf8563_busy())
            {
                start_search(PURPOSE_MEASURE, SOURCE_RTC_SEARCH_EPOCHS);
            }
            break;
        case SOURCE_RTC_SEARCHING:
        case SOURCE_RTC_CONFIRMING: keep_searching(); break;
        case SOURCE_RTC_LOADING:
            if (!pcf8563_busy())
            {
                if (rtc->writes != writes_seen)
                {
                    set_activity(SOURCE_RTC_WAITING);
                }
                else
                {
                    abort_write();
                }
            }
            break;
        case SOURCE_RTC_WAITING: wait_release(); break;
        case SOURCE_RTC_RELEASING:
            if (!pcf8563_busy())
            {
                if (rtc->releases != releases_seen)
                {
                    start_search(PURPOSE_CONFIRM, SOURCE_RTC_SEARCH_EPOCHS);
                }
                else
                {
                    abort_write();
                }
            }
            break;
        default: break;
    }
}


bool source_rtc_set(uint32_t utc_seconds)
{
    if (state.activity != SOURCE_RTC_IDLE || !pcf8563_write(utc_seconds))
    {
        return false;
    }
    wrote_plainly(utc_seconds);
    state.plain_writes++;
    state.second_valid = false; // found again once it is read back
    state.baseline_valid = false;
    measure_due_epoch = epoch_sequence;
    write_wanted = false;
    plain_wanted = false;
    set_by_command = true;
    return true;
}


bool source_rtc_take_set(void)
{
    bool set = set_by_command;

    set_by_command = false;
    return set;
}


const SourceRtcState *source_rtc_state(void)
{
    return &state;
}


const char *source_rtc_activity_name(SourceRtcActivity activity)
{
    switch (activity)
    {
        case SOURCE_RTC_IDLE: return "IDLE";
        case SOURCE_RTC_SEARCHING: return "SEARCHING";
        case SOURCE_RTC_LOADING: return "LOADING";
        case SOURCE_RTC_WAITING: return "WAITING";
        case SOURCE_RTC_RELEASING: return "RELEASING";
        case SOURCE_RTC_CONFIRMING: return "CONFIRMING";
        default: return "?";
    }
}


// Its bound now, the drift predicted since its anchor and where that is.
static void print_bound(void)
{
    uint32_t label;
    int64_t offset_ns;
    uint32_t bound_ns;

    printf("  Bound: ");
    if (!current_label(&label))
    {
        printf("NO TIME\n");
        return;
    }
    printf("+/-");
    utc_print_ns(bound_at(label), false);
    if (!predict(label, &offset_ns, &bound_ns))
    {
        printf(", NOT MEASURED\n");
        return;
    }
    printf("; second ");
    utc_print_ns(offset_ns, true);
    printf(" from UTC's, from ");
    utc_print(state.anchor.utc_seconds);
    printf(state.anchor.aligned ? "\n" : " (PLAIN)\n");
}


void source_rtc_print(bool verbose)
{
    int32_t next_check = (int32_t)(state.next_check_epoch - epoch_sequence);

    printf("  Second: ");
    if (state.second_valid)
    {
        utc_print_duration(epoch_sequence - state.measured_epoch);
        printf(" ago, +/-");
        utc_print_ns((int64_t)state.resolution_ticks * NS_PER_TICK, false);
        if (state.offset_valid)
        {
            printf(", ");
            utc_print_ns((int64_t)state.offset_ms * 1000000LL, true);
        }
    }
    else
    {
        printf("NOT FOUND");
    }
    printf("; rate ");
    if (state.rate_valid)
    {
        print_rate();
    }
    else
    {
        printf("unknown");
    }
    if (state.activity != SOURCE_RTC_IDLE)
    {
        printf("; %s", source_rtc_activity_name(state.activity));
    }
    printf("\n  Set: ");
    if (state.last_write_valid)
    {
        utc_print(state.last_write_utc);
        if (state.last_write_aligned)
        {
            printf(", ");
            print_ticks(state.last_write_error_ticks);
        }
        else
        {
            printf(", PLAIN");
        }
    }
    else
    {
        printf("never");
    }
    printf("; next check ");
    if (next_check > 0)
    {
        utc_print_duration((uint32_t)next_check);
    }
    else
    {
        printf("when synchronised");
    }
    printf("\n");
    print_bound();
    if (verbose)
    {
        printf("  Start delay correction ");
        print_ticks(state.release_correction_ticks);
        printf("; %lu rate updates, %lu searches (%lu failed), %lu checks, %lu set (%lu failed, "
               "%lu late), %lu plain\n",
               (unsigned long)state.rate_updates, (unsigned long)state.searches,
               (unsigned long)state.searches_failed, (unsigned long)state.checks,
               (unsigned long)state.writes, (unsigned long)state.writes_failed,
               (unsigned long)state.late_releases, (unsigned long)state.plain_writes);
    }
}
