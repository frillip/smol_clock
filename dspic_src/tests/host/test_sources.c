/*
 * Closed-loop host test of source selection between GNSS, NTP and holdover:
 * sync/ with the real epoch control loop and the real edge pairing
 * (source_edge.c), against a model of both PPS inputs.
 *
 *   - The oscillator runs 1.9 ppm slow, as in test_sync.c.
 *   - GNSS edges are on the true second; the ESP's are 30 us late (the
 *     ESP's NTP time error), plus 10 us that the ESP knows about and reports
 *     as utc_phase_ns, plus +/-3 us of jitter it does not.
 *   - Both are watched, with 1-1.5 us of interrupt latency; the one routed
 *     to IC1/IC2 is also captured, to the tick below. Routing follows the
 *     select hooks, and a change drops the capture in flight, as
 *     pps_capture_route() does.
 *
 * The GNSS clock: GNSS is taken up first. It is lost for 600 s with NTP at
 * +/-2 ms: holdover stays better throughout. Then, with NTP at +/-20 us, it
 * is lost for longer: holdover is kept until NTP beats it by 2x, the
 * capture is rerouted and the loop slews onto NTP without a step. GNSS
 * returns and takes over again.
 *
 * The NTP-only clock: no receiver, the ESP at +/-11 ms. NTP is taken up, the capture rerouted,
 * and the loop steps onto it and tracks it.
 *
 * The RTC at start-up: its second 0.37 s off the true one, reported as
 * source_rtc.c does (a watched edge, never captured). The time of day is
 * taken from it and the epoch put on its second while free running; the
 * RTC is never selected; NTP takes over when it comes, and the clock steps
 * onto NTP. Then with NTP there from the first epoch and the RTC's second
 * found at the second: NTP waits for the RTC to place the epoch first; and
 * an RTC whose second is never found holds NTP up only a few epochs.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../sync/source_edge.h"
#include "../../sync/source_gnss.h"
#include "../../sync/source_ntp.h"
#include "../../sync/source_rtc.h"
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

// ---- Epoch timer stand-in ----------------------------------------------
static uint32_t requested_period;
static bool request_pending;

void epoch_timer_set_period(uint32_t period_ticks)
{
    requested_period = period_ticks;
    request_pending = true;
}


// ---- The model ------------------------------------------------------------
#define TRUE_SECOND 39999923.75
#define UTC_ORIGIN  1798761000LL // label of true second 0
#define NTP_ERROR   1200.0       // ticks: the ESP's time is 30 us late
static double ntp_step;          // ticks: a correction the ESP has made since
static bool ntp_stepped;         // ... reported this epoch
#define NTP_KNOWN     400.0      // ticks: 10 us of edge offset it reports
#define NTP_JITTER    120.0      // ticks: +/-3 us it does not
#define WATCH_LATENCY 40.0       // ticks, plus up to 20 more

static uint32_t random_state = 12345U;
static double uniform(void) // -1 .. 1
{
    random_state = random_state * 1103515245U + 12345U;
    return ((double)((random_state >> 8) & 0xFFFFU) / 32768.0) - 1.0;
}


// What each source is doing this epoch.
typedef struct
{
    bool gnss;          // receiver present with time and PPS
    uint32_t gnss_tacc; // ns
    bool ntp;           // ESP sending TIME-NTP and PPS
    uint32_t ntp_unc;   // ns
    bool rtc;           // RTC present, its time valid
    bool rtc_second;    // ... and its second found
} Script;

static Script now;
static PpsInput route = PPS_INPUT_GNSS;
static bool route_changed;
static int gnss_selections, ntp_selections;

// Edge sequence -> true second, per input.
#define MAX_EDGES 4100
static long second_of[PPS_INPUT_COUNT][MAX_EDGES];

// ---- Scripted adapters, with the real edge pairing --------------------------
void source_gnss_report(const TimingSnapshot *snapshot, uint32_t now_ms, SyncSourceReport *report)
{
    (void)now_ms;
    memset(report, 0, sizeof(*report));
    report->present = now.gnss;
    report->time_valid = now.gnss;
    report->uncertainty_ns = now.gnss ? now.gnss_tacc : 0U;
    source_edge_fill(snapshot, PPS_INPUT_GNSS, report);
    report->labelled = report->edge;
    if (report->labelled)
    {
        report->utc_seconds =
            (uint32_t)(UTC_ORIGIN + second_of[PPS_INPUT_GNSS][report->edge_sequence]);
    }
}


void source_gnss_select(void)
{
    gnss_selections++;
    route_changed = route != PPS_INPUT_GNSS;
    route = PPS_INPUT_GNSS;
}


void source_ntp_report(const TimingSnapshot *snapshot, uint32_t now_ms, SyncSourceReport *report)
{
    (void)now_ms;
    memset(report, 0, sizeof(*report));
    report->present = now.ntp;
    report->time_valid = now.ntp;
    report->uncertainty_ns = now.ntp ? now.ntp_unc : 0U;
    source_edge_fill(snapshot, PPS_INPUT_ESP, report);
    report->labelled = report->edge;
    if (report->labelled)
    {
        report->utc_seconds =
            (uint32_t)(UTC_ORIGIN + second_of[PPS_INPUT_ESP][report->edge_sequence]);
        report->edge_offset_ns = (int32_t)(NTP_KNOWN * 25.0);
    }
    report->stepped = ntp_stepped;
}


void source_ntp_select(void)
{
    ntp_selections++;
    route_changed = route != PPS_INPUT_ESP;
    route = PPS_INPUT_ESP;
}


// The RTC: its increments RTC_PHASE ticks after the true second, labelled
// with the true second's label (it holds UTC to the second).
#define RTC_PHASE 14800000.0 // 0.37 s
static double rtc_origin;    // the model's pps_origin, for the RTC
// Its bound, and the offset it reports for its edge (its drift predicted):
// its increments then mark the true second rtc_offset_ns before them.
static uint32_t rtc_bound_ns = SOURCE_RTC_UNCERTAINTY_NS;
static int32_t rtc_offset_ns;
void source_rtc_report(const TimingSnapshot *snapshot, uint32_t now_ms, SyncSourceReport *report)
{
    long j;

    (void)now_ms;
    memset(report, 0, sizeof(*report));
    report->present = now.rtc;
    report->time_valid = now.rtc;
    report->uncertainty_ns = rtc_bound_ns;
    report->edge_offset_ns = rtc_offset_ns;
    if (!now.rtc || !now.rtc_second || !snapshot->capture.epoch_capture_valid)
    {
        return;
    }
    // The latest increment at or before our epoch.
    j = (long)floor(((double)snapshot->capture.epoch_capture - rtc_origin - RTC_PHASE) /
                    TRUE_SECOND);
    report->edge = true;
    report->edge_sequence = (uint32_t)(j + 1000);
    report->edge_stamp = (uint32_t)(int64_t)floor(rtc_origin + RTC_PHASE + j * TRUE_SECOND);
    report->labelled = true;
    report->utc_seconds = (uint32_t)(UTC_ORIGIN + j);
}


// ---- The run -------------------------------------------------------------
typedef struct
{
    double pps_origin;
    double epoch;
    uint32_t running;
    uint32_t sequence;
    // Per input: the latest edge index seen, the edge count.
    long last_second[PPS_INPUT_COUNT];
    uint32_t edges[PPS_INPUT_COUNT];
    // The capture.
    bool have_previous;
    int64_t previous_capture;
    long previous_second;
    uint32_t captures;
    uint32_t periods;
} Model;

static void model_init(Model *m)
{
    memset(m, 0, sizeof *m);
    m->pps_origin = 23754758.3;
    m->epoch = 40167998.0;
    rtc_origin = m->pps_origin;
    now.rtc = false;
    m->running = TIMING_NOMINAL_TICKS;
    m->last_second[0] = m->last_second[1] = -1;
    route = PPS_INPUT_GNSS;
    route_changed = false;
    ntp_step = 0.0;
    ntp_stepped = false;
    gnss_selections = ntp_selections = 0;
}


// The time of input's edge for true second i, ticks.
static double edge_time(const Model *m, PpsInput input, long i, double jitter)
{
    double t = m->pps_origin + i * TRUE_SECOND;
    return input == PPS_INPUT_ESP ? t + NTP_ERROR + ntp_step + NTP_KNOWN + jitter : t;
}


// One epoch's snapshot. Returns the true error of our epoch, ticks: our
// epoch minus the nearest true second.
static double model_epoch(Model *m, TimingSnapshot *snap)
{
    long nearest;
    uint8_t input;

    if (request_pending)
    {
        m->running = requested_period;
        request_pending = false;
    }
    m->sequence++;
    if (route_changed)
    {
        // pps_capture_route(): the capture in flight and its period go.
        m->have_previous = false;
        route_changed = false;
    }
    memset(snap, 0, sizeof *snap);
    snap->timer.sequence = m->sequence;
    snap->timer.running_period = m->running;
    snap->capture.reference_input = (uint8_t)route;
    snap->capture.epoch_capture = (uint32_t)(int64_t)m->epoch;
    snap->capture.epoch_capture_valid = true;

    for (input = 0; input < PPS_INPUT_COUNT; input++)
    {
        bool on = input == PPS_INPUT_GNSS ? now.gnss : now.ntp;
        double jitter = input == PPS_INPUT_ESP ? NTP_JITTER * uniform() : 0.0;
        // The latest edge before the measurement slot (1 ms after the epoch).
        long i = (long)floor((m->epoch + 40000.0 - m->pps_origin -
                              (input == PPS_INPUT_ESP ? NTP_ERROR + ntp_step + NTP_KNOWN : 0.0)) /
                             TRUE_SECOND);
        double t;
        int64_t captured;

        if (!on || i == m->last_second[input])
        {
            continue;
        }
        m->last_second[input] = i;
        t = edge_time(m, (PpsInput)input, i, jitter);
        captured = (int64_t)floor(t);
        m->edges[input]++;
        second_of[input][m->edges[input]] = i;
        snap->monitor.inputs[input].valid = true;
        snap->monitor.inputs[input].sequence = m->edges[input];
        snap->monitor.inputs[input].stamp =
            (uint32_t)(captured + (int64_t)(WATCH_LATENCY + 10.0 + 10.0 * uniform()));

        if (input == route)
        {
            snap->capture.reference_capture_valid = true;
            snap->capture.reference_capture = (uint32_t)captured;
            snap->capture.reference_edge_sequence = ++m->captures;
            if (m->have_previous && i == m->previous_second + 1)
            {
                snap->capture.reference_period_valid = true;
                snap->capture.reference_period = (uint32_t)(captured - m->previous_capture);
                snap->capture.reference_period_sequence = ++m->periods;
            }
            m->have_previous = true;
            m->previous_capture = captured;
            m->previous_second = i;
        }
    }
    nearest = lround((m->epoch - m->pps_origin) / TRUE_SECOND);
    return m->epoch - (m->pps_origin + nearest * TRUE_SECOND);
}


static void model_advance(Model *m)
{
    m->epoch += (double)m->running;
}


static void print_transition(const SyncUpdate *update)
{
    if (update->transitioned)
    {
        printf("  ");
        sync_report_transition(update);
        printf("  ");
        sync_report_sources();
    }
}


// ---- The GNSS clock ----------------------------------------------------------
static Script gnss_clock_script(long k)
{
    Script s = {true, 30U, true, 2000000U, false, false};

    if (k >= 300 && k < 900)
    {
        s.gnss = false; // lost: NTP at 2 ms never beats holdover
    }
    if (k >= 1000)
    {
        s.ntp_unc = 20000U; // NTP at 20 us: bound 40 us
    }
    if (k >= 1100 && k < 3600)
    {
        s.gnss = false; // lost for longer: NTP takes over
    }
    return s;
}


static void test_gnss_clock(void)
{
    Model m;
    long k;
    long first_locked = -1, ntp_from = -1, gnss_back = -1;
    bool ntp_during_first_loss = false, left_holdover_early = false;
    double worst_step_ticks = 0.0, worst_gnss = 0.0, ntp_sum = 0.0, ntp_worst = 0.0;
    long ntp_n = 0, synchronised = 0, bounded = 0;
    const SyncStatus *status;

    printf("GNSS clock, with NTP from the ESP\n");
    model_init(&m);
    sync_init();
    for (k = 0; k < 4000; k++)
    {
        TimingSnapshot snap;
        SyncUpdate update;
        double error;

        now = gnss_clock_script(k);
        error = model_epoch(&m, &snap);
        update = sync_update(&snap, (uint32_t)k * 1000U);
        print_transition(&update);
        status = sync_status();

        if (status->state == SYNC_LOCKED && first_locked < 0)
            first_locked = k;
        if (sync_is_synchronised())
        {
            uint32_t bound;
            synchronised++;
            if (sync_uncertainty(&bound) && fabs(error) * 25.0 <= (double)bound)
                bounded++;
        }
        if (k >= 300 && k < 900)
        {
            if (status->source == SYNC_SOURCE_NTP)
                ntp_during_first_loss = true;
            if (status->state != SYNC_HOLDOVER)
                left_holdover_early = true;
        }
        if (k >= 1100 && status->source == SYNC_SOURCE_NTP && ntp_from < 0)
            ntp_from = k;
        // No step on the handover to NTP: every period within the slew limit.
        if (ntp_from >= 0 && k >= ntp_from && k < ntp_from + 300)
        {
            double off = fabs((double)m.running - TRUE_SECOND);
            if (off > worst_step_ticks)
                worst_step_ticks = off;
        }
        if (k >= 3400 && k < 3600 && status->state == SYNC_LOCKED &&
            status->source == SYNC_SOURCE_NTP)
        {
            ntp_sum += error;
            ntp_n++;
            if (fabs(error - NTP_ERROR) > ntp_worst)
                ntp_worst = fabs(error - NTP_ERROR);
        }
        if (k >= 3600 && status->state == SYNC_LOCKED && status->source == SYNC_SOURCE_GNSS &&
            gnss_back < 0)
            gnss_back = k;
        if (gnss_back >= 0 && k > gnss_back + 20 && fabs(error) > worst_gnss)
            worst_gnss = fabs(error);
        model_advance(&m);
    }
    sync_report_status(true);
    status = sync_status();

    printf("  locked at %ld; NTP from %ld, slewed in at up to %.0f ticks/s; on NTP %.1f ticks "
           "late on average (+/-%.1f), GNSS back at %ld, then within %.1f ticks\n",
           first_locked, ntp_from, worst_step_ticks, ntp_n ? ntp_sum / ntp_n : 0.0, ntp_worst,
           gnss_back, worst_gnss);
    CHECK(first_locked > 0 && first_locked < 15);
    // Whenever synchronised, the bound given covers the true error.
    CHECK(synchronised > 2900 && bounded == synchronised);
    // GNSS lost for 600 s with NTP at 2 ms: holdover all the way.
    CHECK(!ntp_during_first_loss && !left_holdover_early);
    // Lost again with NTP at 40 us: holdover until its bound passes 80 us
    // (~1770 s with the bench model), then NTP.
    CHECK(ntp_from > 1100 + 1700 && ntp_from < 1100 + 1850);
    // Slewed, not stepped: no period further from the true second than the
    // NTP profile's correction limit allows (plus the frequency error).
    CHECK(worst_step_ticks < (double)SYNC_NTP_LIMIT_TICKS + 100.0);
    // On NTP: the epoch follows the ESP's corrected edge, 30 us late. The
    // slew of 33 us onto it overshoots by ~12% (a critically damped PI loop
    // does) and settles over minutes: still within 3 us 8 minutes on.
    CHECK(ntp_n > 190 && fabs(ntp_sum / ntp_n - NTP_ERROR) < 120.0 && ntp_worst < 200.0);
    // GNSS back: 2x better than NTP for 10 epochs after qualifying, then
    // stepped onto.
    CHECK(gnss_back > 3600 && gnss_back < 3630);
    CHECK(worst_gnss < 3.0);
    CHECK(status->state == SYNC_LOCKED && status->source == SYNC_SOURCE_GNSS);
    CHECK(route == PPS_INPUT_GNSS);
    CHECK(gnss_selections == 2 && ntp_selections == 1);
}


// ---- The NTP-only clock ------------------------------------------------------
static void test_ntp_clock(void)
{
    Model m;
    long k;
    long first_locked = -1;
    double worst = 0.0, sum = 0.0, excursion = 0.0;
    long n = 0;
    const SyncStatus *status;

    printf("NTP-only clock\n");
    model_init(&m);
    sync_init();
    for (k = 0; k < 2400; k++)
    {
        TimingSnapshot snap;
        SyncUpdate update;
        double error;

        now.gnss = false;
        now.gnss_tacc = 0;
        now.ntp = true;
        now.ntp_unc = 11000000U; // as a bench ESP reports over Wi-Fi
        error = model_epoch(&m, &snap);
        update = sync_update(&snap, (uint32_t)k * 1000U);
        print_transition(&update);
        status = sync_status();
        if (status->state == SYNC_LOCKED && first_locked < 0)
            first_locked = k;
        if (first_locked >= 0 && k > first_locked + 2 && fabs(error - NTP_ERROR) > excursion)
            excursion = fabs(error - NTP_ERROR);
        if (k >= 1800)
        {
            sum += error;
            n++;
            if (fabs(error - NTP_ERROR) > worst)
                worst = fabs(error - NTP_ERROR);
        }
        model_advance(&m);
    }
    status = sync_status();
    printf("  locked at %ld; after 30 minutes %.1f ticks late on average, within +/-%.1f of "
           "the ESP's time; servo integral %.1f ticks\n",
           first_locked, sum / n, worst, epoch_control_state()->pi.integral_q16 / 65536.0);
    printf("  Kalman seeded at %.2f ticks (true -76.25), worst excursion after lock %.0f ticks\n",
           kalman_phase_q16(&epoch_control_state()->pps_kf) / 65536.0, excursion);
    // Seeding takes 16 periods, then the step.
    CHECK(first_locked > 16 && first_locked < 30);
    CHECK(status->state == SYNC_LOCKED && status->source == SYNC_SOURCE_NTP);
    CHECK(route == PPS_INPUT_ESP && ntp_selections == 1 && gnss_selections == 0);
    CHECK(fabs(sum / n - NTP_ERROR) < 20.0 && worst < 60.0);
    // The Kalman filter was seeded once from the ESP's average period, to
    // within its jitter; the servo carries only what is left.
    CHECK(epoch_control_state()->pps_kf.initialized);
    CHECK(fabs(kalman_phase_q16(&epoch_control_state()->pps_kf) / 65536.0 + 76.25) < 16.0);
    CHECK(fabs(epoch_control_state()->pi.integral_q16 / 65536.0) < 16.0);
    // So no excursion of offset / Kp (76 x 64 = 4900 ticks) after locking.
    CHECK(excursion < 1500.0);
}


// ---- An ESP that only knows its time roughly -----------------------------------
// Beyond even the NTP hold limit: still a clock. The time of day is set at
// once, NTP is taken as a fallback and steered by, but the clock does not
// call itself synchronised until the ESP's bound comes down.
static void test_coarse_ntp(void)
{
    Model m;
    long k;
    long time_known = -1, first_locked = -1, synchronised_from = -1;
    long wrong_labels = 0;
    const SyncStatus *status;

    printf("NTP-only clock, ESP at +/-150 ms, then +/-11 ms\n");
    model_init(&m);
    sync_init();
    for (k = 0; k < 400; k++)
    {
        TimingSnapshot snap;
        SyncUpdate update;
        double error;
        long nearest;

        now.gnss = false;
        now.gnss_tacc = 0;
        now.ntp = true;
        now.ntp_unc = k < 200 ? 150000000U : 11000000U;
        error = model_epoch(&m, &snap);
        update = sync_update(&snap, (uint32_t)k * 1000U);
        print_transition(&update);
        status = sync_status();
        nearest = lround((m.epoch - m.pps_origin) / TRUE_SECOND);
        if (status->time.known && time_known < 0)
            time_known = k;
        if (status->time.known && status->state == SYNC_LOCKED &&
            status->time.utc_seconds != (uint32_t)(UTC_ORIGIN + nearest))
            wrong_labels++;
        if (status->state == SYNC_LOCKED && first_locked < 0)
            first_locked = k;
        if (k < 200 && status->state == SYNC_LOCKED)
            CHECK(status->fallback && !sync_is_synchronised());
        if (sync_is_synchronised() && synchronised_from < 0)
            synchronised_from = k;
        (void)error;
        model_advance(&m);
    }
    status = sync_status();
    printf("  time known at %ld, locked at %ld, synchronised from %ld, %ld wrong labels\n",
           time_known, first_locked, synchronised_from, wrong_labels);
    // The time from the first labelled edge, before NTP has qualified.
    CHECK(time_known == 0);
    CHECK(first_locked > 16 && first_locked < 30);
    CHECK(synchronised_from == 200);
    CHECK(wrong_labels == 0);
    CHECK(status->state == SYNC_LOCKED && status->source == SYNC_SOURCE_NTP && !status->fallback);
}


// ---- An NTP correction --------------------------------------------------------
// The ESP corrects its time by 150 us (as a forced resync on the bench did)
// while the NTP-only clock tracks it. With the correction reported, the
// loop works the jump off without taking it for frequency: no overshoot,
// the integral (the frequency) untouched.
typedef struct
{
    double overshoot;       // ticks past the new phase, the other way
    double integral_change; // ticks, largest from before the step
    long settled;           // s from the step until within 10 us for good
} StepResult;

static StepResult ntp_correction(bool reported)
{
    Model m;
    long k, last_far = -1;
    double integral_before = 0.0;
    StepResult r = {0.0, 0.0, 0};
    const double step = 6000.0; // 150 us later

    model_init(&m);
    sync_init();
    for (k = 0; k < 1800; k++)
    {
        TimingSnapshot snap;
        SyncUpdate update;
        double error, integral;

        now.gnss = false;
        now.gnss_tacc = 0;
        now.ntp = true;
        now.ntp_unc = 11000000U;
        ntp_step = k >= 900 ? step : 0.0;
        ntp_stepped = reported && k >= 899 && k < 902; // CORRECTING while it lasts
        error = model_epoch(&m, &snap);
        update = sync_update(&snap, (uint32_t)k * 1000U);
        (void)update;
        integral = epoch_control_state()->pi.integral_q16 / 65536.0;
        if (k == 899)
            integral_before = integral;
        if (k >= 900)
        {
            double off = error - (NTP_ERROR + step);
            if (off > r.overshoot)
                r.overshoot = off;
            if (fabs(integral - integral_before) > r.integral_change)
                r.integral_change = fabs(integral - integral_before);
            if (fabs(off) > 400.0)
                last_far = k;
        }
        model_advance(&m);
    }
    r.settled = last_far + 1 - 900;
    CHECK(sync_status()->state == SYNC_LOCKED && sync_status()->transitions == 2);
    return r;
}


static void test_ntp_correction(void)
{
    StepResult with = ntp_correction(true);
    StepResult without = ntp_correction(false);

    printf(
        "NTP correction of 150 us: reported: overshoot %.0f ticks, integral moved %.1f, within 10 "
        "us after %ld s; unreported: %.0f, %.1f, %ld s\n",
        with.overshoot, with.integral_change, with.settled, without.overshoot,
        without.integral_change, without.settled);
    CHECK(with.overshoot < 150.0 && with.integral_change < 2.0);
    CHECK(with.settled < 300);
    CHECK(without.overshoot > 2.0 * with.overshoot);
}


// ---- The RTC at start-up -------------------------------------------------------
/* An RTC whose bound is past SYNC_RTC_LIMIT_NS (long off, its drift
 * predicted from what was kept): INACCURATE, but it still gives the time
 * and places the epoch, on the second it predicts (200 ms before its
 * increment); the time then has no bound. */
