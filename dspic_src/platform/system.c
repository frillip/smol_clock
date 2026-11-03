#include "system.h"

#include "../../common/boot_layout.h"
#include "boot.h"
#include "fault.h"

// The configuration words are the IPL's (dspic_ipl/ipl_config.c),
// programmed once with it: an application image carries none. Programmed
// without the IPL, the chip runs on the erased configuration words'
// defaults, which this firmware does not expect.

static void clock_init(void);

// Why the PIC last reset, at start-up: from the IPL's mailbox (boot.h).
static uint16_t reset_cause;

void system_init(void)
{
    uint16_t rcon = RCON;

    // First, before anything can reset again: the bits stay set until
    // cleared, so clearing them now makes the next reset's cause its own.
    // The IPL has cleared them already and left the cause in the mailbox.
    RCON = (uint16_t)(RCON & ~BOOT_RESET_CAUSES);
    reset_cause = boot_init(rcon);
    fault_init(reset_cause);
    board_init();
    clock_init();
    // Interrupt priorities are set by the module that owns each interrupt:
    // console_init() for UART1, timing/ for the timers and captures.
    __builtin_enable_interrupts();
}


// OSCCON NOSC: the primary oscillator (EC) through the PLL, and the FRC
// through it.
#define NOSC_PRIPLL 0x03U
#define NOSC_FRCPLL 0x01U
// A switch normally completes within about 1 ms (the PLL's lock). On the
// FRC (3.685 MIPS) this many polls of OSWEN are some 30 ms: past that, the
// new clock is not there.
#define SWITCH_POLLS 20000U
#define OSCTUN_FALLBACK 0x3CU // TUN -4: the FRC 0.188% slow

static bool clock_fallback;

// Switches to nosc, waits for it, and for the PLL's lock. False if it did
// not complete: the switch abandoned, still on the FRC.
static bool clock_switch(uint8_t nosc)
{
    uint16_t polls;

    __builtin_write_OSCCONH(nosc);
    __builtin_write_OSCCONL((uint8_t)(OSCCON | 0x01U));
    for (polls = 0; OSCCONbits.OSWEN != 0U; polls++)
    {
        if (polls >= SWITCH_POLLS)
        {
            __builtin_write_OSCCONL((uint8_t)(OSCCON & 0xFEU)); // abandoned
            return false;
        }
    }
    for (polls = 0; OSCCONbits.LOCK != 1U; polls++)
    {
        if (polls >= SWITCH_POLLS)
        {
            return false;
        }
    }
    return true;
}


static void clock_init(void)
{
    // FRCDIV FRC/2; PLLPRE 8; DOZE 1:8; PLLPOST 1:4; DOZEN disabled; ROI disabled;
    CLKDIV = 0x3146;
    // TUN Center frequency;
    OSCTUN = 0x00;
    // ROON disabled; ROSEL FOSC; RODIV 0; ROSSLP disabled;
    REFOCON = 0x00;
    // PLLDIV 62;
    PLLFBD = 0x3E;
    // AD1MD enabled; T3MD enabled; T4MD enabled; T1MD enabled; U2MD enabled; T2MD enabled; U1MD enabled; SPI2MD enabled; SPI1MD enabled; C1MD enabled; T5MD enabled; I2C1MD enabled;
    PMD1 = 0x00;
    // IC4MD enabled; IC3MD enabled; OC1MD enabled; IC2MD enabled; OC2MD enabled; IC1MD enabled; OC3MD enabled; OC4MD enabled;
    PMD2 = 0x00;
    // CMPMD enabled; CRCMD enabled; I2C2MD enabled;
    PMD3 = 0x00;
    // CTMUMD enabled; REFOMD enabled;
    PMD4 = 0x00;
    // PTGMD enabled; DMA0MD enabled;
    PMD7 = 0x00;
    // EC 40 MHz, PLLPRE 8, PLLDIV 62 (M 64), PLLPOST 4: Fosc 80 MHz.
    if (clock_switch(NOSC_PRIPLL))
    {
        return;
    }
    // No external clock: the FRC (7.37 MHz), PLLPRE 2, PLLDIV 85 (M 87),
    // PLLPOST 4: Fosc 80.15 MHz, 0.19% fast, which OSCTUN -4 (-0.188%)
    // trims to 80.00 MHz, nominal; the FRC's own error remains.
    clock_fallback = true;
    OSCTUN = OSCTUN_FALLBACK;
    CLKDIV = 0x3140;
    PLLFBD = 0x55;
    (void)clock_switch(NOSC_FRCPLL);
}


bool system_clock_fallback(void)
{
    return clock_fallback;
}


void delay_ms(uint16_t milliseconds)
{
    while (milliseconds--)
    {
        __delay_ms(1);
    }
}


void delay_us(uint16_t microseconds)
{
    while (microseconds >= 32)
    {
        __delay_us(32);
        microseconds -= 32;
    }

    while (microseconds--)
    {
        __delay_us(1);
    }
}


uint16_t system_reset_cause(void)
{
    return reset_cause;
}


void system_reset(void)
{
    fault_reset_planned();
    __asm__ volatile("reset");
    while (1)
        ;
}
