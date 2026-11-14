// The revised kalman.c / servo.c, driven as the revised main.c
// drives them in TRACK, including calibration and holdover.
#include "sim_model.h"
#include "../../timing/kalman.h"
#include "../../timing/servo.h"

static KalmanFilter kf;
static EpochPiServo pi;
static EpochFractionalAccumulator acc;
static bool have_kf;
// The period the model's timer is running now: the request before last
// was loaded at this epoch (see sim_model.h), so track what was returned.
static uint32_t running_period, pending_period;

const char *adapter_name(void)
{
    return "revised";
}


void adapter_init(void)
{
    kalman_init(&kf, 40000000UL, PPS_KF_Q, PPS_KF_R);
    servo_pi_init(&pi, EPOCH_Q16_GAIN(1, 8), EPOCH_Q16_GAIN(1, 256), 8, 16);
    servo_fraction_init(&acc);
    servo_pi_enable(&pi, true);
    have_kf = false;
    running_period = 40000000UL;
    pending_period = 0;
}


// Mirrors the hardware pipeline: a request is loaded at the next epoch.
static uint32_t issue(uint32_t request)
{
    pending_period = request;
    return request;
}


uint32_t adapter_epoch(uint32_t measured_period, int32_t phase_ticks, bool have_gnss)
{
    if (pending_period != 0U)
    {
        running_period = pending_period; // loaded by this epoch
        pending_period = 0;
    }
    if (!have_gnss)
    {
        return issue(servo_fraction_next(&acc, 40000000UL,
                                         servo_holdover_offset(&pi, kalman_phase_q16(&kf))));
    }
    if (measured_period != 0U)
    {
        (void)kalman_update(&kf, measured_period);
        have_kf = true;
    }
    if (!have_kf)
    {
        return 0;
    }
    return issue(servo_fraction_next(
        &acc, 40000000UL,
        servo_offset(&pi, kalman_phase_q16(&kf),
                     servo_phase_predict_q16(
                         servo_phase_error_q16(phase_ticks, EPOCH_PHASE_CALIBRATION_Q16),
                         (int32_t)(running_period - 40000000UL), kalman_phase_q16(&kf)))));
}