static void test_rtc_drifted(void)
{
    Model m;
    long k;
    long aligned_at = -1;
    double worst = 0.0;
    bool labels_right = true;
    bool bounded = false;
    const SyncStatus *status;
    uint32_t uncertainty;

    printf("RTC at start-up, bound 2.5 s, 200 ms of drift predicted\n");
    rtc_bound_ns = 2500000000UL;
    rtc_offset_ns = 200000000L;
    model_init(&m);
    sync_init();
    for (k = 0; k < 40; k++)
    {
        TimingSnapshot snap;
        double from_second;
        long nearest;

        now.gnss = false;
        now.gnss_tacc = 0;
        now.rtc = k >= 2;
        now.rtc_second = true;
        now.ntp = false;
        (void)model_epoch(&m, &snap);
        (void)sync_update(&snap, (uint32_t)k * 1000U);
        status = sync_status();
        if (status->aligned && aligned_at < 0)
            aligned_at = k;
        // Our epoch against the second the RTC predicts: 200 ms before its.
        nearest = lround((m.epoch - rtc_origin - RTC_PHASE + 8000000.0) / TRUE_SECOND);
        from_second = m.epoch - (rtc_origin + RTC_PHASE - 8000000.0 + nearest * TRUE_SECOND);
        if (aligned_at >= 0 && k >= aligned_at + 2)
        {
            if (fabs(from_second) > worst)
                worst = fabs(from_second);
            if (status->time.utc_seconds != (uint32_t)(UTC_ORIGIN + nearest))
                labels_right = false;
            if (sync_uncertainty(&uncertainty))
                bounded = true;
        }
        model_advance(&m);
    }
    status = sync_status();
    printf("  aligned at %ld; within %.0f ticks of the predicted second\n", aligned_at, worst);
    CHECK(aligned_at == 2 && status->aligned_to == SYNC_SOURCE_RTC);
    CHECK(status->sources[SYNC_SOURCE_RTC].check == SYNC_CHECK_INACCURATE);
    CHECK(worst < 80.0 * 40.0 + 400.0);
    CHECK(labels_right && !bounded && status->time.known);
    rtc_bound_ns = SOURCE_RTC_UNCERTAINTY_NS;
    rtc_offset_ns = 0;
}


