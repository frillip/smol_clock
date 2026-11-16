/*
 * One NTP server's samples and a weighted line through them, after chronyd
 * (sourcestats.c and regress.c, chrony 4.2), so that each server's path is
 * judged on its own. Portable, for the host tests.
 *
 * Each sample is the raw offset UTC - local (the local timer, never
 * steered), at the local midpoint of the exchange. A line through them
 * gives the server's view of our crystal's frequency error (its slope) and
 * of the offset now (the line run on to now), with their standard errors.
 *
 *   - Weights, as chronyd: each sample's distance is half its round trip
 *     plus NTP_STATS_PRECISION_US. sd = (median - least) / 0.7, kept
 *     between the precision and the least distance; a sample d from the
 *     least has variance (1 + d / sd)^2. A slow exchange, whose queueing
 *     makes its path lopsided, counts for little.
 *   - The line: weighted least squares over the samples kept, then the
 *     runs test on its residuals (too few sign changes means they are not
 *     noise about a line but a curve: the crystal's frequency has moved).
 *     While the test fails the oldest sample is dropped and the line
 *     fitted again, down to NTP_STATS_MIN_SAMPLES. The samples dropped are
 *     kept a while longer (as many as the line has) only to lengthen the
 *     runs test, as chronyd does. Critical runs at 5 % and Student's t at
 *     99.95 % are chronyd's tables.
 *   - Skew: the slope's standard error times t, chronyd's bound on the
 *     frequency error.
 *   - With fewer than three samples there is no line: the offset is the
 *     latest sample's, the frequency unknown and the skew the worst case.
 *
 * At most NTP_STATS_MAX_SAMPLES in the line, where chronyd keeps up to 64:
 * through a few hundred us of Wi-Fi noise the runs test cannot see the
 * crystal's slow wander with temperature, and a line hours long lags it.
 * Simulated (tests/ntp/test_esp_clock.cpp), 16 does better on a LAN with
 * the crystal wandering and Wi-Fi delaying some exchanges, 64 only on a
 * steady crystal or a long Internet path.
 *
 * Unlike chronyd, nothing here steers the samples: they are raw offsets,
 * the same whatever the clock model did since. No asymmetry correction
 * (chronyd's correct_asymmetry), and no per-sample dispersion growth: the
 * ESP keeps samples for at most a few hours, and the weights depend only
 * on the round trips.
 */
#ifndef NTP_STATS_H
#define NTP_STATS_H

#include <stdbool.h>
#include <stdint.h>

#ifndef NTP_STATS_MAX_SAMPLES // overridden only by the host simulation (tests/ntp)
#define NTP_STATS_MAX_SAMPLES    16 // in the line at most (chronyd's maxsamples, 64 by default)
#endif
#define NTP_STATS_RUNS_RATIO     2 // samples kept for the runs test, per sample in the line
#define NTP_STATS_MIN_REGRESS    3 // a line needs this many
#define NTP_STATS_MIN_SAMPLES    6 // the runs test never leaves fewer (chronyd's minsamples)
#define NTP_STATS_SD_TO_DIST     0.7
#define NTP_STATS_PRECISION_US   1.0 // the local timer's
#define NTP_STATS_WORST_SKEW_PPB 2000000.0 // chronyd's WORST_CASE_FREQ_BOUND, 2000 ppm
#define NTP_STATS_WORST_SD_US    4000.0    // chronyd's WORST_CASE_STDDEV_BOUND, 4 ms
#define NTP_STATS_RING           (NTP_STATS_MAX_SAMPLES * NTP_STATS_RUNS_RATIO)

typedef struct
{
    int64_t local_us;  // the exchange's local midpoint
    int64_t offset_us; // UTC - local there
    uint32_t delay_us; // its round trip
    uint32_t server_us; // the server's root delay / 2 + root dispersion
} NtpStatsSample;

typedef struct
{
    NtpStatsSample ring[NTP_STATS_RING];
    uint8_t latest; // ring index of the latest sample
    uint8_t samples; // in the line, the latest of them
    uint8_t runs_samples; // older ones, for the runs test only
    uint32_t added;

    // The latest fit (ntp_stats_regress).
    bool fitted; // a line: else the latest sample alone
    int64_t offset_local_us; // the latest sample's local time ...
    int64_t offset_us;       // ... the line's UTC - local there
    double offset_sd_us;
    double frequency_ppb;    // the line's slope, + = local slow
    double frequency_sd_ppb;
    double skew_ppb;         // the slope's bound
    double sd_us;            // the samples' spread about the line, at weight 1
    uint8_t used;            // samples in the line after the runs test
    uint8_t runs;            // runs among its residuals
    uint8_t best;            // sample with the least distance (0 = oldest in the line)
} NtpStats;

void ntp_stats_reset(NtpStats *stats);
// A sample, then the line through the samples fitted again.
void ntp_stats_add(NtpStats *stats, const NtpStatsSample *sample);
void ntp_stats_regress(NtpStats *stats);

// The line's UTC - local at local_us (the latest sample's offset if there
// is no line).
int64_t ntp_stats_offset_at(const NtpStats *stats, int64_t local_us);
// chronyd's root distance for selection: the best sample's half round trip
// and server share, and the skew over the time since it.
int64_t ntp_stats_distance_us(const NtpStats *stats, int64_t local_us);
// The same with the skew taken as at most skew_cap_ppb.
int64_t ntp_stats_distance_capped_us(const NtpStats *stats, int64_t local_us, double skew_cap_ppb);
// Every sample, and the line, moved by delta_us (a correction changed).
void ntp_stats_shift(NtpStats *stats, int64_t delta_us);
// From the oldest sample in the line to the latest, us.
int64_t ntp_stats_span_us(const NtpStats *stats);
// The latest sample (stats->samples != 0).
const NtpStatsSample *ntp_stats_latest(const NtpStats *stats);

// chronyd's tables, for the tests.
int ntp_stats_critical_runs(int points);
double ntp_stats_t_coefficient(int degrees_of_freedom);

#endif // NTP_STATS_H
