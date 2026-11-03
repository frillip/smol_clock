/*
 * The V2+ dsPIC bootloader's layout: flash regions, records, the mailbox and
 * the limits the IPL, the SPL and the application share. 
 * One copy for all three stages and the host tools.
 * The wire messages are in sml_proto.h (class 0xF0 BOOT).
 *
 * Program addresses are in program address units: 2 per 24-bit instruction.
 * The definitions up to the C section also serve assembly (.S): no casts,
 * no suffixes there, hence BOOT_UL().
 *
 * Records (the application's version record, the SPL record, the test
 * record) hold 16 data bits per instruction, upper byte 0xFF, written a
 * double word (2 instructions) at a time, each double word once. A 32-bit
 * field takes two words, the high half first. record_crc is CRC-16/IBM-3740
 * (sml_crc) over words 0-13, each as 2 bytes big-endian (boot_record_crc()).
 */
#ifndef BOOT_LAYOUT_H
#define BOOT_LAYOUT_H

#ifdef __ASSEMBLER__
#define BOOT_UL(value) value
#else
#define BOOT_UL(value) value##UL
#endif

// ---- Flash --------------------------------------------------------------------
#define BOOT_PAGE_ADDRESSES     BOOT_UL(0x800) // an erase page: 1024 instructions
#define BOOT_PAGE_INSTRUCTIONS  BOOT_UL(1024)
#define BOOT_CHUNK_ADDRESSES    BOOT_UL(0x80) // a BOOT-DATA chunk: 64 instructions
#define BOOT_CHUNK_INSTRUCTIONS BOOT_UL(64)
#define BOOT_CHUNK_BYTES        BOOT_UL(192)  // packed, 3 bytes an instruction
#define BOOT_ROW_ADDRESSES      BOOT_UL(0x80) // a record row: 64 instructions

// IPL: page 0, written only by a programmer.
#define IPL_START            BOOT_UL(0x000000)
#define IPL_SERVICES         BOOT_UL(0x000200) // GOTO ipl_crc32_flash
#define IPL_CODE_END         BOOT_UL(0x000780)
#define IPL_INFO_ADDRESS     BOOT_UL(0x000780) // the info block, a row
#define IPL_END              BOOT_UL(0x000800)
#define IPL_SERVICES_VERSION 1

// SPL: pages 1-3, written only by the application.
#define SPL_START          BOOT_UL(0x000800) // its entry
#define SPL_CODE_MAX       BOOT_UL(3008)     // instructions
#define SPL_RECORD_ADDRESS BOOT_UL(0x001F80)
#define SPL_END            BOOT_UL(0x002000)
#define SPL_PAGES          3

// Application: written only by the SPL.
#define APP_START          BOOT_UL(0x002000) // GOTO __reset
#define APP_FORWARD_TABLE  BOOT_UL(0x002004) // IVT entry A -> APP_FORWARD_TABLE + 2 * (A - 4)
#define APP_VECTORS        254 // IVT entries 0x0004-0x01FE, a GOTO (2 instructions) each
#define APP_END            BOOT_UL(0x029000) // exclusive
#define APP_MAX            BOOT_UL(79872)    // instructions
#define APP_PAGES          78
#define APP_RECORD_ADDRESS BOOT_UL(0x029000) // its own page, erased first

// The SPL's test page: the application region's last page, while free.
#define TEST_START          BOOT_UL(0x028800)
#define TEST_RECORD_ADDRESS BOOT_UL(0x028F80)
#define TEST_CODE_MAX       BOOT_UL(960) // instructions

// Above the application, unchanged by any update.
#define STORE_START       BOOT_UL(0x029800) // settings store, 2 pages (drivers/flash.h)
#define CONFIG_PAGE       BOOT_UL(0x02A800)
#define CONFIG_WORDS      BOOT_UL(0x02AFEC) // written only by a programmer, with the IPL
#define BOOT_VERIFY_LIMIT BOOT_UL(0x029800) // BOOT-VERIFY reads below this

// ---- Records --------------------------------------------------------------------
// Word indices; a record is BOOT_ROW_ADDRESSES long.
#define RECORD_MAGIC     0
#define RECORD_FORMAT    1
#define RECORD_CRC       14 // over words 0-13
#define RECORD_SPARE     15 // 0xFFFF
#define RECORD_COMMIT    16 // 16-17: COMMIT_0, COMMIT_1, written last, alone
#define RECORD_CRC_WORDS 14

#define RECORD_FORMAT_1 1
#define RECORD_COMMIT_0 0xA55A
#define RECORD_COMMIT_1 0x5AA5

// Application version record (and the test record, in the same layout).
#define APP_RECORD_MAGIC_VALUE 0x534D // "SM"
#define APP_RECORD_VERSION     2      // 2-3: major << 16 | minor << 8 | patch
#define APP_RECORD_START       4      // 4-5: APP_START
#define APP_RECORD_LENGTH      6      // 6-7: instructions, even
#define APP_RECORD_CRC32       8      // 8-9: CRC-32 of the packed image
#define APP_RECORD_BUILD_TIME  10     // 10-11: Unix seconds
#define APP_RECORD_SML_VERSION 12     // major << 8 | minor
#define APP_RECORD_WORDS       18

