/*
 * The SPL's core (spl_core.c) against the host port's flash model
 * (port_host.c), over SML frames as the ESP would send them: the start-up
 * modes, the boot window and the boot-loop guard, every BOOT command and
 * every NACK, retransmissions, the test region, the flash guard, and a power
 * cut at every flash operation of a whole update, after which the SPL must
 * accept the old image or the new one, never a mixture, and an update must
 * then succeed.
 */
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/boot_crc.h"
#include "../../../common/boot_layout.h"
#include "../../../common/sml.h"
#include "../../spl_core.h"
#include "../../spl_port.h"
#include "port_host.h"

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

static BootMailbox mailbox;

// ---- Flash contents ------------------------------------------------------------------

#define IPL_VERSION 0x000100UL // 0.1.0
#define SPL_VERSION 0x000102UL
#define SPL_LENGTH  1700U
#define OLD_VERSION 0x000301UL
#define NEW_VERSION 0x000400UL
#define SML_VERSION 0x0205U
#define BUILD_TIME  1793000000UL

static uint32_t old_image[APP_MAX + BOOT_CHUNK_INSTRUCTIONS];
static uint32_t new_image[APP_MAX + BOOT_CHUNK_INSTRUCTIONS];
static uint32_t random_state = 99U;

static uint32_t random24(void)
{
    random_state = random_state * 1664525U + 1013904223U;
    return random_state >> 8;
}


static void put_word(uint32_t record, int index, uint16_t value)
{
    host_put(record + 2U * (uint32_t)index, 0xFF0000UL | value);
}


static uint16_t get_word(uint32_t record, int index)
{
    return (uint16_t)host_get(record + 2U * (uint32_t)index);
}


static void put_long(uint32_t record, int index, uint32_t value)
{
    put_word(record, index, (uint16_t)(value >> 16));
    put_word(record, index + 1, (uint16_t)value);
}


static uint32_t words_crc(const uint32_t *words, uint32_t length)
{
    uint32_t crc = BOOT_CRC32_INIT;
    uint32_t index;

    for (index = 0; index < length; index++)
    {
        uint8_t bytes[3] = {(uint8_t)words[index], (uint8_t)(words[index] >> 8),
                            (uint8_t)(words[index] >> 16)};

        crc = boot_crc32_update(crc, bytes, 3U);
    }
    return ~crc;
}


static uint32_t flash_crc(uint32_t start, uint32_t length)
{
    return ~spl_port_crc32_flash(BOOT_CRC32_INIT, start, length);
}


// An image: random words, every fifth chunk erased (the ESP skips those),
// even length, padded with erased words to a whole chunk.
static void make_image(uint32_t *words, uint32_t length, uint32_t seed)
{
    uint32_t index;

    random_state = seed;
    for (index = 0; index < length; index++)
    {
        bool blank = (index / BOOT_CHUNK_INSTRUCTIONS) % 5U == 3U;

        words[index] = blank ? HOST_ERASED : random24() & 0x00FFFFFEUL;
    }
    for (; index % BOOT_CHUNK_INSTRUCTIONS != 0U; index++)
    {
        words[index] = HOST_ERASED;
    }
}


static void put_record(uint32_t record, uint16_t magic, const uint32_t *longs, int count,
                       uint16_t extra, bool commit)
{
    uint16_t words[RECORD_CRC_WORDS];
    int index;

    memset(words, 0, sizeof(words));
    words[0] = magic;
    words[1] = RECORD_FORMAT_1;
    for (index = 0; index < count; index++)
    {
        words[2 + 2 * index] = (uint16_t)(longs[index] >> 16);
        words[3 + 2 * index] = (uint16_t)longs[index];
    }
    if (magic == APP_RECORD_MAGIC_VALUE)
    {
        words[APP_RECORD_SML_VERSION] = extra;
    }
    else
    {
        words[SPL_RECORD_PROTOCOL] = extra;
    }
    for (index = 0; index < RECORD_CRC_WORDS; index++)
    {
        put_word(record, index, words[index]);
    }
    put_word(record, RECORD_CRC, boot_record_crc(words));
    put_word(record, RECORD_SPARE, 0xFFFF);
    if (commit)
    {
        put_word(record, RECORD_COMMIT, RECORD_COMMIT_0);
        put_word(record, RECORD_COMMIT + 1, RECORD_COMMIT_1);
    }
}


static void install_ipl(void)
{
    put_word(IPL_INFO_ADDRESS, RECORD_MAGIC, IPL_INFO_MAGIC_VALUE);
    put_word(IPL_INFO_ADDRESS, RECORD_FORMAT, RECORD_FORMAT_1);
    put_long(IPL_INFO_ADDRESS, IPL_INFO_VERSION, IPL_VERSION);
}


static void install_spl(bool confirmed, bool rejected)
{
    uint32_t code[SPL_LENGTH];
    uint32_t index;
    uint32_t longs[4];

    for (index = 0; index < BOOT_ROW_ADDRESSES / 2U; index++)
    {
        host_put(SPL_RECORD_ADDRESS + 2U * index, HOST_ERASED);
    }
    random_state = 7U;
    for (index = 0; index < SPL_LENGTH; index++)
    {
        code[index] = random24();
        host_put(SPL_START + 2U * index, code[index]);
    }
    longs[0] = SPL_VERSION;
    longs[1] = SPL_LENGTH;
    longs[2] = words_crc(code, SPL_LENGTH);
    longs[3] = BUILD_TIME;
    put_record(SPL_RECORD_ADDRESS, SPL_RECORD_MAGIC_VALUE, longs, 4, SML_BOOT_PROTOCOL, true);
    if (confirmed)
    {
        put_word(SPL_RECORD_ADDRESS, SPL_RECORD_CONFIRM, SPL_CONFIRM_0);
        put_word(SPL_RECORD_ADDRESS, SPL_RECORD_CONFIRM + 1, SPL_CONFIRM_1);
    }
    if (rejected)
    {
        put_word(SPL_RECORD_ADDRESS, SPL_RECORD_REJECT, 0);
    }
}


static void install_app(const uint32_t *image, uint32_t length, uint32_t version)
{
    uint32_t index;
    uint32_t longs[5];

    for (index = 0; index < length; index++)
    {
        host_put(APP_START + 2U * index, image[index]);
    }
    longs[0] = version;
    longs[1] = APP_START;
    longs[2] = length;
    longs[3] = words_crc(image, length);
    longs[4] = BUILD_TIME;
    put_record(APP_RECORD_ADDRESS, APP_RECORD_MAGIC_VALUE, longs, 5, SML_VERSION, true);
}


static bool flash_holds(const uint32_t *image, uint32_t length)
{
    uint32_t index;

    for (index = 0; index < length; index++)
    {
        if (host_get(APP_START + 2U * index) != image[index])
        {
            return false;
        }
    }
    return true;
}


// The clock as delivered: the IPL, a confirmed SPL, an application.
static void clock_with(uint32_t old_length)
{
    host_reset();
    install_ipl();
    install_spl(true, false);
    make_image(old_image, old_length, 1U);
    install_app(old_image, old_length, OLD_VERSION);
}


// ---- The link ------------------------------------------------------------------------

typedef struct
{
    uint8_t class_id;
    uint8_t id;
    uint16_t length;
    uint8_t payload[SML_MAX_PAYLOAD_BYTES];
} Frame;

