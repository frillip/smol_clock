/*
 * The PPS watch's change-notification handler (timing/pps_monitor.c)
 * against a model of the CN flag: any change on a watched pin sets CNIF,
 * and the handler runs again while CNIF is set.
 *
 * The case that matters, seen on the bench (2026-10-21): the ESP's edge and
 * the GNSS edge a few microseconds apart, the second arriving while the
 * handler is busy with the first. Every edge must be seen, on time.
 */
#include <stdio.h>
#include <string.h>

#include <xc.h>

#include "../../timing/pps_monitor.h"

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

volatile IFS1BITS_t IFS1bits;
volatile IEC1BITS_t IEC1bits;
volatile IPC4BITS_t IPC4bits;
volatile CNENABITS_t CNENAbits;
volatile CNENBBITS_t CNENBbits;

void _CNInterrupt(void);

// ---- The model ----------------------------------------------------------
static int pins[2];
static uint32_t now_ticks; // the capture timebase
static int reads;          // pin reads so far
static int edge_after;     // raise edge_pin after this many reads (0: never)
static int edge_pin;

static void set_pin(int pin, int level)
{
    if (pins[pin] != level)
    {
        pins[pin] = level;
        IFS1bits.CNIF = 1; // any change sets the flag
    }
}


int fake_pin_read(int pin)
{
    int level = pins[pin];

    now_ticks += 4; // a read takes a few cycles
    if (++reads == edge_after)
    {
        set_pin(edge_pin, 1); // just after this read
    }
    return level;
}


uint32_t pps_capture_timebase(void)
{
    return now_ticks;
}


uint32_t epoch_timer_ms(void)
{
    return now_ticks / 40000U;
}


// The hardware: the handler runs while the flag is set.
static void interrupts(void)
{
    int runs = 0;

    while (IFS1bits.CNIF && runs++ < 10)
    {
        _CNInterrupt();
    }
}


// ---- Tests ---------------------------------------------------------------
static void second(uint32_t start, int race_after)
{
    PpsMonitorSample sample;
    uint32_t esp_edge = start;

    // The ESP's edge; the GNSS edge arrives after race_after pin reads of
    // the handler (0: separately, 10 us later).
    now_ticks = esp_edge;
    reads = 0;
    edge_after = race_after;
    edge_pin = 0;
    set_pin(1, 1);
    interrupts();
    if (race_after == 0)
    {
        now_ticks = esp_edge + 400U;
        set_pin(0, 1);
        interrupts();
    }
    edge_after = 0;
    pps_monitor_sample(&sample);
    CHECK(sample.inputs[PPS_INPUT_ESP].valid && sample.inputs[PPS_INPUT_GNSS].valid);
    // Both stamped within a few microseconds of their edges, not at the
    // falling edge 100 ms later.
    CHECK(sample.inputs[PPS_INPUT_ESP].stamp - esp_edge < 100U);
    CHECK(sample.inputs[PPS_INPUT_GNSS].stamp - esp_edge < 600U);

    // The pulses end 100 ms later.
    now_ticks = esp_edge + 4000000U;
    set_pin(1, 0);
    set_pin(0, 0);
    interrupts();
    pps_monitor_sample(&sample);
    CHECK(!sample.inputs[PPS_INPUT_ESP].valid && !sample.inputs[PPS_INPUT_GNSS].valid);
}


int main(void)
{
    const PpsMonitorStats *stats = pps_monitor_stats();
    uint32_t start = 1000000U;
    int race;

    pps_monitor_init();
    CHECK(CNENAbits.CNIEA8 && CNENBbits.CNIEB15 && IEC1bits.CNIE);

    // Apart, then the GNSS edge after each read of the handler's first pass
    // and its second.
    second(start, 0);
    for (race = 1; race <= 4; race++)
    {
        start += 40000000U;
        second(start, race);
    }
    printf("PPS watch: %lu GNSS and %lu ESP edges in 5 seconds, %lu interrupts with nothing new\n",
           (unsigned long)stats->edges[PPS_INPUT_GNSS], (unsigned long)stats->edges[PPS_INPUT_ESP],
           (unsigned long)stats->unseen);
    CHECK(stats->edges[PPS_INPUT_GNSS] == 5 && stats->edges[PPS_INPUT_ESP] == 5);
    CHECK(stats->implausible[PPS_INPUT_GNSS] == 0 && stats->implausible[PPS_INPUT_ESP] == 0);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
