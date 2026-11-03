#include "system.h"

#include "../../common/boot_layout.h"
#include "boot.h"

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
    board_init();
    clock_init();
    // Interrupt priorities are set by the module that owns each interrupt:
    // console_init() for UART1, timing/ for the timers and captures.
    __builtin_enable_interrupts();
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
    // CF no clock failure; NOSC PRIPLL; CLKLOCK unlocked; OSWEN Switch is Complete;
    __builtin_write_OSCCONH((uint8_t)(0x03));
    __builtin_write_OSCCONL((uint8_t)(0x01));
    // Wait for Clock switch to occur
    while (OSCCONbits.OSWEN != 0)
        ;
    while (OSCCONbits.LOCK != 1)
        ;
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
    __asm__ volatile("reset");
    while (1)
        ;
}
