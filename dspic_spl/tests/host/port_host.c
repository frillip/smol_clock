#include "port_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/boot_crc.h"
#include "../../spl_port.h"

uint32_t host_flash[HOST_FLASH_WORDS];
uint32_t host_ms;
uint8_t host_tx[1 << 16];
uint32_t host_tx_length;
long host_erases;
long host_writes;
long host_overwrites;
long host_refused;
long host_services;
bool host_led;
int host_led_changes;
long host_fail_at = HOST_NEVER;
long host_corrupt_at = HOST_NEVER;
long host_dirty_at = HOST_NEVER;
long host_cut_at = HOST_NEVER;
int host_cut_mode;
jmp_buf *host_cut_jump;

static uint8_t rx[1 << 16];
static uint32_t rx_head;
static uint32_t rx_tail;
static uint32_t random_state = 12345U;

static uint32_t random_bits(void)
{
    random_state = random_state * 1103515245U + 12345U;
    return random_state >> 8 & HOST_ERASED;
}


void host_reset_faults(void)
{
    host_fail_at = HOST_NEVER;
    host_corrupt_at = HOST_NEVER;
    host_dirty_at = HOST_NEVER;
    host_cut_at = HOST_NEVER;
    host_cut_mode = 0;
}


void host_reset(void)
{
    uint32_t index;

    for (index = 0; index < HOST_FLASH_WORDS; index++)
    {
        host_flash[index] = HOST_ERASED;
    }
    host_ms = 0;
    host_tx_length = 0;
    rx_head = rx_tail = 0;
    host_erases = host_writes = host_overwrites = host_refused = host_services = 0;
    host_led = false;
    host_led_changes = 0;
    host_reset_faults();
}


void host_reset_link(void)
{
    host_tx_length = 0;
    rx_head = rx_tail = 0;
}


long host_operations(void)
{
    return host_erases + host_writes;
}


void host_put(uint32_t address, uint32_t word)
{
    host_flash[address / 2U] = word & HOST_ERASED;
}


uint32_t host_get(uint32_t address)
{
    return host_flash[address / 2U];
}


void host_rx_push(const uint8_t *data, uint32_t length)
{
    while (length-- != 0U)
    {
        rx[rx_head++ & 0xFFFFU] = *data++;
    }
}


uint32_t host_rx_pending(void)
{
    return rx_head - rx_tail;
}


// ---- spl_port.h --------------------------------------------------------------------

uint32_t spl_port_ms(void)
{
    return host_ms;
}


void spl_port_service(void)
{
    host_services++;
}


bool spl_port_rx(uint8_t *byte)
{
    if (rx_tail == rx_head)
    {
        return false;
    }
    *byte = rx[rx_tail++ & 0xFFFFU];
    return true;
}


void spl_port_tx(const uint8_t *data, uint16_t length)
{
    if (host_tx_length + length > sizeof(host_tx))
    {
        host_tx_length = 0; // a test that never reads: start again
    }
    memcpy(host_tx + host_tx_length, data, length);
    host_tx_length += length;
}


uint32_t spl_port_flash_read(uint32_t address)
{
    if (address == 0xFF0000UL)
    {
        return 0x001F6CUL; // DEVID
    }
    if ((address & 1UL) != 0UL || address / 2U >= HOST_FLASH_WORDS)
    {
        printf("  FAIL: read at 0x%06lX\n", (unsigned long)address);
        exit(1);
    }
    return host_flash[address / 2U];
}


// The operation about to run: true if the power fails during it, after the
// partial effect has been applied by the caller.
static bool cut_now(long operation)
{
    return operation == host_cut_at && host_cut_jump != NULL;
}


bool spl_port_flash_erase(uint32_t address)
{
    long operation = host_operations();
    uint32_t index;

    if (!spl_may_erase(address))
    {
        host_refused++;
        return false;
    }
    host_erases++;
    if (operation == host_fail_at)
    {
        return false;
    }
    if (cut_now(operation))
    {
        // Part-erased: bits only go from 0 to 1, some of them.
        for (index = 0; index < BOOT_PAGE_INSTRUCTIONS && host_cut_mode != 1; index++)
        {
            host_flash[address / 2U + index] |=
                host_cut_mode == 2 ? HOST_ERASED : random_bits() & random_bits();
        }
        longjmp(*host_cut_jump, 1);
    }
    for (index = 0; index < BOOT_PAGE_INSTRUCTIONS; index++)
    {
        host_flash[address / 2U + index] = HOST_ERASED;
    }
    if (operation == host_dirty_at)
    {
        host_flash[address / 2U + 17U] = 0x123456UL;
    }
    return true;
}


static void program(uint32_t address, uint32_t word, bool partial)
{
    uint32_t *cell = &host_flash[address / 2U];

    if (*cell != HOST_ERASED)
    {
        host_overwrites++;
    }
    // Programming only clears bits; a partial write clears some of them.
    *cell &= partial ? (word | random_bits()) : word;
}


bool spl_port_flash_write(uint32_t address, uint32_t first, uint32_t second)
{
    long operation = host_operations();

    if (!spl_may_write(address))
    {
        host_refused++;
        return false;
    }
    host_writes++;
    if (operation == host_fail_at)
    {
        return false;
    }
    if (cut_now(operation))
    {
        if (host_cut_mode != 1)
        {
            program(address, first, host_cut_mode == 0);
            program(address + 2U, second, host_cut_mode == 0);
        }
        longjmp(*host_cut_jump, 1);
    }
    if (operation == host_corrupt_at)
    {
        first ^= 0x000100UL;
    }
    program(address, first & HOST_ERASED, false);
    program(address + 2U, second & HOST_ERASED, false);
    return true;
}


uint32_t spl_port_crc32_flash(uint32_t crc, uint32_t address, uint32_t instructions)
{
    while (instructions-- != 0U)
    {
        uint32_t word = spl_port_flash_read(address);
        uint8_t bytes[3] = {(uint8_t)word, (uint8_t)(word >> 8), (uint8_t)(word >> 16)};

        crc = boot_crc32_update(crc, bytes, 3U);
        address += 2U;
    }
    return crc;
}


void spl_port_led(bool on)
{
    if (on != host_led)
    {
        host_led_changes++;
    }
    host_led = on;
}
