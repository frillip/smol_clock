#ifndef SPL_PORT_H
#define SPL_PORT_H

/*
 * What the SPL's core (spl_core.c) needs of the chip: time, the UART, flash,
 * the LED. spl_port.c on the dsPIC; tests/host/port_host.c on the host, with
 * a flash model, so the core runs unchanged in the host tests.
 *
 * Nothing here uses interrupts. spl_port_service() is the one thing every
 * loop that runs for more than a few tens of microseconds must call: it
 * clears the watchdog and moves received bytes out of the UART's 4-byte
 * FIFO into a ring, so nothing is lost while the core works.
 */

#include <stdbool.h>
#include <stdint.h>

#include "../common/boot_layout.h"

// Milliseconds since start, wrapping at 2^32.
uint32_t spl_port_ms(void);

// The watchdog cleared, the UART's receive FIFO emptied into the ring.
void spl_port_service(void);

// A received byte from the ring. False if none.
bool spl_port_rx(uint8_t *byte);

// Sends length bytes, servicing while it waits for room. Returns once the
// last byte is in the transmit FIFO.
void spl_port_tx(const uint8_t *data, uint16_t length);

// The 24-bit instruction word at a program address (even; 0xFF0000 for DEVID).
uint32_t spl_port_flash_read(uint32_t address);

// Erases the page at address. False if refused by the guard below, or if
// the chip reported a failure (WRERR).
bool spl_port_flash_erase(uint32_t address);

// Writes two 24-bit instruction words at address (a multiple of 4). False
// if refused by the guard, or on WRERR.
bool spl_port_flash_write(uint32_t address, uint32_t first, uint32_t second);

// Continues a CRC-32 over instructions words of flash from address: the
// IPL's service (ipl_crc32_flash) on the chip.
uint32_t spl_port_crc32_flash(uint32_t crc, uint32_t address, uint32_t instructions);

void spl_port_led(bool on);

// ---- The chip only (main.c) -------------------------------------------------------

// Clock to 40 MIPS, the watchdog on, UART1, the LED pin, the timer.
void spl_port_init(void);

// Waits for the last byte to go, puts back every peripheral and pin the SPL
// used as reset left them, turns the watchdog off, returns to FRC, sets the
// mailbox's running and jumps to the application. Does not return.
void spl_port_hand_over(volatile BootMailbox *mailbox);

// Waits for the last byte to go, then a software reset. Does not return.
void spl_port_reset(void);

// ---- The flash guard ------------------------------------------------------------
// The first thing each flash routine checks, on the chip and in the host
// model alike: the SPL writes the application region, its record page and
// nothing else. Never page 0, its own pages, the settings store or the
// configuration page.

static inline bool spl_may_erase(uint32_t address)
{
    return address >= APP_START && address <= APP_RECORD_ADDRESS &&
           (address & (BOOT_PAGE_ADDRESSES - 1UL)) == 0UL;
}

static inline bool spl_may_write(uint32_t address)
{
    return address >= APP_START && address < APP_RECORD_ADDRESS + BOOT_PAGE_ADDRESSES &&
           (address & 3UL) == 0UL;
}

#endif // SPL_PORT_H
