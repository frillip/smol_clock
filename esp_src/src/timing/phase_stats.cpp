#include "phase_stats.h"

#include <math.h>
#include <string.h>

#define TAG_VARIED 0x40U
#define TAG_SET    0x80U

static void clear_sum(PhaseSum *sum)
{
    memset(sum, 0, sizeof(*sum));
}


static void add_to(PhaseSum *sum, int64_t value)
{
    if (sum->count == 0 || value < sum->minimum)
    {
        sum->minimum = value;
    }
    if (sum->count == 0 || value > sum->maximum)
    {
        sum->maximum = value;
    }
    sum->count++;
    sum->sum += (double)value;
    sum->squares += (double)value * (double)value;
}


static void merge(PhaseSum *into, const PhaseSum *from)
{
    if (from->count == 0)
    {
        return;
    }
    if (into->count == 0 || from->minimum < into->minimum)
    {
        into->minimum = from->minimum;
    }
    if (into->count == 0 || from->maximum > into->maximum)
    {
        into->maximum = from->maximum;
    }
    into->count += from->count;
    into->sum += from->sum;
    into->squares += from->squares;
}


void phase_stats_reset(PhaseStats *stats)
{
    uint32_t index;

    memset(stats, 0, sizeof(*stats));
    for (index = 0; index < PHASE_STATS_DAY_MINUTES; index++)
    {
        stats->means[index] = PHASE_STATS_NO_MEAN;
    }
}


void phase_stats_add(PhaseStats *stats, uint32_t seconds, int64_t value_ns)
{
    phase_stats_add_tagged(stats, seconds, value_ns, 0);
}


void phase_stats_add_tagged(PhaseStats *stats, uint32_t seconds, int64_t value_ns, uint8_t tag)
{
    uint32_t minute = seconds / 60U;

    if (!stats->started)
    {
        uint32_t index;

        // A zeroed PhaseStats (static, never reset) as well as a reset one.
        for (index = 0; index < PHASE_STATS_DAY_MINUTES; index++)
        {
            stats->means[index] = PHASE_STATS_NO_MEAN;
            stats->tags[index] = 0;
        }
        stats->started = true;
        stats->minute = minute;
        stats->since_s = seconds;
    }
    else if (minute != stats->minute)
    {
        // The minutes since the latest one, emptied for reuse (all of them
        // after a gap of an hour or more, or a time that went back).
        uint32_t gap = minute - stats->minute;
        uint32_t day_gap = gap;
        uint32_t index;

        if (minute < stats->minute || gap >= PHASE_STATS_MINUTES)
        {
            gap = PHASE_STATS_MINUTES;
        }
        if (minute < stats->minute || day_gap >= PHASE_STATS_DAY_MINUTES)
        {
            day_gap = PHASE_STATS_DAY_MINUTES;
        }
        for (index = 1; index <= gap; index++)
        {
            clear_sum(&stats->minutes[(stats->minute + index) % PHASE_STATS_MINUTES]);
        }
        for (index = 1; index <= day_gap; index++)
        {
            stats->means[(stats->minute + index) % PHASE_STATS_DAY_MINUTES] = PHASE_STATS_NO_MEAN;
            stats->tags[(stats->minute + index) % PHASE_STATS_DAY_MINUTES] = 0;
        }
        stats->minute = minute;
    }
    add_to(&stats->total, value_ns);
    add_to(&stats->minutes[minute % PHASE_STATS_MINUTES], value_ns);
    {
        double mean = phase_sum_mean(&stats->minutes[minute % PHASE_STATS_MINUTES]);

        // Within +/-2.1 s, and never the marker for none.
        mean = mean > 2147483647.0 ? 2147483647.0 : mean < -2147483647.0 ? -2147483647.0 : mean;
        stats->means[minute % PHASE_STATS_DAY_MINUTES] = (int32_t)llround(mean);
    }
    {
        uint8_t *slot = &stats->tags[minute % PHASE_STATS_DAY_MINUTES];

        tag &= PHASE_STATS_TAG_BITS;
        if ((*slot & TAG_SET) != 0 && (*slot & PHASE_STATS_TAG_BITS) != tag)
        {
            *slot |= TAG_VARIED;
        }
        *slot |= (uint8_t)(tag | TAG_SET);
    }
}


void phase_stats_hour(const PhaseStats *stats, uint32_t now_s, PhaseSum *hour)
{
    uint32_t now_minute = now_s / 60U;
    uint32_t index;

    clear_sum(hour);
    if (!stats->started || now_minute < stats->minute)
    {
        return;
    }
    // The buckets still holding one of the last sixty minutes.
    for (index = 0; index < PHASE_STATS_MINUTES; index++)
    {
        uint32_t minute = stats->minute - index;

        if (now_minute - minute < PHASE_STATS_MINUTES && minute <= stats->minute &&
            index <= stats->minute)
        {
            merge(hour, &stats->minutes[minute % PHASE_STATS_MINUTES]);
        }
    }
}


void phase_stats_minute(const PhaseStats *stats, uint32_t now_s, uint32_t ago, PhaseSum *minute)
{
    uint32_t now_minute = now_s / 60U;
    uint32_t wanted;

    clear_sum(minute);
    if (!stats->started || ago >= PHASE_STATS_MINUTES || ago > now_minute)
    {
        return;
    }
    wanted = now_minute - ago;
    // Held only if it is no later than the latest minute added, and within
    // the hour before it (the buckets after it have not been reused yet).
    if (wanted > stats->minute || stats->minute - wanted >= PHASE_STATS_MINUTES)
    {
        return;
    }
    *minute = stats->minutes[wanted % PHASE_STATS_MINUTES];
}


bool phase_stats_minute_mean(const PhaseStats *stats, uint32_t now_s, uint32_t ago,
                             int32_t *mean_ns)
{
    uint32_t now_minute = now_s / 60U;
    uint32_t wanted;

    if (!stats->started || ago >= PHASE_STATS_DAY_MINUTES || ago > now_minute)
    {
        return false;
    }
    wanted = now_minute - ago;
    // As phase_stats_minute(), over the day.
    if (wanted > stats->minute || stats->minute - wanted >= PHASE_STATS_DAY_MINUTES)
    {
        return false;
    }
    *mean_ns = stats->means[wanted % PHASE_STATS_DAY_MINUTES];
    return *mean_ns != PHASE_STATS_NO_MEAN;
}


bool phase_stats_minute_tag(const PhaseStats *stats, uint32_t now_s, uint32_t ago, uint8_t *tag,
                            bool *varied)
{
    int32_t mean;
    uint8_t slot;

    if (!phase_stats_minute_mean(stats, now_s, ago, &mean))
    {
        return false;
    }
    slot = stats->tags[(now_s / 60U - ago) % PHASE_STATS_DAY_MINUTES];
    *tag = slot & PHASE_STATS_TAG_BITS;
    *varied = (slot & TAG_VARIED) != 0;
    return true;
}


double phase_sum_mean(const PhaseSum *sum)
{
    return sum->count != 0 ? sum->sum / sum->count : 0.0;
}


double phase_sum_sd(const PhaseSum *sum)
{
    double mean;
    double variance;

    if (sum->count < 2)
    {
        return 0.0;
    }
    mean = sum->sum / sum->count;
    variance = (sum->squares - sum->count * mean * mean) / (sum->count - 1);
    return variance > 0.0 ? sqrt(variance) : 0.0;
}


double phase_sum_rms(const PhaseSum *sum)
{
    return sum->count != 0 ? sqrt(sum->squares / sum->count) : 0.0;
}
