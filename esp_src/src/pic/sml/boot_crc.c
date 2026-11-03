#include "boot_crc.h"

#include "boot_layout.h"
#include "sml.h"

// A nibble at a time, as the IPL's routine: 16 entries, two steps a byte.
static const uint32_t crc32_nibbles[16] = {0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
                                           0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
                                           0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
                                           0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL};

uint32_t boot_crc32_update(uint32_t crc, const uint8_t *data, uint32_t length)
{
    while (length-- != 0UL)
    {
        crc ^= *data++;
        crc = (crc >> 4) ^ crc32_nibbles[crc & 0x0FUL];
        crc = (crc >> 4) ^ crc32_nibbles[crc & 0x0FUL];
    }
    return crc;
}


uint32_t boot_crc32(const uint8_t *data, uint32_t length)
{
    return ~boot_crc32_update(BOOT_CRC32_INIT, data, length);
}


uint16_t boot_record_crc(const uint16_t *words)
{
    uint16_t crc = SML_CRC_INIT;
    uint8_t bytes[2];
    uint8_t index;

    for (index = 0; index < RECORD_CRC_WORDS; index++)
    {
        bytes[0] = (uint8_t)(words[index] >> 8);
        bytes[1] = (uint8_t)words[index];
        crc = sml_crc(crc, bytes, 2U);
    }
    return crc;
}
