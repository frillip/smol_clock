// The original kalman.c / epoch_servo.c, driven as main.c drives them in
// TRACK. Holdover requests nothing, as in main.c.
#include "sim_model.h"
#include "kalman.h"
#include "epoch_servo.h"

static KalmanFilter kf;
static EpochPiServo pi;
static EpochFractionalAccumulator acc;
static bool have_kf;

const char *adapter_name(void)
{
    return "original";
}


void adapter_init(void)
{
    kalman_init(&kf, 40000000UL, PPS_KF_Q, PPS_KF_R);
    epoch_pi_init(&pi, EPOCH_Q16_GAIN(1, 8), EPOCH_Q16_GAIN(1, 256), 8, 16);
    epoch_fractional_init(&acc);
    epoch_pi_enable(&pi, true);
    have_kf = false;
}


uint32_t adapter_epoch(uint32_t measured_period, int32_t phase_ticks, bool have_gnss)
{
    if (!have_gnss)
    {
        return 0;
    }
    if (measured_period != 0U)
    {
        kalman_update(&kf, measured_period);
        have_kf = true;
    }
    if (!have_kf)
    {
        return 0;
    }
    return epoch_fractional_next(
        &acc, epoch_servo_period_command(&pi, kalman_get_period_q16(&kf), phase_ticks));
}
