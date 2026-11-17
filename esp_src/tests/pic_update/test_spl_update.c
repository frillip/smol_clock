/*
 * The ESP's SPL update (src/pic/spl_update.c) end to end, on the host,
 * against the PIC's own code on one model of its flash: the IPL's choice at
 * every reset (dspic_ipl/ipl_select.c), the SPL's core (dspic_spl), and the
 * application's SPL writer (dspic_src/esp/spl_writer.c) inside a stand-in
 * for the rest of the application:
 *   - an SPL installed from an application that has just started: the wait
 *     for 60 s healthy, the running SPL kept, the test through the new one,
 *     the confirm a minute later;
 *   - the same again (already there), and forced; an older one, not forced;
 *   - refusals: DEVID, an IPL too old, the test page not free;
 *   - a new SPL that never comes up (the IPL gives up on it, the
 *     application rejects it), and one that fails its test: both REJECTED,
 *     and the kept SPL installed again;
 *   - the application reset part-way, answers lost;
 *   - a power cut at the application's flash operations throughout: the IPL
 *     always starts something, and a new update then completes.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../dspic_ipl/ipl_select.h"
#include "../../../dspic_spl/spl_core.h"
#include "../../../dspic_spl/spl_port.h"
#include "../../../dspic_spl/tests/host/port_host.h"
#include "../../../dspic_src/esp/spl_writer.h"
#include "../../src/pic/pic_file.h"
#include "../../src/pic/spl_update.h"
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

#define OLD_SPL    0x000100UL
#define NEW_SPL    0x000200UL
#define APP_LENGTH 4096UL
#define DEVID      0x1F6CUL

uint32_t ipl_flash_read(uint32_t address)
{
    return address == 0xFF0000UL ? DEVID : host_get(address);
}

// ---- The PIC ---------------------------------------------------------------------------

typedef enum
{
    PIC_DEAD, // the IPL found nothing to start
    PIC_SPL,
    PIC_HUNG, // an SPL that never comes up: the watchdog, if it is on
    PIC_APP,
} PicRun;

static BootMailbox mailbox;
static PicRun pic;
static uint32_t pic_since; // when it started running what it runs
static bool hung_watchdog;
static uint32_t broken_crc = 1; // an SPL with this code CRC never comes up
static bool test_refused;       // the SPL fails the test page's ERASE
static bool app_sml;
static uint32_t app_next_sys;
static SmlParser app_parser;
static long resets;
static long app_ops;
static long app_cut_at = -1;
static jmp_buf app_cut;

static void pic_send(uint8_t class_id, uint8_t id, const uint8_t *payload, uint16_t length)
{
    uint8_t frame[SML_MAX_FRAME_BYTES];

    spl_port_tx(frame, sml_frame(class_id, id, payload, length, frame));
}


static uint32_t spl_code_crc(void)
{
    uint32_t length = (host_get(SPL_RECORD_ADDRESS + 8) & 0xFFFF) << 16 |
                      (host_get(SPL_RECORD_ADDRESS + 10) & 0xFFFF);
    uint32_t crc = BOOT_CRC32_INIT;
    uint32_t index;

    for (index = 0; index < length && index < SPL_CODE_MAX; index++)
    {
        uint32_t word = host_get(SPL_START + 2U * index);
        uint8_t bytes[3] = {(uint8_t)word, (uint8_t)(word >> 8), (uint8_t)(word >> 16)};

        crc = boot_crc32_update(crc, bytes, 3);
    }
    return ~crc;
}

// The application's SPL writer, on the same flash.
static uint32_t app_read(uint32_t address)
{
    return ipl_flash_read(address);
}


static void app_cut_check(void)
{
    if (app_ops++ == app_cut_at)
    {
        longjmp(app_cut, 1);
    }
}


static bool app_erase(uint32_t address)
{
    uint32_t index;

    if (address < SPL_START || address >= SPL_END || (address & 0x7FFUL) != 0UL)
    {
        return false;
    }
    if (app_ops == app_cut_at && (rand() & 1) != 0)
    {
        for (index = 0; index < 1024U; index += 5U) // part done
        {
            host_put(address + 2U * index, HOST_ERASED);
        }
    }
    else
    {
        for (index = 0; index < 1024U; index++)
        {
            host_put(address + 2U * index, HOST_ERASED);
        }
    }
    app_cut_check();
    return true;
}


static bool app_write(uint32_t address, uint32_t first, uint32_t second)
{
    if (address < SPL_START || address >= SPL_END || (address & 3UL) != 0UL)
    {
        return false;
    }
    host_put(address, host_get(address) & first & HOST_ERASED);
    if (app_ops != app_cut_at || (rand() & 1) != 0)
    {
        host_put(address + 2U, host_get(address + 2U) & second & HOST_ERASED);
    }
    app_cut_check();
    return true;
}


static void app_send(uint8_t id, const uint8_t *payload, uint16_t length)
{
    pic_send(SML_CLASS_BOOT, id, payload, length);
}


static void app_state(SplWriterApp *app)
{
    app->reset_cause = mailbox.reset_cause;
    app->attempts = mailbox.attempts;
    app->boot = (uint8_t)mailbox.boot;
    app->trials = mailbox.trials;
    app->healthy_s = (uint16_t)((host_ms - pic_since) / 1000U);
    app->save_pending = false;
}


static void app_committed(void)
{
    mailbox.trials = 0; // boot_new_spl()
}


static const SplWriterPort app_port = {app_read, app_erase, app_write,    NULL,
                                       app_send, app_state, app_committed};

static void app_start(void)
{
    pic = PIC_APP;
    pic_since = host_ms;
    app_sml = false;
    sml_parser_init(&app_parser);
    spl_writer_init(&app_port);
}


// A reset of the PIC: the IPL chooses, and that starts.
static void pic_reset(uint16_t cause, bool request)
{
    IplChoice choice;

    resets++;
    {
        // What the PIC sent before it reset still reaches the ESP: the
        // application drains its transmit buffer before ENTER_BOOTLOADER's.
        uint32_t sent = host_tx_length;

        host_reset_link();
        if (request)
        {
            host_tx_length = sent;
        }
    }
    if (request)
    {
        mailbox.request = MAILBOX_REQUEST_STAY;
    }
    if ((cause & BOOT_RESET_POWER) != 0U)
    {
        memset(&mailbox, 0, sizeof(mailbox));
    }
    ipl_mailbox_prepare(&mailbox, cause);
    choice = ipl_select(&mailbox);
    pic_since = host_ms;
    switch (choice.start)
    {
    case IPL_START_SPL:
        if (spl_code_crc() == broken_crc)
        {
            pic = PIC_HUNG;
            hung_watchdog = choice.watchdog;
            break;
        }
        pic = PIC_SPL;
        spl_init(&mailbox);
        break;
    case IPL_START_APP:
        mailbox.running = MAILBOX_RUNNING_VALUE;
        app_start();
        break;
    default:
        pic = PIC_DEAD;
        break;
    }
}


static void app_poll(void)
{
    uint8_t byte;
    SmlFrame frame;

    if ((host_ms - pic_since) / 1000U >= BOOT_HEALTHY_S)
    {
        mailbox.attempts = 0; // boot_service()
    }
    if (host_ms % 10U == 0U)
    {
        uint32_t slot = host_ms % 1000U;

        spl_writer_service(host_ms / 1000U, slot >= 700U && slot <= 900U);
    }
    if (app_sml && (int32_t)(host_ms - app_next_sys) >= 0)
    {
        uint8_t p[SML_STAT_SYS_BYTES] = {0};

        sml_put_u32(&p[4], (host_ms - pic_since) / 1000U);
        pic_send(SML_CLASS_STAT, SML_STAT_SYS, p, sizeof(p));
        app_next_sys = host_ms + 1000U;
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
            app_next_sys = host_ms + 1000U;
        }
        if (frame.class_id == SML_CLASS_BOOT)
        {
            spl_writer_frame(frame.message_id, frame.payload, frame.length);
            continue;
        }
        if (frame.class_id != SML_CLASS_CMD || frame.message_id != SML_CMD_USER ||
            frame.length != SML_CMD_USER_BYTES)
        {
            continue;
        }
        command = sml_get_u16(frame.payload);
        memcpy(&r[4], &frame.payload[4], 2);
        sml_put_u16(&r[6], command);
        if (command == SML_USER_REQUEST_MESSAGE)
        {
            app_next_sys = host_ms; // STAT-SYS now
            pic_send(SML_CLASS_CMD, SML_CMD_RESPONSE, r, sizeof(r));
        }
        else if (command == SML_USER_ENTER_BOOTLOADER)
        {
            uint8_t state = spl_writer_status()->spl_state;

            if (spl_writer_busy())
            {
                sml_put_u16(&r[0], SML_RESULT_NACK);
                sml_put_u16(&r[2], SML_ERROR_BUSY);
            }
            else if ((state & (SML_SPL_STATE_INTACT | SML_SPL_STATE_REJECTED)) !=
                     SML_SPL_STATE_INTACT)
            {
                sml_put_u16(&r[0], SML_RESULT_NACK);
                sml_put_u16(&r[2], SML_ERROR_STATE);
            }
            pic_send(SML_CLASS_CMD, SML_CMD_RESPONSE, r, sizeof(r));
            if (sml_get_u16(r) == SML_RESULT_ACK)
            {
                pic_reset(0x0040, true);
                return;
            }
        }
    }
}


static void pic_poll(void)
{
    switch (pic)
    {
    case PIC_APP: app_poll(); break;
    case PIC_HUNG:
        if (hung_watchdog && host_ms - pic_since >= BOOT_WATCHDOG_MS)
        {
            pic_reset(SML_RESET_WDTO, false);
        }
        break;
    case PIC_SPL:
        switch (spl_poll())
        {
        case SPL_ACTION_HAND_OVER:
            mailbox.running = MAILBOX_RUNNING_VALUE;
            app_start();
            break;
        case SPL_ACTION_RESET: pic_reset(0x0040, false); break;
        default: break;
        }
        break;
    default: break;
    }
}

// ---- The ESP ------------------------------------------------------------------------------

static uint8_t image[3 * 3008];
static PicFileHeader header;
static SplUpdate update;
static SmlParser esp_parser;
static uint32_t pic_tx_read;
static long drop_answers = 0; // DATA-RESPONSEs to lose, from the next
static long pic_reset_at = -1; // reset the PIC at this chunk written

// The kept SPL, as the ESP would stage it.
static uint8_t kept[64 + 3 * 3008];
static uint32_t kept_bytes;
static bool kept_ok;
static PicFileHeader kept_header;

static void esp_send(void *context, uint8_t class_id, uint8_t id, const uint8_t *payload,
                     uint16_t length)
{
    uint8_t frame[SML_MAX_FRAME_BYTES];
    uint16_t bytes = sml_frame(class_id, id, payload, length, frame);

    (void)context;
    if (test_refused && pic == PIC_SPL && class_id == SML_CLASS_BOOT && id == SML_BOOT_COMMAND &&
        payload[2] == SML_BOOT_CMD_ERASE && payload[3] == SML_BOOT_REGION_TEST)
    {
        uint8_t r[SML_BOOT_RESPONSE_BYTES] = {0}; // the SPL cannot erase the test page

        sml_put_u16(&r[0], SML_RESULT_NACK);
        sml_put_u16(&r[2], SML_ERROR_STORAGE);
        memcpy(&r[4], payload, 4);
        pic_send(SML_CLASS_BOOT, SML_BOOT_RESPONSE, r, sizeof(r));
        return;
    }
    if (pic == PIC_APP || pic == PIC_SPL)
    {
        host_rx_push(frame, bytes);
    }
}


static bool esp_read(void *context, uint32_t offset, uint8_t *data, uint32_t length)
{
    (void)context;
    if (offset + length > 3U * header.length)
    {
        return false;
    }
    memcpy(data, image + offset, length);
    return true;
}


static bool esp_stored(void *context, uint32_t version, uint32_t crc32)
{
    (void)context;
    return (kept_ok && kept_header.version == version && kept_header.crc32 == crc32) ||
           (header.version == version && header.crc32 == crc32);
}


static bool keep_begin(void *context, const PicFileHeader *h)
{
    (void)context;
    pic_file_spl_header(h, kept);
    kept_bytes = 64;
    kept_ok = false;
    return true;
}


static bool keep_write(void *context, const uint8_t *data, uint32_t length)
{
    (void)context;
    if (kept_bytes + length > sizeof(kept))
    {
        return false;
    }
    memcpy(kept + kept_bytes, data, length);
    kept_bytes += length;
    return true;
}


static bool keep_end(void *context)
{
    (void)context;
    kept_ok = pic_file_parse(kept, &kept_header) &&
              kept_bytes == pic_file_bytes(&kept_header) &&
              boot_crc32(kept + 64, kept_bytes - 64) == kept_header.crc32;
    return kept_ok;
}


static const SplUpdatePort port = {esp_send,  esp_read, esp_stored, keep_begin,
                                   keep_write, keep_end, NULL};

static void esp_receive(void)
{
    SmlFrame frame;

    while (pic_tx_read < host_tx_length)
    {
        if (!sml_parser_push(&esp_parser, host_tx[pic_tx_read++], &frame))
        {
            continue;
        }
        if (frame.class_id == SML_CLASS_BOOT && frame.message_id == SML_BOOT_DATA_RESPONSE)
        {
            if (pic_reset_at >= 0 && update.written == (uint32_t)pic_reset_at)
            {
                pic_reset_at = -1;
                pic_reset(SML_RESET_EXTR, false); // MCLR, part-way
                return;
            }
            if (drop_answers > 0)
            {
                drop_answers--;
                continue;
            }
        }
        spl_update_frame(&update, frame.class_id, frame.message_id, frame.payload, frame.length,
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
    spl_update_service(&update, host_ms);
    esp_receive();
}


static void run(uint32_t limit)
{
    uint32_t end = host_ms + limit;

    while (spl_update_running(&update) && host_ms != end)
    {
        step();
    }
}


static void settle(uint32_t ms)
{
    uint32_t end = host_ms + ms;

    while (host_ms != end)
    {
        step();
    }
}

// ---- Flash contents ---------------------------------------------------------------------

static void put_word(uint32_t record, unsigned index, uint16_t value)
{
    host_put(record + 2U * index, 0xFF0000UL | value);
}


static void make_app(uint32_t length)
{
    uint16_t words[RECORD_CRC_WORDS] = {0};
    uint32_t crc = BOOT_CRC32_INIT;
    uint32_t index;

    for (index = 0; index < length; index++)
    {
        uint32_t word = (index * 2654435761UL) & HOST_ERASED;
        uint8_t bytes[3] = {(uint8_t)word, (uint8_t)(word >> 8), (uint8_t)(word >> 16)};

        host_put(APP_START + 2U * index, word);
        crc = boot_crc32_update(crc, bytes, 3);
    }
    crc = ~crc;
    words[RECORD_MAGIC] = APP_RECORD_MAGIC_VALUE;
    words[RECORD_FORMAT] = RECORD_FORMAT_1;
    words[APP_RECORD_VERSION + 1] = 0x0307;
    words[APP_RECORD_START] = (uint16_t)(APP_START >> 16);
    words[APP_RECORD_START + 1] = (uint16_t)APP_START;
    words[APP_RECORD_LENGTH] = (uint16_t)(length >> 16);
    words[APP_RECORD_LENGTH + 1] = (uint16_t)length;
    words[APP_RECORD_CRC32] = (uint16_t)(crc >> 16);
    words[APP_RECORD_CRC32 + 1] = (uint16_t)crc;
    words[APP_RECORD_SML_VERSION] = 0x0206;
    for (index = 0; index < RECORD_CRC_WORDS; index++)
    {
        put_word(APP_RECORD_ADDRESS, index, words[index]);
    }
    put_word(APP_RECORD_ADDRESS, RECORD_CRC, boot_record_crc(words));
    put_word(APP_RECORD_ADDRESS, RECORD_COMMIT, RECORD_COMMIT_0);
    put_word(APP_RECORD_ADDRESS, RECORD_COMMIT + 1, RECORD_COMMIT_1);
    put_word(IPL_INFO_ADDRESS, RECORD_MAGIC, IPL_INFO_MAGIC_VALUE);
    put_word(IPL_INFO_ADDRESS, RECORD_FORMAT, RECORD_FORMAT_1);
    put_word(IPL_INFO_ADDRESS, IPL_INFO_VERSION, 0);
    put_word(IPL_INFO_ADDRESS, IPL_INFO_VERSION + 1, 0x0100); // IPL 0.1.0
}


static uint8_t old_code[3 * 3008];
static uint32_t old_length;

// The SPL a programmer left: code, record, committed and confirmed.
static void program_old_spl(void)
{
    uint16_t words[RECORD_CRC_WORDS] = {0};
    uint32_t crc;
    uint32_t index;

    old_length = 1600;
    srand(11);
    for (index = 0; index < 3U * old_length; index++)
    {
        old_code[index] = (uint8_t)rand();
    }
    crc = boot_crc32(old_code, 3U * old_length);
    for (index = 0; index < old_length; index++)
    {
        host_put(SPL_START + 2U * index, (uint32_t)old_code[3 * index] |
                                             (uint32_t)old_code[3 * index + 1] << 8 |
                                             (uint32_t)old_code[3 * index + 2] << 16);
    }
    words[RECORD_MAGIC] = SPL_RECORD_MAGIC_VALUE;
    words[RECORD_FORMAT] = RECORD_FORMAT_1;
    words[SPL_RECORD_VERSION + 1] = (uint16_t)OLD_SPL;
    words[SPL_RECORD_LENGTH + 1] = (uint16_t)old_length;
    words[SPL_RECORD_CRC32] = (uint16_t)(crc >> 16);
    words[SPL_RECORD_CRC32 + 1] = (uint16_t)crc;
    words[SPL_RECORD_BUILD_TIME + 1] = 1234;
    words[SPL_RECORD_PROTOCOL] = SML_BOOT_PROTOCOL;
    for (index = 0; index < RECORD_CRC_WORDS; index++)
    {
        put_word(SPL_RECORD_ADDRESS, index, words[index]);
    }
    put_word(SPL_RECORD_ADDRESS, RECORD_CRC, boot_record_crc(words));
    put_word(SPL_RECORD_ADDRESS, RECORD_SPARE, 0xFFFF);
    put_word(SPL_RECORD_ADDRESS, RECORD_COMMIT, RECORD_COMMIT_0);
    put_word(SPL_RECORD_ADDRESS, RECORD_COMMIT + 1, RECORD_COMMIT_1);
    put_word(SPL_RECORD_ADDRESS, SPL_RECORD_CONFIRM, SPL_CONFIRM_0);
    put_word(SPL_RECORD_ADDRESS, SPL_RECORD_CONFIRM + 1, SPL_CONFIRM_1);
}


// The new SPL's file, as make_image.py writes it.
static void make_new_spl(uint32_t length, unsigned seed, uint32_t version)
{
    uint8_t h[64];
    uint32_t index;

    srand(seed);
    for (index = 0; index < 3U * length; index++)
    {
        image[index] = (uint8_t)rand();
    }
    memset(&header, 0, sizeof(header));
    header.kind = PIC_FILE_SPL;
    header.device_id = DEVID;
    header.start = SPL_START;
    header.version = version;
    header.length = length;
    header.crc32 = boot_crc32(image, 3U * length);
    header.build_time = 1700000000UL;
    header.protocol = SML_BOOT_PROTOCOL;
    pic_file_spl_header(&header, h);
    CHECK(pic_file_parse(h, &header) && header.crc32 == boot_crc32(image, 3U * length));
}


// The SPL in flash: its version, and its state as the IPL sees it.
static uint32_t spl_version(void)
{
    return (host_get(SPL_RECORD_ADDRESS + 4) & 0xFFFF) << 16 | (host_get(SPL_RECORD_ADDRESS + 6) & 0xFFFF);
}


static bool spl_confirmed(void)
{
    return (host_get(SPL_RECORD_ADDRESS + 2U * SPL_RECORD_CONFIRM) & 0xFFFF) == SPL_CONFIRM_0 &&
           (host_get(SPL_RECORD_ADDRESS + 2U * SPL_RECORD_CONFIRM + 2U) & 0xFFFF) == SPL_CONFIRM_1;
}


static bool spl_rejected(void)
{
    return (host_get(SPL_RECORD_ADDRESS + 2U * SPL_RECORD_REJECT) & 0xFFFF) != 0xFFFF;
}


// A clock from the factory: the old SPL, the application, just powered on.
static void fresh(void)
{
    host_reset();
    memset(&mailbox, 0, sizeof(mailbox));
    make_app(APP_LENGTH);
    program_old_spl();
    kept_ok = false;
    broken_crc = 1;
    test_refused = false;
    drop_answers = 0;
    pic_reset_at = -1;
    app_cut_at = -1;
    host_ms = 1;
    memset(&update, 0, sizeof(update));
    update.step = SPL_UPDATE_FAILED; // none running
    pic_reset(SML_RESET_POR | SML_RESET_BOR, false);
    sml_parser_init(&esp_parser);
    pic_tx_read = 0;
    settle(2000); // the SPL's window, then the application
}


static void start(bool force)
{
    host_tx_length = 0;
    pic_tx_read = 0;
    spl_update_start(&update, &port, &header, force, host_ms);
}


int main(void)
{
    uint32_t began;

    printf("SPL update, end to end\n");

    // An update from an application started 2 s ago.
    fresh();
    CHECK(pic == PIC_APP && spl_version() == OLD_SPL);
    make_new_spl(1800, 5, NEW_SPL);
    began = host_ms;
    start(false);
    run(300000);
    CHECK(update.step == SPL_UPDATE_DONE && !update.already);
    if (update.step != SPL_UPDATE_DONE)
    {
        printf("  %s: %s\n", spl_update_step_name(update.step), update.reason);
    }
    printf("  installed and confirmed in %.1f s (the application's healthy waits included)\n",
           (host_ms - began) / 1000.0);
    CHECK(spl_version() == NEW_SPL && spl_confirmed() && !spl_rejected() && spl_code_crc() == header.crc32);
    CHECK(pic == PIC_APP && (mailbox.boot & BOOT_PATH_UNCONFIRMED) != 0U);
    // The old one kept, whole.
    CHECK(update.kept && kept_ok && kept_header.version == OLD_SPL &&
          kept_header.length == old_length && kept_header.build_time == 1234 &&
          memcmp(kept + 64, old_code, 3U * old_length) == 0);
    // The test page left erased.
    CHECK(host_get(TEST_START) == HOST_ERASED && host_get(TEST_RECORD_ADDRESS) == HOST_ERASED);
    // A reset now: the confirmed new SPL, no watchdog, no trial.
    {
        BootMailbox m = mailbox;
        IplChoice choice;

        ipl_mailbox_prepare(&m, 0x0040);
        choice = ipl_select(&m);
        CHECK(choice.start == IPL_START_SPL && (m.boot & BOOT_PATH_UNCONFIRMED) == 0U); // confirmed
    }

    // Again: there already. Forced: installed again, and not kept (it is
    // the file itself).
    start(false);
    run(10000);
    CHECK(update.step == SPL_UPDATE_DONE && update.already);
    start(true);
    run(300000);
    CHECK(update.step == SPL_UPDATE_DONE && !update.already && !update.kept);
    CHECK(spl_version() == NEW_SPL && spl_confirmed());
    // An older one, not forced: the newer one stays, nothing written.
    {
        long ops = app_ops;

        make_new_spl(1800, 7, OLD_SPL);
        start(false);
        run(10000);
        CHECK(update.step == SPL_UPDATE_DONE && update.already && update.newer);
        CHECK(spl_version() == NEW_SPL && spl_confirmed() && app_ops == ops);
        make_new_spl(1800, 5, NEW_SPL);
    }

    // Refusals, before anything is written.
    {
        PicFileHeader saved = header;

        header.device_id = 0x1F6D;
        start(true);
        run(10000);
        CHECK(update.step == SPL_UPDATE_FAILED && strstr(update.reason, "DEVID") != NULL);
        CHECK(update.permanent);
        header = saved;
        header.min_ipl = 0x000200; // needs IPL 0.2.0
        start(true);
        run(10000);
        CHECK(update.step == SPL_UPDATE_FAILED && strstr(update.reason, "IPL") != NULL);
        CHECK(update.permanent);
        header = saved;
    }
    fresh();
    make_app(79000UL); // reaches the test page (from 78848)
    pic_reset(0x0040, false);
    settle(2000);
    make_new_spl(1800, 5, NEW_SPL);
    start(false);
    run(10000);
    CHECK(update.step == SPL_UPDATE_FAILED && strstr(update.reason, "test page") != NULL);
    CHECK(update.permanent);
    if (update.step != SPL_UPDATE_FAILED)
    {
        printf("  %s: %s, pic %d\n", spl_update_step_name(update.step), update.reason, (int)pic);
    }
    CHECK(spl_version() == OLD_SPL);

    // A new SPL that never comes up: three trials under the watchdog, then
    // the application, which rejects it. Then the kept one put back.
    fresh();
    make_new_spl(1800, 6, NEW_SPL);
    broken_crc = header.crc32;
    start(false);
    run(300000);
    CHECK(update.step == SPL_UPDATE_REJECTED && strstr(update.reason, "did not start") != NULL);
    settle(3000); // the reject, in the next window
    CHECK(pic == PIC_APP && spl_version() == NEW_SPL && spl_rejected() && !spl_confirmed());
    CHECK((mailbox.boot & BOOT_PATH_SPL_GIVEN_UP) != 0U && kept_ok);
    {
        PicFileHeader bad = header;

        memcpy(image, kept + 64, 3U * kept_header.length);
        header = kept_header;
        start(true);
        run(300000);
        CHECK(update.step == SPL_UPDATE_DONE);
        if (update.step != SPL_UPDATE_DONE)
        {
            printf("  %s: %s\n", spl_update_step_name(update.step), update.reason);
        }
        CHECK(spl_version() == OLD_SPL && spl_confirmed() && !spl_rejected());
        header = bad;
    }

    // One that fails its test: back to the application, REJECTED, still
    // unconfirmed (so it never erases the application), then the old back.
    fresh();
    make_new_spl(1800, 7, NEW_SPL);
    test_refused = true;
    start(false);
    run(300000);
    CHECK(update.step == SPL_UPDATE_REJECTED && strstr(update.reason, "test") != NULL);
    CHECK(pic == PIC_APP && spl_version() == NEW_SPL && !spl_confirmed());
    test_refused = false;
    memcpy(image, kept + 64, 3U * kept_header.length);
    header = kept_header;
    start(true);
    run(300000);
    CHECK(update.step == SPL_UPDATE_DONE && spl_version() == OLD_SPL && spl_confirmed());

    // The PIC reset part-way through the data (MCLR): a new pass, after the
    // application's 60 s again. Answers lost: sent again.
    fresh();
    make_new_spl(1800, 8, NEW_SPL);
    pic_reset_at = 10;
    start(false);
    run(400000);
    CHECK(update.step == SPL_UPDATE_DONE && update.passes == 1);
    CHECK(spl_version() == NEW_SPL && spl_confirmed());
    fresh();
    make_new_spl(1800, 9, NEW_SPL);
    drop_answers = 3;
    start(false);
    run(300000);
    CHECK(update.step == SPL_UPDATE_DONE && update.passes == 0);

    // A power cut at the application's flash operations, throughout: the
    // IPL always starts something, and an update from there completes.
    {
        long total;
        volatile long at;
        long outcomes[3] = {0, 0, 0}; // the SPL, the application, nothing
        long bad = 0;

        fresh();
        make_new_spl(1800, 10, NEW_SPL);
        app_ops = 0;
        start(false);
        run(300000);
        total = app_ops;
        for (at = 0; at < total; at += (at < 8 || at >= total - 9) ? 1 : 23)
        {
            fresh();
            make_new_spl(1800, 10, NEW_SPL);
            app_ops = 0;
            app_cut_at = at;
            start(false);
            if (setjmp(app_cut) == 0)
            {
                run(300000);
                bad++; // the cut never came
                continue;
            }
            app_cut_at = -1;
            pic_reset(SML_RESET_POR | SML_RESET_BOR, false);
            outcomes[pic == PIC_SPL ? 0 : pic == PIC_APP ? 1 : 2]++;
            if (pic == PIC_DEAD)
            {
                bad++;
                continue;
            }
            settle(2000);
            start(false);
            run(400000);
            if (update.step != SPL_UPDATE_DONE || spl_version() != NEW_SPL || !spl_confirmed())
            {
                printf("  cut at %ld: %s %s\n", (long)at, spl_update_step_name(update.step),
                       update.reason);
                bad++;
            }
        }
        printf("  %ld flash operations of the application; after a cut: the SPL %ld, the "
               "application %ld, nothing %ld\n",
               total, outcomes[0], outcomes[1], outcomes[2]);
        CHECK(bad == 0 && outcomes[2] == 0);
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