static SmlParser out;
static uint32_t out_read;

static void clear_out(void)
{
    host_tx_length = 0;
    out_read = 0;
    sml_parser_init(&out);
}


static bool next_frame(Frame *frame)
{
    SmlFrame found;

    while (out_read < host_tx_length)
    {
        if (sml_parser_push(&out, host_tx[out_read++], &found))
        {
            frame->class_id = found.class_id;
            frame->id = found.message_id;
            frame->length = found.length;
            memcpy(frame->payload, found.payload, found.length);
            return true;
        }
    }
    return false;
}


static int frames_left(void)
{
    Frame frame;
    int count = 0;

    while (next_frame(&frame))
    {
        count++;
    }
    return count;
}


static void send(uint8_t id, const uint8_t *payload, uint16_t length)
{
    uint8_t frame[SML_MAX_FRAME_BYTES];
    uint16_t bytes = sml_frame(SML_CLASS_BOOT, id, payload, length, frame);

    host_rx_push(frame, bytes);
}


// Polls until everything sent has been answered. The last action that was
// not NONE.
static SplAction run(void)
{
    SplAction last = SPL_ACTION_NONE;
    int idle = 0;
    int guard;

    for (guard = 0; guard < 100000 && idle < 4; guard++)
    {
        SplAction action = spl_poll();

        if (action != SPL_ACTION_NONE)
        {
            last = action;
        }
        idle = host_rx_pending() == 0U ? idle + 1 : 0;
    }
    return last;
}


static void command_args(uint16_t token, uint8_t code, uint8_t region, uint32_t key,
                         const uint32_t *longs, uint16_t extra)
{
    uint8_t p[SML_BOOT_COMMAND_BYTES];
    int index;

    memset(p, 0, sizeof(p));
    sml_put_u16(&p[0], token);
    p[2] = code;
    p[3] = region;
    sml_put_u32(&p[4], key);
    if (longs != NULL)
    {
        for (index = 0; index < 5; index++)
        {
            sml_put_u32(&p[8 + 4 * index], longs[index]);
        }
        sml_put_u16(&p[28], extra);
    }
    send(SML_BOOT_COMMAND, p, sizeof(p));
}


static void command(uint16_t token, uint8_t code, uint8_t region, uint32_t key)
{
    command_args(token, code, region, key, NULL, 0);
}


typedef struct
{
    uint16_t status;
    uint16_t detail;
    uint16_t token;
    uint8_t command;
    uint8_t region;
    uint32_t value;
} Response;

// The next BOOT-RESPONSE, skipping BOOT-STATUS. False if none.
static bool response(Response *r)
{
    Frame frame;

    while (next_frame(&frame))
    {
        if (frame.class_id == SML_CLASS_BOOT && frame.id == SML_BOOT_RESPONSE &&
            frame.length == SML_BOOT_RESPONSE_BYTES)
        {
            r->status = sml_get_u16(&frame.payload[0]);
            r->detail = sml_get_u16(&frame.payload[2]);
            r->token = sml_get_u16(&frame.payload[4]);
            r->command = frame.payload[6];
            r->region = frame.payload[7];
            r->value = sml_get_u32(&frame.payload[8]);
            return true;
        }
    }
    return false;
}


// A command answered with one response (or ACCEPTED and a final one).
static Response ask(uint8_t code, uint8_t region, uint32_t key, const uint32_t *longs,
                    uint16_t extra, SplAction *action)
{
    static uint16_t token = 100;
    Response r;
    SplAction a;

    memset(&r, 0xEE, sizeof(r));
    token++;
    command_args(token, code, region, key, longs, extra);
    a = run();
    if (action != NULL)
    {
        *action = a;
    }
    if (!response(&r))
    {
        r.status = 0xFFFF;
        return r;
    }
    CHECK(r.token == token && r.command == code && r.region == region);
    if (r.status == SML_RESULT_ACCEPTED)
    {
        Response final;

        if (!response(&final))
        {
            final.status = 0xFFFE;
        }
        CHECK(final.token == token);
        return final;
    }
    return r;
}


static SML_BOOT_STATUS_PAYLOAD status_of(const Frame *frame)
{
    SML_BOOT_STATUS_PAYLOAD s;

    (void)sml_decode(sml_message(SML_CLASS_BOOT, SML_BOOT_STATUS)->layout, frame->payload, &s);
    return s;
}


// The next BOOT-STATUS in what the SPL sent. False if none.
static bool next_status(SML_BOOT_STATUS_PAYLOAD *s)
{
    Frame frame;

    while (next_frame(&frame))
    {
        if (frame.class_id == SML_CLASS_BOOT && frame.id == SML_BOOT_STATUS &&
            frame.length == SML_BOOT_STATUS_BYTES)
        {
            *s = status_of(&frame);
            return true;
        }
    }
    return false;
}


// STATUS: BOOT-STATUS, then ACK.
static SML_BOOT_STATUS_PAYLOAD status(void)
{
    SML_BOOT_STATUS_PAYLOAD s;
    Response r;

    memset(&s, 0, sizeof(s));
    clear_out();
    command(1, SML_BOOT_CMD_STATUS, 0, 0);
    run();
    CHECK(next_status(&s));
    CHECK(response(&r) && r.status == SML_RESULT_ACK && r.token == 1);
    return s;
}


static void data_frame(uint32_t address, uint8_t region, const uint8_t *chunk, uint32_t crc)
{
    uint8_t p[SML_BOOT_DATA_BYTES];

    memset(p, 0, sizeof(p));
    sml_put_u32(&p[0], address);
    p[4] = region;
    sml_put_u32(&p[8], crc);
    memcpy(&p[12], chunk, BOOT_CHUNK_BYTES);
    send(SML_BOOT_DATA, p, sizeof(p));
}


static void pack_chunk(const uint32_t *words, uint8_t *chunk)
{
    int index;

    for (index = 0; index < (int)BOOT_CHUNK_INSTRUCTIONS; index++)
    {
        chunk[3 * index] = (uint8_t)words[index];
        chunk[3 * index + 1] = (uint8_t)(words[index] >> 8);
        chunk[3 * index + 2] = (uint8_t)(words[index] >> 16);
    }
}


typedef struct
{
    uint16_t status;
    uint16_t detail;
    uint32_t address;
    uint32_t crc32;
} DataResponse;

static DataResponse data_raw(uint32_t address, uint8_t region, const uint8_t *chunk, uint32_t crc)
{
    DataResponse d;
    Frame frame;

    memset(&d, 0xEE, sizeof(d));
    clear_out();
    data_frame(address, region, chunk, crc);
    run();
    while (next_frame(&frame))
    {
        if (frame.id == SML_BOOT_DATA_RESPONSE && frame.length == SML_BOOT_DATA_RESPONSE_BYTES)
        {
            d.status = sml_get_u16(&frame.payload[0]);
            d.detail = sml_get_u16(&frame.payload[2]);
            d.address = sml_get_u32(&frame.payload[4]);
            d.crc32 = sml_get_u32(&frame.payload[8]);
            break;
        }
    }
    return d;
}


static DataResponse data(uint32_t address, uint8_t region, const uint32_t *words)
{
    uint8_t chunk[BOOT_CHUNK_BYTES];

    pack_chunk(words, chunk);
    return data_raw(address, region, chunk, boot_crc32(chunk, BOOT_CHUNK_BYTES));
}


