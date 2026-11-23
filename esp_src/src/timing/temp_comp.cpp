#include "temp_comp.h"

#include <math.h>
#include <string.h>

typedef struct
{
    int64_t local_us;
    int64_t offset_us;
    int64_t delay_us;
    double exposure; // C s, at local_us
    int32_t centic;
} LearnSample;

static TempCompStatus comp;
// The phase in ppb x us (1e-15 s), so it accumulates without rounding.
static int64_t phase_ppb_us;
// The exposure: the integral of temperature - reference, C s.
static double exposure;
static bool started; // phase_local_us set
static int32_t set_ppb_per_c = TEMP_COMP_NOT_SET;

static LearnSample window[TEMP_COMP_LEARN_SAMPLES];
static int window_count;
static int window_head; // the oldest
static int learn_server = -1;
static bool learnt_new;

static int32_t history[TEMP_COMP_HISTORY_MINUTES]; // a reading a minute
static int history_head; // the oldest
static int64_t history_next_us;


void temp_comp_init(void)
{
    memset(&comp, 0, sizeof(comp));
    phase_ppb_us = 0;
    exposure = 0.0;
    started = false;
    set_ppb_per_c = TEMP_COMP_NOT_SET;
    window_count = 0;
    window_head = 0;
    learn_server = -1;
    learnt_new = false;
    history_head = 0;
    history_next_us = 0;
    comp.ppb_per_c = TEMP_COMP_DEFAULT_PPB_PER_C;
    comp.state = TEMP_COMP_OFF;
}


// The exposure's rate now, C (0 without a temperature).
static double exposure_rate(void)
{
    if (!comp.temperature_known || !comp.reference_known)
    {
        return 0.0;
    }
    return (comp.temperature_centic - comp.reference_centic) / 100.0;
}


// The phase and exposure brought up to local_us at the rates so far.
static void accumulate(int64_t local_us)
{
    if (!started)
    {
        started = true;
        comp.phase_local_us = local_us;
        return;
    }
    if (local_us > comp.phase_local_us)
    {
        phase_ppb_us += comp.rate_ppb * (local_us - comp.phase_local_us);
        exposure += exposure_rate() * (double)(local_us - comp.phase_local_us) / 1e6;
        comp.phase_us = phase_ppb_us / 1000000000LL;
        comp.phase_local_us = local_us;
    }
}


// The coefficient in use, its source, the rate and state.
static void update_rate(void)
{
    if (set_ppb_per_c != TEMP_COMP_NOT_SET)
    {
        comp.source = TEMP_COMP_SET;
        comp.ppb_per_c = set_ppb_per_c;
    }
    else if (comp.estimates > 0)
    {
        comp.source = TEMP_COMP_LEARNT;
        comp.ppb_per_c = (int32_t)lround(comp.learnt_ppb_per_c);
    }
    else
    {
        comp.source = TEMP_COMP_DEFAULT;
        comp.ppb_per_c = TEMP_COMP_DEFAULT_PPB_PER_C;
    }
    if (!comp.enabled)
    {
        comp.state = TEMP_COMP_OFF;
        comp.rate_ppb = 0;
    }
    else if (!comp.temperature_known || !comp.reference_known)
    {
        comp.state = TEMP_COMP_NO_TEMPERATURE;
        comp.rate_ppb = 0;
    }
    else
    {
        // Warmer and coefficient -: the crystal slow, so UTC - local grows.
        comp.state = TEMP_COMP_ACTIVE;
        comp.rate_ppb = -(int64_t)comp.ppb_per_c *
                        (comp.temperature_centic - comp.reference_centic) / 100;
    }
}


void temp_comp_restore(int32_t learnt_ppb_per_c, int32_t estimates)
{
    comp.learnt_ppb_per_c = estimates > 0 ? learnt_ppb_per_c : 0.0;
    comp.estimates = estimates > 0 ? estimates : 0;
    update_rate();
}


void temp_comp_configure(int64_t local_us, bool enabled, int32_t set)
{
    if (enabled == comp.enabled && set == set_ppb_per_c)
    {
        return;
    }
    accumulate(local_us);
    if (enabled && !comp.enabled)
    {
        comp.learning_since_us = local_us;
    }
    comp.enabled = enabled;
    set_ppb_per_c = set;
    update_rate();
}


