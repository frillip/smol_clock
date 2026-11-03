#include "spl_update.h"

#include <stdio.h>
#include <string.h>

#include "sml/boot_crc.h"
#include "sml/boot_layout.h"
#include "sml/sml.h"

#define CHUNK_BYTES     192U
#define CHUNK_ADDRESSES 0x80U
#define TEST_CHUNKS     4U

// Timeouts (ms) and tries. The application answers flash work in its flash
// window, once a second, so its are longer than the SPL's.
#define APP_COMMAND_MS   1000U
#define APP_ACCEPTED_MS  1500U
#define APP_ERASE_MS     10000U // three pages, one an epoch
#define APP_COMMIT_MS    5000U
#define APP_CONFIRM_MS   5000U
#define APP_CHUNK_MS     2500U
#define APP_VERIFY_MS    1000U
#define HEALTHY_POLL_MS  5000U
#define CONFIRM_POLL_MS  2000U
#define HEALTHY_LIMIT_MS 150000U // the application must reach 60 s by then
#define ENTER_MS         2000U
#define BUSY_WAIT_MS     1000U
#define WAIT_SPL_MS      25000U // three watchdog trials, then the application
#define ASK_SYS_MS       1000U
#define SPL_COMMAND_MS   500U
#define SPL_ACCEPTED_MS  1000U
#define SPL_WORK_MS      3000U
#define SPL_CHUNK_MS     500U
#define WAIT_APP_MS      10000U
#define COMMAND_TRIES    3U
#define CHUNK_TRIES      5U
#define BUSY_TRIES       5U

static const char *const step_names[] = {
    "STATUS",    "HEALTHY",     "KEEP_RECORD", "KEEP",       "ERASE",       "DATA",
    "VERIFY",    "COMMIT",      "CHECK",       "ENTER",      "WAIT_SPL",    "HOLD",
    "TEST_ERASE", "TEST_DATA",  "TEST_VERIFY", "TEST_COMMIT", "TEST_CLEAN", "RUN",
    "WAIT_APP",  "WAIT_CONFIRM", "CONFIRM",    "DONE",       "FAILED",      "REJECTED",
};

const char *spl_update_step_name(SplUpdateStep step)
{
    return (unsigned)step < sizeof(step_names) / sizeof(step_names[0]) ? step_names[step] : "?";
}


bool spl_update_running(const SplUpdate *update)
{
    return update->step < SPL_UPDATE_DONE;
}


bool spl_update_holds_link(const SplUpdate *u)
{
    return spl_update_running(u) && u->sent && u->step <= SPL_UPDATE_CHECK &&
           u->step != SPL_UPDATE_HEALTHY;
}


static const char *error_name(uint16_t code)
{
    static const char *const names[] = {"NONE",   "UNSUPPORTED", "LENGTH", "ARGUMENT",
                                        "BUSY",   "STALE",       "SCHEMA", "STORAGE",
                                        "VERIFY", "STATE",       "CRC",    "NOT_BLANK"};

    return code < sizeof(names) / sizeof(names[0]) ? names[code] : "?";
}


static void fail(SplUpdate *u, const char *reason)
{
    snprintf(u->reason, sizeof(u->reason), "%s", reason);
    u->step = SPL_UPDATE_FAILED;
}


static void reject(SplUpdate *u, const char *reason)
{
    snprintf(u->reason, sizeof(u->reason), "%s", reason);
    u->step = SPL_UPDATE_REJECTED;
}


static void fail_nack(SplUpdate *u, const char *what, uint16_t detail)
{
    char text[64];

    snprintf(text, sizeof(text), "%s: NACK %s", what, error_name(detail));
    fail(u, text);
}


static bool in_spl_phase(SplUpdateStep step)
{
    return step >= SPL_UPDATE_HOLD && step <= SPL_UPDATE_RUN;
}

// ---- Requests ---------------------------------------------------------------------

static void begin(SplUpdate *u, uint32_t timeout_ms)
{
    u->token = u->next_token++;
    if (u->next_token == 0)
    {
        u->next_token = 0x7800;
    }
    u->accepted = false;
    u->sent = true;
    u->sent_ms = u->now;
    u->timeout_ms = timeout_ms;
}


static void send_user(SplUpdate *u, uint16_t command, uint32_t argument, uint32_t timeout_ms)
{
    uint8_t p[SML_CMD_USER_BYTES];

    begin(u, timeout_ms);
    memset(p, 0, sizeof(p));
    sml_put_u16(&p[0], command);
    sml_put_u16(&p[4], u->token);
    sml_put_u32(&p[6], argument);
    u->port.send(u->port.context, SML_CLASS_CMD, SML_CMD_USER, p, sizeof(p));
}


static void ask_sys(SplUpdate *u, uint32_t timeout_ms)
{
    send_user(u, SML_USER_REQUEST_MESSAGE, (uint32_t)SML_CLASS_STAT << 8 | SML_STAT_SYS,
              timeout_ms);
}


