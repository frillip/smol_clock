/*
 * The ESP crystal's temperature compensation (temp_comp) and its place in
 * the clock (esp_clock): the phase it adds, held when the temperature goes
 * stale or the settings change, the clock's UTC with it and the model's
 * without, the PPS's period, a day of a crystal that follows the room,
 * polled every 512 s, with the compensation and without, and learning the
 * coefficient from the polls.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/timing/esp_clock.h"
#include "../../src/timing/temp_comp.h"

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

#define S 1000000LL

static void test_phase(void)
{
    printf("The phase: a rate from the temperature against the first one\n");
    temp_comp_init();
    CHECK(temp_comp_status()->state == TEMP_COMP_OFF && temp_comp_us(10 * S) == 0);
    temp_comp_configure(1 * S, true, -165);
    CHECK(temp_comp_status()->state == TEMP_COMP_NO_TEMPERATURE);
    temp_comp_temperature(2 * S, 3000); // the reference
    CHECK(temp_comp_status()->state == TEMP_COMP_ACTIVE && temp_comp_rate_ppb() == 0);
    temp_comp_temperature(3 * S, 3200); // 2 C warmer: the crystal 330 ppb slow
    CHECK(temp_comp_rate_ppb() == 330);
    for (int64_t t = 8 * S; t <= 1003 * S; t += 5 * S)
    {
        temp_comp_temperature(t, 3200);
        temp_comp_service(t);
    }
    // 1000 s at 330 ppb: 330 us, and on by the rate between readings.
    CHECK(temp_comp_us(1003 * S) == 330);
    CHECK(temp_comp_us(1503 * S) == 495);
    CHECK(temp_comp_us(1002 * S) == 329); // a sample a second back

    printf("No reading for 15 s: held, the rate on; for 5 min: lost, the phase kept\n");
    temp_comp_service(1017 * S);
    CHECK(temp_comp_status()->state == TEMP_COMP_ACTIVE && !temp_comp_status()->temperature_held);
    temp_comp_service(1019 * S);
    CHECK(temp_comp_status()->state == TEMP_COMP_ACTIVE && temp_comp_status()->temperature_held &&
          temp_comp_rate_ppb() == 330 && temp_comp_us(1019 * S) == 335);
    temp_comp_service(1003 * S + TEMP_COMP_HOLD_US);
    CHECK(temp_comp_status()->state == TEMP_COMP_ACTIVE);
    temp_comp_service(1004 * S + TEMP_COMP_HOLD_US);
    CHECK(temp_comp_status()->state == TEMP_COMP_NO_TEMPERATURE && temp_comp_rate_ppb() == 0 &&
          !temp_comp_status()->temperature_held);
    int64_t held = temp_comp_us(1004 * S + TEMP_COMP_HOLD_US);
    CHECK(held == 330 + 99 && temp_comp_us(5000 * S) == held); // 301 s more at 330 ppb
    temp_comp_temperature(5000 * S, 2900); // back, colder than the reference
    CHECK(temp_comp_rate_ppb() == -165 && temp_comp_us(5000 * S) == held &&
          !temp_comp_status()->temperature_held);

    printf("Settings changed: the phase continuous, the rate from then on\n");
    temp_comp_configure(5000 * S, true, -100);
    CHECK(temp_comp_rate_ppb() == -100 && temp_comp_us(5000 * S) == held);
    temp_comp_configure(6000 * S, false, -100);
    CHECK(temp_comp_status()->state == TEMP_COMP_OFF && temp_comp_rate_ppb() == 0);
    CHECK(temp_comp_us(6000 * S) == held - 100 && temp_comp_us(9000 * S) == held - 100);
    temp_comp_configure(9000 * S, true, 50); // + : fast when warmer
    temp_comp_temperature(9000 * S, 3100);
    CHECK(temp_comp_rate_ppb() == -50);
}


static void test_clock(void)
{
    EspClockSample sample = {};

    printf("The clock: UTC with the compensation, the model without\n");
    temp_comp_init();
    esp_clock_init();
    sample.local_us = 100 * S;
    sample.offset_us = 1800000000LL * S;
    sample.delay_us = 2000;
    esp_clock_update(&sample, sample.local_us, 1024 * S, NULL);
    temp_comp_configure(100 * S, true, -165);
    temp_comp_temperature(100 * S, 3000);
    temp_comp_temperature(101 * S, 3500); // +825 ppb from here
    int64_t local = 10101 * S;
    CHECK(esp_clock_utc_us(local) - esp_clock_model_utc_us(local) == temp_comp_us(local));
    CHECK(temp_comp_us(local) == 8250);
    // The inverse, through the compensation.
    int64_t utc = esp_clock_utc_us(local);
    CHECK(llabs(esp_clock_local_us(utc) - local) <= 1);
    // A UTC second is 825 ppb (0.825 us) fewer local microseconds, Q16.
    CHECK(llabs(esp_clock_period_adjust_q16() + 825 * 65536 / 1000) <= 1);
}


typedef struct
{
    double rms_us;
    double worst_us;
} Result;

// A day: the crystal at -2.9 ppm (fast +) at 30 C, -165 ppb/C, the board
// cycling between 28 and 36 C over swing_s; polled every 512 s with 50 us
// of noise, the compensation's coefficient as given (0: off).
static Result run_day(int32_t coefficient, double tempco, double swing_s, unsigned seed)
{
    // UTC - local, us: a base and what the crystal adds, apart (a double
    // at 1.8e15 holds only quarters of a microsecond).
    const int64_t base = 1800000000LL * S;
    double truth = 0.0;
    double squares = 0.0;
    long seconds = 0;
    Result result = {0.0, 0.0};

    srand(seed);
    temp_comp_init();
    esp_clock_init();
    temp_comp_configure(1, coefficient != 0, coefficient);
    for (int64_t t = 1; t <= 86400; t++)
    {
        double temperature = 32.0 + 4.0 * sin(2.0 * M_PI * t / swing_s);
        double fast_ppm = -2.9 + tempco * (temperature - 30.0) / 1000.0;
        int64_t local = t * S;

        truth -= fast_ppm; // a fast crystal: UTC - local falls
        if (t % 5 == 0)
        {
            temp_comp_temperature(local, (int32_t)lround(temperature * 100.0));
        }
        temp_comp_service(local);
        if (t % 512 == 10)
        {
            EspClockSample sample = {};

            sample.local_us = local;
            sample.offset_us = base + (int64_t)llround(truth + 50.0 * (2.0 * rand() / RAND_MAX - 1.0));
            sample.delay_us = 2000;
            sample.offset_us -= temp_comp_us(local); // as ntp.cpp
            esp_clock_update(&sample, local, 1024 * S, NULL);
        }
        if (t > 3 * 3600 && esp_clock_is_valid())
        {
            double error = (double)(esp_clock_utc_us(local) - local - base) - truth;

            squares += error * error;
            seconds++;
            if (fabs(error) > result.worst_us)
            {
                result.worst_us = fabs(error);
            }
        }
    }
    result.rms_us = sqrt(squares / seconds);
    return result;
}


static void test_day(void)
{
    static const struct
    {
        const char *name;
        double swing_s;
    } swings[] = {{"8 C over 2 h", 7200}, {"8 C over 6 h", 21600}};

    printf("A day of a crystal following the room, -165 ppb/C, polled every 512 s, the "
           "default's -180\n");
    for (const auto &swing : swings)
    {
        Result off = run_day(0, -165, swing.swing_s, 3);
        Result on = run_day(TEMP_COMP_DEFAULT_PPB_PER_C, -165, swing.swing_s, 3);
        Result wrong = run_day(TEMP_COMP_DEFAULT_PPB_PER_C, -120, swing.swing_s, 3);

        printf("  %-14s off RMS %6.1f us worst %6.1f; on %6.1f / %6.1f; crystal -120 ppb/C, "
               "on: %6.1f / %6.1f\n",
               swing.name, off.rms_us, off.worst_us, on.rms_us, on.worst_us, wrong.rms_us,
               wrong.worst_us);
        CHECK(on.rms_us < off.rms_us * 0.5);
        CHECK(wrong.rms_us < off.rms_us * 0.7); // a coefficient 30 % out still helps
    }
}


// The crystal at -2.9 ppm and tempco ppb/C about 30 C; the board between 28
// and 36 C over swing_s (amplitude 0: flat at 32 C); polled every poll_s with
// noise_us, the raw offsets learnt from, for hours. The estimates made.
static int run_learning(double tempco, double swing_s, double amplitude, int poll_s,
                        double noise_us, int hours, unsigned seed)
{
    double truth = 0.0;

    srand(seed);
    temp_comp_init();
    temp_comp_configure(1, true, TEMP_COMP_NOT_SET);
    for (int64_t t = 1; t <= (int64_t)hours * 3600; t++)
    {
        double temperature = 32.0 + amplitude * sin(2.0 * M_PI * t / swing_s);
        int64_t local = t * S;

        truth -= -2.9 + tempco * (temperature - 30.0) / 1000.0;
        if (t % 5 == 0)
        {
            temp_comp_temperature(local, (int32_t)lround(temperature * 100.0));
        }
        temp_comp_service(local);
        if (t % poll_s == 7)
        {
            double noise = noise_us * (2.0 * rand() / RAND_MAX - 1.0);
            // A few slow round trips, their offsets off.
            int slow = rand() % 10 == 0;
            int64_t delay = slow ? 9000 : 2000 + rand() % 300;

            temp_comp_learn(local, (int64_t)llround(truth + noise + (slow ? 3000 : 0)), delay, 1);
        }
    }
    return temp_comp_status()->estimates;
}


static void test_learning(void)
{
    const TempCompStatus *comp = temp_comp_status();
    int32_t ppb, estimates;

    printf("Learning: a -230 ppb/C crystal, 8 C over 6 h, 128 s polls, 100 us of noise\n");
    int made = run_learning(-230, 21600, 4.0, 128, 100.0, 24, 5);
    printf("  %d estimates, learnt %+.1f, last %+.1f +/- %.1f over %.2f C\n", made,
           comp->learnt_ppb_per_c, comp->last_ppb_per_c, comp->last_se_ppb,
           comp->last_range_centic / 100.0);
    CHECK(made >= 3 && fabs(comp->learnt_ppb_per_c + 230) < 10);
    CHECK(comp->source == TEMP_COMP_LEARNT && comp->ppb_per_c == lround(comp->learnt_ppb_per_c));
    CHECK(temp_comp_take_learnt(&ppb, &estimates) && estimates == made && ppb == comp->ppb_per_c);
    CHECK(!temp_comp_take_learnt(&ppb, &estimates)); // once
    CHECK(temp_comp_short_polls(24 * 3600 * S) != TEMP_COMP_POLLS_START); // learnt

    printf("  -190 ppb/C, 2 C over 6 h (the least counted), 128 s polls, 50 us of noise\n");
    made = run_learning(-190, 21600, 1.0, 128, 50.0, 24, 9);
    printf("  %d estimates, learnt %+.1f, last +/- %.1f over %.2f C\n", made, comp->learnt_ppb_per_c,
           comp->last_se_ppb, comp->last_range_centic / 100.0);
    CHECK(made >= 1 && fabs(comp->learnt_ppb_per_c + 190) < 15);

    printf("  +60 ppb/C, 8 C over 2 h, 512 s polls: towards it, at most 30 ppb/C an estimate\n");
    made = run_learning(60, 7200, 4.0, 512, 50.0, 24, 6);
    printf("  %d estimates, learnt %+.1f, last %+.1f\n", made, comp->learnt_ppb_per_c,
           comp->last_ppb_per_c);
    CHECK(made >= 3 && fabs(comp->last_ppb_per_c - 60) < 10);
    CHECK(comp->learnt_ppb_per_c > TEMP_COMP_DEFAULT_PPB_PER_C + 30.0 &&
          comp->learnt_ppb_per_c <= TEMP_COMP_DEFAULT_PPB_PER_C + 30.0 * made + 0.01);

    printf("  A flat room: nothing to learn from, short polls for the first 6 h; 8 h held\n");
    made = run_learning(-230, 21600, 0.0, 128, 100.0, 12, 7);
    CHECK(made == 0 && comp->source == TEMP_COMP_DEFAULT && comp->ppb_per_c == TEMP_COMP_DEFAULT_PPB_PER_C);
    CHECK(comp->window >= 220 && comp->window <= 226 && comp->window_range_centic < 200);
    CHECK(!comp->moving && comp->minutes == TEMP_COMP_HISTORY_MINUTES);
    CHECK(temp_comp_short_polls(5 * 3600 * S) == TEMP_COMP_POLLS_START &&
          temp_comp_short_polls(6 * 3600 * S + 2) == TEMP_COMP_POLLS_NORMAL);
    temp_comp_configure(12 * 3600 * S, true, -150); // set: no short polls to start
    CHECK(temp_comp_short_polls(5 * 3600 * S) == TEMP_COMP_POLLS_NORMAL &&
          comp->source == TEMP_COMP_SET);

    printf("  A PIC restart's 45 s without a reading: the window kept\n");
    {
        int before = comp->window;

        temp_comp_service(12 * 3600 * S + 45 * S);
        CHECK(comp->temperature_held && comp->state == TEMP_COMP_ACTIVE && comp->window == before);
        temp_comp_learn(12 * 3600 * S + 50 * S, 0, 2000, 1);
        CHECK(comp->window == before || comp->window == before + 1);
        temp_comp_temperature(12 * 3600 * S + 55 * S, comp->temperature_centic);
        CHECK(!comp->temperature_held && comp->window >= before);
    }

    printf("  Another server, or the temperature lost for 5 min: the window again\n");
    temp_comp_learn(12 * 3600 * S + 100 * S, 0, 2000, 2);
    CHECK(comp->window == 1);
    temp_comp_service(12 * 3600 * S + 100 * S + TEMP_COMP_HOLD_US);
    CHECK(comp->window == 0 && comp->state == TEMP_COMP_NO_TEMPERATURE);

    printf("  Restored, set over it, forgotten\n");
    temp_comp_init();
    temp_comp_restore(-201, 5);
    temp_comp_configure(1, true, TEMP_COMP_NOT_SET);
    CHECK(comp->source == TEMP_COMP_LEARNT && comp->ppb_per_c == -201 &&
          temp_comp_short_polls(2) == TEMP_COMP_POLLS_NORMAL);
    temp_comp_configure(2, true, -100);
    CHECK(comp->source == TEMP_COMP_SET && comp->ppb_per_c == -100);
    temp_comp_configure(3, true, TEMP_COMP_NOT_SET);
    CHECK(comp->source == TEMP_COMP_LEARNT && comp->ppb_per_c == -201);
    temp_comp_forget(4);
    CHECK(comp->source == TEMP_COMP_DEFAULT && comp->ppb_per_c == TEMP_COMP_DEFAULT_PPB_PER_C && comp->estimates == 0 &&
          !temp_comp_take_learnt(&ppb, &estimates));
    CHECK(temp_comp_short_polls(5) == TEMP_COMP_POLLS_NORMAL); // no temperature
    temp_comp_temperature(6, 3000);
    CHECK(temp_comp_short_polls(7) == TEMP_COMP_POLLS_START);
}


// The board at centic from from_s to to_s, a reading every 5 s.
static void hold_at(int64_t from_s, int64_t to_s, int32_t from_centic, int32_t to_centic)
{
    for (int64_t t = from_s; t < to_s; t += 5)
    {
        temp_comp_temperature(t * S, from_centic + (int32_t)((to_centic - from_centic) *
                                                             (t - from_s) / (to_s - from_s)));
        temp_comp_service(t * S);
    }
}


static void test_moving(void)
{
    const TempCompStatus *comp = temp_comp_status();

    printf("Short polls while the temperature moves, learnt or not\n");
    temp_comp_init();
    temp_comp_restore(-190, 3); // learnt: no start's short polls
    temp_comp_configure(1, true, TEMP_COMP_NOT_SET);
    hold_at(1, 3600, 3000, 3000);
    CHECK(temp_comp_short_polls(3600 * S) == TEMP_COMP_POLLS_NORMAL && comp->moved_centic == 0);
    hold_at(3600, 4800, 3000, 3040); // 0.4 C in 20 min: not yet
    CHECK(!comp->moving);
    hold_at(4800, 5400, 3040, 3060); // 0.6 C within 30 min
    CHECK(comp->moving && temp_comp_short_polls(5400 * S) == TEMP_COMP_POLLS_MOVING);
    CHECK(temp_comp_polls_s(TEMP_COMP_POLLS_MOVING) == 256);
    // Under 0.3 C in 30 min from about 6300 s (the 0.3 C left the 30 min),
    // then 30 min of that.
    hold_at(5400, 7800, 3060, 3060);
    CHECK(comp->moving && comp->steady_minutes > 0);
    hold_at(7800, 9100, 3060, 3060);
    CHECK(!comp->moving && temp_comp_short_polls(9100 * S) == TEMP_COMP_POLLS_NORMAL);
    hold_at(9100, 9100 + 3700, 3060, 3075); // 0.15 C in an hour: steady
    CHECK(!comp->moving);
    // A heating cycle's 0.25 C every 30 min, after moving: the range stays
    // under 0.3 C, so it settles.
    hold_at(12800, 13400, 3075, 3135);
    CHECK(comp->moving);
    for (int64_t t = 13400; t < 13400 + 5400; t += 1800)
    {
        hold_at(t, t + 900, 3135, 3110);
        hold_at(t + 900, t + 1800, 3110, 3135);
    }
    CHECK(!comp->moving);

    printf("  Not learnt yet: 128 s while it moves\n");
    temp_comp_restore(-170, 2);
    hold_at(18800, 19400, 3135, 3200);
    CHECK(temp_comp_short_polls(19400 * S) == TEMP_COMP_POLLS_LEARNING &&
          temp_comp_polls_s(TEMP_COMP_POLLS_LEARNING) == 128);
    temp_comp_configure(19400 * S, true, -150); // set by hand: settled
    CHECK(temp_comp_short_polls(19400 * S) == TEMP_COMP_POLLS_MOVING);
    temp_comp_configure(19400 * S, true, TEMP_COMP_NOT_SET);
    hold_at(19400, 19400 + 7200, 3200, 3200);
    CHECK(temp_comp_short_polls((19400 + 7200) * S) == TEMP_COMP_POLLS_NORMAL);

    printf("  Turned off, or the temperature lost: none; held a while: still moving\n");
    int64_t at = 26600;
    hold_at(at, at + 600, 3200, 3275);
    CHECK(comp->moving);
    temp_comp_configure((at + 600) * S, false, TEMP_COMP_NOT_SET);
    CHECK(temp_comp_short_polls((at + 600) * S) == TEMP_COMP_POLLS_NORMAL);
    temp_comp_configure((at + 600) * S, true, TEMP_COMP_NOT_SET);
    temp_comp_service((at + 640) * S); // a PIC restart's gap: held
    CHECK(comp->moving && comp->temperature_held &&
          temp_comp_short_polls((at + 640) * S) == TEMP_COMP_POLLS_LEARNING);
    temp_comp_service((at + 600) * S + TEMP_COMP_HOLD_US + 1);
    CHECK(!comp->moving && comp->minutes == 0 &&
          temp_comp_short_polls((at + 600) * S + TEMP_COMP_HOLD_US + 1) == TEMP_COMP_POLLS_NORMAL);
}


// The field's troubles (2026-11-21): a crystal at tempco ppb/C whose
// frequency also drifts (drift ppb/h) and wanders (a random walk of walk
// ppb each poll, so neighbouring polls err together), through a board
// temperature profile(t, h): polled every 128 s with 30 us of noise for
// hours. The estimates made, the largest step of the learnt value, the
// shortest time between estimates (h), and the worst learnt value's error.
typedef double (*Profile)(double hours);

typedef struct
{
    int estimates;
    double largest_step;
    double closest_h;
    double worst;
} Learnt;

static Learnt run_profile(double tempco, Profile profile, double drift, double walk, int hours,
                          unsigned seed, double settle_h = 0.0)
{
    const TempCompStatus *comp = temp_comp_status();
    double truth = 0.0, wander = 0.0, learnt = TEMP_COMP_DEFAULT_PPB_PER_C;
    int64_t accepted = 0;
    Learnt result = {0, 0.0, 99.0, 0.0};

    srand(seed);
    temp_comp_init();
    temp_comp_configure(1, true, TEMP_COMP_NOT_SET);
    for (int64_t t = 1; t <= (int64_t)hours * 3600; t++)
    {
        double h = t / 3600.0;
        double temperature = profile(h);
        int64_t local = t * S;

        double drifted = settle_h > 0.0 ? drift * (1.0 - exp(-h / settle_h)) : drift * h;

        truth -= -2.9 + (tempco * (temperature - 30.0) + drifted + wander) / 1000.0;
        if (t % 5 == 0)
        {
            temp_comp_temperature(local, (int32_t)lround(temperature * 100.0));
        }
        temp_comp_service(local);
        if (t % 128 == 7)
        {
            wander += walk * (2.0 * rand() / RAND_MAX - 1.0);
            temp_comp_learn(local, (int64_t)llround(truth + 30.0 * (2.0 * rand() / RAND_MAX - 1.0)), 2000,
                            1);
            if (comp->estimates > result.estimates)
            {
                result.estimates = comp->estimates;
                result.largest_step = fmax(result.largest_step, fabs(comp->learnt_ppb_per_c - learnt));
                result.worst = fmax(result.worst, fabs(comp->learnt_ppb_per_c - tempco));
                if (accepted != 0)
                {
                    result.closest_h = fmin(result.closest_h, (local - accepted) / 3.6e9);
                }
                accepted = local;
                learnt = comp->learnt_ppb_per_c;
            }
        }
    }
    return result;
}


// The morning of 2026-11-21: 33 C, warming to 37 C over 5 h, then steady.
static double morning(double h)
{
    return h < 1.0 ? 33.0 : h < 6.0 ? 33.0 + 4.0 * (h - 1.0) / 5.0 : 37.0;
}


// The day after: heating cycles of +/-1 C every 2 h on a +/-2 C day.
static double heating(double h)
{
    return 33.0 + 2.0 * sin(2.0 * M_PI * h / 24.0) + 1.0 * sin(2.0 * M_PI * h / 2.0);
}


static void test_field(void)
{
    printf("The field's troubles\n");
    Learnt ramp = run_profile(-170, morning, 0.0, 0.0, 12, 11);
    Learnt drift = run_profile(-170, morning, 60.0, 0.0, 12, 11);
    printf("  a one-way ramp, a steady crystal: %d estimates, worst %.1f off\n", ramp.estimates,
           ramp.worst);
    printf("  a one-way ramp, the crystal drifting 60 ppb/h: %d estimates, worst %.1f off\n",
           drift.estimates, drift.worst);
    Learnt settling = run_profile(-170, morning, 150.0, 0.0, 12, 11, 2.0);
    Learnt wandering = run_profile(-170, morning, 0.0, 3.0, 12, 13);
    printf("  a one-way ramp, the crystal settling 150 ppb over 2 h: %d estimates, worst %.1f off\n",
           settling.estimates, settling.worst);
    printf("  a one-way ramp, the crystal wandering: %d estimates, worst %.1f off\n",
           wandering.estimates, wandering.worst);
    // The drift term and the honest error keep drift from being taken for
    // temperature: no estimate, or a good one, and never a step past 30.
    CHECK(drift.estimates == 0 || drift.worst < 40.0);
    CHECK(ramp.estimates == 0 || ramp.worst < 40.0);
    CHECK(settling.estimates == 0 || settling.worst < 40.0);
    CHECK(wandering.estimates == 0 || wandering.worst < 40.0);

    Learnt settled = run_profile(-150, heating, 150.0, 3.0, 48, 14, 2.0);
    printf("  two days of heating cycles, the crystal settling 150 ppb over 2 h and wandering: %d "
           "estimates, learnt %+.1f (worst %.1f off)\n", settled.estimates,
           temp_comp_status()->learnt_ppb_per_c, settled.worst);
    CHECK(settled.estimates >= 4 && fabs(temp_comp_status()->learnt_ppb_per_c + 150) < 15);

    Learnt days = run_profile(-150, heating, 5.0, 3.0, 48, 12);
    printf("  two days of heating cycles, the crystal wandering: %d estimates, largest step %.1f, "
           "closest %.1f h apart, learnt %+.1f (worst %.1f off)\n", days.estimates,
           days.largest_step, days.closest_h, temp_comp_status()->learnt_ppb_per_c, days.worst);
    CHECK(days.estimates >= 4 && days.largest_step <= 30.0 + 1e-9 && days.closest_h >= 4.0);
    CHECK(fabs(temp_comp_status()->learnt_ppb_per_c + 150) < 15);
    CHECK(temp_comp_status()->last_rho > 0.0);
}


int main(void)
{
    test_phase();
    test_clock();
    test_day();
    test_learning();
    test_moving();
    test_field();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