// SPL record.
#define SPL_RECORD_MAGIC_VALUE 0x5350 // "SP"
#define SPL_RECORD_VERSION     2      // 2-3
#define SPL_RECORD_LENGTH      4      // 4-5: instructions from SPL_START, even
#define SPL_RECORD_CRC32       6      // 6-7
#define SPL_RECORD_BUILD_TIME  8      // 8-9
#define SPL_RECORD_PROTOCOL    10     // the bootloader protocol it speaks
#define SPL_RECORD_CONFIRM     18     // 18-19: CONFIRM_0, CONFIRM_1, by the application
#define SPL_RECORD_REJECT      20     // 20-21: any programmed bit rejects
#define SPL_RECORD_WORDS       22
#define SPL_CONFIRM_0          0xC33C
#define SPL_CONFIRM_1          0x3CC3
#define SPL_REJECT_VALUE       0x0000

// IPL info block, in the same word layout: word 1 RECORD_FORMAT_1, word 5 0.
#define IPL_INFO_MAGIC_VALUE 0x4950 // "IP"
#define IPL_INFO_VERSION     2      // 2-3: major << 16 | minor << 8 | patch
#define IPL_INFO_SERVICES    4      // IPL_SERVICES_VERSION
#define IPL_INFO_BUILD_TIME  6      // 6-7
#define IPL_INFO_CRC         8      // CRC-16/IBM-3740 of 0x000000-0x00077F, packed
#define IPL_INFO_WORDS       9

// ---- Mailbox (RAM) ------------------------------------------------------------
/* Eight words below 0x8000, kept over every reset but power-on and
 * brown-out; no stage's linker script allocates them. Above 0x8000 the
 * application's plain reads go through DSRPAG, which its C start-up points
 * at program memory for constants (PSV), so it is not up there. */
#define MAILBOX_ADDRESS       0x7FF0
#define MAILBOX_WORDS         8
#define MAILBOX_MAGIC         0 // word indices
#define MAILBOX_MAGIC_INVERSE 1
#define MAILBOX_REQUEST       2
#define MAILBOX_RUNNING       3
#define MAILBOX_ATTEMPTS      4
#define MAILBOX_BOOT          5
#define MAILBOX_TRIALS        6
#define MAILBOX_RESET_CAUSE   7 // RCON's cause bits as the IPL found them, before clearing them

#define MAILBOX_MAGIC_VALUE   0x4D42
#define MAILBOX_REQUEST_STAY  0xB007 // ENTER_BOOTLOADER: the SPL stays (HELD)
#define MAILBOX_RUNNING_VALUE 0xA99A // the application is running: traps go to it

// MAILBOX_BOOT bits: how this start came about (BOOT-STATUS boot,
// SML_BOOT_PATH_* in sml_proto.h, the same bits).
#define BOOT_PATH_APP_DIRECT   0x01 // the IPL started the application itself
#define BOOT_PATH_UNCONFIRMED  0x02 // an unconfirmed SPL was started
#define BOOT_PATH_NO_SPL       0x04 // the SPL is not intact
#define BOOT_PATH_SPL_GIVEN_UP 0x08 // the SPL is rejected or out of trials
#define BOOT_PATH_LAST_RESORT  0x10 // the SPL started anyway: no application

// ---- Limits -------------------------------------------------------------------
#define SPL_TRIAL_LIMIT  3    // starts of an unconfirmed SPL before the application
#define BOOT_LOOP_LIMIT  3    // hand-overs without the application reporting healthy
#define APP_HEALTHY_S    60   // healthy running that zeroes attempts, allows an SPL update
#define BOOT_WINDOW_MS   1000 // the SPL's boot window
#define BOOT_WATCHDOG_MS 4096 // PR128, PS1024 on the 32 kHz LPRC; 3905 ms measured

/* RCON's reset-cause bits (TRAPR IOPUWR CM EXTR SWR WDTO BOR POR, as
 * SML_RESET_*). The IPL takes them into MAILBOX_RESET_CAUSE and clears them,
 * so the next reset's are its own: a power-on's POR must not linger into a
 * later watchdog reset, or the mailbox would be cleared again. The SPL and
 * the application read the cause from the mailbox. */
#define BOOT_RESET_CAUSES 0xC2D3
#define BOOT_RESET_POWER  0x0003 // POR, BOR: the mailbox starts again

#ifndef __ASSEMBLER__

#include <stdint.h>

// The mailbox, at MAILBOX_ADDRESS.
typedef struct
{
    uint16_t magic;
    uint16_t magic_inverse;
    uint16_t request;
    uint16_t running;
    uint16_t attempts;
    uint16_t boot;
    uint16_t trials;
    uint16_t reset_cause;
} BootMailbox;

/* The IPL's CRC-32 service, at IPL_SERVICES (a GOTO to it). The SPL's and
 * the application's linker scripts define the symbol there
 * (_ipl_crc32_flash = 0x000200). Continues crc over instructions words of
 * flash from address, 3 bytes each, least significant first. Start with
 * BOOT_CRC32_INIT; complement the result. The XC16 calling convention;
 * TBLPAG is restored. Not reentrant with an interrupt that changes TBLPAG. */
uint32_t ipl_crc32_flash(uint32_t crc, uint32_t address, uint32_t instructions);

#endif // __ASSEMBLER__

#endif // BOOT_LAYOUT_H
