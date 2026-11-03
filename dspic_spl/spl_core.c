#include "spl_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "../common/boot_crc.h"
#include "../common/sml.h"
#include "spl_port.h"

#define ERASED_WORD      0xFFFFFFUL
#define NO_REGION        0xFFU
#define CRC_SLICE        8U    // instructions between services: about 20 us at 40 MIPS
#define FRAME_IDLE_MS    100U  // a part-received frame is dropped after this (SML)
#define STATUS_PERIOD_MS 1000U // BOOT-STATUS while nothing else is happening
#define ACTIVITY_MS      250U  // the LED shows activity, not the mode, for this long

// A region the SPL writes: APP, or TEST in the same layout. The code runs
// from start to end (exclusive), the record row at record. The erase covers
// the record's page first, then the code's pages.
typedef struct
{
    uint32_t start;
    uint32_t end;
    uint32_t record;
    uint32_t max; // instructions
} Region;

static const Region regions[] = {
    {APP_START, APP_END, APP_RECORD_ADDRESS, APP_MAX},                     // SML_BOOT_REGION_APP
    {TEST_START, TEST_RECORD_ADDRESS, TEST_RECORD_ADDRESS, TEST_CODE_MAX}, // TEST
};

typedef struct
{
    uint8_t state; // SML_SPL_STATE_*: RECORD, COMMITTED (and the SPL's INTACT, ...)
    uint32_t version;
    uint32_t length;
    uint32_t crc32;
    uint32_t build_time;
} Record;

static struct
{
    volatile BootMailbox *mailbox;
    uint8_t mode;   // SML_BOOT_MODE_*
    uint16_t flags; // REQUESTED, POWER_ON: fixed at entry
    bool image_ok;  // the application's CRC-32 matches its record
    bool checked;   // the CRC-32 has been computed
    uint32_t crc32_computed;
    Record app;
    Record spl;
    uint16_t ipl_version;
    uint32_t device_id;
    uint8_t erased; // the region erased in this session (NO_REGION): DATA and COMMIT
    uint32_t window_start;
    uint32_t last_traffic; // a frame sent or received
    uint32_t last_rx;
    uint32_t activity; // the LED shows activity until then
    bool activity_on;
    SmlParser parser;
    uint8_t tx[SML_MAX_FRAME_BYTES];
} spl;

// ---- Flash ----------------------------------------------------------------------

static uint16_t record_word(uint32_t base, uint8_t index)
{
    return (uint16_t)spl_port_flash_read(base + 2U * (uint32_t)index);
}


static uint32_t record_long(uint32_t base, uint8_t index)
{
    return (uint32_t)record_word(base, index) << 16 | record_word(base, (uint8_t)(index + 1U));
}


static bool record_pair(uint32_t base, uint8_t index, uint16_t first, uint16_t second)
{
    return record_word(base, index) == first && record_word(base, (uint8_t)(index + 1U)) == second;
}


// The CRC-32 of instructions words from address, a slice at a time, the
// UART serviced between slices: 0.15 s for the whole application.
static uint32_t crc32_flash(uint32_t address, uint32_t instructions)
{
    uint32_t crc = BOOT_CRC32_INIT;

    while (instructions != 0UL)
    {
        uint32_t slice = instructions < CRC_SLICE ? instructions : CRC_SLICE;

        crc = spl_port_crc32_flash(crc, address, slice);
        address += 2U * slice;
        instructions -= slice;
        spl_port_service();
    }
    return ~crc;
}


/* A record: RECORD if its magic, format, length (even, 1 to max) and
 * record_crc are right, COMMITTED with its commit as well, and the fields.
 * Both layouts have the version at word 2, and the CRC-32 and build time
 * after the length. */
