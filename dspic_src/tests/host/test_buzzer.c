/*
 * The buzzer (drivers/buzzer.c): patterns played by the Timer5 tick, as V2's
 * chimes, button click and alarm, and the priorities between them. Timer5
 * and the pin are plain variables; the test runs the interrupt each
 * millisecond while the timer is on.
 */
#include <stdio.h>
#include <string.h>

#include <xc.h>
#include "../../drivers/buzzer.h"

volatile uint16_t T5CON, TMR5, PR5;
volatile unsigned _LATB7;
volatile T5CONBITS_t T5CONbits;
volatile IEC1BITS_t IEC1bits;
volatile IFS1BITS_t IFS1bits;
volatile IPC7BITS_t IPC7bits;

void _T5Interrupt(void);

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

// What the pin did: runs of on and off, ms.
static int runs[200];
static int run_count;
static unsigned last_pin;

static void record_start(void)
{
    run_count = 0;
    last_pin = _LATB7;
    runs[0] = 0;
}


// One ms: the interrupt if the timer runs, then the pin sampled.
static void tick(void)
{
    if (T5CONbits.TON && IEC1bits.T5IE)
    {
        IFS1bits.T5IF = 1;
        _T5Interrupt();
    }
    if (_LATB7 != last_pin)
    {
        last_pin = _LATB7;
        run_count++;
        runs[run_count] = 0;
    }
    runs[run_count]++;
}


static void run_ms(int ms)
{
    while (ms-- > 0)
    {
        tick();
    }
}


// Runs until silent (at most limit ms); true if it went silent.
static bool run_out(int limit)
{
    while (limit-- > 0)
    {
        tick();
        if (buzzer_playing() == BUZZER_PRIORITY_NONE && !T5CONbits.TON)
        {
            return true;
        }
    }
    return false;
}


int main(void)
{
    int index;

    buzzer_init();
    CHECK(!_LATB7 && !T5CONbits.TON && PR5 == 4999U && IPC7bits.T5IP == 1U);

    // The 3 o'clock chime: three beeps, 100 on, 100 off.
    CHECK(buzzer_play(&buzzer_chime, 3, BUZZER_PRIORITY_CHIME));
    record_start();
    CHECK(_LATB7 == 1U); // on at once
    CHECK(run_out(2000) && !_LATB7);
    // runs: on 100, off 100, on 100, off 100, on 100, then off to the end.
    CHECK(run_count == 5);
    for (index = 0; index < 5; index++)
    {
        CHECK(runs[index] >= 99 && runs[index] <= 101);
    }

    // Midnight: 12, as 4-4-4 with 300 ms between groups.
    CHECK(buzzer_play(&buzzer_chime, 12, BUZZER_PRIORITY_CHIME));
    record_start();
    CHECK(run_out(5000));
    CHECK(run_count == 23); // 12 beeps and the 11 silences between them
    CHECK(runs[7] >= 299 && runs[7] <= 301 && runs[15] >= 299 && runs[15] <= 301);
    CHECK(runs[5] >= 99 && runs[5] <= 101);

    // A button click during a chime is refused; a chime replaces a click.
    CHECK(buzzer_play(&buzzer_chime, 1, BUZZER_PRIORITY_CHIME));
    run_ms(20);
    CHECK(!buzzer_play(&buzzer_button, 1, BUZZER_PRIORITY_BUTTON));
    CHECK(run_out(500));
    CHECK(buzzer_play(&buzzer_button, 1, BUZZER_PRIORITY_BUTTON));
    record_start();
    CHECK(run_out(500) && runs[0] >= 59 && runs[0] <= 61);
    CHECK(buzzer_play(&buzzer_button, 1, BUZZER_PRIORITY_BUTTON));
    run_ms(10);
    CHECK(buzzer_play(&buzzer_chime, 1, BUZZER_PRIORITY_CHIME));
    CHECK(buzzer_playing() == BUZZER_PRIORITY_CHIME);
    CHECK(run_out(500));

    // The alarm: repeats until stopped; nothing below it gets in, and a
    // lower stop does not end it.
    CHECK(buzzer_play(&buzzer_alarm, 4, BUZZER_PRIORITY_ALARM));
    run_ms(10000);
    CHECK(buzzer_playing() == BUZZER_PRIORITY_ALARM);
    CHECK(!buzzer_play(&buzzer_chime, 1, BUZZER_PRIORITY_CHIME));
    buzzer_stop(BUZZER_PRIORITY_CHIME);
    CHECK(buzzer_playing() == BUZZER_PRIORITY_ALARM);
    buzzer_stop(BUZZER_PRIORITY_ALARM);
    CHECK(buzzer_playing() == BUZZER_PRIORITY_NONE && !_LATB7 && !T5CONbits.TON);
    CHECK(buzzer_stats()->refused == 2 && buzzer_stats()->stopped == 1);

    // Nothing to play.
    CHECK(!buzzer_play(&buzzer_chime, 0, BUZZER_PRIORITY_CHIME));

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
