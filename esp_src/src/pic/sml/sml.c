#include "sml.h"

#include <stddef.h>
#include <string.h>

// ---- Message table --------------------------------------------------------
/* Not built with SML_NO_MESSAGES, which the SPL defines: it frames and
 * parses, but never needs the table, some 400 instructions of program memory
 * as constants, which its linker did not drop. */
#ifndef SML_NO_MESSAGES

// Field sizes in wire order, as in sml_proto.h: 2 and 4 are big-endian
// integers, any other size that many bytes as they are. The host tests check
// that each layout adds up to the struct size and to the length in the table.

#define TZ_ENTRY_FIELDS      4, 4, 2, 1, 1
#define SETTINGS_BODY_FIELDS 2, 2, 4, 4, 4, 4, 4, 2, 1, 1
#define SOURCE_ENTRY_FIELDS  1, 1, 1, 1, 2, 2, 4, 4, 4, 4

static const uint8_t layout_time_auth[] = {2, 1, 1, 4, 4, 4, 0};
static const uint8_t layout_time_pic[] = {2, 1, 1, 1, 1, 1, 1, 4, 4, 4, 4, 4, 4, 4, 2, 0};
static const uint8_t layout_time_ctrl[] = {2, 1, 1, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0};
static const uint8_t layout_time_meas[] = {2, 1, 1, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0};
static const uint8_t layout_time_vctcxo[] = {2, 1, 1, 4, 2, 4, 4, 2, 0};
static const uint8_t layout_time_gnss[] = {2, 1, 1, 4, 4, 4, 2, 2, 2, 4,
                                           4, 4, 4, 4, 4, 2, 1, 1, 4, 0};
static const uint8_t layout_time_ntp[] = {2, 1, 1, 4, 4, 4, 4, 4, 2, 4, 4, 4, 4, 0};
static const uint8_t layout_time_rtc[] = {2, 1, 1, 4, 4, 4, 2, 4, 4, 4, 4, 0};
static const uint8_t layout_time_leap[] = {2, 1, 1, 2, 1, 1, 4, 4, 0};
static const uint8_t layout_tz_state[] = {2,
                                          1,
                                          1,
                                          4,
                                          4,
                                          4,
                                          TZ_ENTRY_FIELDS,
                                          TZ_ENTRY_FIELDS,
                                          TZ_ENTRY_FIELDS,
                                          TZ_ENTRY_FIELDS,
                                          TZ_ENTRY_FIELDS,
                                          TZ_ENTRY_FIELDS,
                                          TZ_ENTRY_FIELDS,
                                          TZ_ENTRY_FIELDS,
                                          TZ_ENTRY_FIELDS,
                                          0};
static const uint8_t layout_stat_ui[] = {2, 1, 1, 1, 1, 2, 2, 2, 4, 4, 4, 4, 4, 0};
static const uint8_t layout_stat_env[] = {2, 2, 2, 2, 2, 2, 2, 0};
static const uint8_t layout_stat_supply[] = {2, 2, 2, 2, 2, 2, 2, 0};
static const uint8_t layout_stat_net[] = {2, 1, 1, 4, 4, 4, 4, 4, 0};
static const uint8_t layout_stat_sys[] = {2, 2, 4, 4, 2, 2, 0};
static const uint8_t layout_stat_sources[] = {2,
                                              1,
                                              1,
                                              1,
                                              1,
                                              2,
                                              4,
                                              SOURCE_ENTRY_FIELDS,
                                              SOURCE_ENTRY_FIELDS,
                                              SOURCE_ENTRY_FIELDS,
                                              SOURCE_ENTRY_FIELDS,
                                              0};
static const uint8_t layout_stat_settings[] = {2, 2, 4, SETTINGS_BODY_FIELDS, 0};
static const uint8_t layout_cmd_user[] = {2, 2, 2, 4, 0};
static const uint8_t layout_cmd_response[] = {2, 2, 2, 2, 4, 0};
static const uint8_t layout_cmd_settings[] = {2, 2, 2, 4, SETTINGS_BODY_FIELDS, 0};
static const uint8_t layout_boot_status[] = {4, 1, 1, 2, 2, 2, 4, 4, 4, 4, 4,
                                             4, 4, 4, 1, 1, 2, 1, 1, 2, 4, 0};
static const uint8_t layout_boot_command[] = {2, 1, 1, 4, 4, 4, 4, 4, 4, 2, 2, 0};
static const uint8_t layout_boot_response[] = {2, 2, 2, 1, 1, 4, 0};
static const uint8_t layout_boot_data[] = {4, 1, 3, 4, SML_BOOT_CHUNK_BYTES, 0};
static const uint8_t layout_boot_data_response[] = {2, 2, 4, 4, 0};
static const uint8_t layout_boot_verify[] = {4, 4, 0};
static const uint8_t layout_boot_verify_response[] = {2, 2, 4, 4, SML_BOOT_CHUNK_BYTES, 0};
static const uint8_t layout_text[] = {0};