static void read_record(Record *record, uint32_t base, uint16_t magic, uint8_t length_index,
                        uint32_t max)
{
    uint16_t words[RECORD_CRC_WORDS];
    uint8_t index;

    memset(record, 0, sizeof(*record));
    for (index = 0; index < RECORD_CRC_WORDS; index++)
    {
        words[index] = record_word(base, index);
    }
    record->length = (uint32_t)words[length_index] << 16 | words[length_index + 1U];
    if (words[RECORD_MAGIC] != magic || words[RECORD_FORMAT] != RECORD_FORMAT_1 ||
        record->length == 0UL || (record->length & 1UL) != 0UL || record->length > max ||
        record_word(base, RECORD_CRC) != boot_record_crc(words))
    {
        record->length = 0;
        return;
    }
    record->state = SML_SPL_STATE_RECORD;
    if (record_pair(base, RECORD_COMMIT, RECORD_COMMIT_0, RECORD_COMMIT_1))
    {
        record->state |= SML_SPL_STATE_COMMITTED;
    }
    record->version = (uint32_t)words[2] << 16 | words[3];
    record->crc32 = (uint32_t)words[length_index + 2U] << 16 | words[length_index + 3U];
    record->build_time = (uint32_t)words[length_index + 4U] << 16 | words[length_index + 5U];
}


static void read_app_record(void)
{
    read_record(&spl.app, APP_RECORD_ADDRESS, APP_RECORD_MAGIC_VALUE, APP_RECORD_LENGTH, APP_MAX);
    if (record_long(APP_RECORD_ADDRESS, APP_RECORD_START) != APP_START)
    {
        memset(&spl.app, 0, sizeof(spl.app));
    }
}


// The application's image against its record: valid only when committed.
static void check_app(void)
{
    spl.image_ok = false;
    spl.checked = false;
    spl.crc32_computed = 0;
    if ((spl.app.state & SML_SPL_STATE_COMMITTED) == 0U)
    {
        return;
    }
    spl.crc32_computed = crc32_flash(APP_START, spl.app.length);
    spl.checked = true;
    spl.image_ok = spl.crc32_computed == spl.app.crc32;
}


static void read_spl_record(void)
{
    Record *own = &spl.spl;

    read_record(own, SPL_RECORD_ADDRESS, SPL_RECORD_MAGIC_VALUE, SPL_RECORD_LENGTH, SPL_CODE_MAX);
    if ((own->state & SML_SPL_STATE_COMMITTED) == 0U)
    {
        return;
    }
    if (record_pair(SPL_RECORD_ADDRESS, SPL_RECORD_CONFIRM, SPL_CONFIRM_0, SPL_CONFIRM_1))
    {
        own->state |= SML_SPL_STATE_CONFIRMED;
    }
    if (!record_pair(SPL_RECORD_ADDRESS, SPL_RECORD_REJECT, 0xFFFFU, 0xFFFFU))
    {
        own->state |= SML_SPL_STATE_REJECTED;
    }
    if (crc32_flash(SPL_START, own->length) == own->crc32)
    {
        own->state |= SML_SPL_STATE_INTACT;
    }
}


// The application does not reach the test page: its record says so.
static bool test_free(void)
{
    return (spl.app.state & SML_SPL_STATE_COMMITTED) != 0U &&
           spl.app.length <= (TEST_START - APP_START) / 2U;
}


// Instruction index of a packed chunk (3 bytes, least significant first),
// or an erased word for no data.
static uint32_t chunk_word(const uint8_t *data, uint8_t index)
{
    const uint8_t *p = data + 3U * index;

    if (data == NULL)
    {
        return ERASED_WORD;
    }
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
}


// The chunk's flash equals data, or with no data, is erased.
static bool chunk_is(uint32_t address, const uint8_t *data)
{
    uint8_t index;

    spl_port_service();
    for (index = 0; index < BOOT_CHUNK_INSTRUCTIONS; index++)
    {
        if (spl_port_flash_read(address + 2U * (uint32_t)index) != chunk_word(data, index))
        {
            return false;
        }
    }
    return true;
}


