/*
 * The ESP's UTC: a model of UTC against the local microsecond timer
 * (esp_timer, the APB crystal), disciplined by NTP samples. Portable, no
 * Arduino dependency, so it can be driven by a host simulation.
 *
 *   utc(t) = anchor_utc + (t - anchor) x (1 + frequency) + slew(t)
 *
 * Each sample is the best exchange of an NTP poll: the offset UTC - local at
 * the local midpoint of the exchange, and its round trip. The sample is
 * judged against the model at its own time, but every change takes effect
 * from the moment of the update, never retroactively: an update moves the
 * clock (and so the ESP PPS edge) only as fast as the slew allows.
 *
 *   - The first sample, or one more than ESP_CLOCK_STEP_US from the model,
 *     steps the clock.
 *   - A correction up to ESP_CLOCK_GENTLE_US is slewed in over the time
 *     constant the caller gives (about two poll intervals), but never faster
 *     than SML_NTP_GENTLE_SLEW_PPM: slow enough for the PIC to follow
 *     without stepping, and the next sample replaces what is left of it, so
 *     each sample's noise is only half applied.
 *   - A larger one is slewed in at ESP_CLOCK_FAST_SLEW_PPM, reported as
 *     correcting (SML_NTP_CORRECTING) until done: the PIC cannot follow it
 *     in any case, so it might as well be quick.
 *   - With an estimate from the caller (the servers' fitted lines,
 *     ntp_stats.h), the correction is that estimate's offset at the update
 *     against the model, and the frequency is the estimate's.
 *   - Without one, or before it knows the frequency: the sample's offset,
 *     and a least-squares line through the raw offsets of the latest
 *     ESP_CLOCK_WINDOW samples, those with a round trip well above the
 *     window's best left out (their asymmetry is unknown). Needs
 *     ESP_CLOCK_FREQUENCY_MIN_US of span; two samples give a first estimate.
 *
 * The uncertainty is a +/- bound in the sense of SML, the tightest that any
 * sample in the frequency window gives: how far the model, run back to the
 * sample's time and on by what is still being slewed in, is from the
 * sample, plus half its round trip, the server's root delay / 2 and root
 * dispersion, and a drift allowance for the time since. The drift allowance
 * is 50 ppm until the frequency is known, 5 ppm after one estimate, then
 * the estimate's skew (or three times the window's standard error of the
 * frequency) plus ESP_CLOCK_DRIFT_FLOOR_PPB (the crystal's wander with
 * temperature), at most ESP_CLOCK_DRIFT_MAX_PPB (ESP_CLOCK_DRIFT_YOUNG_PPB for
 * the skew, large while the lines are young).
 */
#ifndef ESP_CLOCK_H
#define ESP_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define ESP_CLOCK_STEP_US           128000LL
#define ESP_CLOCK_GENTLE_US         8000LL
#define ESP_CLOCK_GENTLE_PPM        5 // = SML_NTP_GENTLE_SLEW_PPM
#define ESP_CLOCK_FAST_SLEW_PPM     100
#define ESP_CLOCK_WINDOW            8
#define ESP_CLOCK_WINDOW_MAX_US     (6LL * 3600 * 1000000)
#define ESP_CLOCK_FREQUENCY_MIN_US  120000000LL
#define ESP_CLOCK_FREQUENCY_MAX_PPB 500000
#define ESP_CLOCK_DRIFT_FLOOR_PPB   500
#define ESP_CLOCK_DRIFT_MAX_PPB     2000
#define ESP_CLOCK_DRIFT_YOUNG_PPB   5000 // the cap on the lines' skew + floor

typedef struct
{
    int64_t local_us;  // local time of the exchange midpoint
    int64_t offset_us; // UTC - local, at local_us
    int64_t delay_us;  // round trip minus the server's processing
    uint32_t root_delay_us;
    uint32_t root_dispersion_us;
    uint8_t stratum;
} EspClockSample;

// The caller's best estimate at the update, from its fitted lines.
typedef struct
{
    int64_t offset_us; // UTC - local at the update's now_us
    bool frequency_known;
    int32_t frequency_ppb; // + = local slow
    int32_t skew_ppb;      // the frequency's bound
} EspClockEstimate;

typedef enum
{
    ESP_CLOCK_STEPPED,
    ESP_CLOCK_SLEWED,      // gently
    ESP_CLOCK_SLEWED_FAST, // faster than ESP_CLOCK_GENTLE_PPM
} EspClockAction;

typedef struct
{
    bool valid;
    uint32_t updates;
    uint32_t steps;
    uint32_t fast_slews;
    uint32_t frequency_samples;   // estimates made
    uint8_t frequency_used;       // samples in the latest estimate
    int32_t frequency_ppb;        // UTC rate against the local timer: + = local slow
    int32_t frequency_error_ppb;  // the latest estimate's standard error, -1 if not known
    int32_t skew_ppb;             // the caller's bound on it, -1 if none
    int64_t last_update_local_us; // local time of the latest sample
    int64_t last_error_us;        // that sample against the model before it
    int64_t last_correction_us;   // what that update corrected at the time of it
    bool estimated;               // the latest update was given an estimate
    int64_t slew_us;              // the correction being slewed in
    int64_t slew_end_us;          // local time it completes
    bool fast;                    // ... at the fast rate
    EspClockSample last;          // the latest sample
} EspClockStatus;

void esp_clock_init(void);
// now_us: local time of the update. time_constant_us: how long a gentle
// correction should take (the slew is never faster than ESP_CLOCK_GENTLE_PPM).
// estimate: the caller's, or NULL. The sample is kept either way, for the
// uncertainty and the fallback frequency.
EspClockAction esp_clock_update(const EspClockSample *sample, int64_t now_us,
                                int64_t time_constant_us, const EspClockEstimate *estimate);

bool esp_clock_is_valid(void);
// Valid only while esp_clock_is_valid().
int64_t esp_clock_utc_us(int64_t local_us);
// The local time at which UTC reaches utc_us.
int64_t esp_clock_local_us(int64_t utc_us);
uint32_t esp_clock_uncertainty_ns(int64_t local_us);
// A step or fast slew is moving the clock at local_us (SML_NTP_CORRECTING).
bool esp_clock_correcting(int64_t local_us);
// Local microseconds to add to 1 000 000 for one UTC second, Q16 (the PPS
// uses it to run on while the loop is busy).
int32_t esp_clock_period_adjust_q16(void);

// The drift allowed for since a sample, ppb (see above).
int64_t esp_clock_drift_ppb(void);
// Samples from now on come from another server: the frequency is kept, but
// the window starts again, as raw offsets from two servers differ by their
// paths' asymmetry and would read as a frequency error.
void esp_clock_new_reference(void);

const EspClockStatus *esp_clock_status(void);

#endif // ESP_CLOCK_H
