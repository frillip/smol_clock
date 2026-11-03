#include "pic_file.h"

#include <stddef.h>
#include <string.h>

#include "sml/boot_crc.h"
#include "sml/boot_layout.h"

#define FILE_FORMAT 1U

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}


static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}


bool pic_file_parse(const uint8_t header[PIC_FILE_HEADER_BYTES], PicFileHeader *out)
{
    uint32_t max;

    memset(out, 0, sizeof(*out));
    if (boot_crc32(header, 60U) != get32(header + 60) || get16(header + 8) != FILE_FORMAT ||
        get16(header + 10) != PIC_FILE_HEADER_BYTES)
    {
        return false;
    }
    out->device_id = get32(header + 12);
    if (memcmp(header, "SMOLPIC", 8) == 0)
    {
        out->kind = PIC_FILE_APP;
        out->start = get32(header + 16);
        out->length = get32(header + 20);
        out->version = get32(header + 24);
        out->crc32 = get32(header + 28);
        out->build_time = get32(header + 32);
        out->sml_version = get16(header + 36);
        if (out->start != APP_START)
        {
            return false;
        }
        max = APP_MAX;
    }
    else if (memcmp(header, "SMOLSPL", 8) == 0)
    {
        out->kind = PIC_FILE_SPL;
        out->start = SPL_START;
        out->version = get32(header + 16);
        out->min_ipl = get32(header + 20);
        out->protocol = get16(header + 24);
        out->build_time = get32(header + 28);
        out->length = get32(header + 32);
        out->crc32 = get32(header + 36);
        max = SPL_CODE_MAX;
    }
    else
    {
        return false;
    }
    return out->length != 0U && (out->length & 1U) == 0U && out->length <= max &&
           out->version <= 0xFFFFFFU;
}


uint32_t pic_file_bytes(const PicFileHeader *header)
{
    return PIC_FILE_HEADER_BYTES + 3U * header->length;
}


bool pic_file_version(const char *text, uint32_t *version)
{
    uint32_t parts[3] = {0, 0, 0};
    unsigned part = 0;
    bool digit = false;

    if (text == NULL)
    {
        return false;
    }
    for (; *text != '\0'; text++)
    {
        if (*text >= '0' && *text <= '9')
        {
            parts[part] = parts[part] * 10U + (uint32_t)(*text - '0');
            if (parts[part] > 255U)
            {
                return false;
            }
            digit = true;
        }
        else if (*text == '.' && digit && part < 2U)
        {
            part++;
            digit = false;
        }
        else
        {
            return false;
        }
    }
    if (part != 2U || !digit)
    {
        return false;
    }
    *version = parts[0] << 16 | parts[1] << 8 | parts[2];
    return true;
}
