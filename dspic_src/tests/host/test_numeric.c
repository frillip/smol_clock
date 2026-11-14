// Numeric tests for the revised estimator and servo: fixed point against a
// double-precision reference, gating, exact period synthesis, and range.
#include "../../timing/kalman.h"
#include "../../timing/servo.h"

#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <stdlib.h>

static int failures, checks;
#define CHECK(cond)                                                  \
    do                                                               \
    {                                                                \
        checks++;                                                    \
        if (!(cond))                                                 \
        {                                                            \
            failures++;                                              \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                            \
    } while (0)

static double gauss(void)
{
    double u1 = (rand() + 1.0) / ((double)RAND_MAX + 2.0),
           u2 = (rand() + 1.0) / ((double)RAND_MAX + 2.0);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}


// Kalman filter against the same filter in double precision.
static void test_kalman_reference(void)
{
    KalmanFilter kf;
    double x = 0.0, p = 0.0, q = 1.0 / 1024.0, r = 1.0, worst = 0.0, worst_gain = 0.0;
    bool started = false;
    double phase = 0.0, true_offset = -80.34;
    long n;

    srand(5);
    kalman_init(&kf, 40000000UL, PPS_KF_Q, PPS_KF_R);
    for (n = 0; n < 1000000L; n++)
    {
        double next_phase;
        uint32_t period;
        true_offset += 0.002 * gauss();
        next_phase = phase + 40000000.0 + true_offset;
        period = (uint32_t)(floor(next_phase + 0.4 * gauss()) - floor(phase + 0.4 * gauss()));
        phase = next_phase;
        kalman_update(&kf, period);
        if (!started)
        {
            x = (double)period - 40000000.0;
            p = r;
            started = true;
        }
        else
        {
            double pm = p + q, k = pm / (pm + r);
            x += k * ((double)period - 40000000.0 - x);
            p = (1.0 - k) * pm;
            if (fabs(k - kf.gain_q30 / 1073741824.0) > worst_gain)
                worst_gain = fabs(k - kf.gain_q30 / 1073741824.0);
        }
        if (fabs(x - kf.offset_q16 / 65536.0) > worst)
            worst = fabs(x - kf.offset_q16 / 65536.0);
    }
    printf("  Kalman vs double, 1e6 updates: max offset error %.2e tick, max gain error %.2e\n",
           worst, worst_gain);
    // The original filter is within 6e-4 tick; the filter's own error
    // against the true offset is about 0.09 tick rms. What remains here is
    // rounding the Q16 state (1.5e-5 tick) once per update.
    CHECK(worst < 2e-4);
    CHECK(worst_gain < 1e-8);
    CHECK(kf.stats.rejected == 0);
}


static void test_kalman_gate(void)
{
    KalmanFilter kf;
    int k;

    kalman_init(&kf, 40000000UL, PPS_KF_Q, PPS_KF_R);
    for (k = 0; k < 200; k++)
    {
        kalman_update(&kf, 39999920UL);
    }
    CHECK(kalman_period(&kf) == 39999920UL);
    // A receiver time step of 1 us looks like a 40-tick period error.
    CHECK(kalman_update(&kf, 39999960UL) == KALMAN_REJECTED);
    CHECK(kalman_period(&kf) == 39999920UL);
    CHECK(kalman_update(&kf, 39999921UL) == KALMAN_ACCEPTED);
    // Out of range is rejected, never converted.
    CHECK(kalman_update(&kf, 40100000UL) == KALMAN_REJECTED);
    // A genuine change: rejected KALMAN_REJECT_LIMIT times, then re-acquired.
    CHECK(kalman_update(&kf, 40000400UL) == KALMAN_REJECTED);
    CHECK(kalman_update(&kf, 40000400UL) == KALMAN_REJECTED);
    CHECK(kalman_update(&kf, 40000400UL) == KALMAN_REACQUIRING);
    CHECK(kalman_update(&kf, 40000400UL) == KALMAN_ACQUIRED);
    CHECK(kalman_period(&kf) == 40000400UL);
    CHECK(kf.stats.reacquisitions == 1);
}


static void test_kalman_display(void)
{
    KalmanFilter kf;
    uint32_t whole;
    uint16_t milli;

    kalman_init(&kf, 40000000UL, PPS_KF_Q, PPS_KF_R);
    kf.initialized = true;
    kf.offset_q16 = -80L * 65536L - 22938L; // -80.35 ticks
    kalman_hz(&kf, &whole, &milli);
    CHECK(whole == 39999919UL && milli == 650U);
    kf.offset_q16 = -1; // rounds up to .000
    kalman_hz(&kf, &whole, &milli);
    CHECK(whole == 40000000UL && milli == 0U);
}


// The sum of synthesised periods must track the sum of commanded periods
// exactly, the difference being the accumulator itself (0..1 tick).
static void test_fractional_exact(void)
{
    EpochFractionalAccumulator acc;
    int64_t requested_q16 = 0, produced = 0, worst = 0;
    long n;

    srand(9);
    servo_fraction_init(&acc);
    for (n = 0; n < 1000000L; n++)
    {
        int32_t offset = (int32_t)((rand() % 400000) - 200000) - 80L * 65536L;
        uint32_t period = servo_fraction_next(&acc, 40000000UL, offset);
        int64_t diff;
        requested_q16 += (int64_t)40000000 * 65536 + offset;
        produced += period;
        diff = requested_q16 - produced * 65536;
        if (diff != (int64_t)acc.accumulator_q16)
            worst = 1;
    }
    CHECK(worst == 0);
    printf("  fractional synthesis, 1e6 epochs: residual equals accumulator exactly\n");
}


static void test_pi_reference(void)
{
    EpochPiServo pi;
    double integ = 0.0, worst = 0.0;
    long n;

    srand(3);
    servo_pi_init(&pi, EPOCH_Q16_GAIN(1, 8), EPOCH_Q16_GAIN(1, 256), 8, 16);
    servo_pi_enable(&pi, true);
    for (n = 0; n < 100000L; n++)
    {
        int32_t ticks = (rand() % 7) - 3;
        int32_t phase = servo_phase_error_q16(ticks, EPOCH_PHASE_CALIBRATION_Q16);
        double m = ticks - 0.5, p = m / 8.0, cand = integ + m / 256.0, corr;
        int32_t got = servo_pi_update(&pi, phase);
        if (cand > 8)
            cand = 8;
        if (cand < -8)
            cand = -8;
        if (!((p + cand > 16 && m > 0) || (p + cand < -16 && m < 0)))
            integ = cand;
        corr = p + integ;
        if (corr > 16)
            corr = 16;
        if (corr < -16)
            corr = -16;
        if (fabs(corr - got / 65536.0) > worst)
            worst = fabs(corr - got / 65536.0);
    }
    printf("  PI vs double, 1e5 updates: max correction error %.2e tick\n", worst);
    CHECK(worst < 1e-4);
    // Full-range input: output clamped, and conditional integration keeps
    // the integral from winding up while P alone saturates the output.
    servo_pi_reset(&pi);
    for (n = 0; n < 1000L; n++)
        (void)servo_pi_update(&pi, servo_phase_error_q16(100000, 0));
    CHECK(pi.correction_q16 == 16L * 65536L && pi.integral_q16 == 0);
    // Moderate input: P = 5, integral rises to its 8-tick limit.
    servo_pi_reset(&pi);
    for (n = 0; n < 1000L; n++)
        (void)servo_pi_update(&pi, servo_phase_error_q16(40, 0));
    CHECK(pi.integral_q16 == 8L * 65536L && pi.correction_q16 == 13L * 65536L);
    CHECK(servo_holdover_offset(&pi, 0) == -pi.integral_q16);
    servo_pi_reset_phase(&pi);
    CHECK(pi.integral_q16 == 8L * 65536L && pi.proportional_q16 == 0);
}


int main(void)
{
    printf("numeric\n");
    test_kalman_reference();
    test_kalman_gate();
    test_kalman_display();
    test_fractional_exact();
    test_pi_reference();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
