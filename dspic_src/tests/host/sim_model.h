/* Closed-loop model shared by the old and new adapters: a 40 MHz-nominal
 * oscillator with a fixed offset, random-walk wander and a slow temperature
 * term; GNSS PPS edges with Gaussian jitter, captured to whole ticks; the
 * epoch edge on a whole tick; commands applied one epoch late, as in main.c. */
#ifndef SIM_MODEL_H
#define SIM_MODEL_H
#include <stdbool.h>
#include <stdint.h>

// Implemented by each adapter. measured_period is 0 when there is no new
// period; have_gnss is false during holdover. Returns the integer period
// to request, or 0 to request nothing (timer keeps its last period).
void adapter_init(void);
uint32_t adapter_epoch(uint32_t measured_period, int32_t phase_ticks, bool have_gnss);
const char *adapter_name(void);

#endif