#define PIC SML_FROM_PIC
#define ESP SML_FROM_ESP

static const SmlMessage messages[] = {
    {SML_CLASS_TIME, SML_TIME_AUTH, SML_TIME_AUTH_BYTES, PIC, layout_time_auth, "TIME-AUTH"},
    {SML_CLASS_TIME, SML_TIME_PIC, SML_TIME_PIC_BYTES, PIC, layout_time_pic, "TIME-PIC"},
    {SML_CLASS_TIME, SML_TIME_CTRL, SML_TIME_CTRL_BYTES, PIC, layout_time_ctrl, "TIME-CTRL"},
    {SML_CLASS_TIME, SML_TIME_VCTCXO, SML_TIME_VCTCXO_BYTES, PIC, layout_time_vctcxo,
     "TIME-VCTCXO"},
    {SML_CLASS_TIME, SML_TIME_MEAS, SML_TIME_MEAS_BYTES, PIC, layout_time_meas, "TIME-MEAS"},
    {SML_CLASS_TIME, SML_TIME_GNSS, SML_TIME_GNSS_BYTES, PIC, layout_time_gnss, "TIME-GNSS"},
    {SML_CLASS_TIME, SML_TIME_NTP, SML_TIME_NTP_BYTES, ESP, layout_time_ntp, "TIME-NTP"},
    {SML_CLASS_TIME, SML_TIME_RTC, SML_TIME_RTC_BYTES, PIC, layout_time_rtc, "TIME-RTC"},
    {SML_CLASS_TIME, SML_TIME_LEAP, SML_TIME_LEAP_BYTES, ESP, layout_time_leap, "TIME-LEAP"},
    {SML_CLASS_TZ, SML_TZ_STATE, SML_TZ_STATE_BYTES, ESP, layout_tz_state, "TZ-STATE"},
    {SML_CLASS_STAT, SML_STAT_UI, SML_STAT_UI_BYTES, PIC, layout_stat_ui, "STAT-UI"},
    {SML_CLASS_STAT, SML_STAT_ENV, SML_STAT_ENV_BYTES, PIC, layout_stat_env, "STAT-ENV"},
    {SML_CLASS_STAT, SML_STAT_SUPPLY, SML_STAT_SUPPLY_BYTES, PIC, layout_stat_supply,
     "STAT-SUPPLY"},
    {SML_CLASS_STAT, SML_STAT_SETTINGS, SML_STAT_SETTINGS_BYTES, PIC, layout_stat_settings,
     "STAT-SETTINGS"},
    {SML_CLASS_STAT, SML_STAT_NET, SML_STAT_NET_BYTES, ESP, layout_stat_net, "STAT-NET"},
    {SML_CLASS_STAT, SML_STAT_SOURCES, SML_STAT_SOURCES_BYTES, PIC, layout_stat_sources,
     "STAT-SOURCES"},
    {SML_CLASS_STAT, SML_STAT_SYS, SML_STAT_SYS_BYTES, PIC, layout_stat_sys, "STAT-SYS"},
    {SML_CLASS_STAT, SML_STAT_DEBUG, 0U, PIC, layout_text, "STAT-DEBUG"},
    {SML_CLASS_CMD, SML_CMD_USER, SML_CMD_USER_BYTES, PIC | ESP, layout_cmd_user, "CMD-USER"},
    {SML_CLASS_CMD, SML_CMD_RESPONSE, SML_CMD_RESPONSE_BYTES, PIC | ESP, layout_cmd_response,
     "CMD-RESPONSE"},
    {SML_CLASS_CMD, SML_CMD_SETTINGS, SML_CMD_SETTINGS_BYTES, ESP, layout_cmd_settings,
     "CMD-SETTINGS"},
    {SML_CLASS_BOOT, SML_BOOT_STATUS, SML_BOOT_STATUS_BYTES, PIC, layout_boot_status,
     "BOOT-STATUS"},
    {SML_CLASS_BOOT, SML_BOOT_COMMAND, SML_BOOT_COMMAND_BYTES, ESP, layout_boot_command,
     "BOOT-COMMAND"},
    {SML_CLASS_BOOT, SML_BOOT_RESPONSE, SML_BOOT_RESPONSE_BYTES, PIC, layout_boot_response,
     "BOOT-RESPONSE"},
    {SML_CLASS_BOOT, SML_BOOT_DATA, SML_BOOT_DATA_BYTES, ESP, layout_boot_data, "BOOT-DATA"},
    {SML_CLASS_BOOT, SML_BOOT_DATA_RESPONSE, SML_BOOT_DATA_RESPONSE_BYTES, PIC,
     layout_boot_data_response, "BOOT-DATA-RESPONSE"},
    {SML_CLASS_BOOT, SML_BOOT_VERIFY, SML_BOOT_VERIFY_BYTES, ESP, layout_boot_verify,
     "BOOT-VERIFY"},
    {SML_CLASS_BOOT, SML_BOOT_VERIFY_RESPONSE, SML_BOOT_VERIFY_RESPONSE_BYTES, PIC,
     layout_boot_verify_response, "BOOT-VERIFY-RESPONSE"},
};