// BOOT-VERIFY: true with the chunk, if ACK and its CRC right.
static bool verify(uint32_t address, uint8_t *chunk, uint16_t *detail)
{
    uint8_t p[SML_BOOT_VERIFY_BYTES];
    Frame frame;

    memset(p, 0, sizeof(p));
    sml_put_u32(&p[0], address);
    clear_out();
    send(SML_BOOT_VERIFY, p, sizeof(p));
    run();
    while (next_frame(&frame))
    {
        if (frame.id == SML_BOOT_VERIFY_RESPONSE && frame.length == SML_BOOT_VERIFY_RESPONSE_BYTES)
        {
            *detail = sml_get_u16(&frame.payload[2]);
            memcpy(chunk, &frame.payload[12], BOOT_CHUNK_BYTES);
            return sml_get_u16(&frame.payload[0]) == SML_RESULT_ACK &&
                   sml_get_u32(&frame.payload[4]) == address &&
                   sml_get_u32(&frame.payload[8]) == boot_crc32(chunk, BOOT_CHUNK_BYTES);
        }
    }
    *detail = 0xFFFF;
    return false;
}


static bool chunk_blank(const uint32_t *words)
{
    uint32_t index;

    for (index = 0; index < BOOT_CHUNK_INSTRUCTIONS; index++)
    {
        if (words[index] != HOST_ERASED)
        {
            return false;
        }
    }
    return true;
}


static void commit_args(uint32_t *longs, uint32_t version, uint32_t start, uint32_t length,
                        uint32_t crc)
{
    longs[0] = version;
    longs[1] = start;
    longs[2] = length;
    longs[3] = crc;
    longs[4] = BUILD_TIME + 1U;
}


// ---- Starting ------------------------------------------------------------------------

// A start from the IPL: the mailbox as the IPL leaves it, then spl_init().
static SML_BOOT_STATUS_PAYLOAD start(uint16_t request, uint16_t attempts, uint16_t reset_cause)
{
    SML_BOOT_STATUS_PAYLOAD s;

    memset(&mailbox, 0, sizeof(mailbox));
    mailbox.magic = MAILBOX_MAGIC_VALUE;
    mailbox.magic_inverse = (uint16_t)~MAILBOX_MAGIC_VALUE;
    mailbox.request = request;
    mailbox.attempts = attempts;
    mailbox.reset_cause = reset_cause;
    mailbox.boot = 0;
    mailbox.trials = 0;
    host_reset_link();
    clear_out();
    spl_init(&mailbox);
    memset(&s, 0, sizeof(s));
    CHECK(next_status(&s));
    return s;
}


// Polls with the clock moving from now to until, a millisecond at a time.
// The first action that is not NONE, and when.
static SplAction wait_until(uint32_t until, uint32_t *when)
{
    while (host_ms < until)
    {
        SplAction action;

        host_ms++;
        action = spl_poll();
        if (action != SPL_ACTION_NONE)
        {
            if (when != NULL)
            {
                *when = host_ms;
            }
            return action;
        }
    }
    return SPL_ACTION_NONE;
}


// ---- Tests --------------------------------------------------------------------------

static void test_guard(void)
{
    static const struct
    {
        uint32_t address;
        bool erase;
        bool write;
    } cases[] = {
        {0x000000, false, false}, {0x000800, false, false}, {0x001F80, false, false},
        {0x001FFC, false, false}, {0x002000, true, true},   {0x002004, false, true},
        {0x002002, false, false}, {0x028800, true, true},   {0x028F80, false, true},
        {0x029000, true, true},   {0x0297FC, false, true},  {0x029800, false, false},
        {0x02A000, false, false}, {0x02A800, false, false}, {0x02AFEC, false, false},
    };
    unsigned index;

    printf("flash guard\n");
    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
    {
        CHECK(spl_may_erase(cases[index].address) == cases[index].erase);
        CHECK(spl_may_write(cases[index].address) == cases[index].write);
    }
    host_reset();
    host_put(0x000800, 0x123456);
    host_put(0x029800, 0x654321);
    CHECK(!spl_port_flash_erase(0x000800));
    CHECK(!spl_port_flash_erase(0x029800));
    CHECK(!spl_port_flash_write(0x001F80, 0, 0));
    CHECK(!spl_port_flash_write(0x029800, 0, 0));
    CHECK(host_get(0x000800) == 0x123456 && host_get(0x029800) == 0x654321);
    CHECK(host_refused == 4 && host_operations() == 0);
}


static void test_window(void)
{
    SML_BOOT_STATUS_PAYLOAD s;
    uint32_t when = 0;

    printf("start-up: window, hand-over\n");
    clock_with(30000);
    s = start(0, 0, 0x0080);
    CHECK(s.mode == SML_BOOT_MODE_WINDOW);
    CHECK((s.flags & SML_BOOT_FLAG_CHECKING) != 0U);
    CHECK((s.flags & (SML_BOOT_FLAG_RECORD | SML_BOOT_FLAG_COMMITTED)) ==
          (SML_BOOT_FLAG_RECORD | SML_BOOT_FLAG_COMMITTED));
    CHECK((s.flags & SML_BOOT_FLAG_IMAGE_OK) == 0U);
    CHECK((s.flags & SML_BOOT_FLAG_POWER_ON) == 0U);
    CHECK(s.window_ms == BOOT_WINDOW_MS);
    CHECK(s.protocol == SML_BOOT_PROTOCOL);
    CHECK(s.device_id == 0x1F6CUL);
    CHECK(s.region_start == APP_START && s.region_end == APP_END);
    CHECK(s.app_version == OLD_VERSION && s.app_length == 30000UL);
    CHECK(s.app_crc32 == words_crc(old_image, 30000));
    CHECK(s.build_time == BUILD_TIME);
    CHECK(s.spl_version == SPL_VERSION && s.spl_crc32 != 0UL);
    CHECK(s.spl_state == (SML_SPL_STATE_RECORD | SML_SPL_STATE_COMMITTED | SML_SPL_STATE_INTACT |
                          SML_SPL_STATE_CONFIRMED));
    CHECK(s.ipl_version == 0x0001U);
    CHECK(s.reset_cause == 0x0080U);
    CHECK(s.healthy_s == 0U);
    CHECK(mailbox.request == 0U && mailbox.running == 0U);

    host_ms = 600;
    s = status();
    CHECK(s.mode == SML_BOOT_MODE_WINDOW);
    CHECK((s.flags & (SML_BOOT_FLAG_CHECKING | SML_BOOT_FLAG_IMAGE_OK)) == SML_BOOT_FLAG_IMAGE_OK);
    CHECK(s.crc32_computed == s.app_crc32);
    CHECK(s.window_ms == 400U);
    CHECK((s.flags & SML_BOOT_FLAG_TEST_FREE) != 0U);

    CHECK(wait_until(5000, &when) == SPL_ACTION_HAND_OVER);
    CHECK(when == BOOT_WINDOW_MS);
    CHECK(mailbox.attempts == 1U);
    CHECK(!host_led);

    printf("start-up: the boot-loop guard\n");
    {
        uint16_t count;

        // A boot loop: a hand-over every start until BOOT_LOOP_LIMIT.
        for (count = 0; count < BOOT_LOOP_LIMIT; count++)
        {
            host_ms = 0;
            s = start(0, count, 0);
            CHECK(s.mode == SML_BOOT_MODE_WINDOW && s.attempts == count);
            CHECK(wait_until(2000, NULL) == SPL_ACTION_HAND_OVER);
            CHECK(mailbox.attempts == count + 1U);
        }
    }
    host_ms = 0;
    s = start(0, BOOT_LOOP_LIMIT, 0);
    CHECK(s.mode == SML_BOOT_MODE_LOOP && s.attempts == BOOT_LOOP_LIMIT);
    CHECK(wait_until(10000, NULL) == SPL_ACTION_NONE);
    clear_out();
    host_ms = 10000;
    s = status();
    CHECK(s.mode == SML_BOOT_MODE_LOOP && (s.flags & SML_BOOT_FLAG_IMAGE_OK) != 0U);
    CHECK(ask(SML_BOOT_CMD_RUN, 0, 0, NULL, 0, NULL).status == SML_RESULT_ACK);
    CHECK(mailbox.attempts == BOOT_LOOP_LIMIT + 1U);

    printf("start-up: requested\n");
    host_ms = 0;
    s = start(MAILBOX_REQUEST_STAY, 0, 0x0040);
    CHECK(s.mode == SML_BOOT_MODE_HELD);
    CHECK((s.flags & SML_BOOT_FLAG_REQUESTED) != 0U);
    CHECK(s.window_ms == 0U);
    CHECK(mailbox.request == 0U);
    CHECK(wait_until(20000, NULL) == SPL_ACTION_NONE);

    printf("start-up: power-on, a mailbox not the IPL's\n");
    host_ms = 0;
    s = start(0, 0, 0x0083);
    CHECK((s.flags & SML_BOOT_FLAG_POWER_ON) != 0U);
    memset(&mailbox, 0x5A, sizeof(mailbox));
    host_reset_link();
    clear_out();
    spl_init(&mailbox);
    CHECK(next_status(&s));
    CHECK(mailbox.magic == MAILBOX_MAGIC_VALUE && mailbox.magic_inverse == 0xB2BDU);
    CHECK(mailbox.attempts == 0U && mailbox.request == 0U && s.mode == SML_BOOT_MODE_WINDOW);
}


