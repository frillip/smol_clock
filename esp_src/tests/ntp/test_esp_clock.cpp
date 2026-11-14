/*
 * esp_clock against a simulated crystal and server, every second for a day:
 * its uncertainty must bound its true error, whatever each path's asymmetry
 * (within half the round trip, as NTP assumes), with the crystal wandering
 * and stepping in frequency, and Wi-Fi delaying some exchanges a lot, one
 * way. Each run twice: steered by the clock's own window (no estimate), and
 * by the server's line (ntp_stats.h) as ntp.cpp steers it, to compare the
 * true error.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/timing/esp_clock.h"
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

typedef struct
{
    const char *name;
    double ppm;          // the crystal's error, local slow
    double wander_ppm;   // a swing on top of it ...
    double wander_s;     // ... over this period
    double step_ppm;     // a step at 12 h (the room warms)
    double delay_us;     // round trip
    double jitter_us;    // added to it, at random
    double asymmetry;    // of the half round trip, the share the path is lopsided, +/-
    bool worst;          // the asymmetry always at its full extent, one way
    double burst_share;  // exchanges Wi-Fi holds up ...
    double burst_us;     // ... by up to this, all one way
    int interval_s;
} Scenario;

typedef struct
{
    double rms_us;
    double worst_us;
    double bound_us;
    long violations;
    double worst_ratio;
} Result;

static double uniform(void)
{
    return (double)rand() / RAND_MAX;
}


static Result run(const Scenario *s, bool lines, unsigned seed)
{
    // Truth: UTC - local, in us, as the crystal runs.
    double truth = 1.8e15;
    double squares = 0.0, bound_sum = 0.0;
    long seconds = 0;
    int64_t next_poll = 10;
    int polls = 0;
    int64_t local;
    NtpStats stats;
    Result result = {0.0, 0.0, 0.0, 0, 0.0};

    srand(seed);
    esp_clock_init();
    ntp_stats_reset(&stats);
    for (local = 1; local <= 86400; local++)
    {
        double ppm = s->ppm + s->wander_ppm * sin(2.0 * M_PI * local / s->wander_s) +
                     (local > 43200 ? s->step_ppm : 0.0);

        truth += ppm; // a second of local time: UTC runs ppm us further
        if (local == next_poll)
        {
            EspClockSample sample;
            double delay = s->delay_us + s->jitter_us * uniform();
            double lopsided = s->worst ? 1.0 : (2.0 * uniform() - 1.0) * s->asymmetry;
            double offset = truth + lopsided * delay / 2.0;

            if (uniform() < s->burst_share)
            {
                // Held up one way: the offset is off by half the extra.
                double extra = s->burst_us * uniform();

                delay += extra;
                offset += (uniform() < 0.5 ? -0.5 : 0.5) * extra;
            }
            sample.local_us = local * 1000000LL;
            sample.offset_us = (int64_t)llround(offset);
            sample.delay_us = (int64_t)delay;
            sample.root_delay_us = 15;
            sample.root_dispersion_us = 15;
            sample.stratum = 1;
            if (lines)
            {
                NtpStatsSample entry = {sample.local_us, sample.offset_us, (uint32_t)sample.delay_us,
                                        15 / 2 + 15};
                EspClockEstimate estimate;

                ntp_stats_add(&stats, &entry);
                estimate.offset_us = ntp_stats_offset_at(&stats, sample.local_us);
                estimate.frequency_known = stats.fitted;
                estimate.frequency_ppb = (int32_t)llround(stats.frequency_ppb);
                estimate.skew_ppb =
                    (int32_t)(stats.skew_ppb < 1e9 ? llround(stats.skew_ppb) : 1000000000LL);
                (void)esp_clock_update(&sample, sample.local_us, 2LL * s->interval_s * 1000000,
                                       esp_clock_is_valid() ? &estimate : NULL);
            }
            else
            {
                (void)esp_clock_update(&sample, sample.local_us, 2LL * s->interval_s * 1000000,
                                       NULL);
            }
            polls++;
            next_poll += polls < 4 ? 64 << (polls - 1) : s->interval_s;
        }
        // Judged from the second hour, once both have learnt the frequency.
        if (local > 3600)
        {
            int64_t now = local * 1000000LL;
            double error = (double)esp_clock_utc_us(now) - (now + truth);
            double bound = esp_clock_uncertainty_ns(now) / 1000.0;

            seconds++;
            squares += error * error;
            if (fabs(error) > result.worst_us)
            {
                result.worst_us = fabs(error);
            }
            if (fabs(error) > bound)
            {
                result.violations++;
            }
            if (fabs(error) / bound > result.worst_ratio)
            {
                result.worst_ratio = fabs(error) / bound;
            }
            bound_sum += bound;
        }
    }
    result.rms_us = sqrt(squares / seconds);
    result.bound_us = bound_sum / seconds;
    return result;
}


int main(void)
{
    static const Scenario scenarios[] = {
        {"LAN, steady, 512 s", -2.9, 0.0, 21600, 0.0, 2500, 500, 0.3, false, 0.0, 0, 512},
        {"LAN, wander 0.2 ppm / 6 h, 512 s", -2.9, 0.2, 21600, 0.0, 2500, 500, 0.3, false, 0.0, 0,
         512},
        {"LAN, wander, 0.4 ppm step, 512 s", 0.9, 0.2, 21600, 0.4, 2500, 500, 0.3, false, 0.0, 0,
         512},
        {"Internet, lopsided at random, 512 s", 1.0, 0.1, 21600, 0.0, 10000, 3000, 1.0, false, 0.0,
         0, 512},
        {"Internet, always lopsided, 512 s", 1.0, 0.1, 21600, 0.0, 10000, 3000, 1.0, true, 0.0, 0,
         512},
        {"LAN, wander, 1024 s", -2.9, 0.2, 21600, 0.0, 2500, 500, 0.3, false, 0.0, 0, 1024},
        // As seen: the crystal swinging 0.5 ppm over an hour, a tenth of the
        // exchanges held up by Wi-Fi by up to 10 ms.
        {"LAN, Wi-Fi bursts, swinging, 128 s", -2.9, 0.5, 3600, 0.0, 2500, 500, 0.1, false, 0.1,
         10000, 128},
        {"LAN, Wi-Fi bursts, swinging, 512 s", -2.9, 0.5, 3600, 0.0, 2500, 500, 0.1, false, 0.1,
         10000, 512},
        {"LAN, Wi-Fi bursts, slow wander, 128 s", -2.9, 0.2, 21600, 0.0, 2500, 500, 0.1, false, 0.1,
         10000, 128},
        {"LAN, Wi-Fi bursts, slow wander, 512 s", -2.9, 0.2, 21600, 0.0, 2500, 500, 0.1, false, 0.1,
         10000, 512},
    };
    unsigned seed = 7;

    printf("esp_clock, a day each: RMS and worst true error, mean bound; window -> lines\n");
    for (const Scenario &s : scenarios)
    {
        Result window = run(&s, false, seed);
        Result line = run(&s, true, seed);

        printf("  %-38s RMS %5.0f -> %5.0f us, worst %6.0f -> %6.0f us, bound %6.0f -> %6.0f us, "
               "error / bound <= %.2f\n",
               s.name, window.rms_us, line.rms_us, window.worst_us, line.worst_us, window.bound_us,
               line.bound_us, line.worst_ratio > window.worst_ratio ? line.worst_ratio
                                                                    : window.worst_ratio);
        CHECK(window.violations == 0);
        CHECK(line.violations == 0);
        // Where Wi-Fi holds exchanges up, the lines do much better. (With
        // the crystal wandering slowly through steady noise the window can
        // do better: a line lags the wander, ntp_stats.h.)
        if (s.burst_share > 0.0)
        {
            CHECK(line.rms_us < window.rms_us * 0.9 && line.worst_us < window.worst_us * 0.7);
        }
        seed++;
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