static void test_chunk(uint8_t *buffer, uint32_t chunk)
{
    uint32_t index;

    for (index = 0; index < CHUNK_BYTES; index++)
    {
        buffer[index] = (uint8_t)(index * 37U + chunk * 101U + 5U);
    }
}


static uint32_t test_crc(void)
{
    uint8_t chunk[CHUNK_BYTES];
    uint32_t crc = BOOT_CRC32_INIT;
    uint32_t index;

    for (index = 0; index < TEST_CHUNKS; index++)
    {
        test_chunk(chunk, index);
        crc = boot_crc32_update(crc, chunk, CHUNK_BYTES);
    }
    return ~crc;
}


static void send_command(SplUpdate *u, uint8_t command, uint8_t region, uint32_t key,
                         uint32_t timeout_ms)
{
    uint8_t p[SML_BOOT_COMMAND_BYTES];

    begin(u, timeout_ms);
    memset(p, 0, sizeof(p));
    sml_put_u16(&p[0], u->token);
    p[2] = command;
    p[3] = region;
    sml_put_u32(&p[4], key);
    if (command == SML_BOOT_CMD_COMMIT && region == SML_BOOT_REGION_SPL)
    {
        sml_put_u32(&p[8], u->image.version);
        sml_put_u32(&p[12], SPL_START);
        sml_put_u32(&p[16], u->image.length);
        sml_put_u32(&p[20], u->image.crc32);
        sml_put_u32(&p[24], u->image.build_time);
        sml_put_u16(&p[28], u->image.protocol);
    }
    else if (command == SML_BOOT_CMD_COMMIT) // TEST: an application-format record
    {
        sml_put_u32(&p[12], TEST_START);
        sml_put_u32(&p[16], TEST_CHUNKS * 64U);
        sml_put_u32(&p[20], test_crc());
    }
    u->port.send(u->port.context, SML_CLASS_BOOT, SML_BOOT_COMMAND, p, sizeof(p));
}


static bool is_test(SplUpdateStep step)
{
    return step == SPL_UPDATE_TEST_DATA || step == SPL_UPDATE_TEST_VERIFY;
}


static uint32_t chunk_address(const SplUpdate *u, uint32_t chunk)
{
    return (is_test(u->step) ? TEST_START : SPL_START) + chunk * CHUNK_ADDRESSES;
}


// The chunk in buffer: the image's, padded with erased bytes, or the test
// pattern. False if the image's is all erased (skipped).
static bool load_chunk(SplUpdate *u, uint32_t chunk, bool *readable)
{
    uint32_t offset = chunk * CHUNK_BYTES;
    uint32_t bytes = 3U * u->image.length - offset;
    uint32_t index;

    *readable = true;
    if (is_test(u->step))
    {
        test_chunk(u->buffer, chunk);
        return true;
    }
    if (bytes > CHUNK_BYTES)
    {
        bytes = CHUNK_BYTES;
    }
    memset(u->buffer, 0xFF, sizeof(u->buffer));
    *readable = u->port.read(u->port.context, offset, u->buffer, bytes);
    for (index = 0; index < CHUNK_BYTES; index++)
    {
        if (u->buffer[index] != 0xFFU)
        {
            return true;
        }
    }
    return false;
}


static bool next_chunk(SplUpdate *u)
{
    bool readable = true;
    uint32_t chunks = is_test(u->step) ? TEST_CHUNKS : u->chunks;

    for (; u->chunk < chunks; u->chunk++)
    {
        if (load_chunk(u, u->chunk, &readable))
        {
            return true;
        }
        if (!readable)
        {
            fail(u, "the staged SPL cannot be read");
            return false;
        }
    }
    return false;
}


static void send_data(SplUpdate *u)
{
    uint8_t p[SML_BOOT_DATA_BYTES];

    begin(u, is_test(u->step) ? SPL_CHUNK_MS : APP_CHUNK_MS);
    memset(p, 0, sizeof(p));
    sml_put_u32(&p[0], chunk_address(u, u->chunk));
    p[4] = is_test(u->step) ? SML_BOOT_REGION_TEST : SML_BOOT_REGION_SPL;
    sml_put_u32(&p[8], boot_crc32(u->buffer, CHUNK_BYTES));
    memcpy(&p[12], u->buffer, CHUNK_BYTES);
    u->port.send(u->port.context, SML_CLASS_BOOT, SML_BOOT_DATA, p, sizeof(p));
}


static void send_verify_at(SplUpdate *u, uint32_t address)
{
    uint8_t p[SML_BOOT_VERIFY_BYTES];

    begin(u, in_spl_phase(u->step) ? SPL_CHUNK_MS : APP_VERIFY_MS);
    memset(p, 0, sizeof(p));
    sml_put_u32(&p[0], address);
    u->port.send(u->port.context, SML_CLASS_BOOT, SML_BOOT_VERIFY, p, sizeof(p));
}


