/*
 * The application's side of the boot mailbox (platform/boot.c): the reset
 * cause from the mailbox or from RCON, a stale request cleared, running set,
 * the boot-loop guard's count zeroed after BOOT_HEALTHY_S, and the request
 * for the SPL.
 */
#include <stdio.h>
#include <string.h>

#include "../../../common/boot_layout.h"
#include "../../../common/sml_proto.h"
#include "../../platform/boot.h"

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

extern volatile BootMailbox boot_host_mailbox;

static void valid_mailbox(uint16_t cause)
{
    memset((void *)&boot_host_mailbox, 0, sizeof boot_host_mailbox);
    boot_host_mailbox.magic = MAILBOX_MAGIC_VALUE;
    boot_host_mailbox.magic_inverse = (uint16_t)~MAILBOX_MAGIC_VALUE;
    boot_host_mailbox.reset_cause = cause;
    boot_host_mailbox.boot = BOOT_PATH_APP_DIRECT | BOOT_PATH_NO_SPL;
    boot_host_mailbox.trials = 2;
    boot_host_mailbox.attempts = 2;
}


int main(void)
{
    // Without the IPL: random RAM, the cause from RCON, nothing written.
    memset((void *)&boot_host_mailbox, 0x5A, sizeof boot_host_mailbox);
    CHECK(boot_init(SML_RESET_POR | SML_RESET_BOR | 0x2000) == (SML_RESET_POR | SML_RESET_BOR));
    CHECK(!boot_mailbox_valid() && boot_path() == 0 && boot_trials() == 0 && boot_attempts() == 0);
    boot_request_spl();
    boot_service(1000);
    CHECK(boot_host_mailbox.request == 0x5A5A && boot_host_mailbox.attempts == 0x5A5A);

    // Behind the IPL: the cause it kept, RCON cleared by it.
    valid_mailbox(SML_RESET_WDTO);
    boot_host_mailbox.request = MAILBOX_REQUEST_STAY; // left by a reset the SPL never saw
    CHECK(boot_init(0) == SML_RESET_WDTO);
    CHECK(boot_mailbox_valid() && boot_path() == (BOOT_PATH_APP_DIRECT | BOOT_PATH_NO_SPL));
    CHECK(boot_host_mailbox.request == 0 && boot_host_mailbox.running == MAILBOX_RUNNING_VALUE);
    CHECK(boot_trials() == 2 && boot_attempts() == 2);

    // Healthy: the count left alone until BOOT_HEALTHY_S, then zeroed.
    boot_service(BOOT_HEALTHY_S - 1);
    CHECK(boot_host_mailbox.attempts == 2 && boot_healthy_s() == BOOT_HEALTHY_S - 1);
    boot_service(BOOT_HEALTHY_S);
    CHECK(boot_host_mailbox.attempts == 0);
    boot_service(100000UL);
    CHECK(boot_healthy_s() == 0xFFFF);
    CHECK(boot_host_mailbox.trials == 2); // not started by an unconfirmed SPL: left alone

    // Started by an unconfirmed SPL: healthy, its trials zeroed too, so
    // ordinary resets before it is confirmed do not spend them.
    valid_mailbox(SML_RESET_SWR);
    boot_host_mailbox.boot = BOOT_PATH_UNCONFIRMED;
    (void)boot_init(0);
    boot_service(BOOT_HEALTHY_S - 1);
    CHECK(boot_host_mailbox.trials == 2);
    boot_service(BOOT_HEALTHY_S);
    CHECK(boot_host_mailbox.trials == 0 && boot_host_mailbox.attempts == 0);

    // ENTER_BOOTLOADER's request.
    boot_request_spl();
    CHECK(boot_host_mailbox.request == MAILBOX_REQUEST_STAY);

    // A cause gathered since the IPL cleared RCON is added; other RCON bits are not.
    valid_mailbox(SML_RESET_SWR);
    CHECK(boot_init(SML_RESET_EXTR | 0x2000) == (SML_RESET_SWR | SML_RESET_EXTR));

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
