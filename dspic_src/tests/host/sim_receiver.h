// Simulated u-blox receiver for host tests: answers MON-VER, CFG-VALSET and
// CFG-VALGET, models baud mismatch on the UART, and emits navigation epochs
// and PPS edges once configured to.
#ifndef SIM_RECEIVER_H
#define SIM_RECEIVER_H
#include <stdint.h>
#include <stdbool.h>
#include "../../gnss/gnss_epoch.h"

typedef struct
{
    bool present;
    uint32_t initial_baud;
    bool has_flash;            // else VALSET to the flash layer is NAKed
    bool supports_tim_tm2;     // else the TIM-TM2 MSGOUT key is unknown
    bool locked;               // PPS and valid time
    bool lose_baud_ack;        // switch baud before sending the ACK
    uint32_t message_delay_ms; // epoch messages after the PPS edge
} SimReceiverConfig;

void sim_init(const SimReceiverConfig *config, int64_t start_unix_seconds);
// Advances the simulation to now_ms (1 ms steps). Exchanges bytes with the
// host UART stand-in.
void sim_step(uint32_t now_ms);
GnssPpsReference sim_pps_reference(void);
// UTC second of the most recent PPS edge actually output.
int64_t sim_utc_of_last_pps(void);
// ... and whether it was an inserted leap second (23:59:60).
bool sim_leap_of_last_pps(void);
// Leap second at the end of the UTC day before event_utc (a midnight):
// +1 inserts 23:59:60, -1 deletes 23:59:59. Announced in NAV-TIMELS.
void sim_schedule_leap(int64_t event_utc, int8_t change);
uint32_t sim_baud(void);
uint32_t sim_config_value(uint32_t key, bool *found);
uint8_t sim_last_layers(void);
uint32_t sim_flash_writes(void);
// Test controls.
void sim_set_locked(bool locked);
void sim_drop_next_pps(void);
// qErr the receiver reported (in TIM-TP) for the last pulse it output.
int32_t sim_qerr_of_last_pps(void);
// Where the receiver sees the clock's epoch edge (TIM-TM2), ns from the second.
void sim_set_epoch_mark_offset(int32_t offset_ns);
// As a provisioned receiver: set a CFG-MSGOUT rate directly.
void sim_set_message_rate(uint32_t key, uint32_t rate);

#endif
