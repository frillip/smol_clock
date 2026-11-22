// ntp_stats: the line found, slow exchanges counted for little, a change of
// frequency pruned by the runs test, the tables, the distance.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/timing/ntp_stats.h"

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

static const int64_t BASE = 1800000000000000LL; // UTC - local, us

// Normal noise, Box-Muller.
static double gaussian(void)
{
    double u = (rand() + 1.0) / (RAND_MAX + 2.0);
    double v = (rand() + 1.0) / (RAND_MAX + 2.0);

    return sqrt(-2.0 * log(u)) * cos(2.0 * M_PI * v);
}


static NtpStatsSample at(int64_t local_s, double offset_us, uint32_t delay_us)
{
    NtpStatsSample sample;

    sample.local_us = local_s * 1000000LL;
    sample.offset_us = BASE + (int64_t)llround(offset_us);
    sample.delay_us = delay_us;
    sample.server_us = 20;
    return sample;
}


int main(void)
{
    NtpStats stats;

    printf("NTP sample statistics\n");
    CHECK(ntp_stats_critical_runs(10) == 3 && ntp_stats_critical_runs(64) == 26);
    CHECK(ntp_stats_critical_runs(500) == 56);
    CHECK(fabs(ntp_stats_t_coefficient(1) - 636.6) < 0.01);
    CHECK(fabs(ntp_stats_t_coefficient(40) - 3.551) < 0.001);
    CHECK(ntp_stats_t_coefficient(100) == 3.5);

    // One sample, two: no line, the latest offset, worst-case skew.
    ntp_stats_reset(&stats);
    {
        NtpStatsSample s = at(1000, 100.0, 500);

        ntp_stats_add(&stats, &s);
    }
    CHECK(!stats.fitted && stats.samples == 1);
    CHECK(ntp_stats_offset_at(&stats, 2000000000LL) == BASE + 100);
    CHECK(stats.skew_ppb == NTP_STATS_WORST_SKEW_PPB);
    CHECK(ntp_stats_distance_us(&stats, 1000000000LL) == 20 + 250);
    CHECK(ntp_stats_distance_us(&stats, 1001000000LL) == 20 + 250 + 2000);
    // Capped (selection's rank for a young line): 1 s at 5 ppm, not 2000.
    CHECK(ntp_stats_distance_capped_us(&stats, 1001000000LL, 5000.0) == 20 + 250 + 5);
    CHECK(ntp_stats_distance_capped_us(&stats, 1001000000LL, 1e18) == 20 + 250 + 2000);
    {
        NtpStatsSample s = at(1128, 110.0, 500);

        ntp_stats_add(&stats, &s);
    }
    CHECK(!stats.fitted && stats.samples == 2);

    // A clean line: +3.25 ppm, exact.
    ntp_stats_reset(&stats);
    for (int i = 0; i < 20; i++)
    {
        NtpStatsSample s = at(1000 + 128 * i, 3.25 * 128 * i, 500);

        ntp_stats_add(&stats, &s);
    }
    CHECK(stats.fitted && stats.samples == NTP_STATS_MAX_SAMPLES && stats.used == NTP_STATS_MAX_SAMPLES);
    // Those pushed out of the line kept for the runs test.
    CHECK(stats.runs_samples == 20 - NTP_STATS_MAX_SAMPLES);
    CHECK(ntp_stats_span_us(&stats) == 128LL * (NTP_STATS_MAX_SAMPLES - 1) * 1000000);
    CHECK(fabs(stats.frequency_ppb - 3250.0) < 5.0);
    CHECK(llabs(ntp_stats_offset_at(&stats, (1000 + 128 * 19 + 512) * 1000000LL) -
                (BASE + (int64_t)llround(3.25 * (128 * 19 + 512)))) <= 2);

    // Noise of 50 us: the slope to within its skew, the spread about 50.
    srand(1);
    ntp_stats_reset(&stats);
    for (int i = 0; i < 64; i++)
    {
        NtpStatsSample s = at(128 * i, -1.5 * 128 * i + 50.0 * gaussian(), 500);

        ntp_stats_add(&stats, &s);
    }
    printf("  noise: %.0f ppb +/- %.0f (skew %.0f), sd %.1f us, %u used, %u runs\n",
           stats.frequency_ppb, stats.frequency_sd_ppb, stats.skew_ppb, stats.sd_us, stats.used,
           stats.runs);
    CHECK(stats.fitted && fabs(stats.frequency_ppb + 1500.0) < stats.skew_ppb);
    CHECK(stats.sd_us > 30.0 && stats.sd_us < 70.0);
    CHECK(stats.used >= NTP_STATS_MAX_SAMPLES - 4);

    // Every fifth exchange slow (round trip 5 ms, lopsided by 2 ms): with
    // chronyd's weights they barely move the line.
    srand(2);
    ntp_stats_reset(&stats);
    for (int i = 0; i < 40; i++)
    {
        bool slow = i % 5 == 2;
        NtpStatsSample s =
            at(128 * i, 2.0 * 128 * i + 10.0 * gaussian() + (slow ? 2000.0 : 0.0), slow ? 5000 : 500);

        ntp_stats_add(&stats, &s);
    }
    printf("  slow exchanges: offset %+lld us, %.0f ppb\n",
           (long long)(ntp_stats_offset_at(&stats, 128 * 39 * 1000000LL) - BASE - 2 * 128 * 39),
           stats.frequency_ppb);
    CHECK(llabs(ntp_stats_offset_at(&stats, 128 * 39 * 1000000LL) - BASE - 2 * 128 * 39) < 40);
    CHECK(fabs(stats.frequency_ppb - 2000.0) < 100.0);
    // The best sample for distance is a fast one.
    CHECK(ntp_stats_distance_us(&stats, 128 * 39 * 1000000LL) < 20 + 250 + 100);

    // The frequency changes by 2 ppm 12 samples back, at 512 s polls: the
    // runs test drops the samples before it, so the line follows the new
    // frequency.
    srand(3);
    ntp_stats_reset(&stats);
    {
        double offset = 0.0;

        for (int i = 0; i < 64; i++)
        {
            NtpStatsSample s;

            offset += (i < 52 ? 1.0 : 3.0) * 512.0;
            s = at(512 * i, offset + 5.0 * gaussian(), 500);
            ntp_stats_add(&stats, &s);
        }
    }
    printf("  frequency step: %.0f ppb from %u samples (%u runs), %u kept for runs\n",
           stats.frequency_ppb, stats.used, stats.runs, stats.runs_samples);
    CHECK(fabs(stats.frequency_ppb - 3000.0) < 150.0);
    CHECK(stats.used <= 13 && stats.samples == stats.used);
    CHECK(stats.runs_samples <= stats.samples);

    // At 128 s polls the path shifts by 200 us for three polls, then
    // returns. Asked for the least span (the crystal compensated), the line
    // is not cut to the shift and what follows it, so its slope stays near
    // the crystal's; without, as chronyd, it is, and tilts.
    printf("  a path shifted for three polls at 128 s:");
    for (int spanned = 1; spanned >= 0; spanned--)
    {
        double worst = 0.0;
        uint8_t least = NTP_STATS_MAX_SAMPLES;

        srand(5);
        ntp_stats_reset(&stats);
        stats.min_span_s = spanned ? NTP_STATS_MIN_SPAN_S : 0.0f;
        for (int i = 0; i < 40; i++)
        {
            double offset = 1.0 * 128.0 * i + (i >= 24 && i < 27 ? 200.0 : 0.0);
            NtpStatsSample s = at(128 * i, offset + 20.0 * gaussian(), 500);

            ntp_stats_add(&stats, &s);
            if (i >= 24)
            {
                double error = fabs(stats.frequency_ppb - 1000.0);

                worst = error > worst ? error : worst;
                least = stats.used < least ? stats.used : least;
            }
        }
        printf(" %s, slope off by %.0f ppb at most, %u samples at least;",
               spanned ? "spanned" : "not", worst, least);
        if (spanned)
        {
            CHECK(worst < 200.0 && least * 128.0 > NTP_STATS_MIN_SPAN_S);
        }
        else
        {
            CHECK(worst > 300.0 && least == NTP_STATS_MIN_SAMPLES);
        }
    }
    printf("\n");

    // Never fewer than NTP_STATS_MIN_SAMPLES after pruning, and the ring
    // wraps cleanly over many samples.
    srand(4);
    ntp_stats_reset(&stats);
    for (int i = 0; i < 1000; i++)
    {
        // A crystal wandering 3 ppm over 3 hours.
        double t = 128.0 * i;
        double offset = 3.0e6 / (2.0 * M_PI / 10800.0) * (1.0 - cos(2.0 * M_PI * t / 10800.0)) * 1e-6;
        NtpStatsSample s = at(128 * i, offset + 20.0 * gaussian(), 500 + (uint32_t)(rand() % 300));

        ntp_stats_add(&stats, &s);
        if ((stats.added >= NTP_STATS_MIN_SAMPLES && stats.used < NTP_STATS_MIN_SAMPLES) ||
            stats.samples + stats.runs_samples > NTP_STATS_RING)
        {
            printf("  at %d: used %u samples %u runs %u\n", i, stats.used, stats.samples, stats.runs_samples);
            CHECK(false);
            break;
        }
    }
    CHECK(stats.added == 1000 && stats.fitted);
    printf("  wander: %u used, %u for runs\n", stats.used, stats.runs_samples);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
