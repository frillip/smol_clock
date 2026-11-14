/*
 * The fault record (platform/fault.c): what a watchdog reset or a trap leaves
 * for the next start, and what is not a fault; the watchdog fed by the
 * epoch's work and by watchdog_extend() only; the task the scheduler names;
 * the application's own reset; the record's place in RAM.
 */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "../../../common/boot_layout.h"
#include "../../../common/sml_proto.h"
#include "../../platform/fault.h"
#include "../../timing/scheduler.h"

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

typedef struct
{
    uint16_t magic;
    uint8_t kind;
    uint8_t trap;
    uint32_t pc;
    uint32_t uptime_s;
    char task[FAULT_TASK_CHARS];
    uint16_t magic_inverse;
    uint8_t running;
    uint8_t spare[5];
} Record;

extern Record fault_host_record;
extern uint32_t fault_host_feeds;
extern bool fault_host_watchdog_on;
extern void (*fault_host_reset)(void);

static jmp_buf reset_point;
static int resets;

static void host_reset(void)
{
    resets++;
    longjmp(reset_point, 1);
}


// A trap taken with this return address (two stacked words), as traps.s
// passes it.
static void trap_at(uint16_t trap, uint32_t pc)
{
    if (setjmp(reset_point) == 0)
    {
        // SR 0x83 and IPL3 in the upper bits, as stacked: only PC<22:16> kept.
        fault_trap(trap, (uint16_t)pc, (uint16_t)(0x8380U | (pc >> 16 & 0x7FU)));
    }
}


int main(void)
{
    const Fault *last;

    printf("fault record\n");
    CHECK(sizeof(Record) == FAULT_RECORD_BYTES);
    CHECK(FAULT_RECORD_ADDRESS + FAULT_RECORD_BYTES <= MAILBOX_ADDRESS);
    CHECK(FAULT_RECORD_ADDRESS % 2 == 0);
    fault_host_reset = host_reset;

    // Power-on: whatever RAM holds is ignored, even a record that looks valid.
    memset(&fault_host_record, 0xA5, sizeof(fault_host_record));
    fault_init(SML_RESET_POR | SML_RESET_BOR);
    last = fault_last();
    CHECK(last->kind == FAULT_NONE);
    CHECK(fault_host_record.magic == 0x4654 && fault_host_record.magic_inverse == 0xB9AB);
    CHECK(fault_host_record.running == 1 && strcmp(fault_host_record.task, "start") == 0);

    // Running: the scheduler names each task, the epoch feeds and counts.
    fault_host_watchdog_on = false;
    fault_host_feeds = 0;
    watchdog_start();
    CHECK(fault_host_watchdog_on && fault_host_feeds == 1);
    fault_task("display load");
    CHECK(strncmp(fault_host_record.task, "display load", FAULT_TASK_CHARS) == 0);
    fault_task("a name too long to keep");
    CHECK(memcmp(fault_host_record.task, "a name too l", FAULT_TASK_CHARS) == 0);
    fault_task("10 ms");
    CHECK(strcmp(fault_host_record.task, "10 ms") == 0);
    CHECK(fault_host_record.task[FAULT_TASK_CHARS - 1] == '\0'); // the rest cleared
    fault_epoch(1234);
    CHECK(fault_host_feeds == 2 && fault_host_record.uptime_s == 1234);
    fault_task("10 ms"); // no feed from anything else
    CHECK(fault_host_feeds == 2);
    watchdog_extend("provision");
    CHECK(fault_host_feeds == 3 && strcmp(fault_host_record.task, "provision") == 0);
    fault_task("10 ms");

    // The watchdog fires in "10 ms": reported, with the task and uptime.
    fault_init(SML_RESET_WDTO);
    last = fault_last();
    CHECK(last->kind == FAULT_WATCHDOG && strcmp(last->task, "10 ms") == 0);
    CHECK(last->uptime_s == 1234 && last->pc == 0);
    fault_print();
    // The record starts again: a later reset by command is not a fault.
    CHECK(fault_host_record.uptime_s == 0 && strcmp(fault_host_record.task, "start") == 0);
    fault_init(SML_RESET_SWR);
    CHECK(fault_last()->kind == FAULT_NONE);

    // A trap: recorded with its address, the reset taken, then reported.
    fault_task("epoch");
    fault_epoch(77);
    resets = 0;
    trap_at(FAULT_TRAP_ADDRESS, 0x01A2B5UL); // SFA set in bit 0: not the PC's
    CHECK(resets == 1 && fault_host_record.kind == FAULT_TRAP);
    fault_init(SML_RESET_SWR);
    last = fault_last();
    CHECK(last->kind == FAULT_TRAP && last->trap == FAULT_TRAP_ADDRESS);
    CHECK(last->pc == 0x01A2B4UL && last->uptime_s == 77 && strcmp(last->task, "epoch") == 0);
    CHECK(strcmp(fault_trap_name(last->trap), "ADDRESS") == 0);
    fault_print();
    CHECK(strcmp(fault_trap_name(FAULT_TRAP_STACK), "STACK") == 0);
    CHECK(strcmp(fault_trap_name(FAULT_TRAP_HARD), "HARD") == 0);
    CHECK(strcmp(fault_trap_name(9), "?") == 0);

    // A trap whose reset is followed by a power cut: nothing reported.
    trap_at(FAULT_TRAP_MATH, 0x2400UL);
    fault_init(SML_RESET_POR | SML_RESET_BOR);
    CHECK(fault_last()->kind == FAULT_NONE);
    // ... or by MCLR before the PIC started again: not a trap's reset.
    trap_at(FAULT_TRAP_MATH, 0x2400UL);
    fault_init(SML_RESET_EXTR);
    CHECK(fault_last()->kind == FAULT_NONE);

    // The application's own reset, then the watchdog before it ran again:
    // the bootloader's, not a task's, and no uptime (the application's, not the SPL's).
    fault_task("10 ms");
    fault_epoch(500);
    fault_reset_planned();
    fault_init(SML_RESET_WDTO);
    last = fault_last();
    CHECK(last->kind == FAULT_WATCHDOG && strcmp(last->task, "SPL") == 0 && last->uptime_s == 0);

    // A watchdog reset with no record at all (lost to a brown-out before):
    // still a watchdog reset, nothing more known.
    memset(&fault_host_record, 0, sizeof(fault_host_record));
    fault_init(SML_RESET_WDTO);
    last = fault_last();
    CHECK(last->kind == FAULT_WATCHDOG && last->task[0] == '\0' && last->uptime_s == 0);

    // A record with rubbish in its task is not printed as it is.
    fault_task("ok");
    fault_host_record.task[1] = (char)0x07;
    fault_init(SML_RESET_WDTO);
    CHECK(strcmp(fault_last()->task, "o") == 0);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