static void test_no_app(void)
{
    SML_BOOT_STATUS_PAYLOAD s;

    printf("start-up: no application\n");
    host_reset();
    install_ipl();
    install_spl(true, false);
    s = start(0, 0, 0);
    CHECK(s.mode == SML_BOOT_MODE_NO_APP);
    CHECK((s.flags & (SML_BOOT_FLAG_RECORD | SML_BOOT_FLAG_CHECKING | SML_BOOT_FLAG_TEST_FREE)) ==
          0U);
    CHECK(s.app_version == 0UL && s.app_length == 0UL);
    CHECK(wait_until(5000, NULL) == SPL_ACTION_NONE);
    CHECK(ask(SML_BOOT_CMD_RUN, 0, 0, NULL, 0, NULL).detail == SML_ERROR_STATE);

    printf("start-up: a corrupt image\n");
    clock_with(20000);
    host_put(APP_START + 2U * 12345U, host_get(APP_START + 2U * 12345U) ^ 0x10UL);
    host_ms = 0;
    s = start(0, 0, 0);
    CHECK(s.mode == SML_BOOT_MODE_WINDOW && (s.flags & SML_BOOT_FLAG_CHECKING) != 0U);
    s = status();
    CHECK(s.mode == SML_BOOT_MODE_NO_APP);
    CHECK((s.flags & SML_BOOT_FLAG_IMAGE_OK) == 0U && (s.flags & SML_BOOT_FLAG_COMMITTED) != 0U);
    CHECK(s.crc32_computed != s.app_crc32 && s.crc32_computed != 0UL);
    CHECK(wait_until(5000, NULL) == SPL_ACTION_NONE);

    printf("start-up: a record without its commit, a bad record CRC\n");
    clock_with(20000);
    put_word(APP_RECORD_ADDRESS, RECORD_COMMIT + 1, 0x5AA4);
    s = start(0, 0, 0);
    CHECK(s.mode == SML_BOOT_MODE_NO_APP);
    CHECK((s.flags & (SML_BOOT_FLAG_RECORD | SML_BOOT_FLAG_COMMITTED)) == SML_BOOT_FLAG_RECORD);
    clock_with(20000);
    put_word(APP_RECORD_ADDRESS, APP_RECORD_BUILD_TIME, 0x1234);
    s = start(0, 0, 0);
    CHECK(s.mode == SML_BOOT_MODE_NO_APP && (s.flags & SML_BOOT_FLAG_RECORD) == 0U);

    printf("start-up: the SPL's own record\n");
    clock_with(2000);
    install_spl(false, false);
    s = start(0, 0, 0);
    CHECK(s.spl_state == (SML_SPL_STATE_RECORD | SML_SPL_STATE_COMMITTED | SML_SPL_STATE_INTACT));
    install_spl(false, true);
    s = start(0, 0, 0);
    CHECK((s.spl_state & SML_SPL_STATE_REJECTED) != 0U);
    install_spl(true, false);
    host_put(SPL_START + 100U, host_get(SPL_START + 100U) ^ 1U);
    s = start(0, 0, 0);
    CHECK((s.spl_state & SML_SPL_STATE_INTACT) == 0U &&
          (s.spl_state & SML_SPL_STATE_CONFIRMED) != 0U);
}