// Writes each double word of the chunk that flash does not already hold,
// in ascending order, then reads the chunk back.
static uint16_t write_chunk(uint32_t address, const uint8_t *data)
{
    uint8_t index;

    for (index = 0; index < BOOT_CHUNK_INSTRUCTIONS; index += 2U)
    {
        uint32_t at = address + 2U * (uint32_t)index;
        uint32_t first = chunk_word(data, index);
        uint32_t second = chunk_word(data, (uint8_t)(index + 1U));

        if (spl_port_flash_read(at) == first && spl_port_flash_read(at + 2U) == second)
        {
            continue;
        }
        if (!spl_port_flash_write(at, first, second))
        {
            return SML_ERROR_STORAGE;
        }
        spl_port_service();
    }
    return chunk_is(address, data) ? SML_ERROR_NONE : SML_ERROR_VERIFY;
}


// ---- Sending --------------------------------------------------------------------

static uint8_t *payload(void)
{
    return spl.tx + SML_HEADER_BYTES;
}


static void send(uint8_t message_id, uint16_t length)
{
    uint16_t bytes = sml_frame(SML_CLASS_BOOT, message_id, payload(), length, spl.tx);

    spl_port_tx(spl.tx, bytes);
    spl.last_traffic = spl_port_ms();
}


static uint8_t saturated(uint16_t value)
{
    return value > 0xFFU ? 0xFFU : (uint8_t)value;
}


static void send_status(void)
{
    uint8_t *p = payload();
    uint16_t flags = spl.flags;
    uint32_t elapsed = spl_port_ms() - spl.window_start;

    flags |= (uint16_t)(spl.app.state & (SML_BOOT_FLAG_RECORD | SML_BOOT_FLAG_COMMITTED));
    if ((spl.app.state & SML_SPL_STATE_COMMITTED) != 0U && !spl.checked)
    {
        flags |= SML_BOOT_FLAG_CHECKING;
    }
    if (spl.image_ok)
    {
        flags |= SML_BOOT_FLAG_IMAGE_OK;
    }
    if (spl.erased != NO_REGION)
    {
        flags |= SML_BOOT_FLAG_ERASED;
    }
    if (test_free())
    {
        flags |= SML_BOOT_FLAG_TEST_FREE;
    }
    memset(p, 0, SML_BOOT_STATUS_BYTES);
    sml_put_u32(&p[0], spl.spl.version);
    p[4] = SML_BOOT_PROTOCOL;
    p[5] = spl.mode;
    sml_put_u16(&p[6], flags);
    sml_put_u16(&p[8], spl.mailbox->reset_cause);
    if (spl.mode == SML_BOOT_MODE_WINDOW && elapsed < BOOT_WINDOW_MS)
    {
        sml_put_u16(&p[10], (uint16_t)(BOOT_WINDOW_MS - elapsed));
    }
    sml_put_u32(&p[12], spl.device_id);
    sml_put_u32(&p[16], APP_START);
    sml_put_u32(&p[20], APP_END);
    sml_put_u32(&p[24], spl.app.version);
    sml_put_u32(&p[28], spl.app.length);
    sml_put_u32(&p[32], spl.app.crc32);
    sml_put_u32(&p[36], spl.crc32_computed);
    sml_put_u32(&p[40], spl.app.build_time);
    p[44] = saturated(spl.mailbox->attempts);
    p[45] = (uint8_t)spl.mailbox->boot;
    sml_put_u16(&p[46], spl.ipl_version);
    p[48] = spl.spl.state;
    p[49] = saturated(spl.mailbox->trials);
    // 50: healthy_s, 0 from the SPL
    sml_put_u32(&p[52], spl.spl.crc32);
    send(SML_BOOT_STATUS, SML_BOOT_STATUS_BYTES);
}


static void respond(const uint8_t *command, uint16_t status, uint16_t detail, uint32_t value)
{
    uint8_t *p = payload();

    sml_put_u16(&p[0], status);
    sml_put_u16(&p[2], detail);
    memcpy(&p[4], command, 4U); // token, command, region
    sml_put_u32(&p[8], value);
    send(SML_BOOT_RESPONSE, SML_BOOT_RESPONSE_BYTES);
}