static void clear_window(void)
{
    window_count = 0;
    window_head = 0;
    comp.window = 0;
    comp.window_range_centic = 0;
}


// The range of the last minutes + 1 readings held (fewer if not held).
static int32_t history_range(int minutes)
{
    int32_t low = INT32_MAX, high = INT32_MIN;
    int count = minutes + 1 < comp.minutes ? minutes + 1 : comp.minutes;

    for (int back = 0; back < count; back++)
    {
        int32_t value = history[(history_head + comp.minutes - 1 - back) % TEMP_COMP_HISTORY_MINUTES];

        low = value < low ? value : low;
        high = value > high ? value : high;
    }
    return count > 0 ? high - low : 0;
}


// A reading a minute: moving once it moves, steady again once it settles.
static void history_add(int64_t local_us, int32_t centic)
{
    if (comp.minutes > 0 && local_us < history_next_us)
    {
        return;
    }
    history_next_us = local_us + 60000000LL;
    if (comp.minutes == TEMP_COMP_HISTORY_MINUTES)
    {
        history[history_head] = centic;
        history_head = (history_head + 1) % TEMP_COMP_HISTORY_MINUTES;
    }
    else
    {
        history[(history_head + comp.minutes) % TEMP_COMP_HISTORY_MINUTES] = centic;
        comp.minutes++;
    }
    comp.moved_centic = history_range(TEMP_COMP_MOVING_MINUTES);
    if (comp.moved_centic >= TEMP_COMP_MOVING_CENTIC)
    {
        comp.moving = true;
        comp.steady_minutes = 0;
    }
    else if (comp.moving && comp.moved_centic < TEMP_COMP_STEADY_CENTIC)
    {
        if (++comp.steady_minutes >= TEMP_COMP_STEADY_MINUTES)
        {
            comp.moving = false;
            comp.steady_minutes = 0;
        }
    }
    else
    {
        comp.steady_minutes = 0;
    }
}


static void history_clear(void)
{
    history_head = 0;
    comp.minutes = 0;
    comp.moving = false;
    comp.moved_centic = 0;
    comp.steady_minutes = 0;
}


void temp_comp_temperature(int64_t local_us, int32_t centic)
{
    accumulate(local_us);
    if (!comp.reference_known)
    {
        comp.reference_known = true;
        comp.reference_centic = centic;
    }
    comp.temperature_known = true;
    comp.temperature_held = false;
    comp.temperature_centic = centic;
    comp.temperature_local_us = local_us;
    history_add(local_us, centic);
    update_rate();
}


void temp_comp_service(int64_t local_us)
{
    if (!comp.temperature_known)
    {
        return;
    }
    if (local_us - comp.temperature_local_us > TEMP_COMP_HOLD_US)
    {
        // Held at the last reading up to now, then none.
        accumulate(local_us);
        comp.temperature_known = false;
        comp.temperature_held = false;
        update_rate();
        clear_window(); // the exposure is not known from here
        history_clear();
    }
    else if (local_us - comp.temperature_local_us > TEMP_COMP_STALE_US)
    {
        comp.temperature_held = true; // the rate and exposure carry on at it
    }
}


int64_t temp_comp_us(int64_t local_us)
{
    if (!started)
    {
        return 0;
    }
    return (phase_ppb_us + comp.rate_ppb * (local_us - comp.phase_local_us)) / 1000000000LL;
}


int64_t temp_comp_rate_ppb(void)
{
    return comp.rate_ppb;
}


static double exposure_at(int64_t local_us)
{
    return exposure + exposure_rate() * (double)(local_us - comp.phase_local_us) / 1e6;
}


static LearnSample *window_at(int index)
{
    return &window[(window_head + index) % TEMP_COMP_LEARN_SAMPLES];
}


