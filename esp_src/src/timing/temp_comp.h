/*
 * The ESP crystal's temperature compensation: the board temperature (the
 * PIC's BME280, STAT-ENV) turned into a correction of the ESP's clock, so
 * the clock follows the crystal as the room warms and cools between NTP
 * polls, rather than an hour late through the servers' lines.
 *
 * Linear and relative: the crystal is taken to run coefficient ppb/C faster
 * (+) or slower (-) for every degree above the first temperature seen since
 * the ESP started (the reference). The absolute values do not matter: what
 * the crystal does at the reference is the lines' to find, as before, and
 * what is left for them is near enough a steady crystal.
 *
 * The coefficient: one set by hand, else the one learnt on this clock, else
 * TEMP_COMP_DEFAULT_PPB_PER_C. The default is from two V2 boards' ESP32
 * modules (2026-11-17/20): -159 and -171 ppb/C against the board
 * temperature measured through the NTP lines, which understate it; -165
 * left about -40 ppb/C, -200 a little the other way. -180 since
 * 2026-11-22: the two boards learnt about -200 and -155.
 *
 * The correction is a phase, us, added to UTC - local (esp_clock.h; local
 * is the ESP's microsecond timer, not local time): the integral of the
 * rate, rate = -coefficient x (temperature - reference) in esp_clock's
 * sense (+ = local slow). NTP samples have it taken off before they reach
 * the lines and the clock model, and the clock's UTC has it added back, so
 * the model only sees what the compensation does not explain. The phase is
 * continuous: a change of coefficient, compensation turned off, or the
 * temperature lost only changes the rate from then on, never the phase so
 * far.
 *
 * A reading not renewed for TEMP_COMP_STALE_US is held (temperature_held):
 * the rate, the exposure and learning carry on at it, up to
 * TEMP_COMP_HOLD_US. Only then is the temperature lost: no rate, and the
 * learning window and the movement history cleared. A PIC restart stops
 * STAT-ENV for 30-45 s (an automatic update at 04:00, 2026-11-23), and
 * clearing then threw away both clocks' 8 h windows; held, a 45 s gap
 * with the board 5 C from the reference and moving 0.5 C costs about 2 us
 * of phase and nothing of the window.
 *
 * Learning: the followed server's raw offsets (the compensation not taken
 * off) over the last TEMP_COMP_LEARN_MAX_AGE_US (at most
 * TEMP_COMP_LEARN_SAMPLES polls, enough for that at the shortest polls)
 * are fitted to offset = a + b t + d t^2 - c x E, E the temperature's
 * exposure (the integral of temperature - reference, C s): b and d the
 * crystal's frequency at the reference and its drift, c the coefficient.
 * The drift term matters: through a one-way ramp of temperature (the room
 * warming all morning) E is nearly a curve in t, and a crystal drifting
 * slowly would pass for temperature. With d in the fit such a window's c is
 * uncertain, and rejected; a rise and a fall in one window still tell them
 * apart (field data, 2026-11-21).
 *
 * The standard error is widened for the residuals' correlation from poll to
 * poll (the crystal's own wander: neighbouring polls err together), by
 * sqrt((1 + r) / (1 - r)), r their lag-1 autocorrelation: without it the
 * fits claimed +/-3 to +/-7 ppb/C while moving 80 to 250 ppb/C in hours.
 * An estimate counts only over TEMP_COMP_LEARN_MIN_RANGE_CENTIC of
 * temperature, with both a rise and a fall of TEMP_COMP_LEARN_MIN_TURN_CENTIC
 * in it (a crystal settling, which no polynomial follows, through a one-way
 * ramp is otherwise taken for temperature even with d), with a standard
 * error within TEMP_COMP_LEARN_MAX_SE_PPB, and
 * TEMP_COMP_LEARN_NEW_US after the last one counted (half the window new,
 * so estimates are not the same data again). The learnt coefficient moves
 * from the default (the first) or from what was learnt by the estimate's
 * difference over n (up to TEMP_COMP_LEARN_WEIGHT), at most
 * TEMP_COMP_LEARN_STEP_PPB a time, so no one window carries it far. Kept in
 * NVS (ntp.cpp). Learning goes on after; a change of followed server, or
 * the temperature lost, starts the window again.
 *
 * Short polls (ntp.cpp), from temp_comp_short_polls(): more samples through
 * each swing, for learning, and between polls less of whatever the
 * compensation does not explain. Asked for while on, with a temperature:
 *   - START, 128 s: nothing set or learnt yet, for TEMP_COMP_START_US;
 *   - the temperature moving (the board has moved by TEMP_COMP_MOVING_CENTIC
 *     within TEMP_COMP_MOVING_MINUTES; over once that range has stayed
 *     under TEMP_COMP_STEADY_CENTIC for TEMP_COMP_STEADY_MINUTES, a reading
 *     a minute kept): LEARNING, 128 s, until TEMP_COMP_LEARN_SETTLED
 *     estimates are learnt (or one is set by hand), then MOVING, 256 s: the
 *     compensation follows the temperature, the polls only what it leaves.
 *
 * Portable: local times are passed in, as esp_clock's.
 */
