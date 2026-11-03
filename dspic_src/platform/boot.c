#include "boot.h"

#include "../../common/boot_layout.h"

// The mailbox, at its fixed place, never initialised by the C start-up. On
// the host, an ordinary variable the tests set (boot_host_mailbox).
#ifdef __XC16__
static volatile BootMailbox mailbox __attribute__((persistent, address(MAILBOX_ADDRESS)));
#else
volatile BootMailbox boot_host_mailbox;
#define mailbox boot_host_mailbox
#endif

static bool valid;
static uint16_t healthy_s;

uint16_t boot_init(uint16_t rcon)
{
    valid = mailbox.magic == MAILBOX_MAGIC_VALUE &&
            mailbox.magic_inverse == (uint16_t)~MAILBOX_MAGIC_VALUE;
    healthy_s = 0;
    if (!valid)
    {
        return (uint16_t)(rcon & BOOT_RESET_CAUSES);
    }
    mailbox.request = 0;
    mailbox.running = MAILBOX_RUNNING_VALUE;
    // Anything RCON gathered since the IPL cleared it (nothing, normally).
    return (uint16_t)((mailbox.reset_cause | rcon) & BOOT_RESET_CAUSES);
}


bool boot_mailbox_valid(void)
{
    return valid;
}


uint8_t boot_path(void)
{
    return valid ? (uint8_t)mailbox.boot : 0U;
}


uint16_t boot_trials(void)
{
    return valid ? mailbox.trials : 0U;
}


uint16_t boot_attempts(void)
{
    return valid ? mailbox.attempts : 0U;
}


void boot_service(uint32_t uptime_s)
{
    healthy_s = uptime_s > 0xFFFFUL ? 0xFFFFU : (uint16_t)uptime_s;
    if (valid && healthy_s >= APP_HEALTHY_S)
    {
        mailbox.attempts = 0;
    }
}


uint16_t boot_healthy_s(void)
{
    return healthy_s;
}


void boot_request_spl(void)
{
    if (valid)
    {
        mailbox.request = MAILBOX_REQUEST_STAY;
    }
}
