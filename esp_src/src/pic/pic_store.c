#include "pic_store.h"

#include <stddef.h>
#include <string.h>

#include "sml/boot_crc.h"

#define READ_BYTES 512U

static const PicStoreFlash *flash;

void pic_store_init(const PicStoreFlash *new_flash)
{
    flash = new_flash;
}


bool pic_store_present(void)
{
    return flash != NULL;
}


uint32_t pic_store_slot_offset(PicFileKind kind, uint8_t slot)
{
    return kind == PIC_FILE_APP ? PIC_STORE_APP_OFFSET + slot * PIC_STORE_APP_SLOT
                                : PIC_STORE_SPL_OFFSET + slot * PIC_STORE_SPL_SLOT;
}


uint32_t pic_store_slot_size(PicFileKind kind)
{
    return kind == PIC_FILE_APP ? PIC_STORE_APP_SLOT : PIC_STORE_SPL_SLOT;
}


// The CRC-32 of length bytes of the partition from offset (Sha256 too, if
// given). False if a read fails.
static bool scan(uint32_t offset, uint32_t length, uint32_t *crc, Sha256 *sha)
{
    uint8_t buffer[READ_BYTES];

    *crc = BOOT_CRC32_INIT;
    while (length != 0U)
    {
        uint32_t take = length < READ_BYTES ? length : READ_BYTES;

        if (!flash->read(offset, buffer, take))
        {
            return false;
        }
        *crc = boot_crc32_update(*crc, buffer, take);
        if (sha != NULL)
        {
            sha256_update(sha, buffer, take);
        }
        offset += take;
        length -= take;
    }
    *crc = ~*crc;
    return true;
}


bool pic_store_get(PicFileKind kind, uint8_t slot, PicStoredFile *file)
{
    uint8_t header[PIC_FILE_HEADER_BYTES];
    uint32_t base = pic_store_slot_offset(kind, slot);
    uint32_t crc;

    memset(file, 0, sizeof(*file));
    file->kind = kind;
    file->slot = slot;
    if (flash == NULL || slot >= PIC_STORE_SLOTS || !flash->read(base, header, sizeof(header)) ||
        !pic_file_parse(header, &file->header) || file->header.kind != kind ||
        pic_file_bytes(&file->header) > pic_store_slot_size(kind) ||
        !scan(base + PIC_FILE_HEADER_BYTES, 3U * file->header.length, &crc, NULL) ||
        crc != file->header.crc32)
    {
        memset(&file->header, 0, sizeof(file->header));
        return false;
    }
    file->valid = true;
    return true;
}


bool pic_store_find(PicFileKind kind, uint32_t version, const uint8_t sha256[SHA256_BYTES],
                    PicStoredFile *file)
{
    uint8_t slot;

    for (slot = 0; slot < PIC_STORE_SLOTS; slot++)
    {
        Sha256 sha;
        uint8_t digest[SHA256_BYTES];
        uint32_t crc;

        if (!pic_store_get(kind, slot, file) || file->header.version != version)
        {
            continue;
        }
        sha256_start(&sha);
        if (scan(pic_store_slot_offset(kind, slot), pic_file_bytes(&file->header), &crc, &sha))
        {
            sha256_finish(&sha, digest);
            if (memcmp(digest, sha256, SHA256_BYTES) == 0)
            {
                return true;
            }
        }
    }
    memset(file, 0, sizeof(*file));
    return false;
}


bool pic_store_pick(PicFileKind kind, uint32_t prefer, PicStoredFile *file)
{
    PicStoredFile files[PIC_STORE_SLOTS];
    int best = -1;
    uint8_t slot;

    for (slot = 0; slot < PIC_STORE_SLOTS; slot++)
    {
        if (!pic_store_get(kind, slot, &files[slot]))
        {
            continue;
        }
        if (prefer != 0U && files[slot].header.version == prefer)
        {
            best = slot;
            break;
        }
        if (best < 0 || files[slot].header.version > files[best].header.version)
        {
            best = slot;
        }
    }
    if (best < 0)
    {
        memset(file, 0, sizeof(*file));
        return false;
    }
    *file = files[best];
    return true;
}


bool pic_store_erase(PicFileKind kind, uint8_t slot)
{
    if (flash == NULL || slot >= PIC_STORE_SLOTS)
    {
        return false;
    }
    return flash->erase(pic_store_slot_offset(kind, slot), PIC_STORE_SECTOR);
}