#undef PIC
#undef ESP

#define MESSAGE_COUNT (sizeof(messages) / sizeof(messages[0]))

const SmlMessage *sml_message(uint8_t class_id, uint8_t message_id)
{
    uint8_t index;

    for (index = 0; index < MESSAGE_COUNT; index++)
    {
        if (messages[index].class_id == class_id && messages[index].message_id == message_id)
        {
            return &messages[index];
        }
    }
    return NULL;
}


const SmlMessage *sml_messages(uint8_t *count)
{
    *count = (uint8_t)MESSAGE_COUNT;
    return messages;
}

#endif // SML_NO_MESSAGES


// ---- CRC ------------------------------------------------------------------

// A nibble at a time: 16 entries, two steps per byte.
static const uint16_t crc_nibbles[16] = {0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5,
                                         0x60C6, 0x70E7, 0x8108, 0x9129, 0xA14A, 0xB16B,
                                         0xC18C, 0xD1AD, 0xE1CE, 0xF1EF};

uint16_t sml_crc(uint16_t crc, const uint8_t *data, uint16_t length)
{
    while (length-- != 0U)
    {
        crc ^= (uint16_t)((uint16_t)*data++ << 8);
        crc = (uint16_t)((crc << 4) ^ crc_nibbles[crc >> 12]);
        crc = (uint16_t)((crc << 4) ^ crc_nibbles[crc >> 12]);
    }
    return crc;
}


// ---- Fields ---------------------------------------------------------------

void sml_put_u16(uint8_t *wire, uint16_t value)
{
    wire[0] = (uint8_t)(value >> 8);
    wire[1] = (uint8_t)value;
}


void sml_put_u32(uint8_t *wire, uint32_t value)
{
    wire[0] = (uint8_t)(value >> 24);
    wire[1] = (uint8_t)(value >> 16);
    wire[2] = (uint8_t)(value >> 8);
    wire[3] = (uint8_t)value;
}


uint16_t sml_get_u16(const uint8_t *wire)
{
    return (uint16_t)((uint16_t)wire[0] << 8 | wire[1]);
}


uint32_t sml_get_u32(const uint8_t *wire)
{
    return (uint32_t)wire[0] << 24 | (uint32_t)wire[1] << 16 | (uint32_t)wire[2] << 8 | wire[3];
}


// Each field is copied through a variable of its own width, so the native
// byte order never matters, and nothing assumes the struct is aligned. Any
// size but 2 and 4 is bytes, copied as they are.
uint16_t sml_encode(const uint8_t *layout, const void *native, uint8_t *wire)
{
    const uint8_t *source = (const uint8_t *)native;
    uint16_t offset = 0;

    for (; *layout != 0U; layout++)
    {
        if (*layout == 4U)
        {
            uint32_t value;
            memcpy(&value, source + offset, 4U);
            sml_put_u32(wire + offset, value);
        }
        else if (*layout == 2U)
        {
            uint16_t value;
            memcpy(&value, source + offset, 2U);
            sml_put_u16(wire + offset, value);
        }
        else
        {
            memcpy(wire + offset, source + offset, *layout);
        }
        offset = (uint16_t)(offset + *layout);
    }
    return offset;
}


uint16_t sml_decode(const uint8_t *layout, const uint8_t *wire, void *native)
{
    uint8_t *destination = (uint8_t *)native;
    uint16_t offset = 0;

    for (; *layout != 0U; layout++)
    {
        if (*layout == 4U)
        {
            uint32_t value = sml_get_u32(wire + offset);
            memcpy(destination + offset, &value, 4U);
        }
        else if (*layout == 2U)
        {
            uint16_t value = sml_get_u16(wire + offset);
            memcpy(destination + offset, &value, 2U);
        }
        else
        {
            memcpy(destination + offset, wire + offset, *layout);
        }
        offset = (uint16_t)(offset + *layout);
    }
    return offset;
}


