#include "fault.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/boot_layout.h"
#include "../../common/sml_proto.h"
#ifdef __XC16__
#include <xc.h>
#endif

#define FAULT_MAGIC 0x4654 // "FT"

// The record, at its fixed place, never initialised by the C start-up. On
// the host, an ordinary variable the tests set (fault_host_record), and the
// watchdog and the reset counted.
typedef struct
{
    uint16_t magic;
    uint8_t kind;    // FAULT_NONE, or FAULT_TRAP from a trap handler
    uint8_t trap;    // FAULT_TRAP_*
    uint32_t pc;     // a trap's return address
    uint32_t uptime_s;
    char task[FAULT_TASK_CHARS]; // not terminated when full
    uint16_t magic_inverse;
    uint8_t running; // the application is running: not left by its own reset
    uint8_t spare[5];
} FaultRecord;

typedef char fault_record_size[sizeof(FaultRecord) == FAULT_RECORD_BYTES ? 1 : -1];
// STAT-SYS carries them as they are.
typedef char fault_sml_kinds[FAULT_WATCHDOG == SML_FAULT_WATCHDOG && FAULT_TRAP == SML_FAULT_TRAP ? 1 : -1];
typedef char fault_sml_traps[FAULT_TRAP_OSCILLATOR == SML_TRAP_OSCILLATOR &&
                                     FAULT_TRAP_ADDRESS == SML_TRAP_ADDRESS &&
                                     FAULT_TRAP_HARD == SML_TRAP_HARD &&
                                     FAULT_TRAP_STACK == SML_TRAP_STACK &&
                                     FAULT_TRAP_MATH == SML_TRAP_MATH && FAULT_TRAP_DMA == SML_TRAP_DMA &&
                                     FAULT_TRAP_SOFT == SML_TRAP_SOFT
                                 ? 1
                                 : -1];

#ifdef __XC16__
static FaultRecord record __attribute__((persistent, address(FAULT_RECORD_ADDRESS)));
#define WATCHDOG_CLEAR() ClrWdt()
#else
FaultRecord fault_host_record;
uint32_t fault_host_feeds;
bool fault_host_watchdog_on;
void (*fault_host_reset)(void); // the test's: a longjmp
#define record           fault_host_record
#define WATCHDOG_CLEAR() (fault_host_feeds++)
#endif

static Fault last;

static void set_task(const char *name)
{
    uint8_t index;

    for (index = 0; index < FAULT_TASK_CHARS; index++)
    {
        record.task[index] = name[index];
        if (name[index] == '\0')
        {
            break;
        }
    }
    for (; index < FAULT_TASK_CHARS; index++)
    {
        record.task[index] = '\0';
    }
}


void fault_init(uint16_t reset_cause)
{
    bool valid = record.magic == FAULT_MAGIC && record.magic_inverse == (uint16_t)~FAULT_MAGIC &&
                 (reset_cause & (SML_RESET_POR | SML_RESET_BOR)) == 0U;

    memset(&last, 0, sizeof(last));
    if ((reset_cause & SML_RESET_WDTO) != 0U)
    {
        last.kind = FAULT_WATCHDOG;
    }
    else if (valid && record.kind == FAULT_TRAP && (reset_cause & SML_RESET_SWR) != 0U)
    {
        last.kind = FAULT_TRAP;
        last.trap = record.trap;
        last.pc = record.pc;
    }
    if (last.kind != FAULT_NONE && valid)
    {
        last.uptime_s = record.uptime_s;
        if (record.running)
        {
            uint8_t index;

            for (index = 0; index < FAULT_TASK_CHARS; index++)
            {
                char c = record.task[index];
                last.task[index] = c >= 0x20 && c < 0x7F ? c : '\0';
                if (c == '\0')
                {
                    break;
                }
            }
        }
        else
        {
            // After the application's own reset: in the bootloader. The
            // uptime is the application's from before it, not the SPL's.
            strcpy(last.task, "SPL");
            last.uptime_s = 0;
        }
    }

    memset(&record, 0, sizeof(record));
    record.magic = FAULT_MAGIC;
    record.magic_inverse = (uint16_t)~FAULT_MAGIC;
    record.running = 1;
    set_task("start");
}


const Fault *fault_last(void)
{
    return &last;
}


void watchdog_start(void)
{
#ifndef __DEBUG
    WATCHDOG_CLEAR();
#ifdef __XC16__
    RCONbits.SWDTEN = 1;
#else
    fault_host_watchdog_on = true;
#endif
#endif
}


void fault_epoch(uint32_t uptime_s)
{
    WATCHDOG_CLEAR();
    record.uptime_s = uptime_s;
}


void watchdog_extend(const char *what)
{
    WATCHDOG_CLEAR();
    set_task(what);
}


void fault_task(const char *name)
{
    set_task(name);
}


void fault_reset_planned(void)
{
    record.running = 0;
    set_task("reset");
}


void fault_trap(uint16_t trap, uint16_t pc_low, uint16_t pc_high)
{
    record.kind = FAULT_TRAP;
    record.trap = (uint8_t)trap;
    // The stacked words: PC<15:1> with SFA in bit 0, then SR<7:0>, IPL3
    // and PC<22:16>.
    record.pc = (uint32_t)(pc_high & 0x007FU) << 16 | (pc_low & 0xFFFEU);
    record.running = 1;
#ifdef __XC16__
    __asm__ volatile("reset");
    while (1)
        ;
#else
    if (fault_host_reset != NULL)
    {
        fault_host_reset();
    }
    abort();
#endif
}


const char *fault_trap_name(uint8_t trap)
{
    static const char *const names[] = {"?",     "OSCILLATOR", "ADDRESS", "HARD",
                                        "STACK", "MATH",       "DMA",     "SOFT"};

    return trap < sizeof(names) / sizeof(names[0]) ? names[trap] : "?";
}


void fault_print(void)
{
    if (last.kind == FAULT_NONE)
    {
        return;
    }
    if (last.kind == FAULT_TRAP)
    {
        printf("Fault: %s trap, before 0x%06lX", fault_trap_name(last.trap), (unsigned long)last.pc);
    }
    else
    {
        printf("Fault: watchdog");
    }
    if (last.task[0] != '\0')
    {
        printf(", in %s", last.task);
    }
    if (last.uptime_s != 0U)
    {
        printf(", up %lu s", (unsigned long)last.uptime_s);
    }
    printf("\n");
}