uint8_t pic_store_choose(PicFileKind kind, uint32_t running)
{
    PicStoredFile files[PIC_STORE_SLOTS];
    uint8_t slot;

    for (slot = 0; slot < PIC_STORE_SLOTS; slot++)
    {
        (void)pic_store_get(kind, slot, &files[slot]);
    }
    for (slot = 0; slot < PIC_STORE_SLOTS; slot++)
    {
        if (!files[slot].valid)
        {
            return slot;
        }
    }
    if (running != 0U)
    {
        if (files[0].header.version == running && files[1].header.version != running)
        {
            return 1;
        }
        if (files[1].header.version == running && files[0].header.version != running)
        {
            return 0;
        }
    }
    return files[0].header.version < files[1].header.version ? 0 : 1;
}


PicStoreResult pic_store_begin(PicStoreWriter *writer, PicFileKind kind, uint8_t slot,
                               uint32_t bytes)
{
    uint32_t sectors = (bytes + PIC_STORE_SECTOR - 1U) / PIC_STORE_SECTOR;

    memset(writer, 0, sizeof(*writer));
    writer->kind = kind;
    writer->slot = slot;
    writer->expected = bytes;
    sha256_start(&writer->sha);
    if (flash == NULL)
    {
        return writer->result = PIC_STORE_NO_PARTITION;
    }
    if (slot >= PIC_STORE_SLOTS || bytes <= PIC_FILE_HEADER_BYTES ||
        bytes > pic_store_slot_size(kind))
    {
        return writer->result = PIC_STORE_TOO_BIG;
    }
    if (!flash->erase(pic_store_slot_offset(kind, slot), sectors * PIC_STORE_SECTOR))
    {
        return writer->result = PIC_STORE_FLASH;
    }
    return writer->result = PIC_STORE_OK;
}


PicStoreResult pic_store_write(PicStoreWriter *writer, const void *data, uint32_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t base = pic_store_slot_offset(writer->kind, writer->slot);

    if (writer->result != PIC_STORE_OK)
    {
        return writer->result;
    }
    if (length > writer->expected - writer->received)
    {
        return writer->result = PIC_STORE_TOO_BIG;
    }
    sha256_update(&writer->sha, bytes, length);
    // The header is held back, to be written last.
    while (length != 0U && writer->received < PIC_FILE_HEADER_BYTES)
    {
        writer->header[writer->received++] = *bytes++;
        length--;
    }
    if (length != 0U)
    {
        if (!flash->write(base + writer->received, bytes, length))
        {
            return writer->result = PIC_STORE_FLASH;
        }
        writer->received += length;
    }
    return PIC_STORE_OK;
}


PicStoreResult pic_store_finish(PicStoreWriter *writer, const uint8_t sha256[SHA256_BYTES],
                                PicStoredFile *file)
{
    uint8_t digest[SHA256_BYTES];
    PicFileHeader header;
    uint32_t crc;
    uint32_t base = pic_store_slot_offset(writer->kind, writer->slot);

    if (writer->result != PIC_STORE_OK)
    {
        return writer->result;
    }
    if (writer->received != writer->expected)
    {
        return writer->result = PIC_STORE_SHORT;
    }
    sha256_finish(&writer->sha, digest);
    if (memcmp(digest, sha256, SHA256_BYTES) != 0)
    {
        return writer->result = PIC_STORE_SHA256;
    }
    if (!pic_file_parse(writer->header, &header) || header.kind != writer->kind ||
        pic_file_bytes(&header) != writer->expected)
    {
        return writer->result = PIC_STORE_HEADER;
    }
    // The image as the flash holds it, not as it arrived.
    if (!scan(base + PIC_FILE_HEADER_BYTES, 3U * header.length, &crc, NULL))
    {
        return writer->result = PIC_STORE_FLASH;
    }
    if (crc != header.crc32)
    {
        return writer->result = PIC_STORE_IMAGE_CRC;
    }
    if (!flash->write(base, writer->header, PIC_FILE_HEADER_BYTES))
    {
        return writer->result = PIC_STORE_FLASH;
    }
    if (!pic_store_get(writer->kind, writer->slot, file))
    {
        return writer->result = PIC_STORE_FLASH;
    }
    return PIC_STORE_OK;
}


bool pic_store_read_image(const PicStoredFile *file, uint32_t offset, void *data, uint32_t length)
{
    if (flash == NULL || !file->valid || offset + length > 3U * file->header.length)
    {
        return false;
    }
    return flash->read(pic_store_slot_offset(file->kind, file->slot) + PIC_FILE_HEADER_BYTES +
                           offset,
                       data, length);
}


const char *pic_store_result_name(PicStoreResult result)
{
    static const char *const names[] = {
        "OK",     "no picfw partition", "too big for its slot",           "flash failed",
        "short",  "SHA-256 wrong",      "not a valid file of that kind", "image CRC-32 wrong",
    };

    return (unsigned)result < sizeof(names) / sizeof(names[0]) ? names[result] : "?";
}