// a x = b for a 3 x 3 symmetric a: true, with x and a's inverse's [2][2]
// (c's variance factor). Gaussian elimination with partial pivoting.
static bool solve3(const double a_in[3][3], const double b_in[3], double x[3], double *inv22)
{
    double m[3][7];

    for (int row = 0; row < 3; row++)
    {
        for (int col = 0; col < 3; col++)
        {
            m[row][col] = a_in[row][col];
            m[row][3 + col] = row == col ? 1.0 : 0.0;
        }
        m[row][6] = b_in[row];
    }
    for (int col = 0; col < 3; col++)
    {
        int pivot = col;

        for (int row = col + 1; row < 3; row++)
        {
            if (fabs(m[row][col]) > fabs(m[pivot][col]))
            {
                pivot = row;
            }
        }
        if (fabs(m[pivot][col]) < 1e-12 * (fabs(a_in[col][col]) + 1e-300))
        {
            return false;
        }
        for (int k = 0; k < 7; k++)
        {
            double swap = m[col][k];

            m[col][k] = m[pivot][k];
            m[pivot][k] = swap;
        }
        for (int row = 0; row < 3; row++)
        {
            if (row != col)
            {
                double factor = m[row][col] / m[col][col];

                for (int k = col; k < 7; k++)
                {
                    m[row][k] -= factor * m[col][k];
                }
            }
        }
    }
    for (int row = 0; row < 3; row++)
    {
        x[row] = m[row][6] / m[row][row];
    }
    *inv22 = m[2][5] / m[2][2];
    return true;
}


// The columns of one sample against the window's first: t, t^2 (h, h^2),
// E (C h); y (us).
static void columns(const LearnSample *first, const LearnSample *s, double x[3], double *y)
{
    double t = (double)(s->local_us - first->local_us) / 3.6e9;

    x[0] = t;
    x[1] = t * t;
    x[2] = s->exposure / 3600.0;
    *y = (double)(s->offset_us - first->offset_us);
}


/*
 * Least squares of the window's samples (those whose round trip is near the
 * best's) to offset = a + b t + d t^2 + c E (temp_comp.h): true, with the
 * coefficient, its standard error (widened for the residuals' correlation),
 * the temperature range covered and that correlation. Centred, in hours, us
 * and C h, so the sums stay small; a by the centring.
 */