static void test_commands(void)
{
    SML_BOOT_STATUS_PAYLOAD s;
    SplAction action;
    Response r;
    uint8_t p[SML_BOOT_COMMAND_BYTES];
    uint8_t chunk[BOOT_CHUNK_BYTES];
    uint16_t detail;

    printf("HOLD, STATUS, RUN, RESET, unknown commands\n");
    clock_with(10000);
    s = start(0, 0, 0);
    host_ms = 200;
    r = ask(SML_BOOT_CMD_HOLD, 0, 0, NULL, 0, NULL);
    CHECK(r.status == SML_RESULT_ACK);
    CHECK(wait_until(5000, NULL) == SPL_ACTION_NONE);
    s = status();
    CHECK(s.mode == SML_BOOT_MODE_HELD && (s.flags & SML_BOOT_FLAG_REQUESTED) == 0U);
    r = ask(SML_BOOT_CMD_CONFIRM, 0, SML_BOOT_KEY_COMMIT, NULL, 0, NULL);
    CHECK(r.status == SML_RESULT_NACK && r.detail == SML_ERROR_UNSUPPORTED);
    r = ask(0x7F, 0, 0, NULL, 0, NULL);
    CHECK(r.status == SML_RESULT_NACK && r.detail == SML_ERROR_UNSUPPORTED);
    r = ask(SML_BOOT_CMD_RESET, 0, 0, NULL, 0, &action);
    CHECK(r.status == SML_RESULT_ACK && action == SPL_ACTION_RESET);
    r = ask(SML_BOOT_CMD_RUN, 0, 0, NULL, 0, &action);
    CHECK(r.status == SML_RESULT_ACK && action == SPL_ACTION_HAND_OVER && mailbox.attempts == 1U);

    printf("RUN in the window\n");
    host_ms = 0;
    s = start(0, 0, 0);
    host_ms = 10;
    r = ask(SML_BOOT_CMD_RUN, 0, 0, NULL, 0, &action);
    CHECK(r.status == SML_RESULT_ACK && action == SPL_ACTION_HAND_OVER);

    printf("writing commands in the window, wrong keys and regions\n");
    host_ms = 0;
    s = start(0, 0, 0);
    host_ms = 10;
    CHECK(ask(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_APP, SML_BOOT_KEY_ERASE, NULL, 0, NULL).detail ==
          SML_ERROR_STATE);
    CHECK(data(APP_START, SML_BOOT_REGION_APP, old_image).detail == SML_ERROR_STATE);
    CHECK(ask(SML_BOOT_CMD_HOLD, 0, 0, NULL, 0, NULL).status == SML_RESULT_ACK);
    CHECK(ask(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_APP, SML_BOOT_KEY_COMMIT, NULL, 0, NULL).detail ==
          SML_ERROR_ARGUMENT);
    CHECK(ask(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_SPL, SML_BOOT_KEY_ERASE, NULL, 0, NULL).detail ==
          SML_ERROR_ARGUMENT);
    CHECK(ask(SML_BOOT_CMD_ERASE, 9, SML_BOOT_KEY_ERASE, NULL, 0, NULL).detail ==
          SML_ERROR_ARGUMENT);
    CHECK(ask(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_APP, SML_BOOT_KEY_ERASE, NULL, 0, NULL).detail ==
          SML_ERROR_ARGUMENT);
    CHECK(host_operations() == 0);

    printf("frames the SPL ignores\n");
    clear_out();
    memset(p, 0, sizeof(p));
    p[2] = SML_BOOT_CMD_STATUS;
    send(SML_BOOT_COMMAND, p, sizeof(p) - 1U); // wrong length
    {
        uint8_t frame[64];
        uint8_t user[SML_CMD_USER_BYTES] = {0};
        uint16_t bytes = sml_frame(SML_CLASS_CMD, SML_CMD_USER, user, sizeof(user), frame);

        host_rx_push(frame, bytes);
    }
    send(0x33, p, 4); // an unknown BOOT message
    host_rx_push((const uint8_t *)"text and \xB5 noise", 17);
    run();
    CHECK(frames_left() == 0);
    command(77, SML_BOOT_CMD_STATUS, 0, 0); // after the noise: answered
    run();
    CHECK(response(&r) && r.token == 77 && r.status == SML_RESULT_ACK);

    printf("a part frame, then quiet, then a whole one\n");
    clear_out();
    host_rx_push((const uint8_t *)"\xB5\x73\xF0\x10\x00\x20\x00", 7);
    run();
    host_ms += 150;
    run();
    command(78, SML_BOOT_CMD_STATUS, 0, 0);
    run();
    CHECK(response(&r) && r.token == 78);

    printf("VERIFY\n");
    CHECK(verify(0x000000, chunk, &detail));
    CHECK(verify(APP_START, chunk, &detail));
    CHECK(chunk[0] == (uint8_t)old_image[0] && chunk[5] == (uint8_t)(old_image[1] >> 16));
    CHECK(verify(0x029780, chunk, &detail));
    CHECK(!verify(0x029800, chunk, &detail) && detail == SML_ERROR_ARGUMENT);
    CHECK(!verify(0x002040, chunk, &detail) && detail == SML_ERROR_ARGUMENT);
    CHECK(!verify(0xFF0000, chunk, &detail) && detail == SML_ERROR_ARGUMENT);
}


// A whole application update from HELD, as the ESP does it (8). Returns
// false at the first answer that is not ACK.
static bool update(const uint32_t *image, uint32_t length, uint32_t version)
{
    uint32_t longs[5];
    uint32_t address;
    Response r;

    r = ask(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_APP, SML_BOOT_KEY_ERASE, NULL, 0, NULL);
    if (r.status != SML_RESULT_ACK || r.value != APP_PAGES + 1U)
    {
        return false;
    }
    for (address = 0; address < length; address += BOOT_CHUNK_INSTRUCTIONS)
    {
        DataResponse d;

        if (chunk_blank(image + address))
        {
            continue;
        }
        d = data(APP_START + 2U * address, SML_BOOT_REGION_APP, image + address);
        if (d.status != SML_RESULT_ACK || d.address != APP_START + 2U * address ||
            d.crc32 != words_crc(image + address, BOOT_CHUNK_INSTRUCTIONS))
        {
            return false;
        }
    }
    for (address = 0; address < length; address += BOOT_CHUNK_INSTRUCTIONS)
    {
        uint8_t chunk[BOOT_CHUNK_BYTES];
        uint8_t expected[BOOT_CHUNK_BYTES];
        uint16_t detail;

        pack_chunk(image + address, expected);
        if (!verify(APP_START + 2U * address, chunk, &detail) ||
            memcmp(chunk, expected, BOOT_CHUNK_BYTES) != 0)
        {
            return false;
        }
    }
    commit_args(longs, version, APP_START, length, words_crc(image, length));
    r = ask(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_APP, SML_BOOT_KEY_COMMIT, longs, SML_VERSION,
            NULL);
    return r.status == SML_RESULT_ACK;
}


