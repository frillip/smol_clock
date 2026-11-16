/*
 * Hardware edges, all timed in local microseconds (esp_timer):
 *
 *   ESP PPS out  GPIO5, a 100 ms pulse whose rising edge is the ESP's UTC
 *                second, made by a 1 MHz gptimer alarm. The timer and
 *                esp_timer count the same crystal, so a local time maps to a
 *                timer count by a fixed offset measured at start.
 *   PIC epoch    GPIO27 rising edge, timestamped in its interrupt.
 *   GNSS PPS     GPIO18 rising edge (only with the solder jumpers fitted).
 *
 * The output runs itself: at each falling edge the interrupt arms the next
 * rising edge, at the time the loop planned with pps_output_plan() if it did,
 * or else one second on, corrected for frequency. So a busy loop (an HTTP
 * request, an OTA check) does not lose a second; it only delays corrections.
 * The edge time recorded is esp_timer read in the interrupt just after the
 * pin is set, so interrupt latency is measured, not hidden. The rising edge
 * is armed early by the average latency (lead_us), so the pin rises on the
 * second rather than 4-6 us after it; latency_us is still measured against
 * the alarm.
 */
#ifndef PPS_H
#define PPS_H

#include <stdbool.h>
#include <stdint.h>

#define PPS_PULSE_US 100000

typedef struct
{
    int64_t local_us;  // when the edge happened
    uint32_t sequence; // edges since start
} PpsEdge;

typedef struct
{
    uint32_t planned;      // rising edges at a time the loop planned
    uint32_t extrapolated; // ... that ran on by themselves
    uint32_t late_plans;   // plans already in the past, skipped a second
    int32_t latency_us;    // latest edge: recorded time minus the alarm time
    int32_t latency_max_us;
    int32_t lead_us;       // how early the rising edge is armed
} PpsOutputStats;

void pps_init(void);

// Starts the output with a rising edge at first_rise_local_us (or the second
// after, if that is too close), or re-plans a running one.
void pps_output_start(int64_t first_rise_local_us, int32_t period_adjust_q16);
// The next rising edge. Call within PPS_PULSE_US of the previous one.
void pps_output_plan(int64_t next_rise_local_us, int32_t period_adjust_q16);
void pps_output_stop(void);
bool pps_output_running(void);
bool pps_output_take(PpsEdge *edge);
const PpsOutputStats *pps_output_stats(void);

bool pps_pic_take(PpsEdge *edge);
bool pps_pic_latest(PpsEdge *edge);
bool pps_gnss_take(PpsEdge *edge);
bool pps_gnss_latest(PpsEdge *edge);

#endif // PPS_H