#ifndef TEMP_COMP_H
#define TEMP_COMP_H

#include <stdbool.h>
#include <stdint.h>

#define TEMP_COMP_DEFAULT_PPB_PER_C (-180) // + = the crystal runs fast when warmer
#define TEMP_COMP_MAX_PPB_PER_C     2000
#define TEMP_COMP_NOT_SET           INT32_MIN  // no coefficient set by hand
#define TEMP_COMP_STALE_US          15000000LL // STAT-ENV comes every 5 s: held after this ...
#define TEMP_COMP_HOLD_US           300000000LL // ... and lost after this (a PIC restart is 30-45 s)

#define TEMP_COMP_LEARN_SAMPLES          256 // 8 h at 128 s: ~10 KB
#define TEMP_COMP_LEARN_MIN_SAMPLES      12
#define TEMP_COMP_LEARN_MAX_AGE_US       (8LL * 3600 * 1000000)
#define TEMP_COMP_LEARN_MIN_RANGE_CENTIC 200 // 2 C ...
#define TEMP_COMP_LEARN_MIN_TURN_CENTIC  100 // ... with 1 C each way
#define TEMP_COMP_LEARN_MAX_SE_PPB       20.0
#define TEMP_COMP_LEARN_MAX_RHO          0.95 // the correlation's widening at most x6.2
#define TEMP_COMP_LEARN_NEW_US           (4LL * 3600 * 1000000)
#define TEMP_COMP_LEARN_WEIGHT           8
#define TEMP_COMP_LEARN_STEP_PPB         30.0
#define TEMP_COMP_LEARN_SETTLED          3 // estimates, then MOVING's longer polls

#define TEMP_COMP_START_US        (6LL * 3600 * 1000000)
#define TEMP_COMP_MOVING_CENTIC   50 // 0.5 C ...
#define TEMP_COMP_MOVING_MINUTES  30 // ... within 30 min: moving
#define TEMP_COMP_STEADY_CENTIC   30 // under 0.3 C in 30 min ...
#define TEMP_COMP_STEADY_MINUTES  30 // ... for 30 min: steady again
#define TEMP_COMP_HISTORY_MINUTES (TEMP_COMP_MOVING_MINUTES + 1)
#define TEMP_COMP_SHORT_S         128 // START and LEARNING
#define TEMP_COMP_MOVING_S        256 // MOVING

typedef enum
{
    TEMP_COMP_OFF = 0,        // turned off
    TEMP_COMP_NO_TEMPERATURE, // on, but no temperature (none yet, or stale)
    TEMP_COMP_ACTIVE,
} TempCompState;

typedef enum
{
    TEMP_COMP_DEFAULT = 0, // nothing learnt or set
    TEMP_COMP_LEARNT,
    TEMP_COMP_SET, // by hand
} TempCompSource;

