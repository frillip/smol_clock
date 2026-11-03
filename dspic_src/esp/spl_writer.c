#include "spl_writer.h"

#include <stddef.h>
#include <string.h>

#include "../../common/boot_crc.h"
#include "../../common/boot_layout.h"
#include "../../common/sml.h"

// Healthy running before CONFIRM (the host tests build it as an older
// application's, APP_HEALTHY_S, too).
#ifndef SPL_WRITER_CONFIRM_S
#define SPL_WRITER_CONFIRM_S APP_CONFIRM_S
#endif

#define ERASED_WORD   0xFFFFFFUL
#define DEVICE_ID     0xFF0000UL // where DEVID reads
#define RECORD_PAGE   (SPL_RECORD_ADDRESS & ~(BOOT_PAGE_ADDRESSES - 1UL))
#define TEST_FREE_MAX ((TEST_START - APP_START) / 2UL) // instructions

typedef enum
{
    OP_NONE,
    OP_ERASE,
    OP_DATA,
    OP_COMMIT,
    OP_CONFIRM,
} Op;

// The erase order: the record's page first, so the SPL stops being intact
// at once, then the code's.
static const uint32_t pages[SPL_PAGES] = {RECORD_PAGE, SPL_START, SPL_START + BOOT_PAGE_ADDRESSES};

static struct
{
    const SplWriterPort *port;
    SplWriterStatus status;
    Op op;
    uint8_t command[SML_BOOT_COMMAND_BYTES]; // the request being worked on
    uint8_t erased;                          // pages erased in this ERASE
    bool epoch_known;
    uint32_t erase_epoch; // the epoch of the last page erase
    uint32_t data_address;
    uint8_t data[BOOT_CHUNK_BYTES];
    uint8_t tx[SML_BOOT_VERIFY_RESPONSE_BYTES]; // the largest answer
} w;

// ---- Flash ----------------------------------------------------------------------

static uint16_t record_word(uint8_t index)
{
    return (uint16_t)w.port->read(SPL_RECORD_ADDRESS + 2U * (uint32_t)index);
}


static uint32_t record_long(uint8_t index)
{
    return (uint32_t)record_word(index) << 16 | record_word((uint8_t)(index + 1U));
}


static bool record_pair(uint8_t index, uint16_t first, uint16_t second)
{
    return record_word(index) == first && record_word((uint8_t)(index + 1U)) == second;
}


static uint32_t crc32_flash(uint32_t address, uint32_t instructions)
{
    uint32_t crc = BOOT_CRC32_INIT;

    if (w.port->crc32 != NULL)
    {
        return ~w.port->crc32(crc, address, instructions);
    }
    while (instructions-- != 0UL)
    {
        uint32_t word = w.port->read(address);
        uint8_t bytes[3] = {(uint8_t)word, (uint8_t)(word >> 8), (uint8_t)(word >> 16)};

        crc = boot_crc32_update(crc, bytes, 3U);
        address += 2U;
    }
    return ~crc;
}


// The SPL's record and code, as the IPL would judge them (§4.2).
static void read_spl(void)
{
    uint16_t words[RECORD_CRC_WORDS];
    uint32_t length = record_long(SPL_RECORD_LENGTH);
    uint8_t index;

    w.status.spl_state = 0;
    w.status.spl_version = 0;
    w.status.spl_crc32 = 0;
    for (index = 0; index < RECORD_CRC_WORDS; index++)
    {
        words[index] = record_word(index);
    }
    if (words[RECORD_MAGIC] != SPL_RECORD_MAGIC_VALUE || words[RECORD_FORMAT] != RECORD_FORMAT_1 ||
        length == 0UL || (length & 1UL) != 0UL || length > SPL_CODE_MAX ||
        record_word(RECORD_CRC) != boot_record_crc(words))
    {
        return;
    }
    w.status.spl_state = SML_SPL_STATE_RECORD;
    w.status.spl_version = record_long(SPL_RECORD_VERSION);
    w.status.spl_crc32 = record_long(SPL_RECORD_CRC32);
    if (!record_pair(RECORD_COMMIT, RECORD_COMMIT_0, RECORD_COMMIT_1))
    {
        return;
    }
    w.status.spl_state |= SML_SPL_STATE_COMMITTED;
    if (record_pair(SPL_RECORD_CONFIRM, SPL_CONFIRM_0, SPL_CONFIRM_1))
    {
        w.status.spl_state |= SML_SPL_STATE_CONFIRMED;
    }
    if (!record_pair(SPL_RECORD_REJECT, 0xFFFFU, 0xFFFFU))
    {
        w.status.spl_state |= SML_SPL_STATE_REJECTED;
    }
    if (crc32_flash(SPL_START, length) == w.status.spl_crc32)
    {
        w.status.spl_state |= SML_SPL_STATE_INTACT;
    }
}