static void test_rtc_start(void)
{
    Model m;
    long k;
    long aligned_at = -1;
    double worst_after_align = 0.0;
    double first_after_align = 0.0;
    bool rtc_selected = false;
    bool synchronised_on_rtc = false;
    bool bound_on_rtc = true;
    bool labels_right = true;
    const SyncStatus *status;
    uint32_t uncertainty;

    printf("RTC at start-up, then NTP\n");
    model_init(&m);
    sync_init();
    for (k = 0; k < 400; k++)
    {
        TimingSnapshot snap;
        SyncUpdate update;
        double error;
        double from_rtc;
        long nearest;

        now.gnss = false;
        now.gnss_tacc = 0;
        now.rtc = k >= 2;
        now.rtc_second = true;
        now.ntp = k >= 60;
        now.ntp_unc = 11000000U;
        error = model_epoch(&m, &snap);
        update = sync_update(&snap, (uint32_t)k * 1000U);
        print_transition(&update);
        status = sync_status();
        if (status->source == SYNC_SOURCE_RTC)
            rtc_selected = true;
        // Our epoch against the RTC's nearest increment.
        nearest = lround((m.epoch - rtc_origin - RTC_PHASE) / TRUE_SECOND);
        from_rtc = m.epoch - (rtc_origin + RTC_PHASE + nearest * TRUE_SECOND);
        if (status->aligned && aligned_at < 0)
            aligned_at = k;
        if (aligned_at >= 0 && k == aligned_at + 2)
            first_after_align = from_rtc;
        if (aligned_at >= 0 && k >= aligned_at + 2 && k < 60)
        {
            if (fabs(from_rtc) > worst_after_align)
                worst_after_align = fabs(from_rtc);
            // Free running on the RTC: its label, its bound, never synchronised.
            if (status->time.utc_seconds != (uint32_t)(UTC_ORIGIN + nearest))
                labels_right = false;
            if (!sync_uncertainty(&uncertainty) || uncertainty != SOURCE_RTC_UNCERTAINTY_NS)
                bound_on_rtc = false;
            if (sync_is_synchronised())
                synchronised_on_rtc = true;
        }
        (void)error;
        model_advance(&m);
    }
    status = sync_status();
    printf("  aligned at %ld by %+ld ticks: %.0f ticks from the RTC's second, then within %.0f "
           "free running\n",
           aligned_at, (long)status->aligned_ticks, first_after_align, worst_after_align);
    // Seeded and aligned once the RTC has been GOOD for 3 epochs.
    CHECK(aligned_at == 2); // the first epoch it is GOOD
    CHECK(status->aligned && status->aligned_to == SYNC_SOURCE_RTC);
    // Our epoch started 0.27 s from the RTC's second (40167998 - 23754758
    // - 14800000 ticks): the step is that, the shorter way.
    CHECK(labs((long)status->aligned_ticks + 1613240L) < 400L);
    // On its second at once; then free running, 1.9 ppm slow: 76 ticks a second.
    CHECK(fabs(first_after_align) < 400.0);
    CHECK(worst_after_align < 80.0 * 60.0);
    CHECK(labels_right && bound_on_rtc && !synchronised_on_rtc);
    CHECK(!rtc_selected && sync_candidate_bound(SYNC_SOURCE_RTC) == SYNC_BOUND_NONE);
    CHECK(sync_candidate_rank(SYNC_SOURCE_RTC) == 0);
    // NTP took over, and the epoch was stepped onto it.
    CHECK(status->state == SYNC_LOCKED && status->source == SYNC_SOURCE_NTP);
    CHECK(status->time.source == SYNC_SOURCE_NTP);
    CHECK(status->sources[SYNC_SOURCE_RTC].check == SYNC_CHECK_GOOD);
}