// ---- Frames ---------------------------------------------------------------

uint16_t sml_frame(uint8_t class_id, uint8_t message_id, const uint8_t *payload, uint16_t length,
                   uint8_t *frame)
{
    uint16_t crc;

    if (length > SML_MAX_PAYLOAD_BYTES)
    {
        return 0;
    }
    frame[0] = SML_MAGIC_1;
    frame[1] = SML_MAGIC_2;
    frame[2] = class_id;
    frame[3] = message_id;
    sml_put_u16(&frame[4], length);
    if (payload != frame + SML_HEADER_BYTES && length != 0U)
    {
        memmove(frame + SML_HEADER_BYTES, payload, length);
    }
    crc = sml_crc(SML_CRC_INIT, frame, (uint16_t)(SML_HEADER_BYTES + length));
    sml_put_u16(frame + SML_HEADER_BYTES + length, crc);
    return (uint16_t)(SML_HEADER_BYTES + length + SML_CRC_BYTES);
}


#ifndef SML_NO_MESSAGES

uint16_t sml_frame_message(uint8_t class_id, uint8_t message_id, const void *native, uint8_t *frame)
{
    const SmlMessage *message = sml_message(class_id, message_id);

    if (message == NULL || message->length == 0U)
    {
        return 0;
    }
    (void)sml_encode(message->layout, native, frame + SML_HEADER_BYTES);
    return sml_frame(class_id, message_id, frame + SML_HEADER_BYTES, message->length, frame);
}

#endif // SML_NO_MESSAGES


// ---- Parser ---------------------------------------------------------------

void sml_parser_init(SmlParser *parser)
{
    memset(parser, 0, sizeof(*parser));
}


static void drop(SmlParser *parser, uint16_t bytes)
{
    parser->count = (uint16_t)(parser->count - bytes);
    if (parser->count != 0U)
    {
        memmove(parser->buffer, parser->buffer + bytes, parser->count);
    }
}


// Looks for a frame at the start of the buffer, dropping one byte at a time
// until one could start there. Every check that fails moves on by a single
// byte, so a frame that starts inside a rejected one is still found.
static bool scan(SmlParser *parser, SmlFrame *frame)
{
    const uint8_t *buffer = parser->buffer;

    while (parser->count != 0U)
    {
        uint16_t length;
        uint16_t total;

        if (buffer[0] != SML_MAGIC_1 || (parser->count >= 2U && buffer[1] != SML_MAGIC_2))
        {
            parser->stats.noise_bytes++;
            drop(parser, 1U);
            continue;
        }
        if (parser->count < SML_HEADER_BYTES)
        {
            return false;
        }
        length = sml_get_u16(&buffer[4]);
        if (length > SML_MAX_PAYLOAD_BYTES)
        {
            parser->stats.length_errors++;
            parser->stats.noise_bytes++;
            drop(parser, 1U);
            continue;
        }
        total = (uint16_t)(SML_HEADER_BYTES + length + SML_CRC_BYTES);
        if (parser->count < total)
        {
            return false;
        }
        if (sml_crc(SML_CRC_INIT, buffer, (uint16_t)(total - SML_CRC_BYTES)) !=
            sml_get_u16(&buffer[total - SML_CRC_BYTES]))
        {
            parser->stats.crc_errors++;
            parser->stats.noise_bytes++;
            drop(parser, 1U);
            continue;
        }
        frame->class_id = buffer[2];
        frame->message_id = buffer[3];
        frame->length = length;
        frame->payload = &buffer[SML_HEADER_BYTES];
        parser->consumed = total;
        parser->stats.frames++;
        return true;
    }
    return false;
}


static void release(SmlParser *parser)
{
    if (parser->consumed != 0U)
    {
        drop(parser, parser->consumed);
        parser->consumed = 0;
    }
}


bool sml_parser_push(SmlParser *parser, uint8_t byte, SmlFrame *frame)
{
    release(parser);
    // Cannot overflow: a buffer of SML_MAX_FRAME_BYTES always resolves to a
    // frame or a rejection by the time it is full.
    parser->buffer[parser->count++] = byte;
    return scan(parser, frame);
}


bool sml_parser_next(SmlParser *parser, SmlFrame *frame)
{
    release(parser);
    return scan(parser, frame);
}


bool sml_parser_timeout(SmlParser *parser)
{
    release(parser);
    if (parser->count == 0U)
    {
        return false;
    }
    parser->stats.timeouts++;
    parser->stats.noise_bytes += parser->count;
    parser->count = 0;
    return true;
}
