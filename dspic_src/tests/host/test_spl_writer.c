/*
 * The application's SPL writer (esp/spl_writer.c) against a model of program
 * memory with the flash's rules, and the IPL's own choice (dspic_ipl's
 * ipl_select.c) of what starts after it:
 *   - BOOT-STATUS in mode APP;
 *   - every refusal: the invariant (60 s healthy), a save pending, BUSY,
 *     keys, regions, the SPL's commands;
 *   - an SPL installed: one page erase an epoch and only in the window,
 *     chunks held for the window, retransmissions, CRC, NOT_BLANK, COMMIT's
 *     CRC check, its repeat;
 *   - CONFIRM only after a start by the unconfirmed SPL; reject after the
 *     IPL gave up on it;
 *   - the guard: nothing written outside the SPL's pages;
 *   - a power cut at every flash operation of an update, of CONFIRM and of
 *     reject: the IPL then starts an intact SPL or the application, never
 *     nothing and never a broken SPL, and a new update completes.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/boot_crc.h"
#include "../../../common/boot_layout.h"
#include "../../../common/sml.h"
#include "../../../dspic_ipl/ipl_select.h"
#include "../../esp/spl_writer.h"

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

#define WORDS      (0x02B000UL / 2U)
#define ERASED     0xFFFFFFUL
#define NEVER      (-1L)
#define APP_LENGTH 4096UL // instructions of the stand-in application
#define OLD_SPL    0x000100UL
#define NEW_SPL    0x000200UL

// ---- Program memory --------------------------------------------------------------

static uint32_t flash[WORDS];
static long operations; // erases and writes since the last reset of the counter
static long cut_at = NEVER;
static int cut_mode; // 0: part done; 1: not started; 2: complete
static jmp_buf cut_jump;
static long refused;
static long overwrites;

static uint32_t read_word(uint32_t address)
{
    if (address == 0xFF0000UL)
    {
        return 0x001F6CUL; // DEVID
    }
    return address / 2U < WORDS ? flash[address / 2U] : ERASED;
}


uint32_t ipl_flash_read(uint32_t address)
{
    return read_word(address);
}


static bool spl_page(uint32_t address)
{
    return address >= SPL_START && address < SPL_END;
}


static void cut_check(void)
{
    if (operations++ == cut_at)
    {
        longjmp(cut_jump, 1);
    }
}


static bool erase(uint32_t address)
{
    uint32_t index;
    bool cut = operations == cut_at;

    if (!spl_page(address) || (address & (BOOT_PAGE_ADDRESSES - 1UL)) != 0UL)
    {
        refused++;
        return false;
    }
    if (!cut || cut_mode == 2)
    {
        for (index = 0; index < BOOT_PAGE_INSTRUCTIONS; index++)
        {
            flash[address / 2U + index] = ERASED;
        }
    }
    else if (cut_mode == 0)
    {
        for (index = 0; index < BOOT_PAGE_INSTRUCTIONS; index += 3U) // some words only
        {
            flash[address / 2U + index] = ERASED;
        }
    }
    cut_check();
    return true;
}


static void program(uint32_t address, uint32_t value)
{
    if (flash[address / 2U] != ERASED)
    {
        overwrites++;
    }
    flash[address / 2U] &= value;
}


static bool write(uint32_t address, uint32_t first, uint32_t second)
{
    bool cut = operations == cut_at;

    if (!spl_page(address) || (address & 3UL) != 0UL)
    {
        refused++;
        return false;
    }
    if (!cut || cut_mode == 2)
    {
        program(address, first & ERASED);
        program(address + 2U, second & ERASED);
    }
    else if (cut_mode == 0)
    {
        program(address, first & ERASED); // the first word only
    }
    cut_check();
    return true;
}

// ---- The link and the application ------------------------------------------------

static SplWriterApp app;

typedef struct
{
    uint8_t id;
    uint8_t payload[SML_BOOT_VERIFY_RESPONSE_BYTES];
    uint16_t length;
} Sent;

static Sent sent[16];
static int sent_count;

static void send(uint8_t id, const uint8_t *payload, uint16_t length)
{
    if (sent_count < 16)
    {
        sent[sent_count].id = id;
        memcpy(sent[sent_count].payload, payload, length);
        sent[sent_count].length = length;
    }
    sent_count++;
}


static void app_state(SplWriterApp *state)
{
    *state = app;
}


static int commits_seen;
static void committed(void)
{
    commits_seen++;
}


static const SplWriterPort port = {read_word, erase, write, NULL, send, app_state, committed};

// The last BOOT-RESPONSE: its status and detail, and value.
static uint16_t last_status, last_detail;
static uint32_t last_value;

static bool response(int index)
{
    if (index >= sent_count || sent[index].id != SML_BOOT_RESPONSE)
    {
        return false;
    }
    last_status = sml_get_u16(&sent[index].payload[0]);
    last_detail = sml_get_u16(&sent[index].payload[2]);
    last_value = sml_get_u32(&sent[index].payload[8]);
    return true;
}


static void command(uint8_t code, uint8_t region, uint32_t key, const uint8_t *args)
{
    uint8_t p[SML_BOOT_COMMAND_BYTES] = {0};

    sml_put_u16(&p[0], 0x7001);
    p[2] = code;
    p[3] = region;
    sml_put_u32(&p[4], key);
    if (args != NULL)
    {
        memcpy(&p[8], args, 24);
    }
    sent_count = 0;
    spl_writer_frame(SML_BOOT_COMMAND, p, sizeof(p));
}


// Runs the windows of epochs, several services each, until nothing is left.
static uint32_t epoch = 100;

static void windows(int epochs)
{
    int e;
    int pass;

    for (e = 0; e < epochs; e++)
    {
        epoch++;
        spl_writer_service(epoch, false); // outside the window: nothing
        for (pass = 0; pass < 20; pass++)
        {
            spl_writer_service(epoch, true);
        }
    }
}

// ---- Images -----------------------------------------------------------------------

static uint8_t spl_code[3 * 3008];
static uint32_t spl_length;

static void make_code(uint32_t length, unsigned seed)
{
    uint32_t index;

    srand(seed);
    spl_length = length;
    for (index = 0; index < 3U * length; index++)
    {
        spl_code[index] = (uint8_t)rand();
    }
}


static uint32_t code_crc(void)
{
    return boot_crc32(spl_code, 3U * spl_length);
}


static void commit_args(uint8_t *args, uint32_t version, uint32_t crc)
{
    memset(args, 0, 24);
    sml_put_u32(&args[0], version);
    sml_put_u32(&args[4], SPL_START);
    sml_put_u32(&args[8], spl_length);
    sml_put_u32(&args[12], crc);
    sml_put_u32(&args[16], 1700000000UL);
    sml_put_u16(&args[20], SML_BOOT_PROTOCOL);
}


static void put_record_word(uint32_t record, unsigned index, uint16_t value)
{
    flash[record / 2U + index] = 0xFF0000UL | value;
}


// An application with a committed record, and the IPL's info block.
static void make_app(void)
{
    uint16_t words[RECORD_CRC_WORDS];
    uint8_t packed[3 * APP_LENGTH];
    uint32_t index;
    uint32_t crc;

    for (index = 0; index < APP_LENGTH; index++)
    {
        flash[APP_START / 2U + index] = (index * 2654435761UL) & ERASED;
        packed[3 * index] = (uint8_t)flash[APP_START / 2U + index];
        packed[3 * index + 1] = (uint8_t)(flash[APP_START / 2U + index] >> 8);
        packed[3 * index + 2] = (uint8_t)(flash[APP_START / 2U + index] >> 16);
    }
    crc = boot_crc32(packed, sizeof(packed));
    memset(words, 0, sizeof(words));
    words[RECORD_MAGIC] = APP_RECORD_MAGIC_VALUE;
    words[RECORD_FORMAT] = RECORD_FORMAT_1;
    words[APP_RECORD_VERSION + 1] = 0x0307;
    words[APP_RECORD_START] = (uint16_t)(APP_START >> 16);
    words[APP_RECORD_START + 1] = (uint16_t)APP_START;
    words[APP_RECORD_LENGTH + 1] = (uint16_t)APP_LENGTH;
    words[APP_RECORD_CRC32] = (uint16_t)(crc >> 16);
    words[APP_RECORD_CRC32 + 1] = (uint16_t)crc;
    words[APP_RECORD_SML_VERSION] = 0x0206;
    for (index = 0; index < RECORD_CRC_WORDS; index++)
    {
        put_record_word(APP_RECORD_ADDRESS, index, words[index]);
    }
    put_record_word(APP_RECORD_ADDRESS, RECORD_CRC, boot_record_crc(words));
    put_record_word(APP_RECORD_ADDRESS, RECORD_COMMIT, RECORD_COMMIT_0);
    put_record_word(APP_RECORD_ADDRESS, RECORD_COMMIT + 1, RECORD_COMMIT_1);
    put_record_word(IPL_INFO_ADDRESS, RECORD_MAGIC, IPL_INFO_MAGIC_VALUE);
    put_record_word(IPL_INFO_ADDRESS, RECORD_FORMAT, RECORD_FORMAT_1);
    put_record_word(IPL_INFO_ADDRESS, IPL_INFO_VERSION, 0x0000);
    put_record_word(IPL_INFO_ADDRESS, IPL_INFO_VERSION + 1, 0x0100); // 0.1.0
}


// The SPL as a programmer leaves it: code, record, commit, confirm.
static void program_spl(uint32_t version, bool confirmed)
{
    uint16_t words[RECORD_CRC_WORDS];
    uint32_t index;
    uint32_t crc = code_crc();

    for (index = SPL_START / 2U; index < SPL_END / 2U; index++)
    {
        flash[index] = ERASED;
    }
    for (index = 0; index < spl_length; index++)
    {
        flash[SPL_START / 2U + index] = (uint32_t)spl_code[3 * index] |
                                        (uint32_t)spl_code[3 * index + 1] << 8 |
                                        (uint32_t)spl_code[3 * index + 2] << 16;
    }
    memset(words, 0, sizeof(words));
    words[RECORD_MAGIC] = SPL_RECORD_MAGIC_VALUE;
    words[RECORD_FORMAT] = RECORD_FORMAT_1;
    words[SPL_RECORD_VERSION] = (uint16_t)(version >> 16);
    words[SPL_RECORD_VERSION + 1] = (uint16_t)version;
    words[SPL_RECORD_LENGTH + 1] = (uint16_t)spl_length;
    words[SPL_RECORD_CRC32] = (uint16_t)(crc >> 16);
    words[SPL_RECORD_CRC32 + 1] = (uint16_t)crc;
    words[SPL_RECORD_PROTOCOL] = SML_BOOT_PROTOCOL;
    for (index = 0; index < RECORD_CRC_WORDS; index++)
    {
        put_record_word(SPL_RECORD_ADDRESS, index, words[index]);
    }
    put_record_word(SPL_RECORD_ADDRESS, RECORD_CRC, boot_record_crc(words));
    put_record_word(SPL_RECORD_ADDRESS, RECORD_SPARE, 0xFFFF);
    put_record_word(SPL_RECORD_ADDRESS, RECORD_COMMIT, RECORD_COMMIT_0);
    put_record_word(SPL_RECORD_ADDRESS, RECORD_COMMIT + 1, RECORD_COMMIT_1);
    if (confirmed)
    {
        put_record_word(SPL_RECORD_ADDRESS, SPL_RECORD_CONFIRM, SPL_CONFIRM_0);
        put_record_word(SPL_RECORD_ADDRESS, SPL_RECORD_CONFIRM + 1, SPL_CONFIRM_1);
    }
}


static void data(uint32_t address, const uint8_t *chunk, uint32_t crc)
{
    uint8_t p[SML_BOOT_DATA_BYTES] = {0};

    sml_put_u32(&p[0], address);
    p[4] = SML_BOOT_REGION_SPL;
    sml_put_u32(&p[8], crc);
    memcpy(&p[12], chunk, BOOT_CHUNK_BYTES);
    sent_count = 0;
    spl_writer_frame(SML_BOOT_DATA, p, sizeof(p));
}


static uint16_t data_answer(void)
{
    return sent_count == 1 && sent[0].id == SML_BOOT_DATA_RESPONSE ? sml_get_u16(&sent[0].payload[2])
                                                                    : 0xFFFFU;
}


static uint8_t chunk_buffer[BOOT_CHUNK_BYTES];

// The SPL's code chunk at address, erased past its end.
static const uint8_t *code_chunk(uint32_t address)
{
    uint32_t first = (uint32_t)((address - SPL_START) / 2U);
    uint32_t index;

    memset(chunk_buffer, 0xFF, sizeof(chunk_buffer));
    for (index = 0; index < BOOT_CHUNK_INSTRUCTIONS && first + index < spl_length; index++)
    {
        memcpy(&chunk_buffer[3 * index], &spl_code[3 * (first + index)], 3);
    }
    return chunk_buffer;
}


// A whole update, as the ESP does it (spec 9.5 steps 3-6). True if every
// answer was the expected one.
static bool install(uint32_t version)
{
    uint8_t args[24];
    uint32_t address;

    command(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_SPL, SML_BOOT_KEY_ERASE, NULL);
    if (!response(0) || last_status != SML_RESULT_ACCEPTED)
    {
        return false;
    }
    sent_count = 0;
    windows(4);
    if (!response(0) || last_status != SML_RESULT_ACK || last_value != SPL_PAGES)
    {
        return false;
    }
    for (address = SPL_START; address < SPL_START + 2U * spl_length;
         address += BOOT_CHUNK_ADDRESSES)
    {
        const uint8_t *chunk = code_chunk(address);

        data(address, chunk, boot_crc32(chunk, BOOT_CHUNK_BYTES));
        if (sent_count != 0)
        {
            return false; // held for the window
        }
        windows(1);
        if (data_answer() != SML_ERROR_NONE)
        {
            return false;
        }
    }
    commit_args(args, version, code_crc());
    command(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_SPL, SML_BOOT_KEY_COMMIT, args);
    if (!response(0) || last_status != SML_RESULT_ACCEPTED)
    {
        return false;
    }
    sent_count = 0;
    windows(1);
    return response(0) && last_status == SML_RESULT_ACK;
}


static IplChoice ipl_start(BootMailbox *mailbox)
{
    ipl_mailbox_prepare(mailbox, 0x0040); // a software reset
    return ipl_select(mailbox);
}


static void fresh(void)
{
    uint32_t index;

    for (index = 0; index < WORDS; index++)
    {
        flash[index] = ERASED;
    }
    make_app();
    make_code(1700, 1);
    program_spl(OLD_SPL, true);
    memset(&app, 0, sizeof(app));
    app.healthy_s = 75;
    cut_at = NEVER;
    operations = 0;
    spl_writer_init(&port);
}


int main(void)
{
    uint8_t args[24];

    printf("SPL writer\n");

    // BOOT-STATUS, mode APP.
    fresh();
    app.boot = 0;
    app.attempts = 2;
    command(SML_BOOT_CMD_STATUS, 0, 0, NULL);
    CHECK(sent_count == 2 && sent[0].id == SML_BOOT_STATUS && response(1) &&
          last_status == SML_RESULT_ACK);
    {
        const uint8_t *p = sent[0].payload;
        uint16_t flags = sml_get_u16(&p[6]);

        CHECK(sml_get_u32(&p[0]) == OLD_SPL && p[4] == SML_BOOT_PROTOCOL && p[5] == SML_BOOT_MODE_APP);
        CHECK(flags == (SML_BOOT_FLAG_RECORD | SML_BOOT_FLAG_COMMITTED | SML_BOOT_FLAG_IMAGE_OK |
                        SML_BOOT_FLAG_TEST_FREE));
        CHECK(sml_get_u32(&p[12]) == 0x1F6C && sml_get_u32(&p[16]) == APP_START &&
              sml_get_u32(&p[20]) == APP_END);
        CHECK(sml_get_u32(&p[24]) == 0x0307 && sml_get_u32(&p[28]) == APP_LENGTH &&
              sml_get_u32(&p[32]) == sml_get_u32(&p[36]));
        CHECK(p[44] == 2 && sml_get_u16(&p[46]) == 0x0001 && sml_get_u16(&p[50]) == 75);
        CHECK(p[48] == (SML_SPL_STATE_RECORD | SML_SPL_STATE_COMMITTED | SML_SPL_STATE_INTACT |
                        SML_SPL_STATE_CONFIRMED));
        CHECK(sml_get_u32(&p[52]) == code_crc());
    }

    // Refusals.
    command(SML_BOOT_CMD_HOLD, 0, 0, NULL);
    CHECK(response(0) && last_status == SML_RESULT_NACK && last_detail == SML_ERROR_UNSUPPORTED);
    command(SML_BOOT_CMD_RUN, 0, 0, NULL);
    CHECK(response(0) && last_detail == SML_ERROR_UNSUPPORTED);
    command(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_APP, SML_BOOT_KEY_ERASE, NULL);
    CHECK(response(0) && last_detail == SML_ERROR_ARGUMENT);
    command(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_SPL, SML_BOOT_KEY_COMMIT, NULL);
    CHECK(response(0) && last_detail == SML_ERROR_ARGUMENT);
    app.healthy_s = 59; // the invariant: 60 s healthy first
    command(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_SPL, SML_BOOT_KEY_ERASE, NULL);
    CHECK(response(0) && last_status == SML_RESULT_NACK && last_detail == SML_ERROR_STATE);
    app.healthy_s = 75;
    app.save_pending = true;
    command(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_SPL, SML_BOOT_KEY_ERASE, NULL);
    CHECK(response(0) && last_detail == SML_ERROR_BUSY);
    app.save_pending = false;
    make_code(1700, 2);
    data(SPL_START, code_chunk(SPL_START), boot_crc32(code_chunk(SPL_START), BOOT_CHUNK_BYTES));
    CHECK(data_answer() == SML_ERROR_STATE); // no ERASE yet
    commit_args(args, NEW_SPL, code_crc());
    command(SML_BOOT_CMD_CONFIRM, SML_BOOT_REGION_SPL, SML_BOOT_KEY_COMMIT, NULL);
    CHECK(response(0) && last_status == SML_RESULT_ACK); // confirmed already: a repeat
    CHECK(!spl_writer_busy() && !spl_writer_owns_flash());

    // ERASE: one page an epoch, only in the window, the record's first.
    command(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_SPL, SML_BOOT_KEY_ERASE, NULL);
    CHECK(response(0) && last_status == SML_RESULT_ACCEPTED);
    CHECK(spl_writer_owns_flash() && spl_writer_busy());
    sent_count = 0;
    spl_writer_service(epoch, false);
    CHECK(read_word(SPL_RECORD_ADDRESS) != ERASED); // nothing outside the window
    command(SML_BOOT_CMD_ERASE, SML_BOOT_REGION_SPL, SML_BOOT_KEY_ERASE, NULL);
    CHECK(response(0) && last_detail == SML_ERROR_BUSY);
    data(SPL_START, code_chunk(SPL_START), 0);
    CHECK(data_answer() == SML_ERROR_BUSY);
    sent_count = 0;
    epoch++;
    spl_writer_service(epoch, true);
    spl_writer_service(epoch, true);
    CHECK(read_word(SPL_RECORD_ADDRESS) == ERASED && read_word(SPL_START) != ERASED);
    CHECK(spl_writer_status()->spl_state == 0);
    CHECK(sent_count == 0);
    windows(1);
    CHECK(read_word(SPL_START) == ERASED && read_word(SPL_START + BOOT_PAGE_ADDRESSES) != ERASED);
    windows(1);
    CHECK(response(0) && last_status == SML_RESULT_ACK && last_value == 3);
    CHECK(!spl_writer_owns_flash() && spl_writer_busy() && spl_writer_status()->session);

    // DATA: held for the window, CRC, a repeat, not blank, out of range.
    {
        const uint8_t *chunk = code_chunk(SPL_START);
        uint32_t crc = boot_crc32(chunk, BOOT_CHUNK_BYTES);
        uint8_t other[BOOT_CHUNK_BYTES];

        data(SPL_START, chunk, crc ^ 1U);
        CHECK(data_answer() == SML_ERROR_CRC);
        data(SPL_START, chunk, crc);
        CHECK(sent_count == 0 && !spl_writer_owns_flash()); // held
        spl_writer_service(epoch, false);
        CHECK(sent_count == 0);
        windows(1);
        CHECK(data_answer() == SML_ERROR_NONE &&
              sml_get_u32(&sent[0].payload[8]) == crc && read_word(SPL_START) != ERASED);
        data(SPL_START, chunk, crc); // its ACK lost: answered at once
        CHECK(data_answer() == SML_ERROR_NONE);
        memcpy(other, chunk, sizeof(other));
        other[7] ^= 0x40;
        data(SPL_START, other, boot_crc32(other, sizeof(other)));
        CHECK(data_answer() == SML_ERROR_NOT_BLANK);
        data(SPL_RECORD_ADDRESS, chunk, crc);
        CHECK(data_answer() == SML_ERROR_ARGUMENT); // the record row is never data
        data(SPL_START + 0x40, chunk, crc);
        CHECK(data_answer() == SML_ERROR_ARGUMENT);
        data(APP_START, chunk, crc);
        CHECK(data_answer() == SML_ERROR_ARGUMENT);
    }

    // VERIFY: any chunk, at once.
    {
        uint8_t v[SML_BOOT_VERIFY_BYTES] = {0};

        sml_put_u32(v, APP_START);
        sent_count = 0;
        spl_writer_frame(SML_BOOT_VERIFY, v, sizeof(v));
        CHECK(sent_count == 1 && sent[0].id == SML_BOOT_VERIFY_RESPONSE &&
              sml_get_u16(&sent[0].payload[0]) == SML_RESULT_ACK &&
              sent[0].payload[12] == (uint8_t)flash[APP_START / 2U]);
        sml_put_u32(v, BOOT_VERIFY_LIMIT);
        sent_count = 0;
        spl_writer_frame(SML_BOOT_VERIFY, v, sizeof(v));
        CHECK(sml_get_u16(&sent[0].payload[2]) == SML_ERROR_ARGUMENT);
    }

    // The rest of the chunks, then COMMIT: a wrong CRC first.
    {
        uint32_t address;

        for (address = SPL_START + BOOT_CHUNK_ADDRESSES; address < SPL_START + 2U * spl_length;
             address += BOOT_CHUNK_ADDRESSES)
        {
            const uint8_t *chunk = code_chunk(address);

            data(address, chunk, boot_crc32(chunk, BOOT_CHUNK_BYTES));
            windows(1);
            CHECK(data_answer() == SML_ERROR_NONE);
        }
        commit_args(args, NEW_SPL, code_crc() ^ 0x10U);
        command(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_SPL, SML_BOOT_KEY_COMMIT, args);
        CHECK(response(0) && last_status == SML_RESULT_ACCEPTED && spl_writer_owns_flash());
        sent_count = 0;
        windows(1);
        CHECK(response(0) && last_detail == SML_ERROR_VERIFY && last_value == code_crc());
        commit_args(args, NEW_SPL, code_crc());
        sml_put_u32(&args[4], APP_START);
        command(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_SPL, SML_BOOT_KEY_COMMIT, args);
        CHECK(response(0) && last_detail == SML_ERROR_ARGUMENT);
        commit_args(args, NEW_SPL, code_crc());
        command(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_SPL, SML_BOOT_KEY_COMMIT, args);
        sent_count = 0;
        windows(1);
        CHECK(response(0) && last_status == SML_RESULT_ACK);
        CHECK(spl_writer_status()->spl_state ==
              (SML_SPL_STATE_RECORD | SML_SPL_STATE_COMMITTED | SML_SPL_STATE_INTACT));
        CHECK(spl_writer_status()->spl_version == NEW_SPL && !spl_writer_busy());
        // Its ACK lost: ACK again. Another record without a new ERASE: STATE.
        command(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_SPL, SML_BOOT_KEY_COMMIT, args);
        sent_count = 0;
        windows(1);
        CHECK(response(0) && last_status == SML_RESULT_ACK);
        commit_args(args, NEW_SPL + 1U, code_crc());
        command(SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_SPL, SML_BOOT_KEY_COMMIT, args);
        sent_count = 0;
        windows(1);
        CHECK(response(0) && last_detail == SML_ERROR_STATE);
    }

    // The IPL now starts the new SPL, unconfirmed: a trial, the watchdog.
    {
        BootMailbox mailbox;
        IplChoice choice;

        memset(&mailbox, 0, sizeof(mailbox));
        choice = ipl_start(&mailbox);
        CHECK(choice.start == IPL_START_SPL && choice.watchdog && mailbox.trials == 1 &&
              (mailbox.boot & BOOT_PATH_UNCONFIRMED) != 0U);
    }

    // CONFIRM: only after a start by that unconfirmed SPL, APP_CONFIRM_S healthy.
    command(SML_BOOT_CMD_CONFIRM, SML_BOOT_REGION_SPL, SML_BOOT_KEY_COMMIT, NULL);
    CHECK(response(0) && last_detail == SML_ERROR_STATE); // this start was not by it
    app.boot = SML_BOOT_PATH_UNCONFIRMED;
    app.healthy_s = APP_CONFIRM_S - 1;
    command(SML_BOOT_CMD_CONFIRM, SML_BOOT_REGION_SPL, SML_BOOT_KEY_COMMIT, NULL);
    CHECK(response(0) && last_detail == SML_ERROR_STATE);
    app.healthy_s = APP_CONFIRM_S;
    command(SML_BOOT_CMD_CONFIRM, SML_BOOT_REGION_SPL, SML_BOOT_KEY_ERASE, NULL);
    CHECK(response(0) && last_detail == SML_ERROR_ARGUMENT);
    command(SML_BOOT_CMD_CONFIRM, SML_BOOT_REGION_SPL, SML_BOOT_KEY_COMMIT, NULL);
    CHECK(response(0) && last_status == SML_RESULT_ACCEPTED && spl_writer_owns_flash());
    sent_count = 0;
    windows(1);
    CHECK(response(0) && last_status == SML_RESULT_ACK);
    CHECK((spl_writer_status()->spl_state & SML_SPL_STATE_CONFIRMED) != 0U);
    {
        BootMailbox mailbox;

        memset(&mailbox, 0, sizeof(mailbox));
        (void)ipl_start(&mailbox);
        CHECK((mailbox.boot & BOOT_PATH_UNCONFIRMED) == 0U && mailbox.trials == 0); // confirmed
    }
    CHECK(refused == 0 && overwrites == 0);

    // Reject: the IPL gave up on an unconfirmed SPL (out of trials).
    fresh();
    program_spl(NEW_SPL, false);
    {
        BootMailbox mailbox;
        IplChoice choice;
        int start;

        memset(&mailbox, 0, sizeof(mailbox));
        for (start = 0; start < SPL_TRIAL_LIMIT; start++)
        {
            choice = ipl_start(&mailbox); // it hangs: the watchdog each time
            CHECK(choice.start == IPL_START_SPL);
        }
        choice = ipl_start(&mailbox);
        CHECK(choice.start == IPL_START_APP && (mailbox.boot & BOOT_PATH_SPL_GIVEN_UP) != 0U);
        app.boot = (uint8_t)mailbox.boot;
        spl_writer_init(&port);
        CHECK(spl_writer_status()->reject_due);
        spl_writer_service(epoch, false);
        CHECK(read_word(SPL_RECORD_ADDRESS + 2U * SPL_RECORD_REJECT) == ERASED);
        windows(1);
        CHECK((spl_writer_status()->spl_state & SML_SPL_STATE_REJECTED) != 0U);
        CHECK(spl_writer_status()->rejects == 1 && !spl_writer_status()->reject_due);
        // Now even with trials back (a power cut), the IPL starts the application.
        memset(&mailbox, 0, sizeof(mailbox));
        choice = ipl_start(&mailbox);
        CHECK(choice.start == IPL_START_APP);
        spl_writer_init(&port); // rejected already: nothing more
        CHECK(!spl_writer_status()->reject_due);
    }
    // A confirmed SPL given up on (rejected for some other reason): left alone.
    fresh();
    app.boot = SML_BOOT_PATH_SPL_GIVEN_UP;
    spl_writer_init(&port);
    CHECK(!spl_writer_status()->reject_due);

    // Power cuts throughout an update, then CONFIRM: whatever is left, the
    // IPL starts an intact SPL or the application, and a new update works.
    {
        long total;
        volatile long at;
        volatile int mode;
        long outcomes[3] = {0, 0, 0}; // old SPL, new SPL, the application
        long bad = 0;

        fresh();
        make_code(1700, 7);
        operations = 0;
        CHECK(install(NEW_SPL));
        total = operations;
        CHECK(total > 3);
        for (mode = 0; mode < 3; mode++)
        {
            for (at = 0; at < total; at++)
            {
                BootMailbox mailbox;
                IplChoice choice;

                fresh();
                make_code(1700, 7);
                operations = 0;
                cut_at = at;
                cut_mode = mode;
                if (setjmp(cut_jump) == 0)
                {
                    (void)install(NEW_SPL);
                    bad++; // the cut never came
                    continue;
                }
                cut_at = NEVER;
                memset(&mailbox, 0, sizeof(mailbox));
                choice = ipl_start(&mailbox);
                if (choice.start == IPL_START_SPL)
                {
                    uint32_t version = (read_word(SPL_RECORD_ADDRESS + 4U) & 0xFFFFUL) << 16 |
                                       (read_word(SPL_RECORD_ADDRESS + 6U) & 0xFFFFUL);

                    outcomes[version == NEW_SPL ? 1 : 0]++;
                }
                else if (choice.start == IPL_START_APP)
                {
                    outcomes[2]++;
                }
                else
                {
                    bad++;
                }
                // The application starts again and the update is done anew.
                app.boot = (uint8_t)mailbox.boot;
                spl_writer_init(&port);
                windows(1); // a reject, if due
                if (!install(NEW_SPL))
                {
                    bad++;
                }
                if ((spl_writer_status()->spl_state & SML_SPL_STATE_INTACT) == 0U)
                {
                    bad++;
                }
            }
        }
        printf("  %ld flash operations, 3 ways each: old SPL %ld, new %ld, the application %ld\n",
               total, outcomes[0], outcomes[1], outcomes[2]);
        CHECK(bad == 0);
        CHECK(outcomes[1] > 0 && outcomes[2] > 0);
        CHECK(overwrites == 0);
    }

    // A cut while writing confirm: not confirmed, or confirmed; never
    // anything the IPL would not start.
    {
        volatile int mode;

        for (mode = 0; mode < 3; mode++)
        {
            BootMailbox mailbox;
            IplChoice choice;

            fresh();
            program_spl(NEW_SPL, false);
            app.boot = SML_BOOT_PATH_UNCONFIRMED;
            app.healthy_s = 70;
            spl_writer_init(&port);
            command(SML_BOOT_CMD_CONFIRM, SML_BOOT_REGION_SPL, SML_BOOT_KEY_COMMIT, NULL);
            operations = 0;
            cut_at = 0;
            cut_mode = mode;
            if (setjmp(cut_jump) == 0)
            {
                windows(1);
            }
            cut_at = NEVER;
            memset(&mailbox, 0, sizeof(mailbox));
            choice = ipl_start(&mailbox);
            CHECK(choice.start == IPL_START_SPL);
            // unconfirmed unless it completed
            CHECK(((mailbox.boot & BOOT_PATH_UNCONFIRMED) != 0U) == (mode != 2));
        }
        // ... and reject: rejected however little of it was written.
        for (mode = 0; mode < 2; mode++)
        {
            BootMailbox mailbox;

            fresh();
            program_spl(NEW_SPL, false);
            app.boot = SML_BOOT_PATH_SPL_GIVEN_UP;
            spl_writer_init(&port);
            operations = 0;
            cut_at = 0;
            cut_mode = mode;
            if (setjmp(cut_jump) == 0)
            {
                windows(1);
            }
            cut_at = NEVER;
            memset(&mailbox, 0, sizeof(mailbox));
            if (mode == 0)
            {
                CHECK(ipl_start(&mailbox).start == IPL_START_APP); // any bit rejects
            }
            else
            {
                CHECK(ipl_start(&mailbox).start == IPL_START_SPL); // not started: a trial
            }
        }
    }
    CHECK(refused == 0);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
