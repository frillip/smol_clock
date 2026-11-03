/*
 * The IPL: page 0 of the V2+ dsPIC, written once with a programmer and never
 * again. Every reset starts here. It prepares the RAM mailbox, chooses what
 * to start (ipl_select.c) and jumps to it, leaving the chip as reset left it:
 * FRC, no peripheral touched, interrupts off. It never writes flash, never
 * uses the UART and never enables interrupts.
 *
 * The rest of page 0 is fixed around it (ipl.gld, ipl_vectors.s): the reset
 * vector, the IVT pointing every interrupt at the application's forwarding
 * table and the traps at the stubs, the GOTO to ipl_crc32_flash at
 * IPL_SERVICES, and the info block at IPL_INFO_ADDRESS (ipl_info.c).
 *
 * C start-up: XC16's own, linked without data initialisation. Nothing here
 * has an initial value or relies on being zeroed, but the mailbox, which is
 * persistent and at its fixed place, so the stack is kept below it.
 */
#include <stdint.h>
#include <xc.h>

#include "../common/boot_layout.h"
#include "ipl_select.h"

#define FCY 3685000UL // FRC 7.37 MHz, 2 clocks an instruction: as reset leaves it
#include <libpic30.h>

static volatile BootMailbox mailbox __attribute__((persistent, address(MAILBOX_ADDRESS)));

// The jumps, as the GOTOs they must be: no call frame left behind, as
// neither comes back. The addresses are SPL_START and APP_START.
#define JUMP_TO_SPL() __asm__ volatile("goto 0x000800")
#define JUMP_TO_APP() __asm__ volatile("goto 0x002000")
typedef char jump_spl_check[(SPL_START == 0x000800UL) ? 1 : -1];
typedef char jump_app_check[(APP_START == 0x002000UL) ? 1 : -1];

// Nothing to start: only a programmer helps. The status LED (RA10) blinks
// 1 s on, 1 s off, for ever; nothing else is touched.
static void blink_for_ever(void)
{
    _TRISA10 = 0;
    for (;;)
    {
        _LATA10 ^= 1;
        __delay_ms(1000);
    }
}


int main(void)
{
    uint16_t rcon = RCON;
    BootMailbox copy;
    IplChoice choice;

    mailbox.running = 0; // the traps reset, rather than jump into the application
    RCON = (uint16_t)(rcon & ~BOOT_RESET_CAUSES);

    // Worked on as a copy: the selection takes a plain pointer.
    copy = *(BootMailbox *)&mailbox;
    ipl_mailbox_prepare(&copy, rcon);
    choice = ipl_select(&copy);
    *(BootMailbox *)&mailbox = copy;

    if (choice.watchdog)
    {
        ClrWdt();
        RCONbits.SWDTEN = 1;
    }
    if (choice.start == IPL_START_SPL)
    {
        JUMP_TO_SPL();
    }
    if (choice.start == IPL_START_APP)
    {
        JUMP_TO_APP();
    }
    blink_for_ever();
    return 0;
}
