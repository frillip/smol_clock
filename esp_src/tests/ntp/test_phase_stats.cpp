// phase_stats: the sums, the hour moving on a minute at a time, gaps.
#include <math.h>
#include <stdio.h>

#include "../../src/timing/phase_stats.h"

static int failures, checks;
#define CHECK(cond)                                          \
    do                                                       \
    {                                                        \
        checks++;                                            \
        if (!(cond))                                         \
        {                                                    \
            failures++;                                      \
            printf("  FAIL line %d: %s\n", __LINE__, #cond); \
        }                                                    \
    } while (0)

static bool near(double a, double b)
{
    return fabs(a - b) < 1e-6 * (fabs(b) + 1.0);
}


int main(void)
{
    PhaseStats stats;
    PhaseSum hour;
    const uint32_t t0 = 1800000000U; // on a minute

    printf("Phase statistics\n");
    phase_stats_reset(&stats);
    phase_stats_hour(&stats, t0, &hour);
    CHECK(hour.count == 0 && stats.total.count == 0);

    // -100, +100 alternating: mean 0, RMS 100, sd 100 * sqrt(n / (n - 1)).
    for (uint32_t s = 0; s < 600; s++)
    {
        phase_stats_add(&stats, t0 + s, (s & 1) ? 100 : -100);
    }
    CHECK(stats.total.count == 600 && near(phase_sum_mean(&stats.total), 0.0));
    CHECK(near(phase_sum_rms(&stats.total), 100.0));
    CHECK(near(phase_sum_sd(&stats.total), 100.0 * sqrt(600.0 / 599.0)));
    CHECK(stats.total.minimum == -100 && stats.total.maximum == 100 && stats.since_s == t0);
    phase_stats_hour(&stats, t0 + 599, &hour);
    CHECK(hour.count == 600);

    // Two hours of +50: the last hour holds only those, the total all.
    for (uint32_t s = 600; s < 600 + 7200; s++)
    {
        phase_stats_add(&stats, t0 + s, 50);
    }
    phase_stats_hour(&stats, t0 + 7799, &hour);
    CHECK(hour.count == 3600 && near(phase_sum_mean(&hour), 50.0) && near(phase_sum_sd(&hour), 0.0));
    CHECK(hour.minimum == 50 && hour.maximum == 50);
    CHECK(stats.total.count == 7800 && stats.total.minimum == -100);

    // Asked later with nothing new: minutes age out of the hour.
    phase_stats_hour(&stats, t0 + 7799 + 1800, &hour);
    CHECK(hour.count == 1800);
    phase_stats_hour(&stats, t0 + 7799 + 3600, &hour);
    CHECK(hour.count == 0);

    // A gap of 20 minutes, then more: the hour spans the gap.
    for (uint32_t s = 7800 + 1200; s < 7800 + 1200 + 600; s++)
    {
        phase_stats_add(&stats, t0 + s, -20);
    }
    phase_stats_hour(&stats, t0 + 7800 + 1799, &hour);
    CHECK(hour.count == 600 + 1800 && hour.minimum == -20 && hour.maximum == 50); // 30 min of +50

    // A gap of over an hour: only what follows.
    phase_stats_add(&stats, t0 + 20000, 7);
    phase_stats_hour(&stats, t0 + 20000, &hour);
    CHECK(hour.count == 1 && hour.minimum == 7);

    // Time going back (the clock stepped): the hour starts again.
    phase_stats_add(&stats, t0 + 10000, 9);
    phase_stats_hour(&stats, t0 + 10000, &hour);
    CHECK(hour.count == 1 && hour.maximum == 9);

    // Single minutes: the latest, one with nothing, one gone.
    {
        PhaseStats one;
        PhaseSum minute;

        phase_stats_reset(&one);
        for (uint32_t s = 0; s < 60; s++)
            phase_stats_add(&one, t0 + s, 10);
        for (uint32_t s = 120; s < 180; s++)
            phase_stats_add(&one, t0 + s, 30);
        phase_stats_minute(&one, t0 + 179, 0, &minute);
        CHECK(minute.count == 60 && near(phase_sum_mean(&minute), 30.0));
        phase_stats_minute(&one, t0 + 179, 1, &minute);
        CHECK(minute.count == 0);
        phase_stats_minute(&one, t0 + 179, 2, &minute);
        CHECK(minute.count == 60 && near(phase_sum_mean(&minute), 10.0));
        phase_stats_minute(&one, t0 + 179 + 60, 0, &minute); // a minute on, nothing new
        CHECK(minute.count == 0);
        phase_stats_minute(&one, t0 + 179 + 59 * 60, 59, &minute); // the 30s, 59 minutes ago
        CHECK(minute.count == 60 && near(phase_sum_mean(&minute), 30.0));
        phase_stats_minute(&one, t0 + 179 + 60 * 60, 59, &minute); // gone
        CHECK(minute.count == 0);
    }

    // Minute means kept for a day: a minute 23 h 59 m ago still there, one
    // a day ago gone, a minute with nothing empty. Zeroed (static) works.
    {
        static PhaseStats day; // zeroed, not reset
        int32_t mean;

        for (uint32_t s = 0; s < 60; s++)
            phase_stats_add(&day, t0 + s, -1500);
        for (uint32_t s = 120; s < 180; s++)
            phase_stats_add(&day, t0 + s, s < 150 ? 1000 : 3000);
        CHECK(phase_stats_minute_mean(&day, t0 + 179, 0, &mean) && mean == 2000);
        CHECK(!phase_stats_minute_mean(&day, t0 + 179, 1, &mean));
        CHECK(phase_stats_minute_mean(&day, t0 + 179, 2, &mean) && mean == -1500);
        CHECK(!phase_stats_minute_mean(&day, t0 + 179, 3, &mean));
        // Asked later: 23 h 59 m on the first minute is still there, a day on it is not.
        CHECK(phase_stats_minute_mean(&day, t0 + 1439 * 60, 1439, &mean) && mean == -1500);
        CHECK(!phase_stats_minute_mean(&day, t0 + 1440 * 60, 1440, &mean));
        // Added to a day later: the day's buckets are reused, the old one gone.
        phase_stats_add(&day, t0 + 1440 * 60 + 5, 7000);
        CHECK(phase_stats_minute_mean(&day, t0 + 1440 * 60 + 5, 0, &mean) && mean == 7000);
        CHECK(phase_stats_minute_mean(&day, t0 + 1440 * 60 + 5, 1438, &mean) && mean == 2000);
        CHECK(!phase_stats_minute_mean(&day, t0 + 1440 * 60 + 5, 1439, &mean));
        // An hour gone but within the day: the summary's hour empty, the mean kept.
        PhaseSum minute;
        phase_stats_minute(&day, t0 + 1440 * 60 + 5 + 3600, 60 - 1, &minute);
        CHECK(minute.count == 0);
        CHECK(phase_stats_minute_mean(&day, t0 + 1440 * 60 + 5 + 3600, 60, &mean) && mean == 7000);
    }

    // Tags: the union over a minute, varied if they differed; cleared with
    // the minute.
    {
        static PhaseStats tagged;
        uint8_t tag = 0;
        bool varied = true;

        for (uint32_t s = 0; s < 60; s++)
            phase_stats_add_tagged(&tagged, t0 + s, 5, 0x04); // server 3 all minute
        for (uint32_t s = 60; s < 120; s++)
            phase_stats_add_tagged(&tagged, t0 + s, 5, s < 90 ? 0x04 : 0x0C); // 3, then 3 and 4
        CHECK(phase_stats_minute_tag(&tagged, t0 + 119, 1, &tag, &varied) && tag == 0x04 && !varied);
        CHECK(phase_stats_minute_tag(&tagged, t0 + 119, 0, &tag, &varied) && tag == 0x0C && varied);
        CHECK(!phase_stats_minute_tag(&tagged, t0 + 119 + 60, 0, &tag, &varied)); // nothing yet
        phase_stats_add_tagged(&tagged, t0 + 1440 * 60 + 5, 5, 0x01); // a day on: reused
        CHECK(phase_stats_minute_tag(&tagged, t0 + 1440 * 60 + 5, 0, &tag, &varied) && tag == 0x01 &&
              !varied);
        phase_stats_add(&tagged, t0 + 1441 * 60, 5); // untagged: 0
        CHECK(phase_stats_minute_tag(&tagged, t0 + 1441 * 60, 0, &tag, &varied) && tag == 0 && !varied);
    }

    phase_stats_reset(&stats);
    CHECK(stats.total.count == 0 && !stats.started);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
