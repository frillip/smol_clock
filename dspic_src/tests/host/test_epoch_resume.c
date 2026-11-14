/*
 * Host test of epoch_timer_resume() (timing/epoch_timer.c): after
 * interrupts were held off for longer than a slot (a flash erase), the slot
 * and the milliseconds catch up with where T2/T3 stands, the slot work
 * missed runs, and T4 is back in phase. SFRs are plain variables
 * (fake_xc/xc.h), set by hand.
 */
#include <stdbool.h>
#include <stdio.h>

#include <xc.h>
#include "../../timing/epoch_timer.h"

void _T3Interrupt(void);
void _T4Interrupt(void);

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

// T2/T3 as if this many ticks had passed since the epoch.
static void timer_at(uint32_t since_epoch)
{
    TMR2 = (uint16_t)since_epoch;
    TMR3HLD = (uint16_t)(since_epoch >> 16);
}


// An epoch, its interrupt on time, then T4 to the given slot.
static void epoch_then_slots(uint16_t slots)
{
    uint16_t index;

    IFS1bits.T4IF = 0;
    TMR2 = 10; // 10 ticks after the period match
    IFS0bits.T3IF = 1;
    _T3Interrupt();
    for (index = 0; index < slots; index++)
    {
        IFS1bits.T4IF = 1;
        _T4Interrupt();
    }
}


int main(void)
{
    uint32_t ms;
    uint32_t sequence;
    uint16_t slot;
    uint32_t slots_made_up;
    uint32_t spans;

    epoch_timer_init();
    epoch_then_slots(0);
    epoch_then_slots(700);
    epoch_timer_position(&sequence, &slot);
    CHECK(slot == 700);
    ms = epoch_timer_ms();

    // A 25.4 ms stall from slot 700: 25 boundaries missed, one of them
    // pending in T4IF.
    timer_at(725UL * EPOCH_SLOT_TICKS + 16000UL);
    IFS1bits.T4IF = 1;
    epoch_timer_resume();
    epoch_timer_position(&sequence, &slot);
    CHECK(slot == 725 && epoch_timer_ms() == ms + 25U);
    CHECK(TMR4 == 16000U && IFS1bits.T4IF == 0);
    epoch_timer_resume_stats(&slots_made_up, &spans);
    CHECK(slots_made_up == 25U && spans == 0U);

    // Nothing missed: nothing changes.
    timer_at(725UL * EPOCH_SLOT_TICKS + 20000UL);
    epoch_timer_resume();
    epoch_timer_position(&sequence, &slot);
    CHECK(slot == 725 && epoch_timer_ms() == ms + 25U && TMR4 == 20000U);

    // Across the measurement slot: its work runs too.
    epoch_then_slots(0);
    (void)epoch_timer_measure_due();
    {
        EpochTimerSample sample;
        epoch_timer_sample(&sample); // clears the flag
    }
    CHECK(!epoch_timer_measure_due());
    timer_at(30UL * EPOCH_SLOT_TICKS + 5UL);
    epoch_timer_resume();
    epoch_timer_position(&sequence, &slot);
    CHECK(slot == 30 && epoch_timer_measure_due());

    // The epoch came and went: left to its interrupt, and counted.
    epoch_then_slots(990);
    ms = epoch_timer_ms();
    IFS0bits.T3IF = 1;
    timer_at(15UL * EPOCH_SLOT_TICKS);
    epoch_timer_resume();
    epoch_timer_position(&sequence, &slot);
    CHECK(slot == 990 && epoch_timer_ms() == ms);
    epoch_timer_resume_stats(&slots_made_up, &spans);
    CHECK(spans == 1U);
    IFS0bits.T3IF = 0;

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
