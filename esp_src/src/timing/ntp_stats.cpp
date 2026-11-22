#include "ntp_stats.h"

#include <float.h>
#include <math.h>
#include <string.h>

/*
 * chronyd's tables (regress.c). Critical runs: the most runs among n
 * residuals that still fail the runs test at 5 %, for n up to 129.
 * Student's t: the 99.95 % quantile for 1 to 40 degrees of freedom, 3.5
 * beyond.
 */
static const uint8_t critical_runs[] = {
    0,  0,  0,  0,  0,  0,  0,  0,  2,  3,  3,  3,  4,  4,  5,  5,  5,  6,  6,  7,  7,  7,
    8,  8,  9,  9,  9,  10, 10, 11, 11, 11, 12, 12, 13, 13, 14, 14, 14, 15, 15, 16, 16, 17,
    17, 18, 18, 18, 19, 19, 20, 20, 21, 21, 21, 22, 22, 23, 23, 24, 24, 25, 25, 26, 26, 26,
    27, 27, 28, 28, 29, 29, 30, 30, 30, 31, 31, 32, 32, 33, 33, 34, 34, 35, 35, 35, 36, 36,
    37, 37, 38, 38, 39, 39, 40, 40, 40, 41, 41, 42, 42, 43, 43, 44, 44, 45, 45, 46, 46, 46,
    47, 47, 48, 48, 49, 49, 50, 50, 51, 51, 52, 52, 52, 53, 53, 54, 54, 55, 55, 56,
};

static const float t_coefficients[] = {
    636.6f, 31.6f,  12.92f, 8.61f,  6.869f, 5.959f, 5.408f, 5.041f, 4.781f, 4.587f,
    4.437f, 4.318f, 4.221f, 4.140f, 4.073f, 4.015f, 3.965f, 3.922f, 3.883f, 3.850f,
    3.819f, 3.792f, 3.768f, 3.745f, 3.725f, 3.707f, 3.690f, 3.674f, 3.659f, 3.646f,
    3.633f, 3.622f, 3.611f, 3.601f, 3.591f, 3.582f, 3.574f, 3.566f, 3.558f, 3.551f,
};

#define TABLE_SIZE(table) ((int)(sizeof(table) / sizeof((table)[0])))


int ntp_stats_critical_runs(int points)
{
    if (points < 0)
    {
        return 0;
    }
    return critical_runs[points < TABLE_SIZE(critical_runs) ? points : TABLE_SIZE(critical_runs) - 1];
}


double ntp_stats_t_coefficient(int degrees_of_freedom)
{
    if (degrees_of_freedom < 1)
    {
        degrees_of_freedom = 1;
    }
    if (degrees_of_freedom > TABLE_SIZE(t_coefficients))
    {
        return 3.5;
    }
    return t_coefficients[degrees_of_freedom - 1];
}


void ntp_stats_reset(NtpStats *stats)
{
    memset(stats, 0, sizeof(*stats));
    stats->skew_ppb = NTP_STATS_WORST_SKEW_PPB;
    stats->frequency_sd_ppb = NTP_STATS_WORST_SKEW_PPB;
    stats->offset_sd_us = NTP_STATS_WORST_SD_US;
    stats->sd_us = NTP_STATS_WORST_SD_US;
}


// Sample i, 0 the oldest in the line, negative into the runs samples.
static const NtpStatsSample *sample_at(const NtpStats *stats, int i)
{
    int index = (int)stats->latest - (stats->samples - 1 - i);

    index %= NTP_STATS_RING;
    if (index < 0)
    {
        index += NTP_STATS_RING;
    }
    return &stats->ring[index];
}


const NtpStatsSample *ntp_stats_latest(const NtpStats *stats)
{
    return &stats->ring[stats->latest];
}


void ntp_stats_shift(NtpStats *stats, int64_t delta_us)
{
    for (int i = 0; i < NTP_STATS_RING; i++)
    {
        stats->ring[i].offset_us += delta_us;
    }
    stats->offset_us += delta_us;
}


int64_t ntp_stats_span_us(const NtpStats *stats)
{
    if (stats->samples == 0)
    {
        return 0;
    }
    return ntp_stats_latest(stats)->local_us - sample_at(stats, 0)->local_us;
}


// The oldest `drop` samples leave the line for the runs samples, which keep
// no more than the line holds (chronyd's prune_register).
static void prune(NtpStats *stats, int drop)
{
    int runs;

    if (drop <= 0)
    {
        return;
    }
    stats->samples = (uint8_t)(stats->samples - drop);
    runs = stats->runs_samples + drop;
    if (runs > stats->samples * (NTP_STATS_RUNS_RATIO - 1))
    {
        runs = stats->samples * (NTP_STATS_RUNS_RATIO - 1);
    }
    stats->runs_samples = (uint8_t)runs;
}


