#ifndef IPL_FLASH_H
#define IPL_FLASH_H

/*
 * Reading program memory. On the chip, table reads; on the host, the tests'
 * flash model (tests/host). The only hardware ipl_select.c and ipl_crc32.c
 * touch, so both run unchanged in the host tests.
 */

#include <stdint.h>

#ifdef __XC16__

#include <xc.h>

// The 24-bit instruction word at a program address (even). TBLPAG restored.
static inline uint32_t ipl_flash_read(uint32_t address)
{
    uint16_t saved = TBLPAG;
    uint16_t low;
    uint16_t high;

    TBLPAG = (uint16_t)(address >> 16);
    low = __builtin_tblrdl((uint16_t)address);
    high = __builtin_tblrdh((uint16_t)address);
    TBLPAG = saved;
    return (uint32_t)(high & 0x00FFU) << 16 | low;
}

#else

uint32_t ipl_flash_read(uint32_t address);

#endif

#endif // IPL_FLASH_H
