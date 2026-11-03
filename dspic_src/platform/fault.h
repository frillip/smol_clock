#ifndef FAULT_H
#define FAULT_H

/*
 * Faults: the watchdog, the trap handlers, and what the last one was.
 *
 * The watchdog (PR128, PS1024 on the LPRC: 3.9 s measured) is fed from the
 * epoch's work, which runs once a second from the main loop when the epoch
 * timer's measurement slot comes round. So it is fed only while both are
 * going: a main loop stuck anywhere, an interrupt that never lets it run, or
 * an epoch timer that has stopped all reset the PIC within 4 s. Nothing else
 * feeds it, but for work that legitimately holds the main loop for seconds,
 * which calls watchdog_extend() as it goes (none does yet; the SPL writer of
 * the bootloader's phase 7 runs inside the epoch's quiet windows instead).
 *
 * The trap handlers (platform/traps.s) take the trap and the address it
 * happened at, and reset. The record of both, 32 bytes at
 * FAULT_RECORD_ADDRESS (common/boot_layout.h), is kept over that reset: what
 * was running (the scheduler's task, kept up to date by fault_task()), the
 * uptime, and for a trap which and where. At the next start fault_init()
 * reads it, with the reset cause: a watchdog reset or a trap is reported
 * (fault_last(), STAT-SYS, the start-up line), anything else is not.
 *
 * A debug build (__DEBUG) never starts the watchdog: a breakpoint would
 * otherwise reset the PIC under the debugger.
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    FAULT_NONE = 0,
    FAULT_WATCHDOG = 1,
    FAULT_TRAP = 2
} FaultKind;

// The traps, numbered in the order of their vectors from 1 (and as SML
// STAT-SYS reports them).
typedef enum
{
    FAULT_TRAP_OSCILLATOR = 1,
    FAULT_TRAP_ADDRESS = 2,
    FAULT_TRAP_HARD = 3,
    FAULT_TRAP_STACK = 4,
    FAULT_TRAP_MATH = 5,
    FAULT_TRAP_DMA = 6,
    FAULT_TRAP_SOFT = 7
} FaultTrap;

#define FAULT_TASK_CHARS 12

typedef struct
{
    FaultKind kind; // FAULT_NONE: the last reset was not a fault
    uint8_t trap;   // FAULT_TRAP_*, for FAULT_TRAP
    uint32_t pc;    // a trap's return address: the instruction after the one at fault
    uint32_t uptime_s;
    char task[FAULT_TASK_CHARS + 1]; // what was running, as the scheduler names it
} Fault;

// At start-up, with the reset cause (SML_RESET_* bits, boot_init()'s): what
// the record says of the last reset; then a new record.
void fault_init(uint16_t reset_cause);
const Fault *fault_last(void);

// The watchdog started: at the end of start-up, before the main loop.
void watchdog_start(void);

// Every epoch, from its work: the watchdog fed, the uptime recorded.
void fault_epoch(uint32_t uptime_s);

// Work that holds the main loop for longer than the watchdog allows, with
// good reason: call often enough (under 3.9 s apart). what names it in the
// record.
void watchdog_extend(const char *what);

// What is running now (the scheduler's hook): copied into the record.
void fault_task(const char *name);

// Before the application's own reset (system_reset()): a watchdog reset
// before it runs again is the bootloader's, not its.
void fault_reset_planned(void);

// From a trap handler: recorded, then a reset. Never returns.
void fault_trap(uint16_t trap, uint16_t pc_low, uint16_t pc_high) __attribute__((noreturn));

// "OSCILLATOR", "ADDRESS", ...
const char *fault_trap_name(uint8_t trap);

// One line about the last fault, if there was one.
void fault_print(void);

#endif // FAULT_H