static void send_verify(SplUpdate *u)
{
    send_verify_at(u, chunk_address(u, u->chunk));
}

// ---- Steps ----------------------------------------------------------------------------

static void go(SplUpdate *u, SplUpdateStep step);


// The request a step sends, again (a timeout, or BUSY).
static void resend(SplUpdate *u)
{
    switch (u->step)
    {
    case SPL_UPDATE_STATUS:
    case SPL_UPDATE_HEALTHY:
    case SPL_UPDATE_CHECK:
    case SPL_UPDATE_WAIT_CONFIRM:
        u->have_status = false;
        send_command(u, SML_BOOT_CMD_STATUS, 0, 0, APP_COMMAND_MS);
        break;
    case SPL_UPDATE_KEEP_RECORD: send_verify_at(u, SPL_RECORD_ADDRESS); break;
    case SPL_UPDATE_KEEP: send_verify_at(u, SPL_START + u->chunk * CHUNK_ADDRESSES); break;
    case SPL_UPDATE_ERASE:
        send_command(u, SML_BOOT_CMD_ERASE, SML_BOOT_REGION_SPL, SML_BOOT_KEY_ERASE,
                     APP_ACCEPTED_MS);
        break;
    case SPL_UPDATE_COMMIT:
        send_command(u, SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_SPL, SML_BOOT_KEY_COMMIT,
                     APP_ACCEPTED_MS);
        break;
    case SPL_UPDATE_DATA:
    case SPL_UPDATE_TEST_DATA: send_data(u); break;
    case SPL_UPDATE_VERIFY:
    case SPL_UPDATE_TEST_VERIFY: send_verify(u); break;
    case SPL_UPDATE_ENTER: send_user(u, SML_USER_ENTER_BOOTLOADER, 0, ENTER_MS); break;
    case SPL_UPDATE_HOLD: send_command(u, SML_BOOT_CMD_HOLD, 0, 0, SPL_COMMAND_MS); break;
    case SPL_UPDATE_TEST_ERASE:
    case SPL_UPDATE_TEST_CLEAN:
        send_command(u, SML_BOOT_CMD_ERASE, SML_BOOT_REGION_TEST, SML_BOOT_KEY_ERASE,
                     SPL_ACCEPTED_MS);
        break;
    case SPL_UPDATE_TEST_COMMIT:
        send_command(u, SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_TEST, SML_BOOT_KEY_COMMIT,
                     SPL_ACCEPTED_MS);
        break;
    case SPL_UPDATE_RUN: send_command(u, SML_BOOT_CMD_RUN, 0, 0, SPL_COMMAND_MS); break;
    case SPL_UPDATE_WAIT_APP: ask_sys(u, ASK_SYS_MS / 2U); break;
    case SPL_UPDATE_CONFIRM:
        send_command(u, SML_BOOT_CMD_CONFIRM, SML_BOOT_REGION_SPL, SML_BOOT_KEY_COMMIT,
                     APP_ACCEPTED_MS);
        break;
    default: break;
    }
}


static void go(SplUpdate *u, SplUpdateStep step)
{
    u->step = step;
    u->tries = 0;
    u->step_ms = u->now;
    u->sent = false;
    switch (step)
    {
    case SPL_UPDATE_DATA:
    case SPL_UPDATE_VERIFY:
    case SPL_UPDATE_TEST_DATA:
    case SPL_UPDATE_TEST_VERIFY:
    {
        SplUpdateStep after = step == SPL_UPDATE_DATA        ? SPL_UPDATE_VERIFY
                              : step == SPL_UPDATE_VERIFY    ? SPL_UPDATE_COMMIT
                              : step == SPL_UPDATE_TEST_DATA ? SPL_UPDATE_TEST_VERIFY
                                                             : SPL_UPDATE_TEST_COMMIT;

        u->chunk = 0;
        if (step == SPL_UPDATE_DATA)
        {
            u->written = 0;
        }
        if (next_chunk(u))
        {
            resend(u);
        }
        else if (u->step != SPL_UPDATE_FAILED)
        {
            go(u, after);
        }
        break;
    }
    case SPL_UPDATE_KEEP: u->chunk = 0; resend(u); break;
    case SPL_UPDATE_WAIT_SPL:
        // Nothing to send: the new SPL speaks first. If it never does, the
        // application will (STAT-SYS), once asked.
        u->sent = true;
        u->sent_ms = u->now;
        u->timeout_ms = 3000U;
        break;
    default: resend(u); break;
    }
}


static void new_pass(SplUpdate *u, const char *why)
{
    char text[96];

    snprintf(u->reason, sizeof(u->reason), "%s", why);
    u->passes++;
    if (u->passes >= SPL_UPDATE_PASSES)
    {
        snprintf(text, sizeof(text), "gave up after %u passes: %s", (unsigned)u->passes, why);
        fail(u, text);
        return;
    }
    go(u, SPL_UPDATE_STATUS);
}


