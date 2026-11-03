/*
 * The SPL: pages 1-3 of the V2+ dsPIC (common/boot_layout.h), started by the
 * IPL at SPL_START. It checks and starts the application after a 1 s boot
 * window, and is the only thing that writes the application region, over
 * SML class 0xF0 from the ESP (spl_core.c). The application writes the SPL.
 *
 * Linked by spl.gld: code from 0x000800 to its record row at 0x001F80, the
 * entry GOTO first, no IVT and no configuration words (the IPL's). No
 * interrupts: everything is polled (spl_port.c).
 */
#include <stdint.h>

#include "../common/boot_layout.h"
#include "spl_core.h"
#include "spl_port.h"

static volatile BootMailbox mailbox __attribute__((persistent, address(MAILBOX_ADDRESS)));

int main(void)
{
    spl_port_init();
    spl_init(&mailbox);
    for (;;)
    {
        SplAction action = spl_poll();

        if (action == SPL_ACTION_HAND_OVER)
        {
            spl_port_hand_over(&mailbox);
        }
        if (action == SPL_ACTION_RESET)
        {
            spl_port_reset();
        }
    }
    return 0;
}
