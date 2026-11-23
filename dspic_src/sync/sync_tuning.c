#include "sync_tuning.h"

#include <stdio.h>
#include "../platform/format.h" // our printf (format.h: the library's is slow)
#include <string.h>

#include "../timing/timing_config.h"
#include "source_edge.h"
#include "source_ntp.h"

// Running statistics of a signed quantity.
typedef struct
{
    uint32_t count;
    int32_t min;
    int32_t max;
    int64_t sum;
    uint64_t sum_squares;
} TuningStat;

typedef struct
{
    bool active;
    uint8_t source;
    uint8_t input;
    uint8_t next_checkpoint;
    bool skip; // the first period was already running when the trial began
    int32_t hold_offset_q16;
    uint32_t periods;
    uint32_t last_period_sequence;
    int64_t error_q16; // held epoch minus reference edge, Q16 ticks
} Trial;

typedef struct
{
    // This minute.
    uint32_t epochs;
    TuningStat reference_period[PPS_INPUT_COUNT]; // ticks off nominal
    TuningStat loop_phase[PPS_INPUT_COUNT];       // ticks, measured epochs
    TuningStat watched_period[PPS_INPUT_COUNT];   // ticks off nominal
    TuningStat latency[PPS_INPUT_COUNT];          // watched minus captured, ticks
    TuningStat source_phase[SYNC_SOURCE_COUNT];   // ticks, from any edge
    uint32_t unpaired[PPS_INPUT_COUNT];           // captured, not watched
    uint32_t gate_rejects;
    uint32_t kalman_rejects;
    uint32_t last_period_sequence;

    // Cumulative.
    uint32_t minutes;
    uint32_t gate_rejects_seen;
    uint32_t kalman_rejects_seen;
    Trial trials[TUNING_TRIALS];
    uint32_t last_trial_start;
    bool trial_started;
    uint32_t trials_started;
    uint32_t trials_completed;
    uint32_t trials_aborted;
    TuningStat checkpoints[SYNC_SOURCE_COUNT][TUNING_CHECKPOINTS]; // ns
} Tuning;

static const uint16_t checkpoint_epochs[TUNING_CHECKPOINTS] = {10, 60, 300, 600, 1800, 3600, 7200};
static const char *const input_names[PPS_INPUT_COUNT] = {"GNSS PPS", "ESP PPS"};

static Tuning tuning;

// --------------------------------------------------------------------------
// Statistics
// --------------------------------------------------------------------------

static void stat_add(TuningStat *stat, int32_t value)
{
    if (stat->count == 0U || value < stat->min)
    {
        stat->min = value;
    }
    if (stat->count == 0U || value > stat->max)
    {
        stat->max = value;
    }
    stat->count++;
    stat->sum += value;
    stat->sum_squares += (uint64_t)((int64_t)value * value);
}


