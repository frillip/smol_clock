#ifndef SPL_CORE_H
#define SPL_CORE_H

/*
 * The SPL's logic: the start-up modes, the boot window and the boot-loop
 * guard, and the bootloader's side of SML class 0xF0 (BOOT-STATUS,
 * BOOT-COMMAND, BOOT-DATA, BOOT-VERIFY) for regions APP and TEST. Portable:
 * everything it needs of the chip goes through spl_port.h, so the host
 * tests run it against a flash model.
 *
 * The ESP owns every timeout and retry. The SPL only answers, one request at
 * a time, and never writes anything but the application region, its record
 * page and the test page (spl_may_erase(), spl_may_write()).
 */

#include <stdint.h>

#include "../common/boot_layout.h"

typedef enum
{
    SPL_ACTION_NONE,      // carry on: call spl_poll() again
    SPL_ACTION_HAND_OVER, // start the application (spl_port.c, main.c)
    SPL_ACTION_RESET,     // a software reset, once the answer has gone
} SplAction;

/* At entry: reads the IPL's info block, the SPL's own record and the
 * application's, chooses the mode, sends the first BOOT-STATUS and then
 * checks the application's CRC-32. The mailbox is the IPL's, at
 * MAILBOX_ADDRESS on the chip: request is cleared here. */
void spl_init(volatile BootMailbox *mailbox);

/* One pass of the main loop: received bytes, at most one request answered,
 * the boot window, the periodic BOOT-STATUS, the LED. For HAND_OVER the
 * mailbox's attempts has been counted, and for both the answer has been
 * sent (still to be flushed). */
SplAction spl_poll(void);

#endif // SPL_CORE_H
