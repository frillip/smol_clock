/*
 * Closed-loop host test of timing/epoch_control.c: the ACQ -> STEP -> TRACK
 * state machine, the Kalman filter, the phase gate and the PI servo, driven
 * through epoch_control_update() exactly as main() drives them, against a
 * model of the hardware:
 *
 *   - a crystal 1.9 ppm slow (39 999 923.75 ticks per GNSS second), as on
 *     the bench;
 *   - GNSS PPS captured to the tick below, with optional Gaussian jitter;
 *   - the epoch edge captured exactly;
 *   - a requested period loaded by the next epoch interrupt, so it governs
 *     the interval after that (the two-epoch pipeline of the hardware).
 *
 * Disturbances: a single bad PPS edge (the reference itself is unchanged),
 * or a permanent shift of the reference (as after a receiver time step).
 * Errors are measured against the true reference, not the captures.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../timing/epoch_control.h"
#include "../../timing/epoch_timer.h"

// The epoch timer, as epoch_control sees it: a request is held until the
// next epoch loads it (see the epoch step in run()).
static uint32_t requested_period;
static bool request_pending;

void epoch_timer_set_period(uint32_t period_ticks)
{
    requested_period = period_ticks;
    request_pending = true;
}


#define TRUE_SECOND 39999923.75
#define EPOCHS      1200L
#define DISTURB_AT  600L

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

static double gauss(void)
{
    double u1 = (rand() + 1.0) / ((double)RAND_MAX + 2.0);
    double u2 = (rand() + 1.0) / ((double)RAND_MAX + 2.0);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * 3.14159265358979323846 * u2);
}


typedef enum
{
    NONE,
    GLITCH,
    SHIFT,
    DROPOUT
} Disturbance;
#define DROPOUT_SECONDS 30L

typedef struct
{
    int32_t after_step;  // true error on the first stepped edge, ticks
    int32_t overshoot;   // largest true error of the opposite sign after it
    long settle;         // s from the step until |error| <= 1 for good
    double mean, rms;    // true error over the 300 s before DISTURB_AT
    double worst_after;  // largest |error| after the disturbance, ticks
    long reacquisitions; // entries to ACQ after the first lock
    long relock;         // s from the disturbance until |error| <= 2 for good
    uint32_t gated;
    long holdovers; // epochs reported as holdover
} Result;

static Result run(double jitter_ticks, unsigned seed, Disturbance kind, double size_ticks)
{
    // Bench start: epoch 16 413 240 ticks after the PPS.
    const double pps_origin = 23754758.3;
    double epoch = 40167998.0;
    uint32_t running = TIMING_NOMINAL_TICKS;
    uint32_t sequence = 0;
    static double error[EPOCHS];
    long k, first_track = -1, step_seq, last_out = -1, last_far = -1;
    long last_index = -100;
    int64_t last_cap = 0;
    EpochSyncState previous_state = EPOCH_STATE_ACQUIRE;
    Result r;
    double sum = 0, sum2 = 0;
    long n = 0;

    memset(&r, 0, sizeof r);
    srand(seed);
    epoch_control_init();
    request_pending = false;

    for (k = 0; k < EPOCHS; k++)
    {
        TimingSnapshot snap;
        long pps_index;
        double reference, edge;
        int64_t cap;
        EpochUpdate update;

        // Epoch interrupt: load any requested period for this interval.
        if (request_pending)
        {
            running = requested_period;
            request_pending = false;
        }
        sequence++;

        // The PPS edge belonging to this epoch: the true reference, and the
        // edge the receiver actually produced.
        pps_index = (long)floor((epoch + 40000.0 - pps_origin) / TRUE_SECOND);
        reference = pps_origin + pps_index * TRUE_SECOND;
        if (kind == SHIFT && k >= DISTURB_AT)
            reference += size_ticks;
        edge = reference + jitter_ticks * gauss();
        if (kind == GLITCH && k == DISTURB_AT)
            edge += size_ticks;
        cap = (int64_t)floor(edge);
        error[k] = epoch - reference;

        memset(&snap, 0, sizeof snap);
        snap.timer.sequence = sequence;
        snap.timer.running_period = running;
        snap.capture.reference_capture = (uint32_t)cap;
        snap.capture.reference_period_valid = (pps_index == last_index + 1);
        snap.capture.reference_period = (uint32_t)(cap - last_cap);
        snap.capture.reference_period_sequence = (uint32_t)pps_index;
        snap.capture.reference_capture_valid = true;
        if (kind == DROPOUT && k >= DISTURB_AT && k < DISTURB_AT + DROPOUT_SECONDS)
        {
            // No PPS from the receiver: no capture, and no period either
            // side of the gap.
            snap.capture.reference_capture_valid = false;
            snap.capture.reference_period_valid = false;
            last_index = -100;
        }
        snap.capture.epoch_capture = (uint32_t)(int64_t)epoch;
        snap.capture.epoch_capture_valid = true;
        last_cap = cap;
        if (snap.capture.reference_capture_valid)
            last_index = pps_index;

        if (epoch_control_state()->state == EPOCH_STATE_TRACK && first_track < 0)
            first_track = k;
        update = epoch_control_update(&snap, true, 0);
        if (snap.capture.reference_capture_valid)
            // A gated edge is measured but run as holdover.
            CHECK(
                update.kind == EPOCH_UPDATE_MEASURED ||
                (update.kind == EPOCH_UPDATE_HOLDOVER && epoch_control_state()->gate.rejected > 0));
        else
        {
            CHECK(update.kind == EPOCH_UPDATE_HOLDOVER);
            r.holdovers++;
        }
        if (first_track >= 0 && previous_state != EPOCH_STATE_ACQUIRE &&
            epoch_control_state()->state == EPOCH_STATE_ACQUIRE)
            r.reacquisitions++;
        previous_state = epoch_control_state()->state;

        epoch += (double)running;
    }

    // The first TRACK measurement is still the pre-step edge; the stepped
    // edge is the next one.
    step_seq = first_track + 1;
    r.after_step = (int32_t)lround(error[step_seq]);
    for (k = step_seq; k < DISTURB_AT; k++)
    {
        int32_t e = (int32_t)lround(error[k]);
        if ((e > 0) != (r.after_step > 0) && abs(e) > abs(r.overshoot))
            r.overshoot = e;
        if (fabs(error[k]) > 1.0)
            last_out = k;
    }
    r.settle = last_out - step_seq + 1;
    for (k = DISTURB_AT - 300; k < DISTURB_AT; k++)
    {
        sum += error[k];
        sum2 += error[k] * error[k];
        n++;
    }
    r.mean = sum / n;
    r.rms = sqrt(sum2 / n - r.mean * r.mean);
    for (k = DISTURB_AT; k < EPOCHS; k++)
    {
        if (fabs(error[k]) > r.worst_after)
            r.worst_after = fabs(error[k]);
        if (fabs(error[k]) > 2.0)
            last_far = k;
    }
    r.relock = (last_far < 0) ? 0 : last_far - DISTURB_AT + 1;
    r.gated = epoch_control_state()->gate.rejected;
    return r;
}


int main(void)
{
    Result clean = run(0.0, 1, NONE, 0);
    Result noisy = run(0.4, 2, NONE, 0);      // 10 ns PPS jitter
    Result rough = run(1.2, 3, NONE, 0);      // 30 ns PPS jitter
    Result small = run(0.4, 4, GLITCH, 200);  // one edge 5 us late
    Result large = run(0.4, 5, GLITCH, 4000); // one edge 100 us late
    Result shift = run(0.4, 6, SHIFT, 1000);  // reference moves 25 us
    Result dropout = run(0.4, 7, DROPOUT, 0); // 30 s without PPS

    printf("acquisition and tracking (true error, ticks of 25 ns)\n");
    printf("  no jitter:    after step %ld, overshoot %ld, settled in %ld s, mean %.2f rms %.2f\n",
           (long)clean.after_step, (long)clean.overshoot, clean.settle, clean.mean, clean.rms);
    printf("  10 ns jitter: after step %ld, overshoot %ld, mean %.2f rms %.2f, gated %lu\n",
           (long)noisy.after_step, (long)noisy.overshoot, noisy.mean, noisy.rms,
           (unsigned long)noisy.gated);
    printf("  30 ns jitter: rms %.2f, gated %lu\n", rough.rms, (unsigned long)rough.gated);
    printf("phase gate\n");
    printf("  one edge 5 us late:   worst %.1f ticks after it, gated %lu, re-acquired %ld\n",
           small.worst_after, (unsigned long)small.gated, small.reacquisitions);
    printf("  one edge 100 us late: worst %.1f ticks after it, gated %lu, re-acquired %ld\n",
           large.worst_after, (unsigned long)large.gated, large.reacquisitions);
    printf("  reference moves 25 us: re-acquired %ld, within 2 ticks again after %ld s\n",
           shift.reacquisitions, shift.relock);
    printf("holdover\n");
    printf("  30 s without PPS: %ld holdover epochs, worst %.1f ticks, re-acquired %ld\n",
           dropout.holdovers, dropout.worst_after, dropout.reacquisitions);

    // A measured phase floors the PPS capture, so the true error settles at
    // the calibration point, zero.
    CHECK(abs(clean.after_step) <= 3);
    CHECK(abs(clean.overshoot) <= 1);
    CHECK(clean.settle <= 15);
    CHECK(fabs(clean.mean) < 0.1);
    CHECK(abs(noisy.after_step) <= 4);
    CHECK(fabs(noisy.mean) < 0.2);
    CHECK(noisy.rms < 1.0);
    CHECK(noisy.gated == 0 && rough.gated == 0);
    CHECK(small.gated == 1 && small.reacquisitions == 0 && small.worst_after < 3.0);
    CHECK(large.gated == 1 && large.reacquisitions == 0 && large.worst_after < 3.0);
    CHECK(shift.reacquisitions == 1 && shift.relock <= 20);
    CHECK(dropout.holdovers == DROPOUT_SECONDS && dropout.reacquisitions == 0);
    CHECK(dropout.worst_after < 3.0);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
