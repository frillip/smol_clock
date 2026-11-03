/*
 * The IPL's CRC-32 service (common/boot_layout.h, ipl_crc32_flash): IEEE
 * 802.3, reflected, over flash read as packed bytes, 3 an instruction, least
 * significant first. The IPL checks images with it, and the SPL and the
 * application call it through the GOTO at IPL_SERVICES, so all three agree
 * on what is intact.
 *
 * A nibble at a time. The 16-entry table is built on the stack at each call
 * (64 bytes, about 1k cycles): the IPL has no initialised data, and a table
 * in RAM between calls would belong to whichever stage called last.
 *
 * The loop over flash is assembly on the chip: 31.8 cycles a byte against
 * the C version's 58.9 at -O1, measured, and some 30 instructions smaller.
 * The host tests run the C version, which computes the same CRC.
 */
#include <stdint.h>

#include "../common/boot_layout.h"
#include "ipl_flash.h"

#define POLYNOMIAL 0xEDB88320UL

#ifdef __XC16__

/* uint32_t ipl_crc32_loop(uint32_t crc, uint32_t address,
 *                         uint32_t instructions, const uint32_t *table)
 * The XC16 convention: crc W1:W0, address W3:W2 (TBLPAG:offset), the count
 * W5:W4, table W6; the result W1:W0. W8-W11 and TBLPAG saved and restored.
 * Each instruction's bytes 7-0, 15-8, then 23-16; TBLPAG carried over
 * 64 K. TBLPAG's address is the device linker script's. */
uint32_t ipl_crc32_loop(uint32_t crc, uint32_t address, uint32_t instructions,
                        const uint32_t *table);

__asm__("        .equ    IPL_TBLPAG, 0x0054\n"
        // One nibble: crc = (crc >> 4) ^ table[crc & 15]. W10-W11 scratch.
        "        .macro  ipl_crc_nibble\n"
        "        and     W0, #15, W10\n"
        "        sl      W10, #2, W10\n"
        "        add     W6, W10, W10\n"
        "        lsr     W0, #4, W0\n"
        "        sl      W1, #12, W11\n"
        "        ior     W0, W11, W0\n"
        "        lsr     W1, #4, W1\n"
        "        mov     [W10++], W11\n"
        "        xor     W0, W11, W0\n"
        "        mov     [W10], W11\n"
        "        xor     W1, W11, W1\n"
        "        .endm\n"
        // One byte, in W7 (upper byte zero).
        "        .macro  ipl_crc_byte\n"
        "        xor     W0, W7, W0\n"
        "        ipl_crc_nibble\n"
        "        ipl_crc_nibble\n"
        "        .endm\n"
        "        .text\n"
        "        .global _ipl_crc32_loop\n"
        "_ipl_crc32_loop:\n"
        "        push    W8\n"
        "        push    W9\n"
        "        push    W10\n"
        "        push    W11\n"
        "        push    IPL_TBLPAG\n"
        "        mov     W3, IPL_TBLPAG\n"
        "1:      ior     W4, W5, W7\n"
        "        bra     z, 3f\n"
        "        tblrdl  [W2], W8\n"
        "        tblrdh  [W2], W9\n"
        "        ze      W8, W7\n"
        "        ipl_crc_byte\n"
        "        lsr     W8, #8, W7\n"
        "        ipl_crc_byte\n"
        "        mov     W9, W7\n"
        "        ipl_crc_byte\n"
        "        add     W2, #2, W2\n"
        "        bra     nc, 2f\n"
        "        inc     IPL_TBLPAG\n"
        "2:      sub     W4, #1, W4\n"
        "        subb    W5, #0, W5\n"
        "        bra     1b\n"
        "3:      pop     IPL_TBLPAG\n"
        "        pop     W11\n"
        "        pop     W10\n"
        "        pop     W9\n"
        "        pop     W8\n"
        "        return\n");

#else

static uint32_t ipl_crc32_loop(uint32_t crc, uint32_t address, uint32_t instructions,
                               const uint32_t *table)
{
    while (instructions-- != 0UL)
    {
        uint32_t word = ipl_flash_read(address);
        uint8_t shift;

        for (shift = 0; shift < 24U; shift = (uint8_t)(shift + 8U))
        {
            crc ^= (word >> shift) & 0xFFUL;
            crc = (crc >> 4) ^ table[crc & 0x0FUL];
            crc = (crc >> 4) ^ table[crc & 0x0FUL];
        }
        address += 2UL;
    }
    return crc;
}

#endif

uint32_t ipl_crc32_flash(uint32_t crc, uint32_t address, uint32_t instructions)
{
    uint32_t table[16];
    uint8_t entry;
    uint8_t bit;

    for (entry = 0; entry < 16U; entry++)
    {
        uint32_t value = entry;

        for (bit = 0; bit < 4U; bit++)
        {
            value = (value & 1UL) != 0UL ? (value >> 1) ^ POLYNOMIAL : value >> 1;
        }
        table[entry] = value;
    }
    return ipl_crc32_loop(crc, address, instructions, table);
}