typedef enum
{
    TEMP_COMP_POLLS_NORMAL = 0,
    TEMP_COMP_POLLS_START,    // nothing set or learnt, since start: 128 s
    TEMP_COMP_POLLS_LEARNING, // the temperature moving, not learnt yet: 128 s
    TEMP_COMP_POLLS_MOVING,   // the temperature moving: 256 s
} TempCompPolls;

typedef struct
{
    TempCompState state;
    bool enabled;
    TempCompSource source;
    int32_t ppb_per_c; // in use
    bool reference_known;
    int32_t reference_centic;   // the first temperature since start
    bool temperature_known;     // a reading within TEMP_COMP_HOLD_US
    bool temperature_held;      // ... but none within TEMP_COMP_STALE_US: the last one held
    int32_t temperature_centic; // the latest
    int64_t temperature_local_us;
    int64_t rate_ppb; // being added to UTC - local's rate now, + = local slow
    int64_t phase_us; // added to UTC - local at phase_local_us
    int64_t phase_local_us;
    // Learning
    double learnt_ppb_per_c;
    int32_t estimates;       // accepted since learning began (also before a restart)
    double last_ppb_per_c;   // the latest fit, accepted or not
    double last_se_ppb;      // ... its standard error
    int32_t last_range_centic; // ... the temperature range it spanned
    int32_t last_turn_centic;  // ... the lesser of its largest rise and fall
    int64_t last_local_us;   // ... when (0: none yet)
    double last_rho;         // ... its residuals' lag-1 autocorrelation
    int64_t accepted_local_us; // the last estimate counted (0: none since start)
    int window;              // samples held now
    int32_t window_range_centic;
    int64_t learning_since_us; // short polls asked for from here
    // The temperature's movement, from a reading a minute
    bool moving;
    int32_t moved_centic; // the range over the last TEMP_COMP_MOVING_MINUTES
    int minutes;          // readings held, to TEMP_COMP_HISTORY_MINUTES
    int steady_minutes;   // moving, but within TEMP_COMP_STEADY_CENTIC so long
} TempCompStatus;

void temp_comp_init(void);
// What was learnt before (NVS), at start or after temp_comp_forget().
void temp_comp_restore(int32_t learnt_ppb_per_c, int32_t estimates);
// The settings, from now on: on or off, and a coefficient set by hand or
// TEMP_COMP_NOT_SET (the learnt one, else the default). Called whenever
// they may have changed.
void temp_comp_configure(int64_t local_us, bool enabled, int32_t set_ppb_per_c);
// A board temperature read at local_us, 0.01 C.
void temp_comp_temperature(int64_t local_us, int32_t centic);
// Often: holds a temperature not renewed, then lets it go.
void temp_comp_service(int64_t local_us);

// The correction added to UTC - local at local_us, us. local_us may be a
// little before the latest update (an NTP sample's midpoint).
int64_t temp_comp_us(int64_t local_us);
// Its rate now, ppb (+ = local slow), for the PPS running on by itself.
int64_t temp_comp_rate_ppb(void);

// A sample of the followed server, numbered server: its raw offset (UTC -
// local, the compensation not taken off), us, and round trip.
void temp_comp_learn(int64_t local_us, int64_t offset_us, int64_t delay_us, int server);
// Short polls wanted, and why (see above): TEMP_COMP_POLLS_NORMAL if not.
TempCompPolls temp_comp_short_polls(int64_t local_us);
// The longest poll for them, s: 0 for NORMAL.
uint32_t temp_comp_polls_s(TempCompPolls polls);
// A new learnt coefficient since the last call: true, with it and the
// estimates behind it, to keep.
bool temp_comp_take_learnt(int32_t *ppb_per_c, int32_t *estimates);
// Nothing learnt: back to the default, learning from now.
void temp_comp_forget(int64_t local_us);

const TempCompStatus *temp_comp_status(void);
const char *temp_comp_state_name(TempCompState state);
const char *temp_comp_source_name(TempCompSource source);
const char *temp_comp_polls_name(TempCompPolls polls); // "start", "learning", "temperature moving"

#endif // TEMP_COMP_H