// A failure once the new SPL is running: back to the application, then
// REJECTED (the caller puts the old one back).
static void test_failed(SplUpdate *u, const char *why)
{
    snprintf(u->reason, sizeof(u->reason), "the new SPL failed its test: %s", why);
    u->test_failed = true; // RUN, then REJECTED
    go(u, SPL_UPDATE_RUN);
}


static void advance(SplUpdate *u)
{
    SplUpdateStep after = u->step == SPL_UPDATE_DATA        ? SPL_UPDATE_VERIFY
                          : u->step == SPL_UPDATE_VERIFY    ? SPL_UPDATE_COMMIT
                          : u->step == SPL_UPDATE_TEST_DATA ? SPL_UPDATE_TEST_VERIFY
                                                            : SPL_UPDATE_TEST_COMMIT;

    u->chunk++;
    u->tries = 0;
    if (next_chunk(u))
    {
        resend(u);
    }
    else if (u->step != SPL_UPDATE_FAILED)
    {
        go(u, after);
    }
}


static uint32_t status_u32(const SplUpdate *u, unsigned offset)
{
    return sml_get_u32(&u->status[offset]);
}


static bool is_running_spl(const SplUpdate *u)
{
    uint8_t state = u->status[48];
    uint8_t wanted = SML_SPL_STATE_COMMITTED | SML_SPL_STATE_INTACT | SML_SPL_STATE_CONFIRMED;

    return (state & (wanted | SML_SPL_STATE_REJECTED)) == wanted &&
           status_u32(u, 0) == u->image.version && status_u32(u, 52) == u->image.crc32;
}


// The first STATUS: whether to go on, and how.
static void status_checked(SplUpdate *u)
{
    uint16_t flags = sml_get_u16(&u->status[6]);
    uint8_t boot = u->status[45];
    uint16_t ipl = sml_get_u16(&u->status[46]);
    uint16_t healthy = sml_get_u16(&u->status[50]);
    uint8_t state = u->status[48];

    if (u->status[5] != SML_BOOT_MODE_APP)
    {
        fail(u, "the PIC is in its SPL: the application installs an SPL");
        return;
    }
    if (u->status[4] != SML_BOOT_PROTOCOL)
    {
        fail(u, "the application speaks another bootloader protocol");
        return;
    }
    if (status_u32(u, 12) != u->image.device_id)
    {
        fail(u, "the PIC is not the device the SPL is for (DEVID)");
        return;
    }
    if (((uint32_t)ipl << 8) < (u->image.min_ipl & 0xFFFF00UL))
    {
        fail(u, "the PIC's IPL is older than the SPL needs");
        return;
    }
    if ((flags & SML_BOOT_FLAG_TEST_FREE) == 0U)
    {
        fail(u, "the application reaches the test page: the SPL cannot be tested");
        return;
    }
    if (is_running_spl(u) && (boot & SML_BOOT_PATH_APP_DIRECT) == 0U && !u->force)
    {
        u->already = true;
        u->step = SPL_UPDATE_DONE;
        return;
    }
    // Newer only, unless forced: a newer SPL, confirmed and trusted, stays.
    if (!u->force && (boot & SML_BOOT_PATH_APP_DIRECT) == 0U &&
        (state & (SML_SPL_STATE_COMMITTED | SML_SPL_STATE_INTACT | SML_SPL_STATE_CONFIRMED |
                  SML_SPL_STATE_REJECTED)) ==
            (SML_SPL_STATE_COMMITTED | SML_SPL_STATE_INTACT | SML_SPL_STATE_CONFIRMED) &&
        status_u32(u, 0) > u->image.version)
    {
        u->already = true;
        u->newer = true;
        u->step = SPL_UPDATE_DONE;
        return;
    }
    if (healthy < APP_HEALTHY_S)
    {
        go(u, SPL_UPDATE_HEALTHY);
        return;
    }
    // Keep the running SPL, if it is worth keeping and nothing holds it.
    if (u->port.stored != NULL && !u->kept &&
        (state & (SML_SPL_STATE_INTACT | SML_SPL_STATE_REJECTED)) == SML_SPL_STATE_INTACT &&
        !(status_u32(u, 0) == u->image.version && status_u32(u, 52) == u->image.crc32) &&
        !u->port.stored(u->port.context, status_u32(u, 0), status_u32(u, 52)))
    {
        go(u, SPL_UPDATE_KEEP_RECORD);
        return;
    }
    go(u, SPL_UPDATE_ERASE);
}