static bool fit(double *ppb_per_c, double *se_ppb, int32_t *range_centic, int32_t *turn_centic,
                double *rho)
{
    int64_t best = INT64_MAX;
    int used = 0;
    double mean[3] = {0.0, 0.0, 0.0}, my = 0.0;
    double xx[3][3] = {{0.0}}, xy[3] = {0.0};
    int32_t low = INT32_MAX, high = INT32_MIN, rise = 0, fall = 0;
    const LearnSample *first = window_at(0);

    for (int index = 0; index < window_count; index++)
    {
        if (window_at(index)->delay_us < best)
        {
            best = window_at(index)->delay_us;
        }
    }
    int64_t limit = best + (best / 2 > 1000 ? best / 2 : 1000);

    for (int index = 0; index < window_count; index++)
    {
        const LearnSample *s = window_at(index);
        double x[3], y;

        if (s->delay_us > limit)
        {
            continue;
        }
        columns(first, s, x, &y);
        used++;
        for (int k = 0; k < 3; k++)
        {
            mean[k] += x[k];
        }
        my += y;
        // The largest rise and fall so far: against the lowest and highest
        // before.
        if (used > 1)
        {
            rise = s->centic - low > rise ? s->centic - low : rise;
            fall = high - s->centic > fall ? high - s->centic : fall;
        }
        low = s->centic < low ? s->centic : low;
        high = s->centic > high ? s->centic : high;
    }
    if (used < TEMP_COMP_LEARN_MIN_SAMPLES)
    {
        return false;
    }
    for (int k = 0; k < 3; k++)
    {
        mean[k] /= used;
    }
    my /= used;
    for (int index = 0; index < window_count; index++)
    {
        const LearnSample *s = window_at(index);
        double x[3], y;

        if (s->delay_us > limit)
        {
            continue;
        }
        columns(first, s, x, &y);
        y -= my;
        for (int k = 0; k < 3; k++)
        {
            x[k] -= mean[k];
        }
        for (int row = 0; row < 3; row++)
        {
            xy[row] += x[row] * y;
            for (int col = 0; col < 3; col++)
            {
                xx[row][col] += x[row] * x[col];
            }
        }
    }
    *range_centic = high - low;
    *turn_centic = rise < fall ? rise : fall;
    double beta[3], inv22;

    if (!solve3(xx, xy, beta, &inv22) || inv22 <= 0.0)
    {
        return false;
    }
    // The residuals in time order: their sum of squares, and their lag-1
    // autocorrelation.
    double ssr = 0.0, lagged = 0.0, previous = 0.0;
    bool have_previous = false;

    for (int index = 0; index < window_count; index++)
    {
        const LearnSample *s = window_at(index);
        double x[3], y;

        if (s->delay_us > limit)
        {
            continue;
        }
        columns(first, s, x, &y);
        double residual = y - my;

        for (int k = 0; k < 3; k++)
        {
            residual -= beta[k] * (x[k] - mean[k]);
        }
        ssr += residual * residual;
        if (have_previous)
        {
            lagged += residual * previous;
        }
        previous = residual;
        have_previous = true;
    }
    double r = ssr > 0.0 ? lagged / ssr : 0.0;

    r = r < 0.0 ? 0.0 : (r > TEMP_COMP_LEARN_MAX_RHO ? TEMP_COMP_LEARN_MAX_RHO : r);
    *rho = r;
    // Residual variance on used - 4 degrees of freedom; c's variance
    // s2 x inv22, widened for the correlation.
    double s2 = ssr / (used - 4);
    double se_c = sqrt(s2 * inv22) * sqrt((1.0 + r) / (1.0 - r));

    // offset (us) = ... + c (us per C h) = c / 3600 us per C s; the phase
    // added is -k E ns, so k = -1000 c / 3600 ppb/C.
    *ppb_per_c = -1000.0 * beta[2] / 3600.0;
    *se_ppb = 1000.0 * se_c / 3600.0;
    return true;
}


void temp_comp_learn(int64_t local_us, int64_t offset_us, int64_t delay_us, int server)
{
    double estimate, se, rho;
    int32_t range, turn;
    LearnSample *slot;

    accumulate(local_us);
    if (!comp.temperature_known || !comp.reference_known)
    {
        clear_window();
        return;
    }
    if (server != learn_server)
    {
        // Another path: another asymmetry, another line.
        clear_window();
        learn_server = server;
    }
    // Out with the old.
    while (window_count > 0 && local_us - window_at(0)->local_us > TEMP_COMP_LEARN_MAX_AGE_US)
    {
        window_head = (window_head + 1) % TEMP_COMP_LEARN_SAMPLES;
        window_count--;
    }
    if (window_count == TEMP_COMP_LEARN_SAMPLES)
    {
        window_head = (window_head + 1) % TEMP_COMP_LEARN_SAMPLES;
        window_count--;
    }
    slot = window_at(window_count);
    slot->local_us = local_us;
    slot->offset_us = offset_us;
    slot->delay_us = delay_us;
    slot->exposure = exposure_at(local_us);
    slot->centic = comp.temperature_centic;
    window_count++;
    comp.window = window_count;
    if (comp.estimates > 0 && comp.accepted_local_us == 0)
    {
        // Learnt before this start: the window starts afresh now.
        comp.accepted_local_us = local_us;
    }
    if (!fit(&estimate, &se, &range, &turn, &rho))
    {
        comp.window_range_centic = 0;
        return;
    }
    comp.window_range_centic = range;
    comp.last_ppb_per_c = estimate;
    comp.last_se_ppb = se;
    comp.last_range_centic = range;
    comp.last_local_us = local_us;
    comp.last_rho = rho;
    comp.last_turn_centic = turn;
    if (range < TEMP_COMP_LEARN_MIN_RANGE_CENTIC || turn < TEMP_COMP_LEARN_MIN_TURN_CENTIC ||
        se > TEMP_COMP_LEARN_MAX_SE_PPB ||
        fabs(estimate) > TEMP_COMP_MAX_PPB_PER_C ||
        (comp.accepted_local_us != 0 &&
         local_us - comp.accepted_local_us < TEMP_COMP_LEARN_NEW_US))
    {
        return;
    }
    // From the default (the first) or what was learnt, by the difference
    // over n, at most TEMP_COMP_LEARN_STEP_PPB.
    double from = comp.estimates > 0 ? comp.learnt_ppb_per_c : (double)TEMP_COMP_DEFAULT_PPB_PER_C;
    int weight;

    comp.estimates++;
    weight = comp.estimates < TEMP_COMP_LEARN_WEIGHT ? comp.estimates : TEMP_COMP_LEARN_WEIGHT;
    double step = (estimate - from) / weight;

    step = step > TEMP_COMP_LEARN_STEP_PPB ? TEMP_COMP_LEARN_STEP_PPB
           : step < -TEMP_COMP_LEARN_STEP_PPB ? -TEMP_COMP_LEARN_STEP_PPB
                                              : step;
    comp.learnt_ppb_per_c = from + step;
    comp.accepted_local_us = local_us;
    learnt_new = true;
    accumulate(local_us);
    update_rate();
}


