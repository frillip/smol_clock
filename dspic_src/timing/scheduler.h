/*
 * Cooperative scheduler: decides which blocks of the main loop run on this
 * pass. The task list itself stays in main(), where it can be read top to
 * bottom:
 *
 *   static SchedulerTask every_10ms = SCHEDULER_EVERY("10 ms", 10);
 *   static SchedulerTask display_load = SCHEDULER_AT_SLOT("display load", 500);
 *
 *   while (1)
 *   {
 *       scheduler_pass();
 *       if (scheduler_due(&every_10ms))
 *       {
 *           poll_gnss();
 *       }
 *       if (scheduler_due(&display_load))
 *       {
 *           display_service();
 *       }
 *   }
 *
 * Time comes from the epoch timer (epoch_timer.h), not a timer of its own:
 * milliseconds that are exactly 1000 per epoch while tracking, and slots
 * counted from each epoch. So a periodic task keeps its phase against the
 * second, and a slot task runs at a fixed point in every second (once the
 * epoch pulse has ended, say) however long the epoch.
 *
 *   SCHEDULER_EVERY(name, period_ms)  every period_ms (at least 1). Due times are fixed
 *       on the timeline: a late run does not delay the next, and periods
 *       missed outright are skipped (and counted), never run back to back.
 *   SCHEDULER_AT_SLOT(name, slot)  once per epoch, at or after that slot.
 *       Epochs that end before it runs are counted as missed.
 *   SCHEDULER_SECTION(name)  work started by something else (the epoch
 *       snapshot, say): scheduler_section() marks its start, for timing only.
 *
 * Each task is timed from the moment it starts (scheduler_due() returning
 * true, or scheduler_section()) to the next call into the scheduler, so a
 * block of the main loop timed this way needs no end marker. Sections can
 * follow each other to time the parts of one block.
 *
 * Every task is listed by scheduler_print() with its runs, skipped or
 * missed periods, its worst lateness, its longest run and its share of the
 * time, and the longest pass of the main loop: the measure of whether
 * something is blocking it. Times are whole milliseconds: runs shorter than
 * 1 ms count only as they cross a millisecond boundary.
 */
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    SCHEDULER_PERIODIC = 0,
    SCHEDULER_SLOT,
    SCHEDULER_SECTION_KIND
} SchedulerKind;

typedef struct SchedulerTask
{
    // Set by the initialiser macros.
    const char *name;
    SchedulerKind kind;
    uint16_t interval; // period in ms, or the slot

    // Kept by the scheduler.
    bool started;
    uint32_t next_ms;    // periodic: when it is next due
    uint32_t last_epoch; // slot: the epoch it last ran in
    uint32_t runs;
    uint32_t missed;            // periods skipped, or epochs without a run
    uint16_t late_max;          // ms after it fell due, worst seen
    uint16_t busy_max;          // longest run, ms
    uint32_t busy_ms;           // time spent running since the last report
    struct SchedulerTask *next; // list of every task seen, for reports
} SchedulerTask;

#define SCHEDULER_EVERY(name, period_ms)                                       \
    {                                                                          \
        (name), SCHEDULER_PERIODIC, (period_ms), false, 0, 0, 0, 0, 0, 0, 0, 0 \
    }
#define SCHEDULER_AT_SLOT(name, slot)                                 \
    {                                                                 \
        (name), SCHEDULER_SLOT, (slot), false, 0, 0, 0, 0, 0, 0, 0, 0 \
    }
#define SCHEDULER_SECTION(name)                                          \
    {                                                                    \
        (name), SCHEDULER_SECTION_KIND, 0, false, 0, 0, 0, 0, 0, 0, 0, 0 \
    }

// Starts from the epoch timer's current time.
void scheduler_init(void);

// Told the name of each task as it starts, and "loop" when it ends (the
// main loop's own work in between): platform/fault.h keeps it, so a
// watchdog reset can say what was running.
typedef void (*SchedulerTaskHook)(const char *name);
void scheduler_set_task_hook(SchedulerTaskHook hook);

// Once at the top of every pass of the main loop: reads the time once, so
// every task in the pass sees the same moment.
void scheduler_pass(void);

// True if task should run in this pass; it is then counted as run, and
// timed until the next call into the scheduler.
bool scheduler_due(SchedulerTask *task);

// A section of work starts now (SCHEDULER_SECTION): counted and timed until
// the next call into the scheduler.
void scheduler_section(SchedulerTask *section);

// The time this pass started: epoch_timer_ms(), and the epoch and slot.
uint32_t scheduler_now_ms(void);
uint32_t scheduler_epoch(void);
uint16_t scheduler_slot(void);

// Each task, and the longest pass of the main loop since the last call
// (which then starts afresh).
void scheduler_print(void);

// The longest pass of the main loop since the last call, ms; starts afresh.
// Separate from scheduler_print()'s, so either can be used alone (STAT-SYS).
uint16_t scheduler_take_longest_pass_ms(void);

#endif // SCHEDULER_H
