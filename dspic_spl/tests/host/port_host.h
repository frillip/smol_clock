#ifndef PORT_HOST_H
#define PORT_HOST_H

/*
 * The SPL's port (spl_port.h) on the host: a model of program memory with
 * the flash's rules, a byte queue for each direction, a clock the tests
 * move, and faults to inject: a failed erase or write (WRERR), a write that
 * reads back wrong, an erase that leaves a word, and a power cut part-way
 * through any operation.
 */

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>

#define HOST_FLASH_WORDS (0x02B000UL / 2)
#define HOST_ERASED      0xFFFFFFUL
#define HOST_NEVER       (-1L)

extern uint32_t host_flash[HOST_FLASH_WORDS];
extern uint32_t host_ms;

// Bytes from the SPL, appended; the tests read and clear them.
extern uint8_t host_tx[1 << 16];
extern uint32_t host_tx_length;

// Counters, and the flash rules broken (a write over a word not erased).
extern long host_erases;
extern long host_writes;
extern long host_overwrites;
extern long host_refused; // calls the guard refused
extern long host_services;
extern bool host_led;
extern int host_led_changes;

// Faults, by operation number (erases and writes counted together from
// host_reset(), the first 0), or HOST_NEVER.
extern long host_fail_at;    // reports WRERR, changes nothing
extern long host_corrupt_at; // a write lands with a bit wrong
extern long host_dirty_at;   // an erase leaves one word programmed
extern long host_cut_at;     // the power fails during it: partial, then longjmp
extern int host_cut_mode;    // 0: part done, at random; 1: not started; 2: complete
extern jmp_buf *host_cut_jump;

void host_reset(void); // flash erased, queues empty, counters and faults cleared
void host_reset_faults(void);
void host_reset_link(void); // both queues emptied: a restart, the flash kept
long host_operations(void);
void host_rx_push(const uint8_t *data, uint32_t length);
uint32_t host_rx_pending(void);
void host_put(uint32_t address, uint32_t word);
uint32_t host_get(uint32_t address);

#endif // PORT_HOST_H
