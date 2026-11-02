/*
 * The bootloader's checksums, in portable C99 CRC-32 for images and chunks,
 * and the record CRC.
 * The same CRC-32 as the IPL's flash routine, as zlib's crc32() and Python's
 * zlib.crc32(): IEEE 802.3, reflected, polynomial 0xEDB88320, initial value
 * 0xFFFFFFFF, final complement. 
 * For the application, the SPL, the ESP and the host tools, the IPL has its own in assembly.
 */ 
#ifndef BOOT_CRC_H
#define BOOT_CRC_H

#include <stdint.h>

#define BOOT_CRC32_INIT 0xFFFFFFFFUL

// Continues crc over length bytes. Start with BOOT_CRC32_INIT; the CRC is
// the complement of the final value.
uint32_t boot_crc32_update(uint32_t crc, const uint8_t *data, uint32_t length);

// The CRC-32 of length bytes, start to finish.
uint32_t boot_crc32(const uint8_t *data, uint32_t length);

// A record's record_crc: CRC-16/IBM-3740 over words 0-13 (RECORD_CRC_WORDS),
// each as 2 bytes big-endian.
uint16_t boot_record_crc(const uint16_t *words);

#endif // BOOT_CRC_H