static void test_update(void)
{
    SML_BOOT_STATUS_PAYLOAD s;
    SplAction action;
    Response r;
    uint32_t longs[5];
    long writes;
    int index;

    printf("a whole update\n");
    clock_with(30000);
    make_image(new_image, 41000, 2U);
    s = start(MAILBOX_REQUEST_STAY, 2, 0);
    host_ms = 100;
    writes = host_writes;
    CHECK(update(new_image, 41000, NEW_VERSION));
    CHECK(host_erases == APP_PAGES + 1);
    CHECK(host_overwrites == 0 && host_refused == 0);
    CHECK(host_writes - writes < 41000 / 2 + 9); // blank pairs skipped
    CHECK(flash_holds(new_image, 41000));
    CHECK(host_get(APP_START + 2U * 41000U) == HOST_ERASED);
    // The record, word for word, as make_image.py writes it.
    {
        uint16_t words[RECORD_CRC_WORDS];

        for (index = 0; index < RECORD_CRC_WORDS; index++)
        {
            words[index] = get_word(APP_RECORD_ADDRESS, index);
        }
        CHECK(words[0] == APP_RECORD_MAGIC_VALUE && words[1] == RECORD_FORMAT_1);
        CHECK(words[2] == 0x0000 && words[3] == 0x0400);
        CHECK(words[4] == 0x0000 && words[5] == 0x2000);
        CHECK(words[6] == 0x0000 && words[7] == 41000);
        CHECK(words[12] == SML_VERSION && words[13] == 0);
        CHECK(get_word(APP_RECORD_ADDRESS, RECORD_CRC) == boot_record_crc(words));
        CHECK(host_get(APP_RECORD_ADDRESS + 2U * RECORD_SPARE) == 0xFFFFFFUL);
        CHECK(host_get(APP_RECORD_ADDRESS + 2U * RECORD_COMMIT) == 0xFFA55AUL);
        CHECK(host_get(APP_RECORD_ADDRESS + 2U * (RECORD_COMMIT + 1)) == 0xFF5AA5UL);
        CHECK(host_get(APP_RECORD_ADDRESS + 2U * APP_RECORD_WORDS) == HOST_ERASED);
    }
    s = status();
    CHECK(s.mode == SML_BOOT_MODE_HELD);
    CHECK((s.flags & (SML_BOOT_FLAG_IMAGE_OK | SML_BOOT_FLAG_COMMITTED)) ==
          (SML_BOOT_FLAG_IMAGE_OK | SML_BOOT_FLAG_COMMITTED));
    CHECK((s.flags & SML_BOOT_FLAG_ERASED) == 0U);
    CHECK(s.app_version == NEW_VERSION && s.attempts == 0U);

    printf("COMMIT again: its ACK lost\n");
    writes = host_writes;
    commit_args(longs, NEW_VERSION, APP_START, 41000, words_crc(new_image, 41000));
    r = ask(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_APP, SML_BOOT_KEY_COMMIT, longs, SML_VERSION,
            NULL);
    CHECK(r.status == SML_RESULT_ACK && host_writes == writes);
    longs[4]++; // a different record: not a repeat
    r = ask(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_APP, SML_BOOT_KEY_COMMIT, longs, SML_VERSION,
            NULL);
    CHECK(r.status == SML_RESULT_NACK && r.detail == SML_ERROR_STATE && host_writes == writes);
    CHECK(data(APP_START + 2U * 41024U, SML_BOOT_REGION_APP, new_image).detail == SML_ERROR_STATE);

    printf("RUN, then the new image from the next start\n");
    r = ask(SML_BOOT_CMD_RUN, 0, 0, NULL, 0, &action);
    CHECK(r.status == SML_RESULT_ACK && action == SPL_ACTION_HAND_OVER && mailbox.attempts == 1U);
    host_ms = 0;
    s = start(0, 1, 0);
    CHECK(s.mode == SML_BOOT_MODE_WINDOW && s.app_version == NEW_VERSION);
    CHECK(wait_until(2000, NULL) == SPL_ACTION_HAND_OVER);

    printf("an update of the whole region, and one too long\n");
    clock_with(1000);
    make_image(new_image, APP_MAX, 3U);
    s = start(MAILBOX_REQUEST_STAY, 0, 0);
    CHECK(update(new_image, APP_MAX, NEW_VERSION));
    CHECK(flash_holds(new_image, APP_MAX) && host_overwrites == 0);
    s = status();
    CHECK((s.flags & SML_BOOT_FLAG_IMAGE_OK) != 0U && (s.flags & SML_BOOT_FLAG_TEST_FREE) == 0U);
    CHECK(ask(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_APP, SML_BOOT_KEY_ERASE, NULL, 0, NULL).status ==
          SML_RESULT_ACK);
    commit_args(longs, NEW_VERSION, APP_START, APP_MAX + 2U, 0);
    CHECK(
        ask(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_APP, SML_BOOT_KEY_COMMIT, longs, 0, NULL).detail ==
        SML_ERROR_ARGUMENT);
    commit_args(longs, NEW_VERSION, APP_START, 101, 0);
    CHECK(
        ask(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_APP, SML_BOOT_KEY_COMMIT, longs, 0, NULL).detail ==
        SML_ERROR_ARGUMENT);
    commit_args(longs, NEW_VERSION, APP_START, 0, 0);
    CHECK(
        ask(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_APP, SML_BOOT_KEY_COMMIT, longs, 0, NULL).detail ==
        SML_ERROR_ARGUMENT);
    commit_args(longs, NEW_VERSION, APP_START + 0x80U, 100, 0);
    CHECK(
        ask(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_APP, SML_BOOT_KEY_COMMIT, longs, 0, NULL).detail ==
        SML_ERROR_ARGUMENT);
    commit_args(longs, NEW_VERSION, APP_START, 100, 0x12345678UL);
    r = ask(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_APP, SML_BOOT_KEY_COMMIT, longs, 0, NULL);
    CHECK(r.status == SML_RESULT_NACK && r.detail == SML_ERROR_VERIFY);
    CHECK(r.value == flash_crc(APP_START, 100));
}