void ntp_stats_add(NtpStats *stats, const NtpStatsSample *sample)
{
    if (stats->samples >= NTP_STATS_MAX_SAMPLES)
    {
        prune(stats, 1);
    }
    if (stats->samples != 0 || stats->runs_samples != 0)
    {
        stats->latest = (uint8_t)((stats->latest + 1) % NTP_STATS_RING);
    }
    stats->ring[stats->latest] = *sample;
    stats->samples++;
    stats->added++;
    ntp_stats_regress(stats);
}


static double median(const double *values, int count)
{
    double sorted[NTP_STATS_MAX_SAMPLES];
    int i;

    memcpy(sorted, values, (size_t)count * sizeof(sorted[0]));
    for (i = 1; i < count; i++)
    {
        double value = sorted[i];
        int j = i;

        for (; j > 0 && sorted[j - 1] > value; j--)
        {
            sorted[j] = sorted[j - 1];
        }
        sorted[j] = value;
    }
    return count % 2 != 0 ? sorted[count / 2] : 0.5 * (sorted[count / 2 - 1] + sorted[count / 2]);
}


static int count_runs(const double *residuals, int count)
{
    int runs = 1;
    int i;

    for (i = 1; i < count; i++)
    {
        if (!((residuals[i - 1] < 0.0 && residuals[i] < 0.0) ||
              (residuals[i - 1] > 0.0 && residuals[i] > 0.0)))
        {
            runs++;
        }
    }
    return runs;
}


typedef struct
{
    double intercept; // at the latest sample
    double slope;     // us per s
    double variance;  // of a sample at weight 1
    double intercept_sd;
    double slope_sd;
    int start;
    int runs;
    int degrees;
} Line;


/*
 * chronyd's RGR_FindBestRegression: x the time back from the latest sample
 * (s, <= 0), y the offset (us), w the variances; x and y from -extra on are
 * valid (the runs samples), w only from 0. False with too few samples.
 */
static bool best_line(const double *x, const double *y, const double *w, int n, int extra,
                      double min_span_s, Line *line)
{
    double residuals[NTP_STATS_RING];
    double P, Q, U, V, W, u, a = 0.0, b = 0.0;
    double squares;
    int start = 0;
    int first;
    int runs;
    int points;
    int i;

    if (n < NTP_STATS_MIN_REGRESS)
    {
        return false;
    }
    for (;;)
    {
        W = U = 0.0;
        for (i = start; i < n; i++)
        {
            U += x[i] / w[i];
            W += 1.0 / w[i];
        }
        u = U / W;
        P = Q = V = 0.0;
        for (i = start; i < n; i++)
        {
            double ui = x[i] - u;

            P += y[i] / w[i];
            Q += y[i] * ui / w[i];
            V += ui * ui / w[i];
        }
        b = Q / V;
        a = P / W - b * u;

        // Residuals also for the samples before start, the runs ones too,
        // up to RUNS_RATIO times the samples in the line.
        first = n - (n - start) * NTP_STATS_RUNS_RATIO;
        if (first < -extra)
        {
            first = -extra;
        }
        for (i = first; i < n; i++)
        {
            residuals[i - first] = y[i] - a - b * x[i];
        }
        runs = count_runs(residuals, n - first);
        // x is seconds back from the latest sample: dropping one more would
        // leave the line from start + 1.
        if (runs > ntp_stats_critical_runs(n - first) || n - start <= NTP_STATS_MIN_REGRESS ||
            n - start <= NTP_STATS_MIN_SAMPLES || x[n - 1] - x[start + 1] < min_span_s)
        {
            if (start != first)
            {
                runs = count_runs(residuals + (start - first), n - start);
            }
            break;
        }
        start++;
    }

    squares = 0.0;
    for (i = start; i < n; i++)
    {
        double r = residuals[i - first];

        squares += r * r / w[i];
    }
    points = n - start;
    squares /= (double)(points - 2);
    line->slope = b;
    line->intercept = a;
    line->slope_sd = sqrt(squares / V);
    {
        double aa = u * line->slope_sd;

        line->intercept_sd = sqrt(squares / W + aa * aa);
    }
    line->variance = squares * (double)points / W;
    line->start = start;
    line->runs = runs;
    line->degrees = points - 2;
    return true;
}


