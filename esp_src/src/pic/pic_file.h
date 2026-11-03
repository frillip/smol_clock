/*
 * The PIC's image files, as dspic_src/tools/make_image.py writes them: a
 * 64-byte header, big-endian, its own CRC-32 in its last 4 bytes, then the
 * packed image (3 bytes an instruction, least significant first) whose
 * CRC-32 the header holds.
 *
 *   .smolpic  the application, from 0x002000 ("SMOLPIC\0")
 *   .smolspl  the second-stage loader, from 0x000800 ("SMOLSPL\0")
 */
#ifndef PIC_FILE_H
#define PIC_FILE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define PIC_FILE_HEADER_BYTES 64U

typedef enum
{
    PIC_FILE_APP = 0,
    PIC_FILE_SPL = 1,
} PicFileKind;

typedef struct
{
    PicFileKind kind;
    uint32_t device_id;
    uint32_t start;   // program address of the image
    uint32_t length;  // instructions
    uint32_t version; // major << 16 | minor << 8 | patch
    uint32_t crc32;   // of the packed image
    uint32_t build_time;
    uint16_t sml_version; // the application's
    uint32_t min_ipl;     // the SPL's: the lowest IPL it runs on
    uint16_t protocol;    // the SPL's bootloader protocol
} PicFileHeader;

// A header: its magic, format, size, CRC-32 and limits (the region its kind
// is written to, an even length). False if any is wrong.
bool pic_file_parse(const uint8_t header[PIC_FILE_HEADER_BYTES], PicFileHeader *out);

// The whole file's size: header and packed image.
uint32_t pic_file_bytes(const PicFileHeader *header);

// "0.4.1" into major << 16 | minor << 8 | patch, each 0-255. False if not.
bool pic_file_version(const char *text, uint32_t *version);

#ifdef __cplusplus
}
#endif

#endif // PIC_FILE_H
