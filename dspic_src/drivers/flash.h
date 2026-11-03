/*
 * Program flash, for the settings store (settings/store.h): erasing a page,
 * writing two instruction words at a time, reading them back.
 *
 * Only the store's own pages can be written. They are reserved here, at a
 * fixed address apart from the application:
 *
 *   0x000000 - 0x0297FF  application (and the bootloader, and the
 *                        application's version and CRC trailer, to come)
 *   0x029800 - 0x02A7FF  the store: FLASH_STORE_PAGES erase pages
 *   0x02A800 - 0x02AFFF  the last page: the configuration words from
 *                        0x02AFEC, and nothing else
 *
 * The last page is reserved too, up to the configuration words, so no
 * application code lands in it. Rewriting anything in it means erasing it,
 * configuration words and all; a reset before they were written again
 * would start the chip on the erased defaults (watchdog on, another
 * oscillator), which a bootloader could not recover from. With nothing
 * else there, an update never needs to erase it: the configuration words
 * are programmed once, with the bootloader.
 *
 * The reservation (flash.c) is placed with address() and marked noload, so
 * the linker puts nothing else there and the application's hex holds
 * nothing for it: programming a new application, by the debugger with its
 * "preserve program memory" range set (nbproject) or by a bootloader that
 * skips the range, leaves the settings as they were.
 *
 * Each instruction word holds 16 bits of data in its low word, as constant
 * data does; the upper byte is written 0xFF. An erase page is 1024
 * instructions (0x800 program addresses, 2 KB of data); a double word is
 * two instructions (4 program addresses, 4 bytes of data). A double word
 * can be written once between erases: writing it again is not allowed.
 *
 * Erasing and writing stop the CPU until the operation is done: the flash
 * cannot be read while it is busy. Interrupts are held off for the whole of
 * it, about 20-30 ms for a page erase and some 50 us for a double word, and
 * the resume hook (flash_set_resume_hook) runs before they are let back on,
 * to put right whatever counts interrupts (epoch_timer_resume). Hardware
 * keeps running throughout: the timers, the captures and the epoch output
 * are not disturbed, but UART bytes arriving while the FIFOs are full are
 * lost. Callers choose a quiet moment (settings/settings.h).
 */
#ifndef FLASH_H
#define FLASH_H

#include <stdbool.h>
#include <stdint.h>

#define FLASH_PAGE_INSTRUCTIONS 1024U
#define FLASH_PAGE_ADDRESSES    0x800UL                 // program addresses per erase page
#define FLASH_PAGE_WORDS        FLASH_PAGE_INSTRUCTIONS // 16-bit data words
#define FLASH_DOUBLE_ADDRESSES  4UL

#define FLASH_STORE_BASE  0x029800UL
#define FLASH_STORE_PAGES 2U
#define FLASH_STORE_END   (FLASH_STORE_BASE + FLASH_STORE_PAGES * FLASH_PAGE_ADDRESSES)

// The last page, and where its configuration words start.
#define FLASH_CONFIG_PAGE  0x02A800UL
#define FLASH_CONFIG_WORDS 0x02AFECUL

#define FLASH_ADDRESS_MASK 0x00FFFFFFUL // a program address: 24 bits

typedef enum
{
    FLASH_OK = 0,
    FLASH_BAD_ADDRESS,  // outside the store, or not aligned
    FLASH_NOT_RESERVED, // the reservation is not where it should be
    FLASH_FAILED,       // the controller reported an error (WRERR)
} FlashResult;

typedef struct
{
    uint32_t erases;
    uint32_t writes; // double words
    uint32_t failures;
    // Measured with the clock given to flash_set_clock(), ticks.
    uint32_t last_erase_ticks;
    uint32_t longest_erase_ticks;
    uint32_t longest_write_ticks;
} FlashStats;

// Checks that the reservations sit where they should; nothing is written
// if not.
bool flash_init(void);

// The data word (low 16 bits) of the instruction at an even address.
uint16_t flash_read_word(uint32_t address);

// Erases the page starting at address (a store page).
FlashResult flash_erase_page(uint32_t address);

// Writes two data words to the two instructions from address (a multiple
// of 4, in the store, erased since it was last written).
FlashResult flash_write_double_word(uint32_t address, uint16_t first, uint16_t second);

// The SPL's pages (0x000800-0x001FFF, common/boot_layout.h), for the
// application's SPL writer (esp/spl_writer.h), and nothing else: the guard
// is the first thing each checks. Whole instruction words, 24 bits.
uint32_t flash_read_instruction(uint32_t address);
FlashResult flash_spl_erase_page(uint32_t address);
FlashResult flash_spl_write(uint32_t address, uint32_t first, uint32_t second);

// A free-running clock to time the operations with (pps_capture_timebase);
// NULL: not timed.
void flash_set_clock(uint32_t (*clock)(void));
// Runs with interrupts still held off, after each operation.
void flash_set_resume_hook(void (*hook)(void));

const FlashStats *flash_stats(void);
const char *flash_result_name(FlashResult result);

#endif // FLASH_H