static uint32_t chunk_word(const uint8_t *data, uint8_t index)
{
    const uint8_t *p = data + 3U * index;

    return data == NULL ? ERASED_WORD : (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
}


// The chunk's flash equals data, or with no data, is erased.
static bool chunk_is(uint32_t address, const uint8_t *data)
{
    uint8_t index;

    for (index = 0; index < BOOT_CHUNK_INSTRUCTIONS; index++)
    {
        if (w.port->read(address + 2U * (uint32_t)index) != chunk_word(data, index))
        {
            return false;
        }
    }
    return true;
}


// Writes the double words of a chunk that flash does not hold yet, then
// reads it back. A double word already right is left alone, so a record
// row can be written in two parts.
static uint16_t write_chunk(uint32_t address, const uint8_t *data)
{
    uint8_t index;

    for (index = 0; index < BOOT_CHUNK_INSTRUCTIONS; index += 2U)
    {
        uint32_t at = address + 2U * (uint32_t)index;
        uint32_t first = chunk_word(data, index);
        uint32_t second = chunk_word(data, (uint8_t)(index + 1U));

        if (w.port->read(at) == first && w.port->read(at + 2U) == second)
        {
            continue;
        }
        if (!w.port->write(at, first, second))
        {
            w.status.failures++;
            return SML_ERROR_STORAGE;
        }
    }
    if (!chunk_is(address, data))
    {
        w.status.failures++;
        return SML_ERROR_VERIFY;
    }
    return SML_ERROR_NONE;
}


static bool page_blank(uint32_t page)
{
    uint32_t address;

    for (address = page; address < page + BOOT_PAGE_ADDRESSES; address += BOOT_CHUNK_ADDRESSES)
    {
        if (!chunk_is(address, NULL))
        {
            return false;
        }
    }
    return true;
}


// A double word of the record, written once and read back.
static uint16_t write_pair(uint8_t index, uint16_t first, uint16_t second)
{
    uint32_t address = SPL_RECORD_ADDRESS + 2U * (uint32_t)index;

    if (!w.port->write(address, 0xFF0000UL | first, 0xFF0000UL | second))
    {
        w.status.failures++;
        return SML_ERROR_STORAGE;
    }
    if (!record_pair(index, first, second))
    {
        w.status.failures++;
        return SML_ERROR_VERIFY;
    }
    return SML_ERROR_NONE;
}

// ---- Answers --------------------------------------------------------------------

static void respond(const uint8_t *command, uint16_t status, uint16_t detail, uint32_t value)
{
    sml_put_u16(&w.tx[0], status);
    sml_put_u16(&w.tx[2], detail);
    memcpy(&w.tx[4], command, 4U); // token, command, region
    sml_put_u32(&w.tx[8], value);
    w.port->send(SML_BOOT_RESPONSE, w.tx, SML_BOOT_RESPONSE_BYTES);
}


static void finish(uint16_t error, uint32_t value)
{
    respond(w.command, (uint16_t)(error == SML_ERROR_NONE ? SML_RESULT_ACK : SML_RESULT_NACK), error,
            value);
    w.op = OP_NONE;
}


static void data_response(uint32_t address, uint16_t error)
{
    sml_put_u16(&w.tx[0], (uint16_t)(error == SML_ERROR_NONE ? SML_RESULT_ACK : SML_RESULT_NACK));
    sml_put_u16(&w.tx[2], error);
    sml_put_u32(&w.tx[4], address);
    sml_put_u32(&w.tx[8],
                error == SML_ERROR_NONE ? crc32_flash(address, BOOT_CHUNK_INSTRUCTIONS) : 0UL);
    w.port->send(SML_BOOT_DATA_RESPONSE, w.tx, SML_BOOT_DATA_RESPONSE_BYTES);
}


static uint16_t app_record_word(uint8_t index)
{
    return (uint16_t)w.port->read(APP_RECORD_ADDRESS + 2U * (uint32_t)index);
}


static uint32_t app_record_long(uint8_t index)
{
    return (uint32_t)app_record_word(index) << 16 | app_record_word((uint8_t)(index + 1U));
}


static uint8_t saturated(uint16_t value)
{
    return value > 0xFFU ? 0xFFU : (uint8_t)value;
}


// BOOT-STATUS with mode APP: the application is answering, so its image is
// good; its record's values stand for the computed CRC.
static void send_status(void)
{
    SplWriterApp app;
    uint8_t *p = w.tx;
    uint16_t flags = SML_BOOT_FLAG_IMAGE_OK;
    uint32_t length = app_record_long(APP_RECORD_LENGTH);
    uint32_t ipl = (w.port->read(IPL_INFO_ADDRESS + 2U * IPL_INFO_VERSION) & 0xFFFFUL) << 16 |
                   (w.port->read(IPL_INFO_ADDRESS + 2U * (IPL_INFO_VERSION + 1U)) & 0xFFFFUL);
    bool committed;
    uint16_t words[RECORD_CRC_WORDS];
    uint8_t index;

    w.port->app(&app);
    for (index = 0; index < RECORD_CRC_WORDS; index++)
    {
        words[index] = app_record_word(index);
    }
    committed = words[RECORD_MAGIC] == APP_RECORD_MAGIC_VALUE &&
                words[RECORD_FORMAT] == RECORD_FORMAT_1 &&
                app_record_word(RECORD_CRC) == boot_record_crc(words) &&
                app_record_word(RECORD_COMMIT) == RECORD_COMMIT_0 &&
                app_record_word(RECORD_COMMIT + 1U) == RECORD_COMMIT_1;
    if (committed)
    {
        flags |= SML_BOOT_FLAG_RECORD | SML_BOOT_FLAG_COMMITTED;
        if (length <= TEST_FREE_MAX)
        {
            flags |= SML_BOOT_FLAG_TEST_FREE;
        }
    }
    if (w.status.session)
    {
        flags |= SML_BOOT_FLAG_ERASED;
    }
    if ((app.reset_cause & BOOT_RESET_POWER) != 0U)
    {
        flags |= SML_BOOT_FLAG_POWER_ON;
    }
    memset(p, 0, SML_BOOT_STATUS_BYTES);
    sml_put_u32(&p[0], w.status.spl_version);
    p[4] = SML_BOOT_PROTOCOL;
    p[5] = SML_BOOT_MODE_APP;
    sml_put_u16(&p[6], flags);
    sml_put_u16(&p[8], app.reset_cause);
    sml_put_u32(&p[12], w.port->read(DEVICE_ID) & 0xFFFFUL);
    sml_put_u32(&p[16], APP_START);
    sml_put_u32(&p[20], APP_END);
    if (committed)
    {
        sml_put_u32(&p[24], app_record_long(APP_RECORD_VERSION));
        sml_put_u32(&p[28], length);
        sml_put_u32(&p[32], app_record_long(APP_RECORD_CRC32));
        sml_put_u32(&p[36], app_record_long(APP_RECORD_CRC32));
        sml_put_u32(&p[40], app_record_long(APP_RECORD_BUILD_TIME));
    }
    p[44] = saturated(app.attempts);
    p[45] = app.boot;
    if ((w.port->read(IPL_INFO_ADDRESS) & 0xFFFFUL) == IPL_INFO_MAGIC_VALUE)
    {
        sml_put_u16(&p[46], (uint16_t)(ipl >> 8)); // major << 8 | minor
    }
    p[48] = w.status.spl_state;
    p[49] = saturated(app.trials);
    sml_put_u16(&p[50], app.healthy_s);
    sml_put_u32(&p[52], w.status.spl_crc32);
    w.port->send(SML_BOOT_STATUS, p, SML_BOOT_STATUS_BYTES);
}

// ---- Requests -------------------------------------------------------------------

// ERASE, COMMIT, CONFIRM: checked, then ACCEPTED and left to the window.
static uint16_t start_writing(const uint8_t *command, bool *accepted)
{
    SplWriterApp app;
    uint8_t code = command[2];
    uint32_t key = sml_get_u32(command + 4);
    const uint8_t *args = command + 8;

    *accepted = false;
    if (key != (code == SML_BOOT_CMD_ERASE ? SML_BOOT_KEY_ERASE : SML_BOOT_KEY_COMMIT) ||
        command[3] != SML_BOOT_REGION_SPL)
    {
        return SML_ERROR_ARGUMENT;
    }
    w.port->app(&app);
    if (w.op != OP_NONE || app.save_pending)
    {
        return SML_ERROR_BUSY;
    }
    if (code == SML_BOOT_CMD_ERASE && app.healthy_s < APP_HEALTHY_S) // the invariant
    {
        return SML_ERROR_STATE;
    }
    if (code == SML_BOOT_CMD_COMMIT)
    {
        uint32_t length = sml_get_u32(args + 8);

        if (sml_get_u32(args + 4) != SPL_START || length == 0UL || (length & 1UL) != 0UL ||
            length > SPL_CODE_MAX)
        {
            return SML_ERROR_ARGUMENT;
        }
    }
    if (code == SML_BOOT_CMD_CONFIRM)
    {
        uint8_t wanted = SML_SPL_STATE_COMMITTED | SML_SPL_STATE_INTACT;

        if ((w.status.spl_state & SML_SPL_STATE_CONFIRMED) != 0U &&
            (w.status.spl_state & SML_SPL_STATE_REJECTED) == 0U)
        {
            return SML_ERROR_NONE; // a repeat, its ACK lost
        }
        if ((app.boot & SML_BOOT_PATH_UNCONFIRMED) == 0U ||
            (w.status.spl_state & (wanted | SML_SPL_STATE_REJECTED)) != wanted ||
            app.healthy_s < SPL_WRITER_CONFIRM_S)
        {
            return SML_ERROR_STATE;
        }
    }
    memcpy(w.command, command, SML_BOOT_COMMAND_BYTES);
    w.op = code == SML_BOOT_CMD_ERASE    ? OP_ERASE
           : code == SML_BOOT_CMD_COMMIT ? OP_COMMIT
                                         : OP_CONFIRM;
    if (w.op == OP_ERASE)
    {
        w.erased = 0;
        w.status.session = false;
    }
    *accepted = true;
    return SML_ERROR_NONE;
}


static void command(const uint8_t *command)
{
    uint16_t error = SML_ERROR_NONE;
    bool accepted = false;

    switch (command[2])
    {
        case SML_BOOT_CMD_STATUS: send_status(); break;
        case SML_BOOT_CMD_ERASE:
        case SML_BOOT_CMD_COMMIT:
        case SML_BOOT_CMD_CONFIRM: error = start_writing(command, &accepted); break;
        default: error = SML_ERROR_UNSUPPORTED; break; // HOLD, RUN, RESET are the SPL's
    }
    respond(command,
            (uint16_t)(accepted                  ? SML_RESULT_ACCEPTED
                       : error == SML_ERROR_NONE ? SML_RESULT_ACK
                                                 : SML_RESULT_NACK),
            error, 0);
}


static void data(const uint8_t *frame)
{
    uint32_t address = sml_get_u32(frame);
    const uint8_t *chunk = frame + 12;
    uint16_t error = SML_ERROR_NONE;

    if (frame[4] != SML_BOOT_REGION_SPL || address < SPL_START || address >= SPL_RECORD_ADDRESS ||
        (address & (BOOT_CHUNK_ADDRESSES - 1UL)) != 0UL)
    {
        error = SML_ERROR_ARGUMENT;
    }
    else if (w.op != OP_NONE)
    {
        error = SML_ERROR_BUSY;
    }
    else if (!w.status.session)
    {
        error = SML_ERROR_STATE;
    }
    else if (boot_crc32(chunk, BOOT_CHUNK_BYTES) != sml_get_u32(frame + 8))
    {
        error = SML_ERROR_CRC;
    }
    else if (!chunk_is(address, chunk)) // else a repeat, its ACK lost
    {
        if (!chunk_is(address, NULL))
        {
            error = SML_ERROR_NOT_BLANK;
        }
        else
        {
            w.data_address = address;
            memcpy(w.data, chunk, BOOT_CHUNK_BYTES);
            w.op = OP_DATA; // answered once written
            return;
        }
    }
    data_response(address, error);
}


static void verify(const uint8_t *frame)
{
    uint32_t address = sml_get_u32(frame);
    uint8_t *p = w.tx;
    uint8_t index;

    memset(p, 0, SML_BOOT_VERIFY_RESPONSE_BYTES);
    sml_put_u32(&p[4], address);
    if (address >= BOOT_VERIFY_LIMIT || (address & (BOOT_CHUNK_ADDRESSES - 1UL)) != 0UL)
    {
        sml_put_u16(&p[0], SML_RESULT_NACK);
        sml_put_u16(&p[2], SML_ERROR_ARGUMENT);
    }
    else
    {
        for (index = 0; index < BOOT_CHUNK_INSTRUCTIONS; index++)
        {
            uint32_t word = w.port->read(address + 2U * (uint32_t)index);

            p[12U + 3U * index] = (uint8_t)word;
            p[13U + 3U * index] = (uint8_t)(word >> 8);
            p[14U + 3U * index] = (uint8_t)(word >> 16);
        }
        sml_put_u32(&p[8], boot_crc32(&p[12], BOOT_CHUNK_BYTES));
    }
    w.port->send(SML_BOOT_VERIFY_RESPONSE, p, SML_BOOT_VERIFY_RESPONSE_BYTES);
}


void spl_writer_frame(uint8_t message_id, const uint8_t *payload, uint16_t length)
{
    if (message_id == SML_BOOT_COMMAND && length == SML_BOOT_COMMAND_BYTES)
    {
        command(payload);
    }
    else if (message_id == SML_BOOT_DATA && length == SML_BOOT_DATA_BYTES)
    {
        data(payload);
    }
    else if (message_id == SML_BOOT_VERIFY && length == SML_BOOT_VERIFY_BYTES)
    {
        verify(payload);
    }
}

// ---- The work, in the window ------------------------------------------------------

// One page an epoch; then every page checked blank.
static void erase_step(uint32_t epoch)
{
    uint8_t index;

    if (w.erased < SPL_PAGES)
    {
        if (w.epoch_known && epoch == w.erase_epoch)
        {
            return;
        }
        w.epoch_known = true;
        w.erase_epoch = epoch;
        if (!w.port->erase(pages[w.erased]))
        {
            w.status.failures++;
            read_spl();
            finish(SML_ERROR_STORAGE, w.erased);
            return;
        }
        w.erased++;
        w.status.erases++;
        if (w.erased == 1U)
        {
            read_spl(); // the record gone: no longer intact
        }
        if (w.erased < SPL_PAGES)
        {
            return;
        }
    }
    for (index = 0; index < SPL_PAGES; index++)
    {
        if (!page_blank(pages[index]))
        {
            w.status.failures++;
            finish(SML_ERROR_STORAGE, w.erased);
            return;
        }
    }
    w.status.session = true;
    finish(SML_ERROR_NONE, w.erased);
}


static void data_step(void)
{
    uint16_t error = write_chunk(w.data_address, w.data);

    if (error == SML_ERROR_NONE)
    {
        w.status.chunks++;
    }
    w.op = OP_NONE;
    data_response(w.data_address, error);
}


static void put_word(uint8_t *row, uint8_t index, uint16_t value)
{
    row[3U * index] = (uint8_t)value;
    row[3U * index + 1U] = (uint8_t)(value >> 8);
    row[3U * index + 2U] = 0xFFU;
}


// The record row as a packed chunk: words 0-15, and commit if asked.
static void build_row(uint8_t *row, const uint8_t *args, bool with_commit)
{
    uint16_t words[RECORD_CRC_WORDS];
    uint8_t index;

    memset(words, 0, sizeof(words));
    words[RECORD_MAGIC] = SPL_RECORD_MAGIC_VALUE;
    words[RECORD_FORMAT] = RECORD_FORMAT_1;
    words[SPL_RECORD_VERSION] = sml_get_u16(args + 0);
    words[SPL_RECORD_VERSION + 1U] = sml_get_u16(args + 2);
    words[SPL_RECORD_LENGTH] = sml_get_u16(args + 8);
    words[SPL_RECORD_LENGTH + 1U] = sml_get_u16(args + 10);
    words[SPL_RECORD_CRC32] = sml_get_u16(args + 12);
    words[SPL_RECORD_CRC32 + 1U] = sml_get_u16(args + 14);
    words[SPL_RECORD_BUILD_TIME] = sml_get_u16(args + 16);
    words[SPL_RECORD_BUILD_TIME + 1U] = sml_get_u16(args + 18);
    words[SPL_RECORD_PROTOCOL] = sml_get_u16(args + 20);
    memset(row, 0xFF, BOOT_CHUNK_BYTES);
    for (index = 0; index < RECORD_CRC_WORDS; index++)
    {
        put_word(row, index, words[index]);
    }
    put_word(row, RECORD_CRC, boot_record_crc(words));
    put_word(row, RECORD_SPARE, 0xFFFFU);
    if (with_commit)
    {
        put_word(row, RECORD_COMMIT, RECORD_COMMIT_0);
        put_word(row, RECORD_COMMIT + 1U, RECORD_COMMIT_1);
    }
}


// The first 18 words of the row as built (the rest, confirm and reject, may
// already be written on a repeat).
static bool row_holds(const uint8_t *row)
{
    uint8_t index;

    for (index = 0; index < RECORD_COMMIT + 2U; index++)
    {
        if (w.port->read(SPL_RECORD_ADDRESS + 2U * (uint32_t)index) != chunk_word(row, index))
        {
            return false;
        }
    }
    return true;
}


static void commit_step(void)
{
    const uint8_t *args = w.command + 8;
    uint32_t length = sml_get_u32(args + 8);
    uint32_t crc = crc32_flash(SPL_START, length);
    uint8_t row[BOOT_CHUNK_BYTES];
    uint16_t error;

    if (crc != sml_get_u32(args + 12))
    {
        finish(SML_ERROR_VERIFY, crc);
        return;
    }
    build_row(row, args, true);
    if (row_holds(row)) // a repeat, its ACK lost
    {
        finish(SML_ERROR_NONE, 0);
        return;
    }
    if (!w.status.session || !chunk_is(SPL_RECORD_ADDRESS, NULL))
    {
        finish(SML_ERROR_STATE, 0);
        return;
    }
    build_row(row, args, false);
    error = write_chunk(SPL_RECORD_ADDRESS, row);
    if (error == SML_ERROR_NONE)
    {
        error = write_pair(RECORD_COMMIT, RECORD_COMMIT_0, RECORD_COMMIT_1);
    }
    w.status.session = false; // the session ends with its record
    read_spl();
    if (error == SML_ERROR_NONE)
    {
        w.status.commits++;
        w.port->committed();
    }
    finish(error, 0);
}


static void confirm_step(void)
{
    uint16_t error = write_pair(SPL_RECORD_CONFIRM, SPL_CONFIRM_0, SPL_CONFIRM_1);

    read_spl();
    if (error == SML_ERROR_NONE)
    {
        w.status.confirms++;
    }
    finish(error, 0);
}


void spl_writer_service(uint32_t epoch, bool window)
{
    if (!window)
    {
        return;
    }
    if (w.status.reject_due && w.op == OP_NONE)
    {
        w.status.reject_due = false;
        if (write_pair(SPL_RECORD_REJECT, SPL_REJECT_VALUE, SPL_REJECT_VALUE) == SML_ERROR_NONE ||
            (w.port->read(SPL_RECORD_ADDRESS + 2U * SPL_RECORD_REJECT) & 0xFFFFUL) != 0xFFFFUL)
        {
            w.status.rejects++;
        }
        read_spl();
        return;
    }
    switch (w.op)
    {
        case OP_ERASE: erase_step(epoch); break;
        case OP_DATA: data_step(); break;
        case OP_COMMIT: commit_step(); break;
        case OP_CONFIRM: confirm_step(); break;
        default: break;
    }
}


void spl_writer_init(const SplWriterPort *port)
{
    SplWriterApp app;

    memset(&w, 0, sizeof(w));
    w.port = port;
    read_spl();
    port->app(&app);
    // Given up on by the IPL (out of trials) while unconfirmed: rejected, so
    // the IPL stops spending trials on it at every start.
    w.status.reject_due = (app.boot & SML_BOOT_PATH_SPL_GIVEN_UP) != 0U &&
                          (w.status.spl_state & (SML_SPL_STATE_COMMITTED | SML_SPL_STATE_CONFIRMED |
                                                 SML_SPL_STATE_REJECTED)) == SML_SPL_STATE_COMMITTED;
}


bool spl_writer_owns_flash(void)
{
    return w.op == OP_ERASE || w.op == OP_COMMIT || w.op == OP_CONFIRM;
}


bool spl_writer_busy(void)
{
    return w.op != OP_NONE || w.status.session;
}


const SplWriterStatus *spl_writer_status(void)
{
    return &w.status;
}