static uint32_t isqrt64(uint64_t value)
{
    uint64_t root = 0;
    uint64_t bit = (uint64_t)1 << 62;

    while (bit > value)
    {
        bit >>= 2;
    }
    while (bit != 0U)
    {
        if (value >= root + bit)
        {
            value -= root + bit;
            root = (root >> 1) + bit;
        }
        else
        {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}


// Mean x 10 (one decimal place), rounded.
static int32_t stat_mean_x10(const TuningStat *stat)
{
    int64_t scaled = stat->sum * 10;
    int64_t half = (int64_t)(stat->count / 2U);

    return (int32_t)((scaled >= 0 ? scaled + half : scaled - half) / (int64_t)stat->count);
}


// Standard deviation x 10, about the mean.
static uint32_t stat_sd_x10(const TuningStat *stat)
{
    int64_t n = (int64_t)stat->count;
    // n.sum_squares - sum^2 = n^2.variance; x100 for the decimal place.
    int64_t spread = (int64_t)stat->sum_squares * n - stat->sum * stat->sum;

    uint64_t root_x10;

    if (spread <= 0)
    {
        return 0;
    }
    root_x10 = (uint64_t)spread > UINT64_MAX / 100U ? (uint64_t)isqrt64((uint64_t)spread) * 10U
                                                    : isqrt64((uint64_t)spread * 100U);
    root_x10 /= stat->count;
    return root_x10 > INT32_MAX ? (uint32_t)INT32_MAX : (uint32_t)root_x10;
}


// RMS about zero.
static uint32_t stat_rms(const TuningStat *stat)
{
    return isqrt64(stat->sum_squares / stat->count);
}


static void print_decimal(int32_t value_x10)
{
    int32_t magnitude = value_x10 < 0 ? -value_x10 : value_x10;
    printf("%c%ld.%ld", value_x10 < 0 ? '-' : '+', (long)(magnitude / 10), (long)(magnitude % 10));
}


// "n 60 mean +1.2 sd 3.4 [-5, +7]", or "none".
static void print_stat(const char *label, const TuningStat *stat)
{
    printf(" %s", label);
    if (stat->count == 0U)
    {
        printf(" none");
        return;
    }
    printf(" n %lu mean ", (unsigned long)stat->count);
    print_decimal(stat_mean_x10(stat));
    printf(" sd ");
    print_decimal((int32_t)stat_sd_x10(stat));
    printf(" [%+ld, %+ld]", (long)stat->min, (long)stat->max);
}


// --------------------------------------------------------------------------
// Holdover trials
// --------------------------------------------------------------------------

static int32_t error_ns(int64_t error_q16)
{
    int64_t ns = error_q16 * (int64_t)SYNC_CAPTURE_RESOLUTION_NS;
    int64_t half = 32768;

    ns = (ns >= 0 ? ns + half : ns - half) / 65536;
    if (ns > INT32_MAX)
    {
        return INT32_MAX;
    }
    return ns < -INT32_MAX ? -INT32_MAX : (int32_t)ns;
}


static void start_trial(const TimingSnapshot *snapshot, uint8_t source)
{
    uint8_t index;

    for (index = 0; index < TUNING_TRIALS; index++)
    {
        Trial *trial = &tuning.trials[index];
        if (!trial->active)
        {
            memset(trial, 0, sizeof(*trial));
            trial->active = true;
            trial->source = source;
            trial->input = snapshot->capture.reference_input;
            trial->skip = true;
            trial->hold_offset_q16 = epoch_control_holdover_offset_q16();
            trial->last_period_sequence = snapshot->capture.reference_period_sequence;
            tuning.trials_started++;
            tuning.last_trial_start = snapshot->timer.sequence;
            tuning.trial_started = true;
            return;
        }
    }
}


static void abort_trial(Trial *trial)
{
    trial->active = false;
    tuning.trials_aborted++;
}


static void follow_trial(Trial *trial, const TimingSnapshot *snapshot, bool locked, uint8_t source)
{
    const PpsCaptureSample *capture = &snapshot->capture;
    int32_t offset_ticks;

    if (!locked || source != trial->source || capture->reference_input != trial->input)
    {
        abort_trial(trial);
        return;
    }
    if (!capture->reference_period_valid ||
        capture->reference_period_sequence == trial->last_period_sequence)
    {
        return;
    }
    if (capture->reference_period_sequence - trial->last_period_sequence != 1U)
    {
        abort_trial(trial);
        return;
    }
    trial->last_period_sequence = capture->reference_period_sequence;
    if (trial->skip)
    {
        trial->skip = false;
        return;
    }
    offset_ticks = (int32_t)(capture->reference_period - TIMING_NOMINAL_TICKS);
    trial->error_q16 += (int64_t)trial->hold_offset_q16 - (int64_t)offset_ticks * 65536;
    trial->periods++;
    if (trial->periods == checkpoint_epochs[trial->next_checkpoint])
    {
        stat_add(&tuning.checkpoints[trial->source][trial->next_checkpoint],
                 error_ns(trial->error_q16));
        if (++trial->next_checkpoint >= TUNING_CHECKPOINTS)
        {
            trial->active = false;
            tuning.trials_completed++;
        }
    }
}


static void follow_trials(const TimingSnapshot *snapshot, const SyncUpdate *update)
{
    const SyncStatus *status = sync_status();
    bool locked = status->state == SYNC_LOCKED;
    uint8_t index;

    for (index = 0; index < TUNING_TRIALS; index++)
    {
        if (tuning.trials[index].active)
        {
            follow_trial(&tuning.trials[index], snapshot, locked, status->source);
        }
    }
    if (locked && update->loop.kind == EPOCH_UPDATE_MEASURED &&
        (!tuning.trial_started ||
         snapshot->timer.sequence - tuning.last_trial_start >= TUNING_TRIAL_INTERVAL))
    {
        start_trial(snapshot, status->source);
    }
}


// --------------------------------------------------------------------------
// Interface
// --------------------------------------------------------------------------

void sync_tuning_init(void)
{
    memset(&tuning, 0, sizeof(tuning));
}


void sync_tuning_epoch(const TimingSnapshot *snapshot, const SyncUpdate *update)
{
    const SyncStatus *status = sync_status();
    const PpsCaptureSample *capture = &snapshot->capture;
    const EpochControl *loop = epoch_control_state();
    uint8_t input = capture->reference_input < PPS_INPUT_COUNT ? capture->reference_input : 0U;
    uint8_t index;

    tuning.epochs++;
    if (capture->reference_period_valid &&
        capture->reference_period_sequence != tuning.last_period_sequence)
    {
        tuning.last_period_sequence = capture->reference_period_sequence;
        stat_add(&tuning.reference_period[input],
                 (int32_t)(capture->reference_period - TIMING_NOMINAL_TICKS));
    }
    if (update->loop.kind == EPOCH_UPDATE_MEASURED && update->loop.phase_valid)
    {
        stat_add(&tuning.loop_phase[input], update->loop.utc_offset_ticks);
    }
    for (index = 0; index < PPS_INPUT_COUNT; index++)
    {
        const PpsMonitorEdge *watched = &snapshot->monitor.inputs[index];
        uint32_t latency;

        if (watched->valid && watched->period_valid)
        {
            stat_add(&tuning.watched_period[index],
                     (int32_t)(watched->period - TIMING_NOMINAL_TICKS));
        }
        if (source_edge_latency(snapshot, (PpsInput)index, &latency))
        {
            stat_add(&tuning.latency[index], (int32_t)latency);
        }
        else if (index == input && capture->reference_capture_valid)
        {
            tuning.unpaired[index]++;
        }
    }
    for (index = 0; index < SYNC_SOURCE_COUNT; index++)
    {
        if (status->sources[index].phase_valid)
        {
            stat_add(&tuning.source_phase[index], status->sources[index].phase_ticks);
        }
    }
    tuning.gate_rejects += loop->gate.rejected - tuning.gate_rejects_seen;
    tuning.gate_rejects_seen = loop->gate.rejected;
    tuning.kalman_rejects += loop->pps_kf.stats.rejected - tuning.kalman_rejects_seen;
    tuning.kalman_rejects_seen = loop->pps_kf.stats.rejected;

    follow_trials(snapshot, update);
}


static void print_trials(void)
{
    uint8_t source;
    uint8_t checkpoint;

    printf("Holdover trials: %lu started, %lu completed, %lu ended early, %u running\r\n",
           (unsigned long)tuning.trials_started, (unsigned long)tuning.trials_completed,
           (unsigned long)tuning.trials_aborted,
           (unsigned)(tuning.trials_started - tuning.trials_completed - tuning.trials_aborted));
    for (source = 0; source < SYNC_SOURCE_COUNT; source++)
    {
        if (tuning.checkpoints[source][0].count == 0U)
        {
            continue;
        }
        printf("  from %s (error held minus true, ns):\r\n", sync_source_name(source));
        for (checkpoint = 0; checkpoint < TUNING_CHECKPOINTS; checkpoint++)
        {
            const TuningStat *stat = &tuning.checkpoints[source][checkpoint];
            if (stat->count == 0U)
            {
                continue;
            }
            printf("    %5us n %3lu rms %8lu mean %+9ld [%+ld, %+ld]\r\n",
                   (unsigned)checkpoint_epochs[checkpoint], (unsigned long)stat->count,
                   (unsigned long)stat_rms(stat), (long)(stat->sum / (int64_t)stat->count),
                   (long)stat->min, (long)stat->max);
        }
    }
}


void sync_tuning_print(void)
{
    const PpsMonitorStats *watch = pps_monitor_stats();
    const SourceNtpState *ntp = source_ntp_state();
    uint8_t index;

    tuning.minutes++;
    printf("\r\n=== TUNING (minute %lu, %lu epochs; ticks of 25ns unless stated) ===\r\n",
           (unsigned long)tuning.minutes, (unsigned long)tuning.epochs);
    for (index = 0; index < PPS_INPUT_COUNT; index++)
    {
        printf("%s: captured period", input_names[index]);
        print_stat("", &tuning.reference_period[index]);
        printf(";");
        print_stat("loop phase", &tuning.loop_phase[index]);
        printf("\r\n  watched period");
        print_stat("", &tuning.watched_period[index]);
        printf(";");
        print_stat("latency", &tuning.latency[index]);
        printf("; unpaired %lu, edges %lu, implausible %lu\r\n",
               (unsigned long)tuning.unpaired[index], (unsigned long)watch->edges[index],
               (unsigned long)watch->implausible[index]);
    }
    for (index = 0; index < SYNC_SOURCE_COUNT; index++)
    {
        printf("%s phase vs epoch:", sync_source_name(index));
        print_stat("", &tuning.source_phase[index]);
        printf("\r\n");
    }
    printf("Loop: %s profile, gate rejects %lu, Kalman rejects %lu; watch: %lu unseen changes\r\n",
           epoch_control_state()->profile->name, (unsigned long)tuning.gate_rejects,
           (unsigned long)tuning.kalman_rejects, (unsigned long)watch->unseen);
    printf("TIME-NTP: %lu taken, %lu paired, %lu unpaired, %lu sequence slips, %lu polls, "
           "%lu corrections, edge estimate avg %+ldns (%lu left out)\r\n",
           (unsigned long)ntp->messages, (unsigned long)ntp->labelled,
           (unsigned long)ntp->unassociated, (unsigned long)ntp->sequence_slips,
           (unsigned long)ntp->updates, (unsigned long)ntp->corrections,
           (long)ntp->phase_average_ns, (unsigned long)ntp->phase_outliers);
    print_trials();

    // The next minute.
    memset(tuning.reference_period, 0, sizeof(tuning.reference_period));
    memset(tuning.loop_phase, 0, sizeof(tuning.loop_phase));
    memset(tuning.watched_period, 0, sizeof(tuning.watched_period));
    memset(tuning.latency, 0, sizeof(tuning.latency));
    memset(tuning.source_phase, 0, sizeof(tuning.source_phase));
    memset(tuning.unpaired, 0, sizeof(tuning.unpaired));
    tuning.epochs = 0;
    tuning.gate_rejects = 0;
    tuning.kalman_rejects = 0;
}
