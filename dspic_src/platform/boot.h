#ifndef BOOT_H
#define BOOT_H

/*
 * The application's side of the boot mailbox (common/boot_layout.h): the
 * eight RAM words at MAILBOX_ADDRESS the IPL and the SPL leave for it, kept
 * over every reset but power-on and brown-out.
 *
 * The IPL clears RCON's reset causes and keeps them in the mailbox, so the
 * application takes the cause from there. Started without the IPL (a debug
 * session, or a chip programmed with the application alone), the mailbox's
 * magic is wrong and RCON is used, as before the bootloader.
 */

#include <stdbool.h>
#include <stdint.h>

// At start-up, with RCON as found: the reset cause (SML_RESET_* bits), from
// the mailbox if it is valid, else from rcon. A request for the SPL left in
// it (by an ENTER_BOOTLOADER whose reset the SPL never saw) is cleared.
uint16_t boot_init(uint16_t rcon);

// True if the mailbox was valid at start-up: the IPL (or SPL) started us.
bool boot_mailbox_valid(void);

// How the IPL started this run (BOOT_PATH_*), the trials an unconfirmed SPL
// has used, and the hand-overs since the application was last healthy. All
// 0 without a valid mailbox.
uint8_t boot_path(void);
uint16_t boot_trials(void);
uint16_t boot_attempts(void);

// Every second from the main loop, with the seconds since start: once the
// application has run for APP_HEALTHY_S, the boot-loop guard's count is
// zeroed. healthy_s: the seconds counted, saturating.
void boot_service(uint32_t uptime_s);
uint16_t boot_healthy_s(void);

// Leave a request for the SPL to stay (HELD) at the next start. The caller
// resets once its answer is sent.
void boot_request_spl(void);

#endif // BOOT_H
