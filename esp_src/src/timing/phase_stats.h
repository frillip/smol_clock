/*
 * Running statistics of a phase measured once a second: the count, mean,
 * standard deviation, RMS (about zero) and extremes, since a start and over
 * the last hour (sixty one-minute buckets, so the hour moves on a minute at
 * a time and a gap only empties the minutes it spans). Each minute's mean
 * is kept for a day, with a tag: the union of the small bit sets given with
 * its values (which NTP servers were in use, say), and whether they varied.
 * Portable.
 */
#ifndef PHASE_STATS_H
#define PHASE_STATS_H

#include <stdbool.h>
#include <stdint.h>

#define PHASE_STATS_MINUTES     60
#define PHASE_STATS_DAY_MINUTES 1440
#define PHASE_STATS_NO_MEAN     INT32_MIN // a minute with nothing
#define PHASE_STATS_TAG_BITS    0x3FU     // tags are 6 bits

typedef struct
{
    uint32_t count;
    double sum;     // ns
    double squares; // ns^2
    int64_t minimum;
    int64_t maximum;
} PhaseSum;

typedef struct
{
    PhaseSum total;
    PhaseSum minutes[PHASE_STATS_MINUTES];
    uint32_t minute; // the latest minute added to (seconds / 60)
    bool started;
    uint32_t since_s; // the first second added since the start
    int32_t means[PHASE_STATS_DAY_MINUTES]; // each minute's mean, ns, by minute % 1440
    uint8_t tags[PHASE_STATS_DAY_MINUTES];  // its tags' union, and varied and set flags
} PhaseStats;

void phase_stats_reset(PhaseStats *stats);
// One value, at a time in seconds (UTC, say).
void phase_stats_add(PhaseStats *stats, uint32_t seconds, int64_t value_ns);
// The same, tagged (PHASE_STATS_TAG_BITS).
void phase_stats_add_tagged(PhaseStats *stats, uint32_t seconds, int64_t value_ns, uint8_t tag);
// The last hour to now_s (the minutes before it that have been seen).
void phase_stats_hour(const PhaseStats *stats, uint32_t now_s, PhaseSum *hour);
// One minute of it: ago 0 is now_s's minute, up to PHASE_STATS_MINUTES - 1.
// Empty (count 0) if nothing was added in it, or it has gone.
void phase_stats_minute(const PhaseStats *stats, uint32_t now_s, uint32_t ago, PhaseSum *minute);

// One minute's mean, ns: ago 0 is now_s's minute, up to
// PHASE_STATS_DAY_MINUTES - 1. False if nothing was added in it, or it has
// gone.
bool phase_stats_minute_mean(const PhaseStats *stats, uint32_t now_s, uint32_t ago,
                             int32_t *mean_ns);

// The tags of that minute: their union, and whether they differed within
// it. False as phase_stats_minute_mean().
bool phase_stats_minute_tag(const PhaseStats *stats, uint32_t now_s, uint32_t ago, uint8_t *tag,
                            bool *varied);

double phase_sum_mean(const PhaseSum *sum);
double phase_sum_sd(const PhaseSum *sum); // about the mean
double phase_sum_rms(const PhaseSum *sum); // about zero

#endif // PHASE_STATS_H