// The sample in the line offering the least distance, given the skew
// (chronyd's find_best_sample_index).
static void find_best(NtpStats *stats)
{
    const NtpStatsSample *latest = ntp_stats_latest(stats);
    double best_distance = DBL_MAX;
    int i;

    stats->best = 0;
    for (i = 0; i < stats->samples; i++)
    {
        const NtpStatsSample *sample = sample_at(stats, i);
        double elapsed = (double)(latest->local_us - sample->local_us) * 1e-6;
        double distance =
            sample->server_us + 0.5 * sample->delay_us + elapsed * stats->skew_ppb * 1e-3;

        if (distance < best_distance)
        {
            best_distance = distance;
            stats->best = (uint8_t)i;
        }
    }
}


void ntp_stats_regress(NtpStats *stats)
{
    double x[NTP_STATS_RING];
    double y[NTP_STATS_RING];
    double distances[NTP_STATS_MAX_SAMPLES];
    double w[NTP_STATS_MAX_SAMPLES];
    const NtpStatsSample *latest;
    double least = DBL_MAX;
    double sd;
    int n = stats->samples;
    int extra = stats->runs_samples;
    Line line;
    int i;

    if (n == 0)
    {
        return;
    }
    latest = ntp_stats_latest(stats);
    for (i = -extra; i < n; i++)
    {
        const NtpStatsSample *sample = sample_at(stats, i);

        x[i + extra] = (double)(sample->local_us - latest->local_us) * 1e-6;
        y[i + extra] = (double)(sample->offset_us - latest->offset_us);
    }
    for (i = 0; i < n; i++)
    {
        distances[i] = 0.5 * sample_at(stats, i)->delay_us + NTP_STATS_PRECISION_US;
        least = distances[i] < least ? distances[i] : least;
    }
    sd = (median(distances, n) - least) / NTP_STATS_SD_TO_DIST;
    sd = sd < NTP_STATS_PRECISION_US ? NTP_STATS_PRECISION_US : sd > least ? least : sd;
    least += NTP_STATS_PRECISION_US;
    for (i = 0; i < n; i++)
    {
        double weight = 1.0;

        if (distances[i] > least)
        {
            weight += (distances[i] - least) / sd;
        }
        w[i] = weight * weight;
    }

    stats->fitted = best_line(x + extra, y + extra, w, n, extra, stats->min_span_s, &line);
    stats->offset_local_us = latest->local_us;
    if (stats->fitted)
    {
        double skew = line.slope_sd * ntp_stats_t_coefficient(line.degrees) * 1000.0;

        stats->offset_us = latest->offset_us + (int64_t)llround(line.intercept);
        stats->offset_sd_us = line.intercept_sd;
        stats->frequency_ppb = line.slope * 1000.0;
        stats->frequency_sd_ppb = line.slope_sd * 1000.0 < 1e-3 ? 1e-3 : line.slope_sd * 1000.0;
        stats->skew_ppb = skew < 1e-3 ? 1e-3 : skew > 1e11 ? 1e11 : skew;
        stats->sd_us = sqrt(line.variance) > 1e-3 ? sqrt(line.variance) : 1e-3;
        stats->used = (uint8_t)(n - line.start);
        stats->runs = (uint8_t)line.runs;
        prune(stats, line.start);
    }
    else
    {
        // The latest sample alone; the frequency kept from before.
        stats->offset_us = latest->offset_us;
        stats->offset_sd_us = NTP_STATS_WORST_SD_US;
        stats->frequency_sd_ppb = NTP_STATS_WORST_SKEW_PPB;
        stats->skew_ppb = NTP_STATS_WORST_SKEW_PPB;
        stats->sd_us = NTP_STATS_WORST_SD_US;
        stats->used = (uint8_t)n;
        stats->runs = 0;
    }
    find_best(stats);
}


int64_t ntp_stats_offset_at(const NtpStats *stats, int64_t local_us)
{
    double elapsed = (double)(local_us - stats->offset_local_us);

    if (!stats->fitted)
    {
        return stats->offset_us;
    }
    return stats->offset_us + (int64_t)llround(elapsed * stats->frequency_ppb * 1e-9);
}


int64_t ntp_stats_distance_capped_us(const NtpStats *stats, int64_t local_us, double skew_cap_ppb)
{
    const NtpStatsSample *best = sample_at(stats, stats->best);
    double elapsed = (double)(local_us - best->local_us) * 1e-6;
    double skew = stats->skew_ppb < skew_cap_ppb ? stats->skew_ppb : skew_cap_ppb;
    double distance;

    if (elapsed < 0.0)
    {
        elapsed = 0.0;
    }
    distance = best->server_us + 0.5 * best->delay_us + elapsed * skew * 1e-3;
    return distance > 1e12 ? (int64_t)1e12 : (int64_t)llround(distance);
}


int64_t ntp_stats_distance_us(const NtpStats *stats, int64_t local_us)
{
    return ntp_stats_distance_capped_us(stats, local_us, 1e18);
}
