/*
 * The SML codec (esp/sml.c): CRC, the message table against serial_proto.h,
 * big-endian conversion, golden frames, and the stream parser's recovery
 * from noise, damage and false headers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/sml.h"

static int failures, checks;
#define CHECK(cond)                                          \
    do                                                       \
    {                                                        \
        checks++;                                            \
        if (!(cond))                                         \
        {                                                    \
            failures++;                                      \
            printf("  FAIL line %d: %s\n", __LINE__, #cond); \
        }                                                    \
    } while (0)

// An independent bitwise CRC-16/IBM-3740, to check the nibble table.
static uint16_t crc_bitwise(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    while (length--)
    {
        crc ^= (uint16_t)(*data++ << 8);
        for (int bit = 0; bit < 8; bit++)
            crc = (uint16_t)(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}


static void print_frame(const char *name, const uint8_t *frame, uint16_t length)
{
    printf("  %s (%u bytes):", name, length);
    for (uint16_t i = 0; i < length; i++)
        printf("%s%02X", i % 16 == 0 ? "\n    " : " ", frame[i]);
    printf("\n");
}


static void test_crc(void)
{
    uint8_t data[256];
    CHECK(sml_crc(SML_CRC_INIT, (const uint8_t *)"123456789", 9) == 0x29B1);
    for (int n = 0; n < 1000; n++)
    {
        uint16_t length = (uint16_t)(rand() % 256);
        for (int i = 0; i < length; i++)
            data[i] = (uint8_t)rand();
        if (sml_crc(SML_CRC_INIT, data, length) != crc_bitwise(data, length))
        {
            CHECK(0);
            break;
        }
    }
}


// Struct sizes from sml_proto.h, by class and ID.
static size_t struct_size(uint8_t class_id, uint8_t message_id)
{
    static const struct
    {
        uint8_t class_id, message_id;
        size_t size;
    } sizes[] = {
        {SML_CLASS_TIME, SML_TIME_AUTH, sizeof(SML_TIME_AUTH_PAYLOAD)},
        {SML_CLASS_TIME, SML_TIME_PIC, sizeof(SML_TIME_PIC_PAYLOAD)},
        {SML_CLASS_TIME, SML_TIME_CTRL, sizeof(SML_TIME_CTRL_PAYLOAD)},
        {SML_CLASS_TIME, SML_TIME_VCTCXO, sizeof(SML_TIME_VCTCXO_PAYLOAD)},
        {SML_CLASS_TIME, SML_TIME_MEAS, sizeof(SML_TIME_MEAS_PAYLOAD)},
        {SML_CLASS_TIME, SML_TIME_GNSS, sizeof(SML_TIME_GNSS_PAYLOAD)},
        {SML_CLASS_TIME, SML_TIME_NTP, sizeof(SML_TIME_NTP_PAYLOAD)},
        {SML_CLASS_TIME, SML_TIME_RTC, sizeof(SML_TIME_RTC_PAYLOAD)},
        {SML_CLASS_TIME, SML_TIME_LEAP, sizeof(SML_TIME_LEAP_PAYLOAD)},
        {SML_CLASS_TZ, SML_TZ_STATE, sizeof(SML_TZ_STATE_PAYLOAD)},
        {SML_CLASS_STAT, SML_STAT_UI, sizeof(SML_STAT_UI_PAYLOAD)},
        {SML_CLASS_STAT, SML_STAT_ENV, sizeof(SML_STAT_ENV_PAYLOAD)},
        {SML_CLASS_STAT, SML_STAT_SUPPLY, sizeof(SML_STAT_SUPPLY_PAYLOAD)},
        {SML_CLASS_STAT, SML_STAT_SETTINGS, sizeof(SML_STAT_SETTINGS_PAYLOAD)},
        {SML_CLASS_STAT, SML_STAT_NET, sizeof(SML_STAT_NET_PAYLOAD)},
        {SML_CLASS_STAT, SML_STAT_SOURCES, sizeof(SML_STAT_SOURCES_PAYLOAD)},
        {SML_CLASS_STAT, SML_STAT_SYS, sizeof(SML_STAT_SYS_PAYLOAD)},
        {SML_CLASS_STAT, SML_STAT_DEBUG, 0},
        {SML_CLASS_CMD, SML_CMD_USER, sizeof(SML_CMD_USER_PAYLOAD)},
        {SML_CLASS_CMD, SML_CMD_RESPONSE, sizeof(SML_CMD_RESPONSE_PAYLOAD)},
        {SML_CLASS_CMD, SML_CMD_SETTINGS, sizeof(SML_CMD_SETTINGS_PAYLOAD)},
        {SML_CLASS_BOOT, SML_BOOT_STATUS, sizeof(SML_BOOT_STATUS_PAYLOAD)},
        {SML_CLASS_BOOT, SML_BOOT_COMMAND, sizeof(SML_BOOT_COMMAND_PAYLOAD)},
        {SML_CLASS_BOOT, SML_BOOT_RESPONSE, sizeof(SML_BOOT_RESPONSE_PAYLOAD)},
        {SML_CLASS_BOOT, SML_BOOT_DATA, sizeof(SML_BOOT_DATA_PAYLOAD)},
        {SML_CLASS_BOOT, SML_BOOT_DATA_RESPONSE, sizeof(SML_BOOT_DATA_RESPONSE_PAYLOAD)},
        {SML_CLASS_BOOT, SML_BOOT_VERIFY, sizeof(SML_BOOT_VERIFY_PAYLOAD)},
        {SML_CLASS_BOOT, SML_BOOT_VERIFY_RESPONSE, sizeof(SML_BOOT_VERIFY_RESPONSE_PAYLOAD)},
    };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        if (sizes[i].class_id == class_id && sizes[i].message_id == message_id)
            return sizes[i].size;
    return 9999;
}


// Every layout adds up to its struct; every field round-trips; and a field
// of value 0x01020304 lands on the wire as 01 02 03 04.
static void test_table(void)
{
    uint8_t count;
    const SmlMessage *messages = sml_messages(&count);

    CHECK(count == 28);
    for (uint8_t m = 0; m < count; m++)
    {
        const SmlMessage *message = &messages[m];
        uint8_t native[256], back[256], wire[256];
        uint16_t sum = 0;
        int fields = 0;

        for (const uint8_t *f = message->layout; *f; f++)
        {
            CHECK(*f == 1 || *f == 2 || *f == 4 || message->class_id == SML_CLASS_BOOT);
            sum += *f;
            fields++;
        }
        if (sum != message->length || struct_size(message->class_id, message->message_id) != sum)
            printf("  %s: layout %u, table %u, struct %zu\n", message->name, sum, message->length,
                   struct_size(message->class_id, message->message_id));
        CHECK(sum == message->length);
        CHECK(struct_size(message->class_id, message->message_id) == sum);
        CHECK(sml_message(message->class_id, message->message_id) == message);

        for (int trial = 0; trial < 50; trial++)
        {
            for (int i = 0; i < sum; i++)
                native[i] = (uint8_t)rand();
            memset(back, 0xEE, sizeof back);
            CHECK(sml_encode(message->layout, native, wire) == sum);
            CHECK(sml_decode(message->layout, wire, back) == sum);
            CHECK(memcmp(native, back, sum) == 0);
        }
        // Byte order, field by field.
        {
            uint16_t offset = 0;
            for (const uint8_t *f = message->layout; *f; offset += *f, f++)
            {
                uint32_t v32 = 0x01020304;
                uint16_t v16 = 0x0102;
                memset(native, 0, sizeof native);
                if (*f == 4)
                    memcpy(native + offset, &v32, 4);
                else if (*f == 2)
                    memcpy(native + offset, &v16, 2);
                else // bytes, as they are
                    for (int b = 0; b < *f; b++)
                        native[offset + b] = (uint8_t)(b + 1);
                sml_encode(message->layout, native, wire);
                for (int b = 0; b < *f; b++)
                    CHECK(wire[offset + b] == b + 1);
            }
        }
    }
    CHECK(sml_message(SML_CLASS_TZ, 0x10) == NULL); // retired TZ-FUTURE
    CHECK(sml_message(SML_CLASS_BOOT, SML_BOOT_COMMAND) != NULL);
    CHECK(sml_message(SML_CLASS_BOOT, 0x02) == NULL);
}


static void test_golden(void)
{
    uint8_t frame[SML_MAX_FRAME_BYTES];
    uint16_t length;

    // TIME-AUTH: locked to GNSS, 2026-10-18T12:34:56Z, epoch 150 ns late,
    // +/-75 ns.
    {
        SML_TIME_AUTH_PAYLOAD auth = {0};
        static const uint8_t expected[] = {0xB5, 0x73, 0x10, 0x01, 0x00, 0x10, 0x00, 0x07,
                                           0x05, 0x02, 0x6A, 0xD4, 0xBC, 0xF0, 0xFF, 0xFF,
                                           0xFF, 0x6A, 0x00, 0x00, 0x00, 0x4B};
        auth.flags = SML_AUTH_UTC_VALID | SML_AUTH_UNCERTAINTY_VALID | SML_AUTH_SOURCE_VALID;
        auth.source = SML_SOURCE_GNSS;
        auth.lock_state = SML_LOCK_LOCKED;
        auth.utc_seconds = 1792326896UL;
        auth.utc_phase_ns = -150;
        auth.uncertainty_ns = 75;
        length = sml_frame_message(SML_CLASS_TIME, SML_TIME_AUTH, &auth, frame);
        CHECK(length == 24);
        CHECK(memcmp(frame, expected, sizeof expected) == 0);
        CHECK((frame[22] << 8 | frame[23]) == crc_bitwise(frame, 22));
        print_frame("TIME-AUTH", frame, length);
    }

    // TZ-STATE: London, as in the timezone architecture document.
    {
        static const uint32_t next_utc[8] = {1792890000UL, 1806195600UL, 1824944400UL,
                                             1837645200UL, 1856394000UL, 1869094800UL,
                                             1887843600UL, 1901149200UL};
        SML_TZ_STATE_PAYLOAD state;
        memset(&state, 0, sizeof state);
        state.flags = SML_TZ_VALID;
        state.source = SML_TZ_SOURCE_GNSS;
        state.count = 8;
        state.issued_utc = 1791911413UL;
        state.horizon_end_utc = 1919293200UL;
        state.current.utc_seconds = 1774746000UL;
        state.current.dst_offset_s = 3600;
        state.current.flags = SML_TZ_ENTRY_DST_ACTIVE;
        for (int i = 0; i < 8; i++)
        {
            state.next[i].utc_seconds = next_utc[i];
            state.next[i].dst_offset_s = 3600;
            state.next[i].flags = (uint8_t)(i % 2 ? SML_TZ_ENTRY_DST_ACTIVE : 0);
        }
        length = sml_frame_message(SML_CLASS_TZ, SML_TZ_STATE, &state, frame);
        CHECK(length == 132);
        // Header, flags/source/count, issued, horizon, current, next[0], next[7].
        CHECK(frame[2] == 0x20 && frame[3] == 0x01 && frame[4] == 0x00 && frame[5] == 124);
        CHECK(frame[6] == 0x00 && frame[7] == 0x01 && frame[8] == 3 && frame[9] == 8);
        CHECK(sml_get_u32(&frame[6 + 4]) == 1791911413UL);
        CHECK(sml_get_u32(&frame[6 + 8]) == 1919293200UL);
        CHECK(sml_get_u32(&frame[6 + 16]) == 1774746000UL && frame[6 + 26] == 1);
        CHECK(sml_get_u16(&frame[6 + 24]) == 3600);
        CHECK(sml_get_u32(&frame[6 + 28]) == 1792890000UL && frame[6 + 38] == 0);
        CHECK(sml_get_u32(&frame[6 + 112]) == 1901149200UL && frame[6 + 122] == 1);
        print_frame("TZ-STATE (London)", frame, length);
    }

    // CMD-USER: SET_RTC 2026-10-18T12:34:56Z, token 0x1234 (as the document).
    {
        SML_CMD_USER_PAYLOAD command = {SML_USER_SET_RTC, 0, 0x1234, 1792326896UL};
        static const uint8_t expected[] = {0xB5, 0x73, 0x80, 0x10, 0x00, 0x0A, 0x00, 0x20, 0x00,
                                           0x00, 0x12, 0x34, 0x6A, 0xD4, 0xBC, 0xF0, 0xF9, 0x54};
        length = sml_frame_message(SML_CLASS_CMD, SML_CMD_USER, &command, frame);
        CHECK(length == 18 && memcmp(frame, expected, sizeof expected) == 0);
        print_frame("CMD-USER", frame, length);
    }

    // BOOT-COMMAND: ERASE the application region, token 0x1234 (as the document).
    {
        SML_BOOT_COMMAND_PAYLOAD command = {0};
        uint8_t expected[40] = {0xB5, 0x73, 0xF0, 0x10, 0x00, 0x20, 0x12, 0x34,
                                0x03, 0x00, 0x45, 0x52, 0x41, 0x53};

        expected[38] = 0x8D;
        expected[39] = 0x92;
        command.token = 0x1234;
        command.command = SML_BOOT_CMD_ERASE;
        command.region = SML_BOOT_REGION_APP;
        command.key = SML_BOOT_KEY_ERASE;
        length = sml_frame_message(SML_CLASS_BOOT, SML_BOOT_COMMAND, &command, frame);
        CHECK(length == 40 && memcmp(frame, expected, sizeof expected) == 0);
        print_frame("BOOT-COMMAND", frame, length);
    }

    // BOOT-DATA: the chunk goes as it is, after its big-endian header.
    {
        SML_BOOT_DATA_PAYLOAD data = {0};
        SML_BOOT_DATA_PAYLOAD back;

        data.address = 0x00002080UL;
        data.region = SML_BOOT_REGION_APP;
        data.crc32 = 0xCBF43926UL;
        for (int i = 0; i < (int)SML_BOOT_CHUNK_BYTES; i++)
            data.data[i] = (uint8_t)i;
        length = sml_frame_message(SML_CLASS_BOOT, SML_BOOT_DATA, &data, frame);
        CHECK(length == 212);
        CHECK(frame[6] == 0x00 && frame[7] == 0x00 && frame[8] == 0x20 && frame[9] == 0x80);
        CHECK(frame[14] == 0xCB && frame[17] == 0x26);
        CHECK(frame[18] == 0 && frame[19] == 1 && frame[18 + 191] == 191);
        CHECK(sml_decode(sml_message(SML_CLASS_BOOT, SML_BOOT_DATA)->layout, &frame[6], &back) ==
              SML_BOOT_DATA_BYTES);
        CHECK(memcmp(&back, &data, sizeof data) == 0);
    }

    // STAT-DEBUG: raw text.
    length = sml_frame(SML_CLASS_STAT, SML_STAT_DEBUG, (const uint8_t *)"Hi\r\n", 4, frame);
    CHECK(length == 12 && frame[5] == 4 && memcmp(&frame[6], "Hi\r\n", 4) == 0);
    print_frame("STAT-DEBUG", frame, length);
    CHECK(sml_frame(SML_CLASS_STAT, SML_STAT_DEBUG, frame, 241, frame) == 0);
    CHECK(sml_frame_message(SML_CLASS_STAT, SML_STAT_DEBUG, frame, frame) == 0);
}


// ---- Parser ---------------------------------------------------------------

static int frames_seen;
static SmlFrame last;
static uint8_t last_payload[256];

static void feed(SmlParser *parser, const uint8_t *bytes, size_t length)
{
    SmlFrame frame;
    for (size_t i = 0; i < length; i++)
    {
        if (sml_parser_push(parser, bytes[i], &frame))
        {
            do
            {
                frames_seen++;
                last = frame;
                memcpy(last_payload, frame.payload, frame.length);
                last.payload = last_payload;
            } while (sml_parser_next(parser, &frame));
        }
    }
}


static uint16_t make_frame(uint8_t *frame, uint8_t id, uint16_t length, uint8_t fill)
{
    uint8_t payload[256];
    memset(payload, fill, length);
    return sml_frame(SML_CLASS_STAT, id, payload, length, frame);
}


static void test_parser(void)
{
    SmlParser parser;
    uint8_t frame[SML_MAX_FRAME_BYTES], stream[2000];
    uint16_t length;
    size_t n;

    sml_parser_init(&parser);
    // Text, then a frame, then a frame split byte by byte across calls.
    feed(&parser, (const uint8_t *)"old firmware text\r\n", 19);
    length = make_frame(frame, 0xF0, 5, 'x');
    frames_seen = 0;
    feed(&parser, frame, length);
    CHECK(frames_seen == 1 && last.class_id == SML_CLASS_STAT && last.length == 5);
    CHECK(parser.stats.noise_bytes == 19);
    for (uint16_t i = 0; i < length; i++)
        feed(&parser, &frame[i], 1);
    CHECK(frames_seen == 2);

    // A damaged frame is dropped; the next good one is found.
    frame[8] ^= 0x40;
    feed(&parser, frame, length);
    frame[8] ^= 0x40;
    feed(&parser, frame, length);
    CHECK(frames_seen == 3 && parser.stats.crc_errors == 1);

    // A length over the maximum is not a header.
    {
        static const uint8_t bad[] = {0xB5, 0x73, 0x30, 0xF0, 0x00, 0xF1};
        feed(&parser, bad, sizeof bad);
        feed(&parser, frame, length);
        CHECK(frames_seen == 4 && parser.stats.length_errors == 1);
    }

    // A false header claiming 200 bytes swallows a real frame; once enough
    // follows for its CRC to fail, the rescan finds the real one, and the
    // frame after it, both from the same push.
    n = 0;
    stream[n++] = 0xB5;
    stream[n++] = 0x73;
    stream[n++] = 0x10;
    stream[n++] = 0x01;
    stream[n++] = 0x00;
    stream[n++] = 200;
    length = make_frame(frame, 0xF0, 20, 'a');
    memcpy(&stream[n], frame, length);
    n += length;
    length = make_frame(frame, 0xF0, 30, 'b');
    memcpy(&stream[n], frame, length);
    n += length;
    while (n < 6 + 200 + 2)
        stream[n++] = '.';
    frames_seen = 0;
    feed(&parser, stream, n);
    CHECK(frames_seen == 2 && last.length == 30 && last.payload[0] == 'b');

    // A partial frame then silence: dropped by the timeout.
    feed(&parser, frame, 10);
    CHECK(sml_parser_timeout(&parser) && parser.stats.timeouts == 1);
    CHECK(!sml_parser_timeout(&parser));
    frames_seen = 0;
    feed(&parser, frame, length);
    CHECK(frames_seen == 1);

    // Biggest frame.
    length = make_frame(frame, 0xF0, SML_MAX_PAYLOAD_BYTES, 'z');
    CHECK(length == SML_MAX_FRAME_BYTES);
    feed(&parser, frame, length);
    CHECK(frames_seen == 2 && last.length == SML_MAX_PAYLOAD_BYTES);
}


// Random frames in random noise (noise rich in magic bytes): every frame is
// recovered, whatever false headers the noise forms.
static void test_fuzz(void)
{
    SmlParser parser;
    uint8_t frame[SML_MAX_FRAME_BYTES];
    int sent = 0, frames = 20000;

    sml_parser_init(&parser);
    frames_seen = 0;
    for (int k = 0; k < frames; k++)
    {
        int noise = rand() % 8;
        for (int i = 0; i < noise; i++)
        {
            uint8_t byte = (uint8_t)(rand() % 4 == 0 ? 0xB5 : rand() % 4 == 0 ? 0x73 : rand());
            feed(&parser, &byte, 1);
        }
        uint16_t length = make_frame(frame, (uint8_t)k, (uint16_t)(rand() % 64), (uint8_t)rand());
        feed(&parser, frame, length);
        sent++;
    }
    // Flush anything a trailing false header still holds.
    for (int i = 0; i < (int)SML_MAX_FRAME_BYTES; i++)
    {
        uint8_t dot = '.';
        feed(&parser, &dot, 1);
    }
    printf("  fuzz: %d of %d frames recovered, %lu CRC rejections, %lu length rejections\n",
           frames_seen, sent, (unsigned long)parser.stats.crc_errors,
           (unsigned long)parser.stats.length_errors);
    CHECK(frames_seen == sent);
}


int main(void)
{
    srand(12345);
    test_crc();
    test_table();
    test_golden();
    test_parser();
    test_fuzz();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