static void test_data(void)
{
    SML_BOOT_STATUS_PAYLOAD s;
    uint32_t words[BOOT_CHUNK_INSTRUCTIONS];
    uint8_t chunk[BOOT_CHUNK_BYTES];
    DataResponse d;
    long writes;
    unsigned index;

    printf("DATA: every answer\n");
    clock_with(5000);
    s = start(MAILBOX_REQUEST_STAY, 0, 0);
    CHECK(data(APP_START, SML_BOOT_REGION_APP, old_image).detail == SML_ERROR_STATE);
    CHECK(ask(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_APP, SML_BOOT_KEY_ERASE, NULL, 0, NULL).status ==
          SML_RESULT_ACK);
    s = status();
    CHECK(s.mode == SML_BOOT_MODE_HELD && (s.flags & SML_BOOT_FLAG_ERASED) != 0U);
    CHECK((s.flags & (SML_BOOT_FLAG_RECORD | SML_BOOT_FLAG_IMAGE_OK | SML_BOOT_FLAG_TEST_FREE)) ==
          0U);

    make_image(new_image, 2048, 4U);
    CHECK(data(APP_START + 0x40U, SML_BOOT_REGION_APP, new_image).detail == SML_ERROR_ARGUMENT);
    CHECK(data(APP_START - 0x80U, SML_BOOT_REGION_APP, new_image).detail == SML_ERROR_ARGUMENT);
    CHECK(data(APP_END, SML_BOOT_REGION_APP, new_image).detail == SML_ERROR_ARGUMENT);
    CHECK(data(SPL_START, SML_BOOT_REGION_SPL, new_image).detail == SML_ERROR_ARGUMENT);
    CHECK(data(APP_START, SML_BOOT_REGION_TEST, new_image).detail == SML_ERROR_ARGUMENT);
    CHECK(data(APP_START, 5, new_image).detail == SML_ERROR_ARGUMENT);
    pack_chunk(new_image, chunk);
    d = data_raw(APP_START, SML_BOOT_REGION_APP, chunk, boot_crc32(chunk, BOOT_CHUNK_BYTES) ^ 1U);
    CHECK(d.status == SML_RESULT_NACK && d.detail == SML_ERROR_CRC && d.address == APP_START);
    CHECK(host_writes == 0);

    d = data(APP_END - 0x80U, SML_BOOT_REGION_APP, new_image);
    CHECK(d.status == SML_RESULT_ACK && d.address == APP_END - 0x80U);
    writes = host_writes;
    d = data(APP_END - 0x80U, SML_BOOT_REGION_APP, new_image);
    CHECK(d.status == SML_RESULT_ACK && host_writes == writes); // a repeat: nothing written
    d = data(APP_END - 0x80U, SML_BOOT_REGION_APP, new_image + 64);
    CHECK(d.status == SML_RESULT_NACK && d.detail == SML_ERROR_NOT_BLANK && host_writes == writes);

    for (index = 0; index < BOOT_CHUNK_INSTRUCTIONS; index++)
    {
        words[index] = (index == 10U || index == 41U) ? 0x000123UL : HOST_ERASED;
    }
    writes = host_writes;
    CHECK(data(APP_START + 0x100U, SML_BOOT_REGION_APP, words).status == SML_RESULT_ACK);
    CHECK(host_writes - writes == 2); // only the pairs that hold something
    CHECK(host_get(APP_START + 0x100U + 20U) == 0x000123UL);

    host_fail_at = host_operations();
    d = data(APP_START + 0x200U, SML_BOOT_REGION_APP, new_image);
    CHECK(d.status == SML_RESULT_NACK && d.detail == SML_ERROR_STORAGE);
    host_reset_faults();
    host_corrupt_at = host_operations() + 3;
    d = data(APP_START + 0x280U, SML_BOOT_REGION_APP, new_image);
    CHECK(d.status == SML_RESULT_NACK && d.detail == SML_ERROR_VERIFY);
    host_reset_faults();
    CHECK(host_overwrites == 0);

    printf("ERASE: failures, and the blank check\n");
    host_fail_at = host_operations() + 5;
    {
        Response r =
            ask(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_APP, SML_BOOT_KEY_ERASE, NULL, 0, NULL);

        CHECK(r.status == SML_RESULT_NACK && r.detail == SML_ERROR_STORAGE && r.value == 5U);
    }
    s = status();
    CHECK((s.flags & SML_BOOT_FLAG_ERASED) == 0U);
    CHECK(data(APP_START, SML_BOOT_REGION_APP, new_image).detail == SML_ERROR_STATE);
    host_reset_faults();
    host_dirty_at = host_operations() + 40;
    {
        Response r =
            ask(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_APP, SML_BOOT_KEY_ERASE, NULL, 0, NULL);

        CHECK(r.status == SML_RESULT_NACK && r.detail == SML_ERROR_STORAGE && r.value == 79U);
    }
    host_reset_faults();
    host_dirty_at = host_operations(); // the record's page
    {
        Response r =
            ask(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_APP, SML_BOOT_KEY_ERASE, NULL, 0, NULL);

        CHECK(r.status == SML_RESULT_NACK && r.detail == SML_ERROR_STORAGE && r.value == 79U);
    }
    host_reset_faults();
    CHECK(ask(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_APP, SML_BOOT_KEY_ERASE, NULL, 0, NULL).status ==
          SML_RESULT_ACK);

    printf("COMMIT: a failed record write\n");
    make_image(new_image, 128, 5U);
    CHECK(data(APP_START, SML_BOOT_REGION_APP, new_image).status == SML_RESULT_ACK);
    CHECK(data(APP_START + 0x80U, SML_BOOT_REGION_APP, new_image + 64).status == SML_RESULT_ACK);
    {
        uint32_t longs[5];
        Response r;

        commit_args(longs, NEW_VERSION, APP_START, 128, words_crc(new_image, 128));
        host_fail_at = host_operations() + 8; // the commit
        r = ask(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_APP, SML_BOOT_KEY_COMMIT, longs, 0, NULL);
        CHECK(r.status == SML_RESULT_NACK && r.detail == SML_ERROR_STORAGE);
        host_reset_faults();
        s = status();
        CHECK((s.flags & SML_BOOT_FLAG_IMAGE_OK) == 0U &&
              (s.flags & SML_BOOT_FLAG_COMMITTED) == 0U);
        r = ask(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_APP, SML_BOOT_KEY_COMMIT, longs, 0, NULL);
        CHECK(r.status == SML_RESULT_NACK && r.detail == SML_ERROR_STATE); // erase first
        CHECK(ask(SML_BOOT_CMD_RUN, 0, 0, NULL, 0, NULL).detail == SML_ERROR_STATE);
        CHECK(ask(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_APP, SML_BOOT_KEY_ERASE, NULL, 0, NULL)
                  .status == SML_RESULT_ACK);
        CHECK(data(APP_START, SML_BOOT_REGION_APP, new_image).status == SML_RESULT_ACK);
        CHECK(data(APP_START + 0x80U, SML_BOOT_REGION_APP, new_image + 64).status ==
              SML_RESULT_ACK);
        host_corrupt_at = host_operations() + 2;
        r = ask(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_APP, SML_BOOT_KEY_COMMIT, longs, 0, NULL);
        CHECK(r.status == SML_RESULT_NACK && r.detail == SML_ERROR_VERIFY);
        host_reset_faults();
    }
    CHECK(host_overwrites == 0 && host_refused == 0);

    printf("COMMIT after a restart, without its ERASE\n");
    clock_with(5000);
    make_image(new_image, 128, 5U);
    (void)start(MAILBOX_REQUEST_STAY, 0, 0);
    CHECK(ask(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_APP, SML_BOOT_KEY_ERASE, NULL, 0, NULL).status ==
          SML_RESULT_ACK);
    CHECK(data(APP_START, SML_BOOT_REGION_APP, new_image).status == SML_RESULT_ACK);
    CHECK(data(APP_START + 0x80U, SML_BOOT_REGION_APP, new_image + 64).status == SML_RESULT_ACK);
    (void)start(0, 0, 0x0083); // the power went
    {
        uint32_t longs[5];
        Response r;

        commit_args(longs, NEW_VERSION, APP_START, 128, words_crc(new_image, 128));
        CHECK(ask(SML_BOOT_CMD_HOLD, 0, 0, NULL, 0, NULL).status == SML_RESULT_ACK);
        r = ask(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_APP, SML_BOOT_KEY_COMMIT, longs, 0, NULL);
        CHECK(r.status == SML_RESULT_NACK && r.detail == SML_ERROR_STATE);
        CHECK(host_get(APP_RECORD_ADDRESS) == HOST_ERASED);
    }
}


static void test_invariant(void)
{
    SML_BOOT_STATUS_PAYLOAD s;
    Response r;

    printf("an unconfirmed SPL never erases a valid application\n");
    clock_with(9000);
    install_spl(false, false);
    s = start(MAILBOX_REQUEST_STAY, 0, 0);
    r = ask(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_APP, SML_BOOT_KEY_ERASE, NULL, 0, NULL);
    CHECK(r.status == SML_RESULT_NACK && r.detail == SML_ERROR_STATE && host_operations() == 0);
    CHECK(ask(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_TEST, SML_BOOT_KEY_ERASE, NULL, 0, NULL).status ==
          SML_RESULT_ACK);

    printf("but erases one that is not valid\n");
    host_put(APP_START + 0x400U, 0);
    s = start(MAILBOX_REQUEST_STAY, 0, 0);
    CHECK((s.flags & SML_BOOT_FLAG_COMMITTED) != 0U);
    r = ask(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_APP, SML_BOOT_KEY_ERASE, NULL, 0, NULL);
    CHECK(r.status == SML_RESULT_ACK && r.value == APP_PAGES + 1U);
    host_reset();
    install_ipl();
    install_spl(false, false);
    s = start(0, 0, 0);
    CHECK(s.mode == SML_BOOT_MODE_NO_APP);
    r = ask(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_APP, SML_BOOT_KEY_ERASE, NULL, 0, NULL);
    CHECK(r.status == SML_RESULT_ACK);
}