TempCompPolls temp_comp_short_polls(int64_t local_us)
{
    if (!comp.enabled || !comp.temperature_known)
    {
        return TEMP_COMP_POLLS_NORMAL;
    }
    if (comp.moving)
    {
        bool settled = comp.source == TEMP_COMP_SET ||
                       (comp.source == TEMP_COMP_LEARNT && comp.estimates >= TEMP_COMP_LEARN_SETTLED);

        return settled ? TEMP_COMP_POLLS_MOVING : TEMP_COMP_POLLS_LEARNING;
    }
    if (comp.source == TEMP_COMP_DEFAULT && local_us - comp.learning_since_us < TEMP_COMP_START_US)
    {
        return TEMP_COMP_POLLS_START;
    }
    return TEMP_COMP_POLLS_NORMAL;
}


bool temp_comp_take_learnt(int32_t *ppb_per_c, int32_t *estimates)
{
    if (!learnt_new)
    {
        return false;
    }
    learnt_new = false;
    *ppb_per_c = (int32_t)lround(comp.learnt_ppb_per_c);
    *estimates = comp.estimates;
    return true;
}


void temp_comp_forget(int64_t local_us)
{
    accumulate(local_us);
    comp.learnt_ppb_per_c = 0.0;
    comp.estimates = 0;
    comp.last_local_us = 0;
    comp.accepted_local_us = 0;
    comp.learning_since_us = local_us;
    learnt_new = false;
    clear_window();
    update_rate();
}


const TempCompStatus *temp_comp_status(void)
{
    return &comp;
}


const char *temp_comp_state_name(TempCompState state)
{
    switch (state)
    {
        case TEMP_COMP_OFF: return "OFF";
        case TEMP_COMP_NO_TEMPERATURE: return "NO TEMPERATURE";
        case TEMP_COMP_ACTIVE: return "ACTIVE";
    }
    return "?";
}


uint32_t temp_comp_polls_s(TempCompPolls polls)
{
    switch (polls)
    {
        case TEMP_COMP_POLLS_NORMAL: return 0;
        case TEMP_COMP_POLLS_START:
        case TEMP_COMP_POLLS_LEARNING: return TEMP_COMP_SHORT_S;
        case TEMP_COMP_POLLS_MOVING: return TEMP_COMP_MOVING_S;
    }
    return 0;
}


const char *temp_comp_polls_name(TempCompPolls polls)
{
    switch (polls)
    {
        case TEMP_COMP_POLLS_NORMAL: return "normal";
        case TEMP_COMP_POLLS_START: return "start";
        case TEMP_COMP_POLLS_LEARNING: return "learning";
        case TEMP_COMP_POLLS_MOVING: return "temperature moving";
    }
    return "?";
}


const char *temp_comp_source_name(TempCompSource source)
{
    switch (source)
    {
        case TEMP_COMP_DEFAULT: return "default";
        case TEMP_COMP_LEARNT: return "learnt";
        case TEMP_COMP_SET: return "set";
    }
    return "?";
}
