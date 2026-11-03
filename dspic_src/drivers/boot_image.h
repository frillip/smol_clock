#ifndef BOOT_IMAGE_H
#define BOOT_IMAGE_H

/*
 * What flash holds of the boot stages (common/boot_layout.h): the IPL's info
 * block, the SPL's record, the application's own record. Read-only: the
 * application writes the SPL in a later phase, never its own record.
 *
 * Image CRC-32s go through the IPL's service at IPL_SERVICES, and only when
 * the IPL's info block is there: without the IPL, 0x000200 is not code.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    bool present; // the info block's magic and format
    uint32_t version;
    uint16_t services;
    uint32_t build_time;
} BootIpl;

typedef struct
{
    uint8_t state; // SML_SPL_STATE_*: RECORD, COMMITTED, INTACT, CONFIRMED, REJECTED
    uint32_t version;
    uint32_t length; // instructions
    uint32_t crc32;
    uint32_t build_time;
} BootSpl;

typedef struct
{
    bool committed; // record, record CRC and commit right (the image not checked)
    uint32_t version;
    uint32_t length;
    uint32_t crc32;
    uint32_t build_time;
} BootApp;

void boot_image_ipl(BootIpl *ipl);

// The SPL's record; with check_crc and the IPL present, its code's CRC-32
// too (INTACT): 3008 instructions at most, 7 ms at 40 MIPS.
void boot_image_spl(BootSpl *spl, bool check_crc);

void boot_image_app(BootApp *app);

// The SPL can be started: intact and not rejected (ENTER_BOOTLOADER).
bool boot_image_spl_usable(void);

#endif // BOOT_IMAGE_H