static void test_region_test(void)
{
    SML_BOOT_STATUS_PAYLOAD s;
    uint32_t pattern[TEST_CODE_MAX];
    uint32_t longs[5];
    uint32_t address;
    uint32_t app_crc;
    Response r;

    printf("the test region\n");
    clock_with(30000);
    app_crc = flash_crc(APP_START, 30000);
    s = start(MAILBOX_REQUEST_STAY, 0, 0);
    CHECK((s.flags & SML_BOOT_FLAG_TEST_FREE) != 0U);
    CHECK(data(TEST_START, SML_BOOT_REGION_TEST, old_image).detail == SML_ERROR_STATE);
    r = ask(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_TEST, SML_BOOT_KEY_ERASE, NULL, 0, NULL);
    CHECK(r.status == SML_RESULT_ACK && r.value == 1U && host_erases == 1);
    make_image(pattern, TEST_CODE_MAX, 6U);
    for (address = 0; address < TEST_CODE_MAX; address += BOOT_CHUNK_INSTRUCTIONS)
    {
        CHECK(data(TEST_START + 2U * address, SML_BOOT_REGION_TEST, pattern + address).status ==
                  SML_RESULT_ACK ||
              chunk_blank(pattern + address));
    }
    CHECK(data(TEST_RECORD_ADDRESS, SML_BOOT_REGION_TEST, pattern).detail == SML_ERROR_ARGUMENT);
    CHECK(data(APP_START, SML_BOOT_REGION_APP, pattern).detail == SML_ERROR_STATE);
    commit_args(longs, 0x010203UL, TEST_START, TEST_CODE_MAX + 2U, 0);
    CHECK(ask(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_TEST, SML_BOOT_KEY_COMMIT, longs, 0, NULL)
              .detail == SML_ERROR_ARGUMENT);
    commit_args(longs, 0x010203UL, APP_START, TEST_CODE_MAX, 0);
    CHECK(ask(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_TEST, SML_BOOT_KEY_COMMIT, longs, 0, NULL)
              .detail == SML_ERROR_ARGUMENT);
    commit_args(longs, 0x010203UL, TEST_START, TEST_CODE_MAX, words_crc(pattern, TEST_CODE_MAX));
    r = ask(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_TEST, SML_BOOT_KEY_COMMIT, longs, 0x0205, NULL);
    CHECK(r.status == SML_RESULT_ACK);
    CHECK(get_word(TEST_RECORD_ADDRESS, 0) == APP_RECORD_MAGIC_VALUE);
    CHECK(get_word(TEST_RECORD_ADDRESS, APP_RECORD_START + 1) == (uint16_t)TEST_START);
    CHECK(host_get(TEST_RECORD_ADDRESS + 2U * RECORD_COMMIT) == 0xFFA55AUL);
    CHECK(flash_crc(APP_START, 30000) == app_crc); // the application untouched
    s = status();
    CHECK((s.flags & SML_BOOT_FLAG_IMAGE_OK) != 0U && s.app_version == OLD_VERSION);
    CHECK((s.flags & SML_BOOT_FLAG_ERASED) == 0U && s.mode == SML_BOOT_MODE_HELD);
    r = ask(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_TEST, SML_BOOT_KEY_ERASE, NULL, 0, NULL);
    CHECK(r.status == SML_RESULT_ACK);
    CHECK(host_get(TEST_RECORD_ADDRESS) == HOST_ERASED && host_overwrites == 0);
    CHECK(flash_crc(APP_START, 30000) == app_crc);

    printf("the test region, while the application reaches it\n");
    clock_with(TEST_START / 2U - APP_START / 2U + 2U);
    s = start(MAILBOX_REQUEST_STAY, 0, 0);
    CHECK((s.flags & SML_BOOT_FLAG_TEST_FREE) == 0U);
    r = ask(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_TEST, SML_BOOT_KEY_ERASE, NULL, 0, NULL);
    CHECK(r.status == SML_RESULT_NACK && r.detail == SML_ERROR_STATE && host_operations() == 0);
    clock_with(TEST_START / 2U - APP_START / 2U);
    s = start(MAILBOX_REQUEST_STAY, 0, 0);
    CHECK((s.flags & SML_BOOT_FLAG_TEST_FREE) != 0U);
}


static void test_status_and_led(void)
{
    SML_BOOT_STATUS_PAYLOAD s;
    int changes;
    int count = 0;

    printf("BOOT-STATUS every second while idle; the LED\n");
    clock_with(4000);
    host_ms = 0;
    s = start(MAILBOX_REQUEST_STAY, 0, 0);
    clear_out();
    changes = host_led_changes;
    CHECK(wait_until(5000, NULL) == SPL_ACTION_NONE);
    while (next_status(&s))
    {
        count++;
    }
    CHECK(count == 5);
    CHECK(host_led_changes - changes >= 38 && host_led_changes - changes <= 41); // 4 Hz
    host_reset();
    install_ipl();
    install_spl(true, false);
    host_ms = 0;
    s = start(0, 0, 0);
    changes = host_led_changes;
    CHECK(wait_until(3000, NULL) == SPL_ACTION_NONE);
    CHECK(host_led_changes - changes >= 11 && host_led_changes - changes <= 13); // twice a second
    clock_with(4000);
    host_ms = 0;
    s = start(0, 0, 0);
    changes = host_led_changes;
    CHECK(wait_until(900, NULL) == SPL_ACTION_NONE);
    CHECK(host_led_changes == changes && !host_led); // the window: dark
}


static void test_power_cuts(void)
{
    static jmp_buf jump;
    static long cut;
    static int mode;
    static int mixed;
    static int old_kept;
    static int new_found;
    static int none;
    long operations;

    printf("a power cut at every flash operation of an update\n");
    make_image(new_image, 3000, 8U);
    clock_with(2500);
    (void)start(MAILBOX_REQUEST_STAY, 0, 0);
    operations = host_operations();
    CHECK(update(new_image, 3000, NEW_VERSION));
    operations = host_operations() - operations;

    for (cut = 0; cut < operations * 3; cut++)
    {
        SML_BOOT_STATUS_PAYLOAD s;

        mode = (int)(cut % 3);
        clock_with(2500);
        (void)start(MAILBOX_REQUEST_STAY, 0, 0);
        host_cut_mode = mode;
        host_cut_at = host_operations() + cut / 3;
        host_cut_jump = &jump;
        if (setjmp(jump) == 0)
        {
            (void)update(new_image, 3000, NEW_VERSION);
            CHECK(!"no cut");
        }
        host_cut_jump = NULL;
        host_reset_faults();

        // The power comes back: the IPL starts the SPL, which must take
        // the old image or the new one, whole, or none.
        host_ms = 0;
        s = start(0, 0, 0x0083);
        s = status();
        if ((s.flags & SML_BOOT_FLAG_IMAGE_OK) != 0U)
        {
            if (s.app_version == OLD_VERSION && flash_holds(old_image, 2500) &&
                s.app_length == 2500U)
            {
                old_kept++;
            }
            else if (s.app_version == NEW_VERSION && flash_holds(new_image, 3000) &&
                     s.app_length == 3000U)
            {
                new_found++;
            }
            else
            {
                mixed++;
            }
            CHECK(s.mode == SML_BOOT_MODE_WINDOW);
        }
        else
        {
            none++;
            CHECK(s.mode == SML_BOOT_MODE_NO_APP);
            CHECK(wait_until(3000, NULL) == SPL_ACTION_NONE);
        }
        // And the update can always be done again.
        CHECK(ask(SML_BOOT_CMD_HOLD, 0, 0, NULL, 0, NULL).status == SML_RESULT_ACK);
        CHECK(update(new_image, 3000, NEW_VERSION));
        s = status();
        CHECK((s.flags & SML_BOOT_FLAG_IMAGE_OK) != 0U && s.app_version == NEW_VERSION);
    }
    CHECK(mixed == 0);
    CHECK(old_kept >= 1 && new_found >= 1 && none > 3000);
    printf("  %ld operations, 3 ways each: old image %d, new %d, none %d\n", operations, old_kept,
           new_found, none);
}


int main(void)
{
    test_guard();
    test_window();
    test_no_app();
    test_commands();
    test_update();
    test_data();
    test_invariant();
    test_region_test();
    test_status_and_led();
    test_power_cuts();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