// NTP ready from the first epoch: it waits while the RTC's second is found.
static void test_rtc_first(bool rtc_found)
{
    Model m;
    long k;
    long aligned_at = -1, acquired_at = -1;
    double from_rtc_at_4 = 0.0;
    const SyncStatus *status;

    printf("NTP from the start, the RTC's second %s\n",
           rtc_found ? "found at epoch 2" : "never found");
    model_init(&m);
    sync_init();
    for (k = 0; k < 60; k++)
    {
        TimingSnapshot snap;
        SyncUpdate update;

        now.gnss = false;
        now.gnss_tacc = 0;
        now.rtc = true;
        now.rtc_second = rtc_found && k >= 2;
        now.ntp = true;
        now.ntp_unc = 11000000U;
        (void)model_epoch(&m, &snap);
        update = sync_update(&snap, (uint32_t)k * 1000U);
        print_transition(&update);
        status = sync_status();
        if (status->aligned && aligned_at < 0)
            aligned_at = k;
        if (status->state == SYNC_ACQUIRE && acquired_at < 0)
            acquired_at = k;
        if (k == 4)
        {
            // Stepped onto the RTC's second, before NTP's own step.
            long nearest = lround((m.epoch - rtc_origin - RTC_PHASE) / TRUE_SECOND);
            from_rtc_at_4 = m.epoch - (rtc_origin + RTC_PHASE + nearest * TRUE_SECOND);
        }
        model_advance(&m);
    }
    status = sync_status();
    printf("  aligned at %ld, NTP taken at %ld; epoch 4 %.0f ticks from the RTC's second\n",
           aligned_at, acquired_at, from_rtc_at_4);
    if (rtc_found)
    {
        CHECK(aligned_at == 2 && acquired_at >= aligned_at);
        CHECK(fabs(from_rtc_at_4) < 1000.0);
    }
    else
    {
        CHECK(aligned_at < 0 && acquired_at >= SYNC_ALIGN_WAIT_EPOCHS &&
              acquired_at <= SYNC_ALIGN_WAIT_EPOCHS + 1);
    }
    CHECK(status->state == SYNC_LOCKED && status->source == SYNC_SOURCE_NTP);
}


