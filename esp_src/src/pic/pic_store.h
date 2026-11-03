/*
 * The PIC's files staged on the ESP: the picfw partition (512 KB), in fixed
 * slots, each holding one file (pic_file.h) exactly as downloaded.
 *
 *   0x00000  application slot 0   240 KB
 *   0x3C000  application slot 1   240 KB
 *   0x78000  SPL slot 0            12 KB
 *   0x7B000  SPL slot 1            12 KB
 *   0x7E000  unused                 8 KB
 *
 * No record anywhere says what a slot holds: a slot is valid exactly when its
 * header's CRC-32 and its image's both check, so leftovers in the partition
 * (from an older partition table, a write cut short) never count. A file is
 * written image first, header last: until the header is written the slot is
 * invalid, and a power cut leaves it so.
 *
 * Which slot a new file goes to: the one that is not valid, else the one
 * whose version is not the one the PIC is running (the last good one stays),
 * else the one with the lower version.
 *
 * Portable: the flash is reached through PicStoreFlash, the partition on the
 * ESP (pic_store_flash.cpp), memory in the host tests.
 */
#ifndef PIC_STORE_H
#define PIC_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "../util/sha256.h"
#include "pic_file.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define PIC_STORE_BYTES        0x80000UL
#define PIC_STORE_SECTOR       0x1000UL
#define PIC_STORE_APP_SLOT     0x3C000UL // 240 KB
#define PIC_STORE_SPL_SLOT     0x3000UL  // 12 KB
#define PIC_STORE_APP_OFFSET   0x00000UL
#define PIC_STORE_SPL_OFFSET   0x78000UL
#define PIC_STORE_SLOTS        2

typedef struct
{
    bool (*read)(uint32_t offset, void *data, uint32_t length);
    bool (*write)(uint32_t offset, const void *data, uint32_t length);
    bool (*erase)(uint32_t offset, uint32_t length); // whole sectors
} PicStoreFlash;

typedef struct
{
    bool valid;
    PicFileKind kind;
    uint8_t slot;
    PicFileHeader header;
} PicStoredFile;

typedef enum
{
    PIC_STORE_OK = 0,
    PIC_STORE_NO_PARTITION,
    PIC_STORE_TOO_BIG,   // the file does not fit its slot
    PIC_STORE_FLASH,     // an erase, write or read failed
    PIC_STORE_SHORT,     // fewer bytes than announced
    PIC_STORE_SHA256,    // not the file the manifest names
    PIC_STORE_HEADER,    // not a valid file of this kind
    PIC_STORE_IMAGE_CRC, // the image does not match its header, as written
} PicStoreResult;

typedef struct
{
    PicFileKind kind;
    uint8_t slot;
    uint32_t expected; // bytes announced
    uint32_t received;
    uint8_t header[PIC_FILE_HEADER_BYTES];
    Sha256 sha;
    PicStoreResult result;
} PicStoreWriter;

// The flash to use; NULL when there is no picfw partition (an image on the
// old partition table): every call then fails with NO_PARTITION.
void pic_store_init(const PicStoreFlash *flash);
bool pic_store_present(void);

uint32_t pic_store_slot_offset(PicFileKind kind, uint8_t slot);
uint32_t pic_store_slot_size(PicFileKind kind);

// A slot's file, checked: header and image CRC-32 (the image read whole).
bool pic_store_get(PicFileKind kind, uint8_t slot, PicStoredFile *file);

// A valid slot of that kind holding that version whose whole file has that
// SHA-256: the download can be skipped. False if none.
bool pic_store_find(PicFileKind kind, uint32_t version, const uint8_t sha256[SHA256_BYTES],
                    PicStoredFile *file);

// A valid file to install without the network: the one holding prefer (the
// version the PIC runs, 0 if none or unknown), else the newest. False if
// there is none.
bool pic_store_pick(PicFileKind kind, uint32_t prefer, PicStoredFile *file);

// Empties a slot: the sector holding its header erased, so it no longer
// holds a valid file. False if the flash failed or there is no picfw.
bool pic_store_erase(PicFileKind kind, uint8_t slot);

// The slot a new file goes to. running: the version the PIC reports, 0 if
// unknown.
uint8_t pic_store_choose(PicFileKind kind, uint32_t running);

// Writing a file as it arrives: begin erases what it needs of the slot,
// write takes the bytes in order, finish checks the SHA-256, the header and
// the image as written, then writes the header. Any failure leaves the slot
// invalid.
PicStoreResult pic_store_begin(PicStoreWriter *writer, PicFileKind kind, uint8_t slot,
                               uint32_t bytes);
PicStoreResult pic_store_write(PicStoreWriter *writer, const void *data, uint32_t length);
PicStoreResult pic_store_finish(PicStoreWriter *writer, const uint8_t sha256[SHA256_BYTES],
                                PicStoredFile *file);

// Packed image bytes of a stored file, from offset (0: the first instruction).
bool pic_store_read_image(const PicStoredFile *file, uint32_t offset, void *data, uint32_t length);

const char *pic_store_result_name(PicStoreResult result);

#ifdef __cplusplus
}
#endif

#endif // PIC_STORE_H