static void activity(void)
{
    spl.activity = spl_port_ms() + ACTIVITY_MS;
    spl.activity_on = !spl.activity_on;
    spl_port_led(spl.activity_on);
}


// ---- Commands -------------------------------------------------------------------

// The region of a writing command, or NULL if it is not one the SPL writes.
static const Region *region_of(uint8_t region)
{
    if (region == SML_BOOT_REGION_APP)
    {
        return &regions[0];
    }
    if (region == SML_BOOT_REGION_TEST)
    {
        return &regions[1];
    }
    return NULL;
}


static bool erase_page(uint32_t page, uint32_t *pages)
{
    spl_port_service();
    if (!spl_port_flash_erase(page))
    {
        return false;
    }
    (*pages)++;
    return true;
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


// The record's page, then the code's pages, then every page checked blank.
// *pages: the pages erased.
static bool erase_region(const Region *region, uint32_t *pages)
{
    uint32_t record_page = region->record & (uint32_t) ~(BOOT_PAGE_ADDRESSES - 1UL);
    uint32_t page;

    *pages = 0;
    if (!erase_page(record_page, pages))
    {
        return false;
    }
    for (page = region->start; page < region->end; page += BOOT_PAGE_ADDRESSES)
    {
        if (page != record_page && !erase_page(page, pages))
        {
            return false;
        }
    }
    if (!page_blank(record_page))
    {
        return false;
    }
    for (page = region->start; page < region->end; page += BOOT_PAGE_ADDRESSES)
    {
        if (!page_blank(page))
        {
            return false;
        }
    }
    return true;
}


// ERASE: SML_ERROR_*, *value the pages erased.
static uint16_t command_erase(const uint8_t *command, const Region *region, uint32_t *value)
{
    bool app = command[3] == SML_BOOT_REGION_APP;
    bool ok;

    if (!app && !test_free())
    {
        return SML_ERROR_STATE;
    }
    // The invariant: an unconfirmed SPL never erases a valid application.
    if (app && spl.image_ok && (spl.spl.state & SML_SPL_STATE_CONFIRMED) == 0U)
    {
        return SML_ERROR_STATE;
    }
    respond(command, SML_RESULT_ACCEPTED, SML_ERROR_NONE, 0);
    spl.erased = NO_REGION;
    if (app)
    {
        spl.image_ok = false;
        spl.checked = false;
        spl.crc32_computed = 0;
        if (spl.mode != SML_BOOT_MODE_HELD)
        {
            spl.mode = SML_BOOT_MODE_NO_APP;
        }
    }
    ok = erase_region(region, value);
    if (app)
    {
        read_app_record();
    }
    if (!ok)
    {
        return SML_ERROR_STORAGE;
    }
    spl.erased = command[3];
    return SML_ERROR_NONE;
}


// A record word into a packed row: 16 bits, upper byte 0xFF.
static void put_word(uint8_t *row, uint8_t index, uint16_t value)
{
    row[3U * index] = (uint8_t)value;
    row[3U * index + 1U] = (uint8_t)(value >> 8);
}


// COMMIT: SML_ERROR_*, *value the CRC-32 found on a mismatch. The record row
// is built as a packed chunk, so the chunk routines compare and write it:
// words 0-15 first, read back, then the commit, read back.
static uint16_t command_commit(const uint8_t *command, const Region *region, uint32_t *value)
{
    const uint8_t *args = command + 8;
    uint32_t length = sml_get_u32(args + 8);
    uint8_t row[BOOT_CHUNK_BYTES];
    uint16_t words[RECORD_CRC_WORDS];
    uint16_t error;
    uint8_t index;

    if (sml_get_u32(args + 4) != region->start || length == 0UL || (length & 1UL) != 0UL ||
        length > region->max)
    {
        return SML_ERROR_ARGUMENT;
    }
    respond(command, SML_RESULT_ACCEPTED, SML_ERROR_NONE, 0);
    *value = crc32_flash(region->start, length);
    if (*value != sml_get_u32(args + 12))
    {
        return SML_ERROR_VERIFY;
    }
    words[RECORD_MAGIC] = APP_RECORD_MAGIC_VALUE;
    words[RECORD_FORMAT] = RECORD_FORMAT_1;
    for (index = 0; index < 10U; index++) // version, start, length, crc32, build_time
    {
        words[APP_RECORD_VERSION + index] = sml_get_u16(args + 2U * index);
    }
    words[APP_RECORD_SML_VERSION] = sml_get_u16(args + 20);
    words[APP_RECORD_SML_VERSION + 1U] = 0;
    memset(row, 0xFF, sizeof(row));
    for (index = 0; index < RECORD_CRC_WORDS; index++)
    {
        put_word(row, index, words[index]);
    }
    put_word(row, RECORD_CRC, boot_record_crc(words));
    put_word(row, RECORD_COMMIT, RECORD_COMMIT_0);
    put_word(row, RECORD_COMMIT + 1U, RECORD_COMMIT_1);
    if (!chunk_is(region->record, row)) // else a repeat, its ACK lost
    {
        if (spl.erased != command[3] || !chunk_is(region->record, NULL))
        {
            return SML_ERROR_STATE;
        }
        memset(&row[3U * RECORD_COMMIT], 0xFF, 6U);
        error = write_chunk(region->record, row);
        if (error != SML_ERROR_NONE)
        {
            return error;
        }
        put_word(row, RECORD_COMMIT, RECORD_COMMIT_0);
        put_word(row, RECORD_COMMIT + 1U, RECORD_COMMIT_1);
        error = write_chunk(region->record, row);
        if (error != SML_ERROR_NONE)
        {
            return error;
        }
    }
    spl.erased = NO_REGION; // the session ends with its record
    if (command[3] == SML_BOOT_REGION_APP)
    {
        read_app_record();
        spl.checked = true;
        spl.crc32_computed = *value;
        spl.image_ok = (spl.app.state & SML_SPL_STATE_COMMITTED) != 0U;
        spl.mailbox->attempts = 0; // a new application: its own count
        spl.mode = SML_BOOT_MODE_HELD;
    }
    *value = 0;
    return SML_ERROR_NONE;
}


static SplAction command(const uint8_t *frame)
{
    uint8_t code = frame[2];
    const Region *region = region_of(frame[3]);
    uint16_t error = SML_ERROR_NONE;
    uint32_t value = 0;
    SplAction action = SPL_ACTION_NONE;

    switch (code)
    {
        case SML_BOOT_CMD_STATUS: send_status(); break;
        case SML_BOOT_CMD_HOLD: spl.mode = SML_BOOT_MODE_HELD; break;
        case SML_BOOT_CMD_RUN:
            if (spl.image_ok)
            {
                spl.mailbox->attempts++;
                action = SPL_ACTION_HAND_OVER;
            }
            else
            {
                error = SML_ERROR_STATE;
            }
            break;
        case SML_BOOT_CMD_RESET: action = SPL_ACTION_RESET; break;
        case SML_BOOT_CMD_ERASE:
        case SML_BOOT_CMD_COMMIT:
            if (region == NULL ||
                sml_get_u32(frame + 4) !=
                    (code == SML_BOOT_CMD_ERASE ? SML_BOOT_KEY_ERASE : SML_BOOT_KEY_COMMIT))
            {
                error = SML_ERROR_ARGUMENT;
            }
            else if (spl.mode == SML_BOOT_MODE_WINDOW) // HOLD first
            {
                error = SML_ERROR_STATE;
            }
            else
            {
                error = code == SML_BOOT_CMD_ERASE ? command_erase(frame, region, &value)
                                                   : command_commit(frame, region, &value);
                activity();
            }
            break;
        default: // CONFIRM is the application's
            error = SML_ERROR_UNSUPPORTED;
            break;
    }
    respond(frame, (uint16_t)(error == SML_ERROR_NONE ? SML_RESULT_ACK : SML_RESULT_NACK), error,
            value);
    return action;
}


static void data(const uint8_t *frame)
{
    uint32_t address = sml_get_u32(frame);
    const Region *region = region_of(frame[4]);
    const uint8_t *chunk = frame + 12;
    uint16_t error = SML_ERROR_NONE;
    uint8_t *p = payload();

    if (region == NULL || address < region->start || address >= region->end ||
        (address & (BOOT_CHUNK_ADDRESSES - 1UL)) != 0UL)
    {
        error = SML_ERROR_ARGUMENT;
    }
    else if (spl.erased != frame[4])
    {
        error = SML_ERROR_STATE;
    }
    else if (boot_crc32(chunk, BOOT_CHUNK_BYTES) != sml_get_u32(frame + 8))
    {
        error = SML_ERROR_CRC;
    }
    else if (!chunk_is(address, chunk)) // else a repeat, its ACK lost
    {
        error = chunk_is(address, NULL) ? write_chunk(address, chunk) : SML_ERROR_NOT_BLANK;
    }
    sml_put_u16(&p[0], (uint16_t)(error == SML_ERROR_NONE ? SML_RESULT_ACK : SML_RESULT_NACK));
    sml_put_u16(&p[2], error);
    sml_put_u32(&p[4], address);
    sml_put_u32(&p[8], error == SML_ERROR_NONE ? ~spl_port_crc32_flash(BOOT_CRC32_INIT, address,
                                                                       BOOT_CHUNK_INSTRUCTIONS)
                                               : 0UL);
    send(SML_BOOT_DATA_RESPONSE, SML_BOOT_DATA_RESPONSE_BYTES);
    activity();
}


static void verify(const uint8_t *frame)
{
    uint32_t address = sml_get_u32(frame);
    uint8_t *p = payload();
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
            uint32_t word = spl_port_flash_read(address + 2U * (uint32_t)index);

            p[12U + 3U * index] = (uint8_t)word;
            p[13U + 3U * index] = (uint8_t)(word >> 8);
            p[14U + 3U * index] = (uint8_t)(word >> 16);
        }
        sml_put_u32(&p[8], boot_crc32(&p[12], BOOT_CHUNK_BYTES));
    }
    send(SML_BOOT_VERIFY_RESPONSE, SML_BOOT_VERIFY_RESPONSE_BYTES);
    activity();
}


