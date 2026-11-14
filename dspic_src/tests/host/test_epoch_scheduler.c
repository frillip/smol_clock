/*
 * Host test of timing/epoch_timer.c and timing/epoch_output.c: the T3
 * restart of the T4 scheduler, slot timing, the millisecond tick, the
 * measurement trigger and the OC1 pulse re-arm. The modules are compiled as
 * written (interrupt attributes stripped); SFRs are plain variables driven by
 * a cycle-level model of T2/T3 (period match), T4 (1 ms slots) and OC1.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <xc.h>
#include "../../timing/epoch_output.h"
#include "../../timing/epoch_timer.h"

void _T3Interrupt(void);
void _T4Interrupt(void);

#define SLOT_MIDPOINT_TICKS     (EPOCH_SLOT_TICKS / 2UL)
#define EPOCH_NOMINAL_MINUS_ONE (TIMING_NOMINAL_TICKS - 1UL)
#define OC1CON1_ARMED           0x1C09U // the register image epoch_output.c arms with

static int failures, checks;
#define CHECK(cond)                                                                       \
    do                                                                                    \
    {                                                                                     \
        checks++;                                                                         \
        if (!(cond))                                                                      \
        {                                                                                 \
            failures++;                                                                   \
            if (failures < 20)                                                            \
                printf("  FAIL line %d: %s (epoch %ld)\n", __LINE__, #cond, epoch_index); \
        }                                                                                 \
    } while (0)

static long epoch_index;
static uint32_t expected_ms, expected_at_epoch;
static bool deferred_pending;

// T4 model: when running, its count is (now - start + preload) mod PR4+1.
static int64_t t4_start;
static uint16_t t4_preload;
static bool t4_running;

static uint16_t t4_count_at(int64_t now)
{
    return (uint16_t)((now - t4_start + t4_preload) % (int64_t)EPOCH_SLOT_TICKS);
}


// Time of the first T4 boundary strictly after 'after'.
static int64_t t4_next_boundary(int64_t after)
{
    int64_t since = after - t4_start + t4_preload;
    int64_t slots = since / (int64_t)EPOCH_SLOT_TICKS + 1;
    return t4_start - t4_preload + slots * (int64_t)EPOCH_SLOT_TICKS;
}


// Runs one epoch interval [epoch, next_epoch): T4 boundaries before the
// next epoch are serviced at once, except those that fall after the next
// epoch but inside its T3 latency, which stay pending in T4IF.
#define OC1CON1_TRIGMODE 0x0008U
#define OC1CON2_OCTRIG   0x0080U
#define OC1CON2_TRIGSTAT 0x0040U
#define OC1CON2_SYNCSEL  0x001FU
#define SYNCSEL_TIMER3   0x000DU

/*
 * OC1 response to the Timer3 trigger, from DS70358C. Deliberately strict:
 * only the configuration that has been seen to work on hardware fires.
 *   - single compare, OCM = 001, waiting for a trigger (OCTRIG, TRIGSTAT
 *     clear) from Timer3;
 *   - one-shot timer (TRIGMODE), so TRIGSTAT is clear again before the next
 *     epoch without software writing it (software cannot, 13.3.3.13 note 1);
 *   - 0 < OC1R < OC1RS: a compare beyond the timer's run never happens
 *     (Table 13-1). OC1RS = 0 is the fault reported on hardware.
 * Returns whether the pin rose.
 */
static bool oc1_hardware_trigger(void)
{
    bool waiting = ((OC1CON1 & 0x0007U) == 0x0001U) && ((OC1CON2 & OC1CON2_OCTRIG) != 0U) &&
                   ((OC1CON2 & OC1CON2_TRIGSTAT) == 0U) &&
                   ((OC1CON2 & OC1CON2_SYNCSEL) == SYNCSEL_TIMER3);
    bool one_shot = (OC1CON1 & OC1CON1_TRIGMODE) != 0U;

    if (!waiting)
    {
        return false;
    }
    if (!one_shot)
    {
        OC1CON2 |= OC1CON2_TRIGSTAT; // runs on, never re-armable
    }
    if (OC1R == 0U || OC1R >= OC1RS)
    {
        return false;
    }
    _RB10 = 1;
    IFS0bits.OC1IF = 1;
    return true; // TRIGSTAT cleared at OC1RS
}