// ---- RESYNC ------------------------------------------------------------------
/* Locked on GNSS, with NTP at +/-2 ms: a resync free runs for the epochs it
 * takes the sources to qualify again, then takes GNSS up again, slewing (it
 * is already on its edge). GNSS then lost: holdover, which NTP at 2 ms never
 * beats; a resync drops holdover, and NTP is taken up, slewed onto. */
static void test_resync(void)
{
    Model m;
    long k;
    long gnss_again = -1, ntp_from = -1;
    double worst_period = 0.0;
    const SyncStatus *status;
    const SyncTransition *transition;
    uint32_t corrections = 0;

    printf("RESYNC\n");
    model_init(&m);
    sync_init();
    for (k = 0; k < 700; k++)
    {
        TimingSnapshot snap;
        SyncUpdate update;

        now.gnss = k < 400;
        now.gnss_tacc = 30U;
        now.ntp = true;
        now.ntp_unc = 2000000U;
        now.rtc = false;
        (void)model_epoch(&m, &snap);
        if (k == 200 || k == 500)
        {
            sync_resync();
        }
        update = sync_update(&snap, (uint32_t)k * 1000U);
        print_transition(&update);
        status = sync_status();
        transition = sync_last_transition();
        if (k == 199)
        {
            CHECK(status->state == SYNC_LOCKED && status->source == SYNC_SOURCE_GNSS);
            corrections = status->time.corrections;
        }
        if (k == 200 || k == 500)
        {
            CHECK(status->state == SYNC_FREE_RUN && transition->reason == SYNC_REASON_RESYNC);
            CHECK(!status->holdover.ready && status->time.known);
        }
        if (k > 200 && k < 400 && gnss_again < 0 && status->source == SYNC_SOURCE_GNSS)
            gnss_again = k;
        if (k == 499)
        {
            CHECK(status->state == SYNC_HOLDOVER && status->source == SYNC_SOURCE_GNSS);
        }
        if (k > 500 && ntp_from < 0 && status->source == SYNC_SOURCE_NTP)
            ntp_from = k;
        // Slewed, not stepped, through both.
        if ((k > 200 && k < 260) || (k > 500 && k < 560))
        {
            double off = fabs((double)m.running - TRUE_SECOND);
            if (off > worst_period)
                worst_period = off;
        }
        model_advance(&m);
    }
    status = sync_status();
    printf("  GNSS again at %ld, NTP from %ld, periods within %.0f ticks\n", gnss_again, ntp_from,
           worst_period);
    CHECK(gnss_again > 200 && gnss_again <= 200 + SYNC_QUALIFY_EPOCHS + 1);
    CHECK(ntp_from > 500 && ntp_from <= 500 + SYNC_QUALIFY_EPOCHS + 1);
    CHECK(worst_period < (double)SYNC_NTP_LIMIT_TICKS + 100.0);
    CHECK(status->state == SYNC_LOCKED && status->source == SYNC_SOURCE_NTP);
    CHECK(status->time.corrections == corrections); // the time of day never moved
    CHECK(route == PPS_INPUT_ESP);
}


int main(void)
{
    test_gnss_clock();
    test_ntp_clock();
    test_coarse_ntp();
    test_ntp_correction();
    test_rtc_start();
    test_rtc_drifted();
    test_rtc_first(true);
    test_rtc_first(false);
    test_resync();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