// A complete frame. Only class 0xF0 requests of the right length are
// answered; anything else is ignored, without an answer.
static SplAction frame_received(const SmlFrame *frame)
{
    if (frame->class_id != SML_CLASS_BOOT)
    {
        return SPL_ACTION_NONE;
    }
    if (frame->message_id == SML_BOOT_COMMAND && frame->length == SML_BOOT_COMMAND_BYTES)
    {
        return command(frame->payload);
    }
    if (frame->message_id == SML_BOOT_DATA && frame->length == SML_BOOT_DATA_BYTES)
    {
        data(frame->payload);
    }
    else if (frame->message_id == SML_BOOT_VERIFY && frame->length == SML_BOOT_VERIFY_BYTES)
    {
        verify(frame->payload);
    }
    return SPL_ACTION_NONE;
}


// ---- Start-up and the loop ---------------------------------------------------------

void spl_init(volatile BootMailbox *mailbox)
{
    uint16_t request;

    memset(&spl, 0, sizeof(spl));
    spl.mailbox = mailbox;
    spl.erased = NO_REGION;
    sml_parser_init(&spl.parser);
    // Started without the IPL (a debugger): a mailbox of our own.
    if (mailbox->magic != MAILBOX_MAGIC_VALUE ||
        mailbox->magic_inverse != (uint16_t)~MAILBOX_MAGIC_VALUE)
    {
        mailbox->magic = MAILBOX_MAGIC_VALUE;
        mailbox->magic_inverse = (uint16_t)~MAILBOX_MAGIC_VALUE;
        mailbox->attempts = 0;
        mailbox->boot = 0;
        mailbox->trials = 0;
        mailbox->reset_cause = 0;
    }
    mailbox->running = 0;
    request = mailbox->request;
    mailbox->request = 0;
    if ((mailbox->reset_cause & BOOT_RESET_POWER) != 0U)
    {
        spl.flags |= SML_BOOT_FLAG_POWER_ON;
    }

    if (record_pair(IPL_INFO_ADDRESS, RECORD_MAGIC, IPL_INFO_MAGIC_VALUE, RECORD_FORMAT_1))
    {
        spl.ipl_version = (uint16_t)(record_long(IPL_INFO_ADDRESS, IPL_INFO_VERSION) >> 8);
    }
    spl.device_id = spl_port_flash_read(0xFF0000UL);
    read_spl_record();
    read_app_record();

    if (request == MAILBOX_REQUEST_STAY)
    {
        spl.mode = SML_BOOT_MODE_HELD;
        spl.flags |= SML_BOOT_FLAG_REQUESTED;
    }
    else if ((spl.app.state & SML_SPL_STATE_COMMITTED) == 0U)
    {
        spl.mode = SML_BOOT_MODE_NO_APP;
    }
    else if (mailbox->attempts >= BOOT_LOOP_LIMIT)
    {
        spl.mode = SML_BOOT_MODE_LOOP;
    }
    else
    {
        spl.mode = SML_BOOT_MODE_WINDOW;
    }
    spl_port_led(false);
    spl.window_start = spl_port_ms();
    send_status(); // CHECKING, if there is an image to check
    check_app();
    if (!spl.image_ok && spl.mode != SML_BOOT_MODE_HELD)
    {
        spl.mode = SML_BOOT_MODE_NO_APP;
    }
    spl.last_rx = spl_port_ms();
}