static void status_answered(SplUpdate *u)
{
    uint16_t healthy = sml_get_u16(&u->status[50]);
    uint8_t state = u->status[48];

    switch (u->step)
    {
    case SPL_UPDATE_STATUS: status_checked(u); break;
    case SPL_UPDATE_HEALTHY:
        if (healthy >= APP_HEALTHY_S)
        {
            go(u, SPL_UPDATE_STATUS);
        }
        else if (u->now - u->step_ms >= HEALTHY_LIMIT_MS)
        {
            fail(u, "the application did not run 60 s without a restart");
        }
        else
        {
            u->sent = false; // asked again in HEALTHY_POLL_MS
            u->sent_ms = u->now;
            u->timeout_ms = HEALTHY_POLL_MS;
        }
        break;
    case SPL_UPDATE_CHECK:
        if ((state & (SML_SPL_STATE_COMMITTED | SML_SPL_STATE_INTACT)) ==
                (SML_SPL_STATE_COMMITTED | SML_SPL_STATE_INTACT) &&
            status_u32(u, 0) == u->image.version && status_u32(u, 52) == u->image.crc32)
        {
            go(u, SPL_UPDATE_ENTER);
        }
        else
        {
            new_pass(u, "CHECK: the new SPL is not committed and intact");
        }
        break;
    case SPL_UPDATE_WAIT_CONFIRM:
        if (u->status[5] != SML_BOOT_MODE_APP)
        {
            break; // the SPL's own: not the application's
        }
        if ((u->status[45] & SML_BOOT_PATH_UNCONFIRMED) == 0U)
        {
            reject(u, "the application was not started by the new SPL");
        }
        else if (healthy >= u->confirm_s)
        {
            go(u, SPL_UPDATE_CONFIRM);
        }
        else if (u->now - u->step_ms >= HEALTHY_LIMIT_MS)
        {
            fail(u, "the application did not keep running: the new SPL is left unconfirmed");
        }
        else
        {
            u->sent = false;
            u->sent_ms = u->now;
            u->timeout_ms = CONFIRM_POLL_MS;
        }
        break;
    default: break;
    }
}


static void command_answered(SplUpdate *u, uint16_t status, uint16_t detail, uint32_t value)
{
    char text[64];

    (void)value;
    if (status == SML_RESULT_ACCEPTED)
    {
        u->accepted = true;
        u->sent_ms = u->now;
        u->timeout_ms = u->step == SPL_UPDATE_ERASE    ? APP_ERASE_MS
                        : u->step == SPL_UPDATE_COMMIT ? APP_COMMIT_MS
                        : u->step == SPL_UPDATE_CONFIRM ? APP_CONFIRM_MS
                                                        : SPL_WORK_MS;
        return;
    }
    if (status != SML_RESULT_ACK && detail == SML_ERROR_BUSY && u->tries < BUSY_TRIES &&
        !in_spl_phase(u->step))
    {
        u->tries++;
        u->sent = false; // a settings save first: asked again shortly
        u->sent_ms = u->now;
        u->timeout_ms = BUSY_WAIT_MS;
        return;
    }
    switch (u->step)
    {
    case SPL_UPDATE_STATUS:
    case SPL_UPDATE_HEALTHY:
    case SPL_UPDATE_CHECK:
    case SPL_UPDATE_WAIT_CONFIRM:
        if (status != SML_RESULT_ACK)
        {
            fail_nack(u, "STATUS", detail);
        }
        else if (u->have_status)
        {
            status_answered(u);
        }
        break;
    case SPL_UPDATE_ERASE:
        if (status == SML_RESULT_ACK)
        {
            go(u, SPL_UPDATE_DATA);
        }
        else if (detail == SML_ERROR_STATE)
        {
            go(u, SPL_UPDATE_HEALTHY); // restarted since: not 60 s yet
        }
        else if (detail == SML_ERROR_STORAGE)
        {
            new_pass(u, "ERASE: NACK STORAGE");
        }
        else
        {
            fail_nack(u, "ERASE", detail);
        }
        break;
    case SPL_UPDATE_COMMIT:
        if (status == SML_RESULT_ACK)
        {
            go(u, SPL_UPDATE_CHECK);
        }
        else if (detail == SML_ERROR_VERIFY || detail == SML_ERROR_STATE ||
                 detail == SML_ERROR_STORAGE)
        {
            snprintf(text, sizeof(text), "COMMIT: NACK %s", error_name(detail));
            new_pass(u, text);
        }
        else
        {
            fail_nack(u, "COMMIT", detail);
        }
        break;
    case SPL_UPDATE_HOLD:
        status == SML_RESULT_ACK ? go(u, SPL_UPDATE_TEST_ERASE) : test_failed(u, "HOLD refused");
        break;
    case SPL_UPDATE_TEST_ERASE:
    case SPL_UPDATE_TEST_CLEAN:
    case SPL_UPDATE_TEST_COMMIT:
        if (status != SML_RESULT_ACK)
        {
            snprintf(text, sizeof(text), "%s: NACK %s", spl_update_step_name(u->step),
                     error_name(detail));
            test_failed(u, text);
        }
        else
        {
            go(u, u->step == SPL_UPDATE_TEST_ERASE    ? SPL_UPDATE_TEST_DATA
                  : u->step == SPL_UPDATE_TEST_COMMIT ? SPL_UPDATE_TEST_CLEAN
                                                      : SPL_UPDATE_RUN);
        }
        break;
    case SPL_UPDATE_RUN:
        if (status == SML_RESULT_ACK)
        {
            go(u, SPL_UPDATE_WAIT_APP);
        }
        else
        {
            reject(u, "RUN refused by the new SPL");
        }
        break;
    case SPL_UPDATE_CONFIRM:
        if (status == SML_RESULT_ACK)
        {
            u->step = SPL_UPDATE_DONE;
        }
        else if (status == SML_RESULT_NACK && detail == SML_ERROR_STATE &&
                 u->confirm_s < APP_HEALTHY_S)
        {
            // An older application, which confirms after APP_HEALTHY_S.
            u->confirm_s = APP_HEALTHY_S;
            go(u, SPL_UPDATE_WAIT_CONFIRM);
        }
        else
        {
            fail_nack(u, "CONFIRM", detail);
        }
        break;
    default: break;
    }
}


