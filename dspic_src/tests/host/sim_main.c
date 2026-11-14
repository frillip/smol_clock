#include "sim_model.h"

#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <stdlib.h>

static double gauss(void)
{
    double u1 = (rand() + 1.0) / ((double)RAND_MAX + 2.0),
           u2 = (rand() + 1.0) / ((double)RAND_MAX + 2.0);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}


// Runs seconds epochs; holdover from holdover_start for holdover_length.
// Prints the output edge error against true UTC (ns).
static void run(long seconds, double jitter_ns, double temp_ppb, long holdover_start,
                long holdover_length, unsigned seed)
{
    const double f0 = 39999919.66;
    double y_rw = 0.0, tau, sum = 0.0, sum2 = 0.0, worst = 0.0, err_ns = 0.0;
    long count = 0;
    int64_t epoch;
    uint32_t period_now = 40000000UL, pending = 0;
    int64_t last_cap = 0;
    bool have_last = false;

    srand(seed);
    adapter_init();
    tau = 1000.0 * rand() / RAND_MAX;
    epoch = (int64_t)floor(tau);
    for (long k = 1; k <= seconds; k++)
    {
        double y;
        int64_t cap;
        bool in_holdover =
            holdover_start > 0 && k >= holdover_start && k < holdover_start + holdover_length;
        uint32_t request;

        y_rw += 1e-11 * gauss();
        y = y_rw + temp_ppb * 1e-9 * sin(2.0 * M_PI * (double)k / 3600.0);
        tau += f0 * (1.0 + y);
        epoch += period_now;
        err_ns = ((double)epoch - tau) * 25.0;
        if (k > 1000 && !in_holdover)
        {
            sum += err_ns;
            sum2 += err_ns * err_ns;
            count++;
            if (fabs(err_ns) > worst)
                worst = fabs(err_ns);
        }
        if (pending != 0U)
        {
            period_now = pending;
            pending = 0;
        }
        cap = (int64_t)floor(tau + jitter_ns * gauss() / 25.0);
        if (in_holdover)
        {
            request = adapter_epoch(0, 0, false);
        }
        else
        {
            uint32_t measured = have_last ? (uint32_t)(cap - last_cap) : 0U;
            request = adapter_epoch(measured, (int32_t)(epoch - cap), true);
            last_cap = cap;
            have_last = true;
        }
        if (request != 0U)
            pending = request;
        if (holdover_start > 0 && k == holdover_start + holdover_length - 1)
        {
            printf("  %-8s holdover %5lds, temperature off: drift %8.2f us\n", adapter_name(),
                   holdover_length, err_ns / 1000.0);
        }
    }
    if (holdover_start == 0)
    {
        double mean = sum / (double)count;
        printf("  %-8s jitter %4.0f ns: mean %6.2f  rms %5.2f  max %5.1f  (ns, %ld epochs)\n",
               adapter_name(), jitter_ns, mean, sqrt(sum2 / (double)count - mean * mean), worst,
               count);
    }
}


int main(void)
{
    run(100000, 10.0, 50.0, 0, 0, 11);
    run(100000, 20.0, 50.0, 0, 0, 12);
    run(7200, 10.0, 0.0, 3600, 3600, 13);
    return 0;
}
