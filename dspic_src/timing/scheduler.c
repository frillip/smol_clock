#include "scheduler.h"

#include <stddef.h>
#include <stdio.h>

#include "../platform/time_ms.h"
#include "../platform/utc.h"
#include "epoch_timer.h"

static uint32_t now_ms;
static uint32_t epoch;
static uint16_t slot;
static bool passed;
static uint32_t pass_start_ms;
static uint16_t pass_max_ms;
static uint16_t pass_max_taken_ms; // for scheduler_take_longest_pass_ms()
static uint32_t passes;
static SchedulerTask *tasks;   // every task seen, in order of first use
static SchedulerTask *running; // timed until the next call in
static uint32_t running_since_ms;
static uint32_t report_start_ms;
static SchedulerTaskHook task_hook; // told what runs: the fault record's

static void enrol(SchedulerTask *task)
{
    SchedulerTask **end = &tasks;

    while (*end != NULL)
    {
        end = &(*end)->next;
    }
    *end = task;
    task->next = NULL;
    task->started = true;
    task->next_ms = now_ms + task->interval;
    task->last_epoch = epoch - 1U; // due in this epoch
}


// Ends the timing of whatever ran since the last call into the scheduler.
static void stop_running(void)
{
    if (running != NULL)
    {
        uint32_t busy = time_ms_elapsed(epoch_timer_ms(), running_since_ms);
        running->busy_ms += busy;
        if (busy > running->busy_max)
        {
            running->busy_max = busy > UINT16_MAX ? UINT16_MAX : (uint16_t)busy;
        }
        running = NULL;
        if (task_hook != NULL)
        {
            task_hook("loop");
        }
    }
}


static void start_running(SchedulerTask *task)
{
    task->runs++;
    running = task;
    running_since_ms = epoch_timer_ms();
    if (task_hook != NULL)
    {
        task_hook(task->name);
    }
}


static void note_lateness(SchedulerTask *task, uint32_t late)
{
    if (late > task->late_max)
    {
        task->late_max = late > UINT16_MAX ? UINT16_MAX : (uint16_t)late;
    }
}


static bool periodic_due(SchedulerTask *task)
{
    uint32_t late;

    if ((int32_t)(now_ms - task->next_ms) < 0)
    {
        return false; // not yet: wrap-safe for half the 49-day range
    }
    late = time_ms_elapsed(now_ms, task->next_ms);
    note_lateness(task, late);
    // Whole periods that went by entirely are skipped, not run in a burst.
    task->missed += late / task->interval;
    task->next_ms += task->interval * (late / task->interval + 1U);
    return true;
}


static bool slot_due(SchedulerTask *task)
{
    uint32_t epochs = epoch - task->last_epoch;

    if (epochs == 0U || slot < task->interval)
    {
        return false;
    }
    note_lateness(task, (uint32_t)(slot - task->interval));
    task->missed += epochs - 1U;
    task->last_epoch = epoch;
    return true;
}


void scheduler_set_task_hook(SchedulerTaskHook hook)
{
    task_hook = hook;
}


void scheduler_init(void)
{
    tasks = NULL;
    running = NULL;
    passed = false;
    passes = 0;
    pass_max_ms = 0;
    pass_max_taken_ms = 0;
    scheduler_pass();
    report_start_ms = now_ms;
}


void scheduler_pass(void)
{
    stop_running();
    now_ms = epoch_timer_ms();
    epoch_timer_position(&epoch, &slot);
    if (passed)
    {
        uint32_t pass_ms = time_ms_elapsed(now_ms, pass_start_ms);
        if (pass_ms > pass_max_ms)
        {
            pass_max_ms = pass_ms > UINT16_MAX ? UINT16_MAX : (uint16_t)pass_ms;
        }
        if (pass_ms > pass_max_taken_ms)
        {
            pass_max_taken_ms = pass_ms > UINT16_MAX ? UINT16_MAX : (uint16_t)pass_ms;
        }
    }
    passed = true;
    pass_start_ms = now_ms;
    passes++;
}


bool scheduler_due(SchedulerTask *task)
{
    bool due;

    stop_running();
    if (!task->started)
    {
        enrol(task);
    }
    due = task->kind == SCHEDULER_SLOT ? slot_due(task) : periodic_due(task);
    if (due)
    {
        start_running(task);
    }
    return due;
}


void scheduler_section(SchedulerTask *section)
{
    stop_running();
    if (!section->started)
    {
        enrol(section);
    }
    start_running(section);
}


uint32_t scheduler_now_ms(void)
{
    return now_ms;
}


uint32_t scheduler_epoch(void)
{
    return epoch;
}


uint16_t scheduler_slot(void)
{
    return slot;
}


// Share of the reporting window, in tenths of a percent.
static uint32_t permille(uint32_t part, uint32_t whole)
{
    return whole == 0U ? 0U : (part * 1000U + whole / 2U) / whole;
}


uint16_t scheduler_take_longest_pass_ms(void)
{
    uint16_t longest = pass_max_taken_ms;

    pass_max_taken_ms = 0;
    return longest;
}


void scheduler_print(void)
{
    SchedulerTask *task;
    uint32_t window_ms;
    uint32_t resumed_slots;
    uint32_t resume_spans;

    stop_running();
    window_ms = time_ms_elapsed(epoch_timer_ms(), report_start_ms);
    epoch_timer_resume_stats(&resumed_slots, &resume_spans);
    printf("Scheduler: %lu passes in ", (unsigned long)passes);
    utc_print_ns((int64_t)window_ms * 1000000LL, false);
    printf(", longest %u ms", pass_max_ms);
    if (resumed_slots != 0U || resume_spans != 0U)
    {
        // Held up by flash erases (drivers/flash.h), since start-up.
        printf("; %lu slots made up after flash stalls, %lu stalls across an epoch",
               (unsigned long)resumed_slots, (unsigned long)resume_spans);
    }
    printf("\r\n");
    for (task = tasks; task != NULL; task = task->next)
    {
        uint32_t share = permille(task->busy_ms, window_ms);

        if (task->kind == SCHEDULER_SECTION_KIND)
        {
            printf("  %-14s section       : %lu runs", task->name, (unsigned long)task->runs);
        }
        else
        {
            printf("  %-14s %s %4u ms: %lu runs, %lu %s, worst %u ms late", task->name,
                   task->kind == SCHEDULER_SLOT ? "at slot" : "every  ", task->interval,
                   (unsigned long)task->runs, (unsigned long)task->missed,
                   task->kind == SCHEDULER_SLOT ? "epochs missed" : "periods skipped",
                   task->late_max);
        }
        printf("; busy max %u ms, %lu.%lu%%\r\n", task->busy_max, (unsigned long)(share / 10U),
               (unsigned long)(share % 10U));
        task->busy_ms = 0;
        task->busy_max = 0;
    }
    passes = 0;
    pass_max_ms = 0;
    report_start_ms = epoch_timer_ms();
}
