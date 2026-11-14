/*
 * The ESP's PIC update (src/pic/pic_update.c) against the PIC's own SPL
 * (dspic_spl/spl_core.c on its host port's flash model) and a stand-in for
 * the PIC's application, wired together a millisecond at a time:
 *   - an update from the running application, from an SPL already held, in
 *     its window, with no application;
 *   - an image already there, forced again;
 *   - every refusal: no SPL, an unconfirmed one, another DEVID, too long;
 *   - answers lost and frames damaged on the way, BUSY;
 *   - the PIC reset part-way, three passes used up;
 *   - a power cut at flash operations throughout an update, both chips
 *     starting again, and the update then always completing.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../dspic_spl/spl_core.h"
#include "../../../dspic_spl/spl_port.h"
#include "../../../dspic_spl/tests/host/port_host.h"
#include "../../src/pic/pic_update.h"
#include "../../src/pic/sml/boot_crc.h"
#include "../../src/pic/sml/boot_layout.h"
#include "../../src/pic/sml/sml.h"

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

#define OLD_VERSION 0x000301UL
#define NEW_VERSION 0x000400UL

// ---- The PIC: SPL core, or the application stand-in -------------------------------

static BootMailbox mailbox;
static bool in_app;
static bool app_sml;           // the application's link in SML mode
static uint32_t app_next_sys;  // its next STAT-SYS
static uint16_t app_nack;      // the application answers ENTER with this (0: ACK)
static int app_busy;           // ... BUSY this many times first
static uint32_t app_reports;   // the version STAT-SYS reports (0: the record's)
static SmlParser app_parser;
static long resets;

static void put_word(uint32_t record, unsigned index, uint16_t value)
{
    host_put(record + 2U * index, 0xFF0000UL | value);
}


static uint32_t record_version(void)
{
    return (host_get(APP_RECORD_ADDRESS + 4) & 0xFFFF) << 16 |
           (host_get(APP_RECORD_ADDRESS + 6) & 0xFFFF);
}


static void pic_send(uint8_t class_id, uint8_t id, const uint8_t *payload, uint16_t length)
{
    uint8_t frame[SML_MAX_FRAME_BYTES];
    uint16_t bytes = sml_frame(class_id, id, payload, length, frame);

    spl_port_tx(frame, bytes);
}


// A reset of the PIC: the IPL (here: the mailbox it leaves) and the SPL.
static void pic_reset(uint16_t cause, bool request)
{
    if ((cause & BOOT_RESET_POWER) != 0U)
    {
        memset(&mailbox, 0, sizeof(mailbox));
    }
    mailbox.magic = MAILBOX_MAGIC_VALUE;
    mailbox.magic_inverse = (uint16_t)~MAILBOX_MAGIC_VALUE;
    mailbox.reset_cause = cause;
    if (request)
    {
        mailbox.request = MAILBOX_REQUEST_STAY;
    }
    in_app = false;
    resets++;
    host_reset_link();
    spl_init(&mailbox);
}


static void app_start(void)
{
    static const char banner[] = "\x1b[2J\x1b[1;1HPIC 0.3.1, up 00:00:00\r\n";

    in_app = true;
    app_sml = false;
    sml_parser_init(&app_parser);
    spl_port_tx((const uint8_t *)banner, sizeof(banner) - 1);
}


static void app_stat_sys(void)
{
    uint8_t p[SML_STAT_SYS_BYTES] = {0};

    sml_put_u32(&p[8], app_reports != 0 ? app_reports : record_version());
    pic_send(SML_CLASS_STAT, SML_STAT_SYS, p, sizeof(p));
}


static void app_poll(void)
{
    uint8_t byte;
    SmlFrame frame;

    if (app_sml && (int32_t)(host_ms - app_next_sys) >= 0)
    {
        app_stat_sys();
        app_next_sys = host_ms + 1000;
    }
    while (spl_port_rx(&byte))
    {
        uint16_t command;
        uint8_t r[SML_CMD_RESPONSE_BYTES] = {0};

        if (!sml_parser_push(&app_parser, byte, &frame))
        {
            continue;
        }
        if (!app_sml)
        {
            app_sml = true;
            app_next_sys = host_ms + 1000;
        }
        if (frame.class_id != SML_CLASS_CMD || frame.message_id != SML_CMD_USER ||
            frame.length != SML_CMD_USER_BYTES)
        {
            continue; // the application's other messages; BOOT-* it ignores (phase 7)
        }
        command = sml_get_u16(frame.payload);
        memcpy(&r[4], &frame.payload[4], 2);
        sml_put_u16(&r[6], command);
        if (command == SML_USER_REQUEST_MESSAGE)
        {
            app_stat_sys();
            pic_send(SML_CLASS_CMD, SML_CMD_RESPONSE, r, sizeof(r));
        }
        else if (command == SML_USER_ENTER_BOOTLOADER)
        {
            if (app_busy > 0 || app_nack != 0)
            {
                sml_put_u16(&r[0], SML_RESULT_NACK);
                sml_put_u16(&r[2], app_busy > 0 ? SML_ERROR_BUSY : app_nack);
                app_busy -= app_busy > 0;
                pic_send(SML_CLASS_CMD, SML_CMD_RESPONSE, r, sizeof(r));
                continue;
            }
            pic_send(SML_CLASS_CMD, SML_CMD_RESPONSE, r, sizeof(r));
            pic_reset(0x0040, true);
            return;
        }
    }
}


static void pic_poll(void)
{
    if (in_app)
    {
        app_poll();
        return;
    }
    switch (spl_poll())
    {
    case SPL_ACTION_HAND_OVER:
        mailbox.running = MAILBOX_RUNNING_VALUE;
        app_start();
        break;
    case SPL_ACTION_RESET:
        pic_reset(0x0040, false);
        break;
    default:
        break;
    }
}

// ---- The ESP: the update, and the link between them ------------------------------------

static uint8_t image[3 * 79872 + 192];
static PicFileHeader header;
static PicUpdate update;
static SmlParser esp_parser;
static uint32_t pic_tx_read;
// Faults on the wire.
static long drop_from_pic = -1;  // the n-th frame from the PIC is lost
static long drop_every = 0;      // ... and every one after it, if set
static long damage_to_pic = -1;  // the n-th frame to the PIC arrives damaged
static uint8_t drop_id = 0xFF;   // only frames with this ID count (0xFF: all)
static uint8_t drop_class = SML_CLASS_BOOT; // ... of this class
static long from_pic_count;
static long to_pic_count;
static long frames_to_pic;

static void esp_send(void *context, uint8_t class_id, uint8_t id, const uint8_t *payload,
                     uint16_t length)
{
    uint8_t frame[SML_MAX_FRAME_BYTES];
    uint16_t bytes = sml_frame(class_id, id, payload, length, frame);

    (void)context;
    frames_to_pic++;
    if (drop_id == 0xFF || (id == drop_id && class_id == drop_class))
    {
        if (to_pic_count++ == damage_to_pic)
        {
            frame[10] ^= 0x40;
        }
    }
    host_rx_push(frame, bytes);
}


static bool esp_read(void *context, uint32_t offset, uint8_t *data, uint32_t length)
{
    (void)context;
    if (offset + length > 3 * header.length)
    {
        return false;
    }
    memcpy(data, image + offset, length);
    return true;
}


static const PicUpdatePort port = {esp_send, esp_read, NULL};

// What the PIC sent, to the update (and the ESP's own record of BOOT-STATUS).
static bool spl_seen; // a BOOT-STATUS since the last STAT-SYS
static void esp_receive(void)
{
    SmlFrame frame;

    while (pic_tx_read < host_tx_length)
    {
        if (!sml_parser_push(&esp_parser, host_tx[pic_tx_read++], &frame))
        {
            continue;
        }
        if (frame.class_id == SML_CLASS_BOOT && frame.message_id == SML_BOOT_STATUS)
        {
            spl_seen = frame.payload[5] != SML_BOOT_MODE_APP;
        }
        if (frame.class_id == SML_CLASS_STAT && frame.message_id == SML_STAT_SYS)
        {
            spl_seen = false;
        }
        if (drop_id == 0xFF || (frame.message_id == drop_id && frame.class_id == drop_class))
        {
            long n = from_pic_count++;

            if (n == drop_from_pic || (drop_every != 0 && drop_from_pic >= 0 && n > drop_from_pic))
            {
                continue;
            }
        }
        pic_update_frame(&update, frame.class_id, frame.message_id, frame.payload, frame.length,
                         host_ms);
    }
    if (pic_tx_read == host_tx_length)
    {
        host_tx_length = 0;
        pic_tx_read = 0;
    }
}


static void step(void)
{
    host_ms++;
    pic_poll();
    esp_receive();
    pic_update_service(&update, host_ms);
    esp_receive();
}


// Runs the update to its end, or for at most limit ms.
static void run(uint32_t limit)
{
    uint32_t end = host_ms + limit;

    while (pic_update_running(&update) && host_ms != end)
    {
        step();
    }
}

// ---- Flash contents ----------------------------------------------------------------------

static uint32_t words_crc(const uint32_t *words, uint32_t length)
{
    uint32_t crc = BOOT_CRC32_INIT;
    uint32_t index;

    for (index = 0; index < length; index++)
    {
        uint8_t bytes[3] = {(uint8_t)words[index], (uint8_t)(words[index] >> 8),
                            (uint8_t)(words[index] >> 16)};

        crc = boot_crc32_update(crc, bytes, 3);
    }
    return ~crc;
}


static void put_record(uint32_t record, uint16_t magic, const uint32_t *longs, int count,
                       uint16_t extra, bool spl, bool confirmed)
{
    uint16_t words[RECORD_CRC_WORDS] = {0};
    int index;

    words[0] = magic;
    words[1] = RECORD_FORMAT_1;
    for (index = 0; index < count; index++)
    {
        words[2 + 2 * index] = (uint16_t)(longs[index] >> 16);
        words[3 + 2 * index] = (uint16_t)longs[index];
    }
    words[spl ? SPL_RECORD_PROTOCOL : APP_RECORD_SML_VERSION] = extra;
    for (index = 0; index < RECORD_CRC_WORDS; index++)
    {
        put_word(record, (unsigned)index, words[index]);
    }
    put_word(record, RECORD_CRC, boot_record_crc(words));
    put_word(record, RECORD_SPARE, 0xFFFF);
    put_word(record, RECORD_COMMIT, RECORD_COMMIT_0);
    put_word(record, RECORD_COMMIT + 1, RECORD_COMMIT_1);
    if (spl && confirmed)
    {
        put_word(record, SPL_RECORD_CONFIRM, SPL_CONFIRM_0);
        put_word(record, SPL_RECORD_CONFIRM + 1, SPL_CONFIRM_1);
    }
}


static uint32_t old_words[8000];

// The clock as delivered: the IPL's info block, an SPL, an application.
static void clock_with(bool confirmed, bool with_app)
{
    uint32_t code[1700];
    uint32_t longs[5];
    uint32_t index;

    host_reset();
    put_word(IPL_INFO_ADDRESS, RECORD_MAGIC, IPL_INFO_MAGIC_VALUE);
    put_word(IPL_INFO_ADDRESS, RECORD_FORMAT, RECORD_FORMAT_1);
    put_word(IPL_INFO_ADDRESS, IPL_INFO_VERSION + 1, 0x0100);
    srand(7);
    for (index = 0; index < 1700; index++)
    {
        code[index] = (uint32_t)rand() & 0xFFFFFF;
        host_put(SPL_START + 2 * index, code[index]);
    }
    longs[0] = 0x000100;
    longs[1] = 1700;
    longs[2] = words_crc(code, 1700);
    longs[3] = 1793000000;
    put_record(SPL_RECORD_ADDRESS, SPL_RECORD_MAGIC_VALUE, longs, 4, 1, true, confirmed);
    if (with_app)
    {
        srand(1);
        for (index = 0; index < 8000; index++)
        {
            old_words[index] = (uint32_t)rand() & 0xFFFFFE;
            host_put(APP_START + 2 * index, old_words[index]);
        }
        longs[0] = OLD_VERSION;
        longs[1] = APP_START;
        longs[2] = 8000;
        longs[3] = words_crc(old_words, 8000);
        longs[4] = 1793000000;
        put_record(APP_RECORD_ADDRESS, APP_RECORD_MAGIC_VALUE, longs, 5, 0x0205, false, false);
    }
}


// The new image: random words, a few blank chunks, as make_image.py packs it.
static void make_image(uint32_t length, uint32_t version)
{
    uint32_t index;

    srand(2);
    memset(image, 0xFF, sizeof(image));
    for (index = 0; index < length; index++)
    {
        uint32_t word = (index / 64) % 9 == 4 ? 0xFFFFFF : ((uint32_t)rand() & 0xFFFFFE);

        image[3 * index] = (uint8_t)word;
        image[3 * index + 1] = (uint8_t)(word >> 8);
        image[3 * index + 2] = (uint8_t)(word >> 16);
    }
    memset(&header, 0, sizeof(header));
    header.kind = PIC_FILE_APP;
    header.device_id = 0x1F6C;
    header.start = APP_START;
    header.length = length;
    header.version = version;
    header.crc32 = boot_crc32(image, 3 * length);
    header.build_time = 1793100000;
    header.sml_version = 0x0205;
}


static bool flash_holds_image(void)
{
    uint32_t index;

    for (index = 0; index < header.length; index++)
    {
        uint32_t word = image[3 * index] | (uint32_t)image[3 * index + 1] << 8 |
                        (uint32_t)image[3 * index + 2] << 16;

        if (host_get(APP_START + 2 * index) != word)
        {
            return false;
        }
    }
    return true;
}


static void reset_faults(void)
{
    drop_from_pic = -1;
    drop_every = 0;
    damage_to_pic = -1;
    drop_id = 0xFF;
    drop_class = SML_CLASS_BOOT;
    from_pic_count = 0;
    to_pic_count = 0;
    app_nack = 0;
    app_busy = 0;
    app_reports = 0;
    host_reset_faults();
}


// The clock running its application; the update started from it.
static void start_from_app(bool confirmed, uint32_t length, uint32_t version, bool force)
{
    clock_with(confirmed, true);
    reset_faults();
    host_ms = 0;
    memset(&mailbox, 0, sizeof(mailbox));
    pic_reset(0x0083, false);
    while (!in_app && host_ms < 3000)
    {
        host_ms++;
        pic_poll();
    }
    host_tx_length = 0;
    pic_tx_read = 0;
    sml_parser_init(&esp_parser);
    spl_seen = false;
    resets = 0;
    frames_to_pic = 0;
    make_image(length, version);
    pic_update_start(&update, &port, &header, force, false, host_ms);
}

// ---- Tests ----------------------------------------------------------------------------------

static void test_update(void)
{
    long erases;

    printf("an update from the running application\n");
    start_from_app(true, 30000, NEW_VERSION, false);
    CHECK(in_app);
    run(60000);
    CHECK(update.step == PIC_UPDATE_DONE && !update.already && update.passes == 0);
    CHECK(flash_holds_image() && record_version() == NEW_VERSION);
    CHECK(in_app && host_overwrites == 0 && resets == 1);
    CHECK(update.written > 400 && update.written < 469); // blank chunks skipped
    printf("  %lu ms, %ld frames to the PIC\n", (unsigned long)host_ms, frames_to_pic);

    printf("the same image again, then forced\n");
    erases = host_erases;
    pic_update_start(&update, &port, &header, false, false, host_ms);
    run(30000);
    CHECK(update.step == PIC_UPDATE_DONE && update.already && host_erases == erases);
    pic_update_start(&update, &port, &header, true, false, host_ms);
    run(60000);
    CHECK(update.step == PIC_UPDATE_DONE && !update.already && host_erases == erases + 79);
    CHECK(flash_holds_image());

    printf("the whole region\n");
    start_from_app(true, 79872, NEW_VERSION, false);
    run(120000);
    CHECK(update.step == PIC_UPDATE_DONE && flash_holds_image());
}


static void test_in_spl(void)
{
    printf("the PIC already in its SPL: held, in its window, with no application\n");
    start_from_app(true, 5000, NEW_VERSION, false);
    // ENTER_BOOTLOADER by hand earlier: held.
    pic_reset(0x0040, true);
    pic_update_start(&update, &port, &header, false, true, host_ms);
    run(60000);
    CHECK(update.step == PIC_UPDATE_DONE && flash_holds_image());

    // Reset: the window is running when the update starts.
    start_from_app(true, 5000, NEW_VERSION, false);
    pic_reset(0x0080, false);
    host_ms += 100;
    pic_update_start(&update, &port, &header, false, true, host_ms);
    run(60000);
    CHECK(update.step == PIC_UPDATE_DONE && flash_holds_image() && update.passes == 0);

    // No application at all.
    clock_with(true, false);
    reset_faults();
    host_ms = 0;
    pic_reset(0x0083, false);
    make_image(5000, NEW_VERSION);
    pic_update_start(&update, &port, &header, false, true, host_ms);
    run(60000);
    CHECK(update.step == PIC_UPDATE_DONE && flash_holds_image());
}


static void test_refusals(void)
{
    printf("refusals\n");
    start_from_app(true, 5000, NEW_VERSION, false);
    app_nack = SML_ERROR_STATE;
    run(10000);
    CHECK(update.step == PIC_UPDATE_FAILED && strstr(update.reason, "no usable SPL") != NULL);
    CHECK(in_app);

    start_from_app(true, 5000, NEW_VERSION, false);
    app_busy = 2;
    run(60000);
    CHECK(update.step == PIC_UPDATE_DONE);

    start_from_app(false, 5000, NEW_VERSION, false);
    run(10000);
    CHECK(update.step == PIC_UPDATE_FAILED && strstr(update.reason, "not confirmed") != NULL);
    CHECK(host_erases == 0);
    // Forced, the SPL refuses it itself: the invariant.
    start_from_app(false, 5000, NEW_VERSION, true);
    run(10000);
    CHECK(update.step == PIC_UPDATE_FAILED && strstr(update.reason, "ERASE: NACK STATE") != NULL);
    CHECK(host_erases == 0 && record_version() == OLD_VERSION);

    start_from_app(true, 5000, NEW_VERSION, false);
    header.device_id = 0x1F6D;
    pic_update_start(&update, &port, &header, false, false, host_ms);
    run(10000);
    CHECK(update.step == PIC_UPDATE_FAILED && strstr(update.reason, "DEVID") != NULL);

    start_from_app(true, 5000, NEW_VERSION, false);
    header.length = 79874;
    pic_update_start(&update, &port, &header, false, false, host_ms);
    run(10000);
    CHECK(update.step == PIC_UPDATE_FAILED && strstr(update.reason, "fit") != NULL);

    printf("the new application reports another version\n");
    start_from_app(true, 5000, NEW_VERSION, false);
    app_reports = 0x000999;
    run(60000);
    CHECK(update.step == PIC_UPDATE_FAILED && strstr(update.reason, "0.9.153") != NULL);

    printf("no answer from the application\n");
    start_from_app(true, 5000, NEW_VERSION, false);
    drop_class = SML_CLASS_CMD;
    drop_id = SML_CMD_RESPONSE;
    drop_from_pic = 0;
    drop_every = 1;
    // Every CMD-RESPONSE lost: ENTER_BOOTLOADER's ACK too. The PIC acts on
    // it all the same, and the update finds the SPL by its BOOT-STATUS.
    run(60000);
    CHECK(update.step == PIC_UPDATE_DONE);
}


static void test_faults(void)
{
    printf("answers lost, frames damaged\n");
    start_from_app(true, 8000, NEW_VERSION, false);
    drop_id = SML_BOOT_DATA_RESPONSE;
    drop_from_pic = 17;
    run(60000);
    CHECK(update.step == PIC_UPDATE_DONE && update.passes == 0 && flash_holds_image());

    start_from_app(true, 8000, NEW_VERSION, false);
    drop_id = SML_BOOT_DATA;
    damage_to_pic = 30;
    run(60000);
    CHECK(update.step == PIC_UPDATE_DONE && update.passes == 0 && flash_holds_image());

    start_from_app(true, 8000, NEW_VERSION, false);
    drop_id = SML_BOOT_VERIFY_RESPONSE;
    drop_from_pic = 5;
    run(60000);
    CHECK(update.step == PIC_UPDATE_DONE && update.passes == 0);

    start_from_app(true, 8000, NEW_VERSION, false);
    drop_id = SML_BOOT_RESPONSE; // ERASE's ACCEPTED: the ACK still ends it
    drop_from_pic = 1;
    run(60000);
    CHECK(update.step == PIC_UPDATE_DONE);

    printf("a write that goes wrong on the PIC\n");
    start_from_app(true, 8000, NEW_VERSION, false);
    host_corrupt_at = host_operations() + 200;
    run(60000);
    CHECK(update.step == PIC_UPDATE_DONE && update.passes == 1 && flash_holds_image());
    CHECK(strstr(update.reason, "DATA") != NULL);

    printf("three passes used up\n");
    start_from_app(true, 8000, NEW_VERSION, false);
    drop_id = SML_BOOT_DATA_RESPONSE;
    drop_from_pic = 10;
    drop_every = 1;
    run(120000);
    CHECK(update.step == PIC_UPDATE_FAILED && strstr(update.reason, "gave up after 3") != NULL);
    // The SPL keeps waiting, with no application, for the next attempt.
    CHECK(!in_app && spl_seen);
    reset_faults();
    pic_update_start(&update, &port, &header, false, spl_seen, host_ms);
    run(60000);
    CHECK(update.step == PIC_UPDATE_DONE && flash_holds_image());
}


static void test_resets(void)
{
    uint32_t at;

    printf("the PIC reset part-way\n");
    for (at = 0; at < 4; at++)
    {
        start_from_app(true, 8000, NEW_VERSION, false);
        // Into the update until a point in the DATA, VERIFY, COMMIT or RUN.
        while (pic_update_running(&update) &&
               update.step != (at == 0   ? PIC_UPDATE_DATA
                               : at == 1 ? PIC_UPDATE_VERIFY
                               : at == 2 ? PIC_UPDATE_COMMIT
                                         : PIC_UPDATE_RUN))
        {
            step();
        }
        if (at < 2)
        {
            int n;

            for (n = 0; n < 40; n++)
            {
                step();
            }
        }
        pic_reset(0x0080, false); // MCLR: the mailbox stays
        run(60000);
        CHECK(update.step == PIC_UPDATE_DONE && flash_holds_image());
        CHECK(in_app);
    }
}


static void test_power_cuts(void)
{
    static jmp_buf jump;
    static long cut;
    static int mode;
    static int done;
    long operations;

    printf("a power cut part-way, both chips starting again\n");
    start_from_app(true, 6000, NEW_VERSION, false);
    operations = host_operations();
    run(60000);
    CHECK(update.step == PIC_UPDATE_DONE);
    operations = host_operations() - operations;
    for (cut = 0; cut < operations; cut += 37)
    {
        for (mode = 0; mode < 3; mode++)
        {
            start_from_app(true, 6000, NEW_VERSION, false);
            host_cut_at = host_operations() + cut;
            host_cut_mode = mode;
            host_cut_jump = &jump;
            if (setjmp(jump) == 0)
            {
                run(60000);
                CHECK(!"no cut");
            }
            host_cut_jump = NULL;
            host_reset_faults();
            // Both chips start again: the ESP knows only what the PIC says.
            pic_reset(0x0083, false);
            host_tx_length = 0;
            pic_tx_read = 0;
            sml_parser_init(&esp_parser);
            spl_seen = false;
            {
                int n;

                for (n = 0; n < 50; n++) // the ESP comes up
                {
                    host_ms++;
                    pic_poll();
                    esp_receive();
                }
            }
            // An update now: from the SPL (no application, or still in its
            // window), or from the old or the new application.
            pic_update_start(&update, &port, &header, false, spl_seen, host_ms);
            run(60000);
            CHECK(update.step == PIC_UPDATE_DONE && flash_holds_image());
            done += update.step == PIC_UPDATE_DONE;
        }
    }
    printf("  %ld operations, %d updates completed after a cut\n", operations, done);
}


int main(void)
{
    test_update();
    test_in_spl();
    test_refusals();
    test_faults();
    test_resets();
    test_power_cuts();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