static void data_answered(SplUpdate *u, const uint8_t *p)
{
    uint16_t status = sml_get_u16(&p[0]);
    uint16_t detail = sml_get_u16(&p[2]);
    char text[64];

    if (sml_get_u32(&p[4]) != chunk_address(u, u->chunk))
    {
        return;
    }
    if (status == SML_RESULT_ACK)
    {
        if (sml_get_u32(&p[8]) != boot_crc32(u->buffer, CHUNK_BYTES))
        {
            u->step == SPL_UPDATE_TEST_DATA ? test_failed(u, "DATA: the flash's CRC-32 differs")
                                            : new_pass(u, "DATA: the flash's CRC-32 differs");
            return;
        }
        u->written++;
        advance(u);
        return;
    }
    snprintf(text, sizeof(text), "DATA 0x%06lX: NACK %s", (unsigned long)chunk_address(u, u->chunk),
             error_name(detail));
    if (detail == SML_ERROR_CRC && ++u->tries < CHUNK_TRIES)
    {
        send_data(u);
    }
    else if (detail == SML_ERROR_BUSY && ++u->tries < CHUNK_TRIES)
    {
        u->sent = false;
        u->sent_ms = u->now;
        u->timeout_ms = BUSY_WAIT_MS;
    }
    else if (u->step == SPL_UPDATE_TEST_DATA)
    {
        test_failed(u, text);
    }
    else if (detail == SML_ERROR_ARGUMENT)
    {
        fail(u, text);
    }
    else
    {
        new_pass(u, text);
    }
}


// The running SPL's record row, read back: its length, build time and
// protocol, for the kept file's header; then its code.
static void keep_record(SplUpdate *u, const uint8_t *data)
{
    uint16_t words[SPL_RECORD_WORDS];
    PicFileHeader header;
    uint8_t index;

    for (index = 0; index < SPL_RECORD_WORDS; index++)
    {
        words[index] = (uint16_t)(data[3U * index] | data[3U * index + 1U] << 8);
    }
    memset(&header, 0, sizeof(header));
    header.kind = PIC_FILE_SPL;
    header.device_id = u->image.device_id;
    header.start = SPL_START;
    header.version = (uint32_t)words[SPL_RECORD_VERSION] << 16 | words[SPL_RECORD_VERSION + 1U];
    header.length = (uint32_t)words[SPL_RECORD_LENGTH] << 16 | words[SPL_RECORD_LENGTH + 1U];
    header.crc32 = (uint32_t)words[SPL_RECORD_CRC32] << 16 | words[SPL_RECORD_CRC32 + 1U];
    header.build_time =
        (uint32_t)words[SPL_RECORD_BUILD_TIME] << 16 | words[SPL_RECORD_BUILD_TIME + 1U];
    header.protocol = words[SPL_RECORD_PROTOCOL];
    if (header.version != status_u32(u, 0) || header.crc32 != status_u32(u, 52) ||
        header.length == 0U || header.length > SPL_CODE_MAX ||
        !u->port.keep_begin(u->port.context, &header))
    {
        go(u, SPL_UPDATE_ERASE); // not kept: carry on without
        return;
    }
    u->keep_length = header.length;
    go(u, SPL_UPDATE_KEEP);
}


