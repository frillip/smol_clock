#include "fake_control.h"

#include <string.h>

EpochControl fake_control;
int fake_control_seeds;
int32_t fake_control_seed_q16;

const EpochProfile epoch_profile_precise = {"precise", 0, 0, 0, 0, 0, 0, true};
static const EpochProfile gentle = {"gentle", 0, 0, 0, 0, 0, 0, false};

const EpochControl *epoch_control_state(void)
{
    if (fake_control.profile == NULL)
    {
        fake_control.profile = &gentle;
    }
    return &fake_control;
}


void epoch_control_seed(int32_t offset_q16)
{
    fake_control_seeds++;
    fake_control_seed_q16 = offset_q16;
}


void fake_control_tracking(int32_t offset_q16, uint32_t epochs)
{
    memset(&fake_control, 0, sizeof(fake_control));
    fake_control.state = EPOCH_STATE_TRACK;
    fake_control.profile = &epoch_profile_precise;
    fake_control.reference_epochs = epochs;
    kalman_init(&fake_control.pps_kf, 40000000UL, PPS_KF_Q, PPS_KF_R);
    kalman_seed(&fake_control.pps_kf, offset_q16);
}
