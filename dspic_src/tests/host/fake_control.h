/*
 * Host stand-in for the control loop as the settings see it
 * (timing/epoch_control.h): its state set by the test, and the seed given
 * at start-up recorded.
 */
#ifndef FAKE_CONTROL_H
#define FAKE_CONTROL_H

#include "../../timing/epoch_control.h"

extern EpochControl fake_control;
extern int fake_control_seeds;
extern int32_t fake_control_seed_q16;

// Tracking a reference that teaches the filter, for epochs, at offset_q16.
void fake_control_tracking(int32_t offset_q16, uint32_t epochs);

#endif
