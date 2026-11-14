/*
 * Host test of the cooperative scheduler, against a stand-in epoch timer
 * whose milliseconds, epoch and slot the test sets directly.
 */
#include <stdio.h>
#include <string.h>

#include "timing/scheduler.h"
#include "timing/epoch_timer.h"

static uint32_t fake_ms, fake_epoch;
static uint16_t fake_slot;

uint32_t epoch_timer_ms(void)
{
    return fake_ms;
}


void epoch_timer_position(uint32_t *sequence, uint16_t *slot)
{
    *sequence = fake_epoch;
    *slot = fake_slot;
}


void epoch_timer_resume_stats(uint32_t *slots, uint32_t *spans)
{
    *slots = 0;
    *spans = 0;
}


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

// The clock at an absolute millisecond: epochs of exactly 1000 ms.
static int hooked;
static const char *hooked_name;

static void note_task(const char *name)
{
    hooked++;
    hooked_name = name;
}


static void at(uint32_t ms)
{
    fake_ms = ms;
    fake_epoch = ms / 1000U;
    fake_slot = (uint16_t)(ms % 1000U);
}


int main(void)
{
    static SchedulerTask every_10ms = SCHEDULER_EVERY("10 ms", 10U);
    static SchedulerTask every_100ms = SCHEDULER_EVERY("100 ms", 100U);
    static SchedulerTask at_500 = SCHEDULER_AT_SLOT("slot 500", 500U);
    uint32_t ms;
    uint32_t run_times[8];
    int fast_runs = 0;

    at(5000);
    scheduler_init();

    // One pass per millisecond for a second: due exactly on the period,
    // phase kept from the first use, the slot task once at slot 500.
    for (ms = 5000; ms < 6000; ms++)
    {
        at(ms);
        scheduler_pass();
        if (scheduler_due(&every_10ms))
        {
            if (fast_runs < 8)
                run_times[fast_runs] = ms;
            fast_runs++;
        }
        (void)scheduler_due(&every_100ms);
        if (scheduler_due(&at_500))
            CHECK(fake_slot == 500U);
    }
    printf("steady: 10 ms task ran %d times, first at %lu, %lu\n", fast_runs,
           (unsigned long)run_times[0], (unsigned long)run_times[1]);
    CHECK(fast_runs == 99 && run_times[0] == 5010 && run_times[1] == 5020);
    CHECK(every_100ms.runs == 9 && at_500.runs == 1);
    CHECK(every_10ms.missed == 0 && every_10ms.late_max == 0 && at_500.missed == 0);

    // A pass blocked for 25 ms: one late run, two periods skipped, and the
    // next due time stays on the 10 ms grid.
    at(6000);
    scheduler_pass();
    CHECK(scheduler_due(&every_10ms)); // due at 6000
    at(6035);
    scheduler_pass();
    CHECK(scheduler_due(&every_10ms));
    CHECK(every_10ms.late_max == 25 && every_10ms.missed == 2);
    at(6039);
    scheduler_pass();
    CHECK(!scheduler_due(&every_10ms));
    at(6040);
    scheduler_pass();
    CHECK(scheduler_due(&every_10ms));

    // The slot task: once per epoch, never before its slot; epochs that
    // pass without a run are counted.
    at(6400);
    scheduler_pass();
    CHECK(!scheduler_due(&at_500)); // epoch 6 not yet at 500
    at(6700);
    scheduler_pass();
    CHECK(scheduler_due(&at_500) && !scheduler_due(&at_500));
    at(8200); // epoch 7 went by entirely
    scheduler_pass();
    CHECK(!scheduler_due(&at_500));
    at(8510);
    scheduler_pass();
    CHECK(scheduler_due(&at_500));
    CHECK(at_500.missed == 1 && at_500.late_max == 200);

    // A stretched epoch (an acquisition step of 1.4 s): still once.
    fake_ms = 9000;
    fake_epoch = 9;
    for (fake_slot = 0; fake_slot < 1400; fake_slot++)
    {
        fake_ms++;
        scheduler_pass();
        (void)scheduler_due(&at_500);
    }
    CHECK(at_500.runs == 4 && at_500.missed == 1);

    // Busy time: a task is timed until the next call into the scheduler, and
    // sections time the parts of a block started elsewhere.
    {
        static SchedulerTask part_a = SCHEDULER_SECTION("part a");
        static SchedulerTask part_b = SCHEDULER_SECTION("part b");
        at(10000);
        scheduler_pass();
        scheduler_section(&part_a);
        fake_ms = 10030; // part a took 30 ms
        scheduler_section(&part_b);
        fake_ms = 10035; // part b took 5 ms
        scheduler_pass();
        CHECK(part_a.busy_max == 30 && part_b.busy_max == 5 && part_a.runs == 1);
        at(10040);
        scheduler_pass();
        CHECK(scheduler_due(&every_10ms));
        fake_ms = 10047;
        CHECK(!scheduler_due(&at_500)); // closes the 10 ms task's run: 7 ms
        CHECK(every_10ms.busy_max >= 7);
        scheduler_print();
        CHECK(part_a.busy_max == 0 && part_a.busy_ms == 0); // afresh after a report
    }

    // The longest pass is reported, then starts afresh.
    at(20000);
    scheduler_pass();
    at(20001);
    scheduler_pass();
    scheduler_print();
    at(20002);
    scheduler_pass();
    at(20003);
    scheduler_pass();
    scheduler_print();

    // STAT-SYS's longest pass: its own maximum, kept apart from the report's.
    at(30000);
    scheduler_pass();
    CHECK(scheduler_take_longest_pass_ms() >= 9997); // the jump from 20003
    at(30025);                                       // a 25 ms pass
    scheduler_pass();
    scheduler_print(); // does not reset the other
    at(30026);
    scheduler_pass();
    CHECK(scheduler_take_longest_pass_ms() == 25);
    CHECK(scheduler_take_longest_pass_ms() == 0); // taken

    // The task hook (the fault record's): each task as it starts, "loop"
    // when its run ends, nothing for a task not due.
    {
        static SchedulerTask part_c = SCHEDULER_SECTION("part c");
        scheduler_set_task_hook(note_task);
        at(40000);
        scheduler_pass();
        CHECK(hooked == 0);
        (void)scheduler_due(&every_10ms);
        CHECK(hooked == 1 && strcmp(hooked_name, "10 ms") == 0);
        (void)scheduler_due(&at_500); // not due: the 10 ms run ends
        CHECK(hooked == 2 && strcmp(hooked_name, "loop") == 0);
        scheduler_section(&part_c);
        CHECK(hooked == 3 && strcmp(hooked_name, "part c") == 0);
        scheduler_pass();
        CHECK(hooked == 4 && strcmp(hooked_name, "loop") == 0);
        scheduler_pass();
        CHECK(hooked == 4);
        scheduler_set_task_hook(NULL);
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
