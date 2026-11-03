#ifndef IPL_SELECT_H
#define IPL_SELECT_H

/*
 * What the IPL starts (see README.md), with no hardware
 * but flash reads (ipl_flash.h), so the host tests run it as it is.
 *
 *   1  the SPL, if intact, not rejected, and trials < SPL_TRIAL_LIMIT; an
 *      unconfirmed one uses a trial. For a confirmed one, trials counts its
 *      hangs in a row (watchdog resets while it ran, SPL_HUNG), zeroed by
 *      any other reset. Every SPL start runs under the watchdog
 *   2  the application, if its record is valid and its image intact
 *      (APP_DIRECT, with NO_SPL or SPL_GIVEN_UP)
 *   3  the SPL, if intact, ignoring trials and reject (LAST_RESORT)
 *   4  nothing
 *
 * Intact: the record's magic, format, length and record_crc right, commit
 * exact, and the CRC-32 over the image matching. Confirmed: confirm exact.
 * Rejected: any bit of reject programmed.
 */

#include <stdbool.h>
#include <stdint.h>

#include "../common/boot_layout.h"

typedef enum
{
    IPL_START_NONE,
    IPL_START_SPL,
    IPL_START_APP,
} IplStart;

typedef struct
{
    IplStart start;
    bool watchdog; // enable it first: every SPL start
} IplChoice;

// The mailbox for this start, from RCON as found: started again on
// power-on or brown-out, or when its magic is not right; then the cause
// recorded, running cleared, and boot cleared but for SPL_HUNG (a watchdog
// reset while the application was not running).
void ipl_mailbox_prepare(BootMailbox *mailbox, uint16_t rcon);

// The choice, with the mailbox's boot, trials and running set for it.
IplChoice ipl_select(BootMailbox *mailbox);

#endif // IPL_SELECT_H
