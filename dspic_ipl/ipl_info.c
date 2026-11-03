/*
 * The IPL info block, at IPL_INFO_ADDRESS (common/boot_layout.h): what the
 * SPL reports in BOOT-STATUS and the application in pic -v. A word per
 * instruction. The build time and the CRC-16 of page 0 below the block are
 * left erased here and filled in by tools/make_image.py ipl, which makes
 * the hex that is released and programmed.
 */
#include <stdint.h>

#include "../common/boot_layout.h"
#include "ipl_version.h"

// Not static, and used: nothing in the IPL reads it, and the compiler drops
// an unreferenced static before the linker's keep can count.
const __prog__ uint16_t ipl_info[IPL_INFO_WORDS]
    __attribute__((space(prog), address(IPL_INFO_ADDRESS), used, keep)) = {
        IPL_INFO_MAGIC_VALUE,
        RECORD_FORMAT_1,
        IPL_VERSION_MAJOR,
        IPL_VERSION_MINOR << 8 | IPL_VERSION_PATCH,
        IPL_SERVICES_VERSION,
        0,
        0xFFFF, // build time, high: make_image.py ipl
        0xFFFF, // build time, low
        0xFFFF, // CRC-16 of 0x000000-0x00077F: make_image.py ipl
};