// The LED by mode, unless it is showing activity: on a 1024 ms cycle,
// HELD 128 ms on and off, NO_APP and LOOP twice at the start of each.
static void led(uint32_t now)
{
    uint16_t phase = (uint16_t)now & 0x3FFU;
    bool on = false;

    if ((int32_t)(spl.activity - now) > 0)
    {
        return;
    }
    if (spl.mode == SML_BOOT_MODE_HELD)
    {
        on = (phase & 0x80U) == 0U;
    }
    else if (spl.mode == SML_BOOT_MODE_NO_APP || spl.mode == SML_BOOT_MODE_LOOP)
    {
        on = phase < 0x180U && (phase & 0x80U) == 0U;
    }
    spl_port_led(on);
}


SplAction spl_poll(void)
{
    SmlFrame frame;
    uint8_t byte;
    uint32_t now;

    spl_port_service();
    now = spl_port_ms();
    while (spl_port_rx(&byte))
    {
        spl.last_rx = now;
        spl.last_traffic = now;
        if (sml_parser_push(&spl.parser, byte, &frame))
        {
            // One request at a time: answer it before reading on. The
            // parser keeps any bytes behind it.
            return frame_received(&frame);
        }
    }
    if (sml_parser_next(&spl.parser, &frame))
    {
        return frame_received(&frame);
    }
    if (now - spl.last_rx >= FRAME_IDLE_MS)
    {
        (void)sml_parser_timeout(&spl.parser);
        spl.last_rx = now;
    }
    if (spl.mode == SML_BOOT_MODE_WINDOW && now - spl.window_start >= BOOT_WINDOW_MS)
    {
        if (spl.image_ok)
        {
            spl.mailbox->attempts++;
            return SPL_ACTION_HAND_OVER;
        }
        spl.mode = SML_BOOT_MODE_NO_APP;
    }
    if (now - spl.last_traffic >= STATUS_PERIOD_MS)
    {
        send_status();
    }
    led(now);
    return SPL_ACTION_NONE;
}
