/*
 * Long printouts in pieces, one per pass of the main loop, so no pass is
 * held up by them. `pic -v` printed in one go took over 500 ms (field,
 * 2026-11-22): the integer formatting of a few hundred printf calls and
 * waiting for the 1 KB TX ring, while the epoch's work, the display and the
 * ESP link waited.
 *
 * A step is a function that prints one piece. Steps run in the order
 * queued, one per print_queue_service(), and only while the TX ring has
 * room (PRINT_QUEUE_ROOM), so a step seldom waits for it. A step may queue
 * more steps: they run next, before anything queued earlier, in the order
 * the step queued them, so a step that prints a table can queue its rows,
 * and a row a step that repeats itself (arg + 1) until done.
 *
 * What a step prints is the state when it runs, not when queued: a printout
 * spread over a few passes may straddle an epoch. Other text (per-epoch
 * debug lines, events) can fall between the steps.
 *
 * Portable: no hardware, for the host tests.
 */
#ifndef PRINT_QUEUE_H
#define PRINT_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#define PRINT_QUEUE_LENGTH 32
// A step runs only while the TX ring has at least this much free.
#define PRINT_QUEUE_ROOM 512U

typedef void (*PrintStep)(uint32_t now_ms, uint16_t arg, bool verbose);

// Queues step(arg, verbose): at the end, or from inside a running step,
// after the steps that step has queued so far. False if the queue is full
// (counted), the step not queued.
bool print_queue_add(PrintStep step, uint16_t arg, bool verbose);
// Free places: a caller about to queue several steps checks first.
uint8_t print_queue_free(void);
bool print_queue_busy(void);

// Runs the next step, if there is one and room says the TX ring has room.
// True if one ran.
bool print_queue_service(uint32_t now_ms, bool room);

typedef struct
{
    uint32_t steps;   // run
    uint32_t dropped; // not queued: full
    uint8_t most;     // most queued at once
    PrintStep last;   // the step run last, and its arg, to name a slow one
    uint16_t last_arg;
} PrintQueueStats;

const PrintQueueStats *print_queue_stats(void);
// Empties the queue (host tests).
void print_queue_reset(void);

#endif // PRINT_QUEUE_H