static int pulse_end_events;
static int64_t last_measure_time, last_pulse_end_time;

// Interrupts held off around an epoch: 'before' cycles ahead of it (any T4
// boundary in that window waits in T4IF) and 'latency' cycles after it
// (when the T3 handler finally runs). Together always under 1 ms.
typedef struct
{
    int64_t before;
    int64_t latency;
} HoldOff;

static HoldOff choose_hold_off(void)
{
    HoldOff h;
    if (rand() % 20 == 0)
    {
        h.before = rand() % 39900;
        h.latency = 10 + rand() % (39980 - h.before);
    }
    else
    {
        h.before = 0;
        h.latency = 10 + rand() % 40;
    }
    return h;
}


int main(void)
{
    const long epochs = 200000;
    int64_t now_epoch = 40000000LL; // first epoch, 1 s after start
    int64_t clock_origin = now_epoch;
    uint32_t period = 40000000UL;
    uint32_t running_expected = 40000000UL; // loaded at the current epoch
    long late_isr = 0, steps = 0, deferred_before = 0;
    HoldOff hold = {0, 20};
    srand(21);

    // The configuration reported on hardware (OC1RS = 0) must not fire in
    // this model, or the model proves nothing.
    epoch_output_init();
    OC1RS = 0;
    CHECK(!oc1_hardware_trigger());
    CHECK(_RB10 == 0 && IFS0bits.OC1IF == 0);

    epoch_output_init();
    epoch_timer_init();
    CHECK(T2CONbits.T32 && T2CONbits.TON && !T4CONbits.TON);
    CHECK(PR3 == (uint16_t)((EPOCH_NOMINAL_MINUS_ONE) >> 16) &&
          PR2 == (uint16_t)EPOCH_NOMINAL_MINUS_ONE);
    CHECK(IPC2bits.T3IP == 4 && IPC6bits.T4IP == 4);

    for (epoch_index = 0; epoch_index < epochs; epoch_index++)
    {
        int64_t isr_time = now_epoch + hold.latency;
        int64_t next_epoch;
        HoldOff next_hold;
        int64_t b;

        if (hold.latency > 1000)
            late_isr++;

        // OC1: the Timer3 trigger at the epoch raises the pin.
        CHECK(oc1_hardware_trigger());

        // A boundary between the epoch and the handler is pending too.
        if (t4_running && t4_next_boundary(now_epoch - 1) < isr_time)
        {
            IFS1bits.T4IF = 1;
        }

        // Expected tick from the true T4 position at the epoch: one for a
        // boundary at or before the epoch still waiting in T4IF, one if the
        // epoch is half a slot or more past the last boundary.
        if (!t4_running)
        {
            expected_ms += 1U;
        }
        else
        {
            int64_t first_at_or_after = t4_next_boundary(now_epoch - 1);
            int64_t last_at_or_before = (first_at_or_after == now_epoch)
                                            ? now_epoch
                                            : first_at_or_after - (int64_t)EPOCH_SLOT_TICKS;
            if (deferred_pending)
                expected_ms += 1U;
            if (now_epoch - last_at_or_before >= (int64_t)SLOT_MIDPOINT_TICKS)
                expected_ms += 1U;
        }
        expected_at_epoch = expected_ms;

        // T3 handler.
        TMR2 = (uint16_t)hold.latency;
        TMR4 = t4_running ? t4_count_at(isr_time) : 0;
        T4CONbits.TON = t4_running;
        _T3Interrupt();
        CHECK(PR3 == (uint16_t)((running_expected - 1UL) >> 16) &&
              PR2 == (uint16_t)(running_expected - 1UL));
        CHECK(T4CONbits.TON == 1);
        CHECK(IFS1bits.T4IF == 0);
        t4_running = true;
        t4_start = isr_time;
        t4_preload = TMR4;
        CHECK(t4_preload == (uint16_t)hold.latency);
        deferred_pending = false;

        // Next period: mostly tracking, occasionally an acquisition step.
        if (rand() % 500 == 0)
        {
            period = 20000000UL + (uint32_t)(rand() % 40000000); // 0.5 .. 1.5 s
            steps++;
        }
        else
        {
            // Alternate between a slow (-2 ppm) and a fast (+2 ppm) crystal,
            // so the last T4 boundary falls either a whole ms or a few
            // cycles before the epoch.
            uint32_t base = ((epoch_index / 1000) % 2 == 0) ? 39999919UL : 40000079UL;
            period = base + (uint32_t)(rand() % 2);
        }
        // Request it as the control loop would, after this epoch: the
        // running interval is unchanged, the request loads at the next.
        epoch_timer_set_period(period);
        next_epoch = now_epoch + (int64_t)running_expected;
        next_hold = choose_hold_off();

        // Boundaries of this interval. Those in the hold-off window ahead of
        // the next epoch are not serviced: they wait in T4IF.
        for (b = t4_next_boundary(isr_time - 1); b <= next_epoch; b = t4_next_boundary(b))
        {
            int64_t offset = b - now_epoch;
            uint16_t slot_before = epoch_timer_slot();

            CHECK(offset % (int64_t)EPOCH_SLOT_TICKS == 0);
            if (b >= next_epoch - next_hold.before)
            {
                IFS1bits.T4IF = 1; // held off; at most one fits
                deferred_pending = true;
                deferred_before++;
                break;
            }
            _T4Interrupt();
            expected_ms++;
            CHECK(epoch_timer_slot() == (uint16_t)(slot_before + 1U));
            CHECK((int64_t)epoch_timer_slot() * (int64_t)EPOCH_SLOT_TICKS == offset);
            if (epoch_timer_slot() == EPOCH_MEASURE_SLOT)
            {
                EpochTimerSample timer;
                CHECK(epoch_timer_measure_due());
                epoch_timer_sample(&timer);
                CHECK(!epoch_timer_measure_due());
                CHECK(timer.sequence == (uint32_t)(epoch_index + 1));
                CHECK(timer.running_period == running_expected);
                last_measure_time = b;
            }
            if (epoch_timer_slot() == EPOCH_PULSE_PROBE_SLOT)
            {
                // Mid-pulse: armed mode, one-shot timer finished, pin high.
                EpochOutputSample output;
                epoch_output_sample(&output);
                CHECK(output.probe_con1 == OC1CON1_ARMED);
                CHECK((output.probe_con2 & OC1CON2_TRIGSTAT) == 0);
                CHECK(output.probe_pin);
            }
            if (epoch_timer_slot() == EPOCH_PULSE_END_SLOT)
            {
                // Pulse ended exactly 100 ms after the epoch, re-armed low.
                // Re-arming is a mode change, which drives the pin low.
                CHECK(OC1CON1 == OC1CON1_ARMED);
                CHECK((OC1CON2 & OC1CON2_TRIGSTAT) == 0);
                CHECK(IFS0bits.OC1IF == 0);
                _RB10 = 0;
                pulse_end_events++;
                last_pulse_end_time = b;
            }
        }
        CHECK(last_measure_time == now_epoch + (int64_t)EPOCH_SLOT_TICKS);
        CHECK(last_pulse_end_time == now_epoch + 100LL * (int64_t)EPOCH_SLOT_TICKS);
        CHECK(epoch_timer_ms() == expected_ms);

        now_epoch = next_epoch;
        hold = next_hold;
        running_expected = period;
    }

    printf("scheduler: %ld epochs, %ld acquisition steps, %ld late T3 handlers,\n"
           "  %ld T4 boundaries held off across an epoch\n",
           epochs, steps, late_isr, deferred_before);
    printf("  slots on whole ms after each epoch, measurement at 1 ms, pulse end at 100 ms\n");
    {
        EpochOutputSample output;
        EpochTimerSample timer;
        epoch_output_sample(&output);
        epoch_timer_sample(&timer);
        printf("  output pulses seen %lu, missed %lu\n", (unsigned long)output.pulses_seen,
               (unsigned long)output.pulses_missed);
        CHECK(timer.handler_late == 0);
        CHECK(output.pulses_seen == (uint32_t)epochs && output.pulses_missed == 0);
    }
    {
        int64_t elapsed_ms = (now_epoch - clock_origin) / (int64_t)EPOCH_SLOT_TICKS;
        printf("  ms tick %lu, exact at every epoch (raw oscillator: %lld ms)\n",
               (unsigned long)epoch_timer_ms(), (long long)elapsed_ms);
    }
    CHECK(pulse_end_events == epochs);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