static void verify_answered(SplUpdate *u, const uint8_t *p)
{
    char text[64];
    uint32_t expected = u->step == SPL_UPDATE_KEEP_RECORD ? SPL_RECORD_ADDRESS
                        : u->step == SPL_UPDATE_KEEP      ? SPL_START + u->chunk * CHUNK_ADDRESSES
                                                          : chunk_address(u, u->chunk);

    if (sml_get_u32(&p[4]) != expected)
    {
        return;
    }
    if (sml_get_u16(&p[0]) != SML_RESULT_ACK || boot_crc32(&p[12], CHUNK_BYTES) != sml_get_u32(&p[8]))
    {
        if (++u->tries < CHUNK_TRIES)
        {
            resend(u); // damaged on the way back: ask again
        }
        else if (u->step == SPL_UPDATE_KEEP || u->step == SPL_UPDATE_KEEP_RECORD)
        {
            go(u, SPL_UPDATE_ERASE); // not kept
        }
        else if (u->step == SPL_UPDATE_TEST_VERIFY)
        {
            test_failed(u, "VERIFY: answers damaged");
        }
        else
        {
            new_pass(u, "VERIFY: answers damaged");
        }
        return;
    }
    if (u->step == SPL_UPDATE_KEEP_RECORD)
    {
        keep_record(u, &p[12]);
        return;
    }
    if (u->step == SPL_UPDATE_KEEP)
    {
        uint32_t left = 3U * u->keep_length - u->chunk * CHUNK_BYTES;
        uint32_t bytes = left < CHUNK_BYTES ? left : CHUNK_BYTES;

        if (!u->port.keep_write(u->port.context, &p[12], bytes))
        {
            go(u, SPL_UPDATE_ERASE);
            return;
        }
        u->chunk++;
        u->tries = 0;
        if (u->chunk * CHUNK_BYTES < 3U * u->keep_length)
        {
            resend(u);
            return;
        }
        u->kept = u->port.keep_end(u->port.context);
        go(u, SPL_UPDATE_ERASE);
        return;
    }
    if (memcmp(&p[12], u->buffer, CHUNK_BYTES) != 0)
    {
        snprintf(text, sizeof(text), "VERIFY 0x%06lX: the flash differs",
                 (unsigned long)chunk_address(u, u->chunk));
        u->step == SPL_UPDATE_TEST_VERIFY ? test_failed(u, text) : new_pass(u, text);
        return;
    }
    advance(u);
}


void spl_update_start(SplUpdate *update, const SplUpdatePort *port, const PicFileHeader *image,
                      bool force, uint32_t now_ms)
{
    memset(update, 0, sizeof(*update));
    update->port = *port;
    update->image = *image;
    update->force = force;
    update->now = now_ms;
    update->next_token = 0x7800;
    update->chunks = (3U * image->length + CHUNK_BYTES - 1U) / CHUNK_BYTES;
    update->confirm_s = APP_CONFIRM_S;
    go(update, SPL_UPDATE_STATUS);
}


// A BOOT-STATUS while waiting for the new SPL: up, held, and the new one?
static void spl_came_up(SplUpdate *u, const uint8_t *payload)
{
    uint8_t mode = payload[5];

    if (mode == SML_BOOT_MODE_APP)
    {
        return;
    }
    if (sml_get_u32(&payload[0]) != u->image.version || sml_get_u32(&payload[52]) != u->image.crc32 ||
        (payload[45] & SML_BOOT_PATH_UNCONFIRMED) == 0U)
    {
        // Another SPL started: the new one did not.
        reject(u, "another SPL started, not the new one");
        return;
    }
    go(u, mode == SML_BOOT_MODE_WINDOW ? SPL_UPDATE_HOLD : SPL_UPDATE_TEST_ERASE);
}


void spl_update_frame(SplUpdate *u, uint8_t class_id, uint8_t message_id, const uint8_t *payload,
                      uint16_t length, uint32_t now_ms)
{
    u->now = now_ms;
    if (!spl_update_running(u))
    {
        return;
    }
    if (class_id == SML_CLASS_CMD && message_id == SML_CMD_RESPONSE &&
        length == SML_CMD_RESPONSE_BYTES && sml_get_u16(&payload[4]) == u->token &&
        u->step == SPL_UPDATE_ENTER)
    {
        uint16_t status = sml_get_u16(&payload[0]);
        uint16_t detail = sml_get_u16(&payload[2]);

        if (status == SML_RESULT_ACK)
        {
            go(u, SPL_UPDATE_WAIT_SPL);
        }
        else if (detail == SML_ERROR_BUSY && u->tries < BUSY_TRIES)
        {
            u->accepted = true;
            u->sent_ms = now_ms;
            u->timeout_ms = BUSY_WAIT_MS;
        }
        else
        {
            fail_nack(u, "ENTER_BOOTLOADER", detail);
        }
        return;
    }
    if (class_id == SML_CLASS_STAT && message_id == SML_STAT_SYS && length == SML_STAT_SYS_BYTES)
    {
        if (u->step == SPL_UPDATE_WAIT_SPL)
        {
            reject(u, "the new SPL did not start: the IPL started the application");
        }
        else if (u->step == SPL_UPDATE_WAIT_APP)
        {
            if (u->test_failed)
            {
                u->step = SPL_UPDATE_REJECTED; // back in the application, after a failed test
            }
            else
            {
                go(u, SPL_UPDATE_WAIT_CONFIRM);
            }
        }
        return;
    }
    if (class_id != SML_CLASS_BOOT)
    {
        return;
    }
    if (message_id == SML_BOOT_STATUS && length == SML_BOOT_STATUS_BYTES)
    {
        if (u->step == SPL_UPDATE_WAIT_SPL)
        {
            spl_came_up(u, payload);
        }
        else if (u->sent && !u->have_status &&
                 (u->step == SPL_UPDATE_STATUS || u->step == SPL_UPDATE_HEALTHY ||
                  u->step == SPL_UPDATE_CHECK || u->step == SPL_UPDATE_WAIT_CONFIRM))
        {
            memcpy(u->status, payload, sizeof(u->status));
            u->have_status = true;
        }
        else if (payload[5] == SML_BOOT_MODE_WINDOW && in_spl_phase(u->step) &&
                 u->step != SPL_UPDATE_HOLD)
        {
            test_failed(u, "the PIC reset during the test");
        }
        return;
    }
    if (message_id == SML_BOOT_RESPONSE && length == SML_BOOT_RESPONSE_BYTES &&
        sml_get_u16(&payload[4]) == u->token && u->sent)
    {
        command_answered(u, sml_get_u16(&payload[0]), sml_get_u16(&payload[2]),
                         sml_get_u32(&payload[8]));
        return;
    }
    if (message_id == SML_BOOT_DATA_RESPONSE && length == SML_BOOT_DATA_RESPONSE_BYTES &&
        (u->step == SPL_UPDATE_DATA || u->step == SPL_UPDATE_TEST_DATA) && u->sent)
    {
        data_answered(u, payload);
        return;
    }
    if (message_id == SML_BOOT_VERIFY_RESPONSE && length == SML_BOOT_VERIFY_RESPONSE_BYTES &&
        (u->step == SPL_UPDATE_VERIFY || u->step == SPL_UPDATE_TEST_VERIFY ||
         u->step == SPL_UPDATE_KEEP || u->step == SPL_UPDATE_KEEP_RECORD) &&
        u->sent)
    {
        verify_answered(u, payload);
    }
}


void spl_update_service(SplUpdate *u, uint32_t now_ms)
{
    char text[64];

    u->now = now_ms;
    if (!spl_update_running(u) || now_ms - u->sent_ms < u->timeout_ms)
    {
        return;
    }
    if (!u->sent) // a wait before asking (again): HEALTHY, BUSY
    {
        resend(u);
        return;
    }
    switch (u->step)
    {
    case SPL_UPDATE_WAIT_SPL:
        if (now_ms - u->step_ms >= WAIT_SPL_MS)
        {
            fail(u, "neither the new SPL nor the application answered");
        }
        else
        {
            // Not up yet: perhaps it never will be, and the application is
            // back. Asking costs nothing if the SPL is up (it ignores it).
            ask_sys(u, ASK_SYS_MS);
        }
        break;
    case SPL_UPDATE_ENTER:
        if (u->accepted) // BUSY: ask again
        {
            if (++u->tries >= BUSY_TRIES)
            {
                fail(u, "ENTER_BOOTLOADER: BUSY");
            }
            else
            {
                resend(u);
            }
        }
        else if (++u->tries < COMMAND_TRIES)
        {
            resend(u);
        }
        else
        {
            fail(u, "ENTER_BOOTLOADER: no answer from the application");
        }
        break;
    case SPL_UPDATE_ERASE:
    case SPL_UPDATE_COMMIT:
        snprintf(text, sizeof(text), "%s: no answer%s", spl_update_step_name(u->step),
                 u->accepted ? " after ACCEPTED" : "");
        new_pass(u, text);
        break;
    case SPL_UPDATE_WAIT_APP:
        if (now_ms - u->step_ms >= WAIT_APP_MS)
        {
            fail(u, "no STAT-SYS from the application after the test");
        }
        else
        {
            resend(u);
        }
        break;
    default:
        if (++u->tries < (u->step == SPL_UPDATE_DATA || u->step == SPL_UPDATE_VERIFY ||
                                  u->step == SPL_UPDATE_TEST_DATA ||
                                  u->step == SPL_UPDATE_TEST_VERIFY || u->step == SPL_UPDATE_KEEP
                              ? CHUNK_TRIES
                              : COMMAND_TRIES))
        {
            resend(u);
        }
        else if (u->step == SPL_UPDATE_KEEP || u->step == SPL_UPDATE_KEEP_RECORD)
        {
            go(u, SPL_UPDATE_ERASE);
        }
        else if (in_spl_phase(u->step) && u->step != SPL_UPDATE_RUN)
        {
            snprintf(text, sizeof(text), "%s: no answer", spl_update_step_name(u->step));
            test_failed(u, text);
        }
        else if (u->step == SPL_UPDATE_RUN)
        {
            reject(u, "RUN: no answer from the new SPL");
        }
        else if (u->step == SPL_UPDATE_DATA || u->step == SPL_UPDATE_VERIFY ||
                 u->step == SPL_UPDATE_CHECK)
        {
            snprintf(text, sizeof(text), "%s: no answer", spl_update_step_name(u->step));
            new_pass(u, text);
        }
        else
        {
            snprintf(text, sizeof(text), "%s: no answer", spl_update_step_name(u->step));
            fail(u, text);
        }
        break;
    }
}
