#include "pic_update.h"

#include <stdio.h>
#include <string.h>

#include "sml/boot_crc.h"
#include "sml/boot_layout.h"
#include "sml/sml.h"

#define CHUNK_BYTES     192U
#define CHUNK_ADDRESSES 0x80U

// Timeouts (ms) and tries for each request, with the ESP on the UART
// itself: a chunk takes the PIC about 7 ms.
#define ENTER_MS        2000U
#define ENTER_TRIES     3U
#define BUSY_WAIT_MS    1000U
#define BUSY_TRIES      5U
#define WAIT_SPL_MS     4000U
#define COMMAND_MS      500U
#define COMMAND_TRIES   3U
#define ACCEPTED_MS     1000U
#define ERASE_MS        10000U
#define COMMIT_MS       3000U
#define CHUNK_MS        500U
#define CHUNK_TRIES     5U
#define WAIT_APP_MS     10000U
#define ASK_SYS_MS      500U

static const char *const step_names[] = {
    "ENTER", "WAIT_SPL", "STATUS", "HOLD", "ERASE",  "DATA",
    "VERIFY", "COMMIT", "RUN",    "WAIT_APP", "DONE", "FAILED",
};

const char *pic_update_step_name(PicUpdateStep step)
{
    return (unsigned)step < sizeof(step_names) / sizeof(step_names[0]) ? step_names[step] : "?";
}


bool pic_update_running(const PicUpdate *update)
{
    return update->step != PIC_UPDATE_DONE && update->step != PIC_UPDATE_FAILED;
}


static const char *error_name(uint16_t code)
{
    static const char *const names[] = {"NONE",   "UNSUPPORTED", "LENGTH", "ARGUMENT",
                                        "BUSY",   "STALE",       "SCHEMA", "STORAGE",
                                        "VERIFY", "STATE",       "CRC",    "NOT_BLANK"};

    return code < sizeof(names) / sizeof(names[0]) ? names[code] : "?";
}


static void fail(PicUpdate *u, const char *reason)
{
    snprintf(u->reason, sizeof(u->reason), "%s", reason);
    u->step = PIC_UPDATE_FAILED;
}


static void fail_nack(PicUpdate *u, const char *what, uint16_t detail)
{
    char text[64];

    snprintf(text, sizeof(text), "%s: NACK %s", what, error_name(detail));
    fail(u, text);
}

// ---- Requests ---------------------------------------------------------------------

static void begin(PicUpdate *u, uint32_t timeout_ms)
{
    u->token = u->next_token++;
    if (u->next_token == 0)
    {
        u->next_token = 0x7000;
    }
    u->accepted = false;
    u->sent_ms = u->now;
    u->timeout_ms = timeout_ms;
}


static void send_user(PicUpdate *u, uint16_t command, uint32_t argument, uint32_t timeout_ms)
{
    uint8_t p[SML_CMD_USER_BYTES];

    begin(u, timeout_ms);
    memset(p, 0, sizeof(p));
    sml_put_u16(&p[0], command);
    sml_put_u16(&p[4], u->token);
    sml_put_u32(&p[6], argument);
    u->port.send(u->port.context, SML_CLASS_CMD, SML_CMD_USER, p, sizeof(p));
}


static void send_command(PicUpdate *u, uint8_t command, uint8_t region, uint32_t key,
                         uint32_t timeout_ms)
{
    uint8_t p[SML_BOOT_COMMAND_BYTES];

    begin(u, timeout_ms);
    memset(p, 0, sizeof(p));
    sml_put_u16(&p[0], u->token);
    p[2] = command;
    p[3] = region;
    sml_put_u32(&p[4], key);
    if (command == SML_BOOT_CMD_COMMIT)
    {
        sml_put_u32(&p[8], u->image.version);
        sml_put_u32(&p[12], u->image.start);
        sml_put_u32(&p[16], u->image.length);
        sml_put_u32(&p[20], u->image.crc32);
        sml_put_u32(&p[24], u->image.build_time);
        sml_put_u16(&p[28], u->image.sml_version);
    }
    u->port.send(u->port.context, SML_CLASS_BOOT, SML_BOOT_COMMAND, p, sizeof(p));
}


// The chunk into buffer, padded with erased bytes. False if it is all erased
// (it is skipped) or cannot be read.
static bool load_chunk(PicUpdate *u, uint32_t chunk, bool *readable)
{
    uint32_t offset = chunk * CHUNK_BYTES;
    uint32_t bytes = 3U * u->image.length - offset;
    uint32_t index;

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


static uint32_t chunk_address(const PicUpdate *u, uint32_t chunk)
{
    return u->image.start + chunk * CHUNK_ADDRESSES;
}


// The next chunk from u->chunk that holds something, in buffer. False when
// none is left (or the image cannot be read: failed).
static bool next_chunk(PicUpdate *u)
{
    bool readable = true;

    for (; u->chunk < u->chunks; u->chunk++)
    {
        if (load_chunk(u, u->chunk, &readable))
        {
            return true;
        }
        if (!readable)
        {
            fail(u, "the staged image cannot be read");
            return false;
        }
    }
    return false;
}


static void send_data(PicUpdate *u)
{
    uint8_t p[SML_BOOT_DATA_BYTES];

    begin(u, CHUNK_MS);
    memset(p, 0, sizeof(p));
    sml_put_u32(&p[0], chunk_address(u, u->chunk));
    p[4] = SML_BOOT_REGION_APP;
    sml_put_u32(&p[8], boot_crc32(u->buffer, CHUNK_BYTES));
    memcpy(&p[12], u->buffer, CHUNK_BYTES);
    u->port.send(u->port.context, SML_CLASS_BOOT, SML_BOOT_DATA, p, sizeof(p));
}


static void send_verify(PicUpdate *u)
{
    uint8_t p[SML_BOOT_VERIFY_BYTES];

    begin(u, CHUNK_MS);
    memset(p, 0, sizeof(p));
    sml_put_u32(&p[0], chunk_address(u, u->chunk));
    u->port.send(u->port.context, SML_CLASS_BOOT, SML_BOOT_VERIFY, p, sizeof(p));
}

// ---- Steps ----------------------------------------------------------------------------

// Enters a step and sends its first request.
static void go(PicUpdate *u, PicUpdateStep step)
{
    u->step = step;
    u->tries = 0;
    u->step_ms = u->now;
    switch (step)
    {
    case PIC_UPDATE_ENTER:
        send_user(u, SML_USER_ENTER_BOOTLOADER, 0, ENTER_MS);
        break;
    case PIC_UPDATE_WAIT_SPL:
        begin(u, WAIT_SPL_MS);
        break;
    case PIC_UPDATE_STATUS:
        u->have_status = false;
        send_command(u, SML_BOOT_CMD_STATUS, 0, 0, COMMAND_MS);
        break;
    case PIC_UPDATE_HOLD:
        send_command(u, SML_BOOT_CMD_HOLD, 0, 0, COMMAND_MS);
        break;
    case PIC_UPDATE_ERASE:
        u->written = 0;
        send_command(u, SML_BOOT_CMD_ERASE, SML_BOOT_REGION_APP, SML_BOOT_KEY_ERASE, ACCEPTED_MS);
        break;
    case PIC_UPDATE_DATA:
    case PIC_UPDATE_VERIFY:
        u->chunk = 0;
        if (next_chunk(u))
        {
            step == PIC_UPDATE_DATA ? send_data(u) : send_verify(u);
        }
        else if (u->step != PIC_UPDATE_FAILED)
        {
            go(u, step == PIC_UPDATE_DATA ? PIC_UPDATE_VERIFY : PIC_UPDATE_COMMIT);
        }
        break;
    case PIC_UPDATE_COMMIT:
        send_command(u, SML_BOOT_CMD_COMMIT, SML_BOOT_REGION_APP, SML_BOOT_KEY_COMMIT,
                     ACCEPTED_MS);
        break;
    case PIC_UPDATE_RUN:
        send_command(u, SML_BOOT_CMD_RUN, 0, 0, COMMAND_MS);
        break;
    case PIC_UPDATE_WAIT_APP:
        send_user(u, SML_USER_REQUEST_MESSAGE, (uint32_t)SML_CLASS_STAT << 8 | SML_STAT_SYS,
                  ASK_SYS_MS);
        break;
    default:
        break;
    }
}


// Something went wrong that another pass may put right: from STATUS again.
static void new_pass(PicUpdate *u, const char *why)
{
    char text[96];

    snprintf(u->reason, sizeof(u->reason), "%s", why);
    u->passes++;
    if (u->passes >= PIC_UPDATE_PASSES)
    {
        snprintf(text, sizeof(text), "gave up after %u passes: %s", (unsigned)u->passes, why);
        fail(u, text);
        return;
    }
    go(u, PIC_UPDATE_STATUS);
}


// The next chunk to write or read back, or the next step.
static void advance(PicUpdate *u)
{
    u->chunk++;
    u->tries = 0;
    if (next_chunk(u))
    {
        u->step == PIC_UPDATE_DATA ? send_data(u) : send_verify(u);
    }
    else if (u->step != PIC_UPDATE_FAILED)
    {
        go(u, u->step == PIC_UPDATE_DATA ? PIC_UPDATE_VERIFY : PIC_UPDATE_COMMIT);
    }
}


static uint32_t status_u32(const PicUpdate *u, unsigned offset)
{
    return sml_get_u32(&u->status[offset]);
}


// STATUS answered: what the SPL reports, against the image.
static void status_checked(PicUpdate *u)
{
    uint8_t mode = u->status[5];
    uint16_t flags = sml_get_u16(&u->status[6]);
    uint8_t spl_state = u->status[48];

    if (mode == SML_BOOT_MODE_WINDOW)
    {
        go(u, PIC_UPDATE_HOLD);
        return;
    }
    if (mode == SML_BOOT_MODE_APP)
    {
        fail(u, "the application answered STATUS: not in the SPL");
        return;
    }
    if (u->status[4] != SML_BOOT_PROTOCOL)
    {
        fail(u, "the SPL speaks another bootloader protocol");
        return;
    }
    if (status_u32(u, 12) != u->image.device_id)
    {
        fail(u, "the PIC is not the device the image is for (DEVID)");
        return;
    }
    if (status_u32(u, 16) != u->image.start ||
        status_u32(u, 20) < u->image.start + 2U * u->image.length)
    {
        fail(u, "the image does not fit the PIC's application region");
        return;
    }
    if ((flags & SML_BOOT_FLAG_IMAGE_OK) != 0U && status_u32(u, 24) == u->image.version &&
        status_u32(u, 32) == u->image.crc32 && !u->force)
    {
        u->already = true;
        go(u, PIC_UPDATE_RUN);
        return;
    }
    if ((spl_state & SML_SPL_STATE_CONFIRMED) == 0U && !u->force)
    {
        fail(u, "the SPL is not confirmed: it would not erase the application");
        return;
    }
    go(u, PIC_UPDATE_ERASE);
}


// A BOOT-RESPONSE to the request in flight: (status, detail, value).
static void command_answered(PicUpdate *u, uint16_t status, uint16_t detail)
{
    if (status == SML_RESULT_ACCEPTED)
    {
        u->accepted = true;
        u->sent_ms = u->now;
        u->timeout_ms = u->step == PIC_UPDATE_ERASE ? ERASE_MS : COMMIT_MS;
        return;
    }
    switch (u->step)
    {
    case PIC_UPDATE_STATUS:
        if (status != SML_RESULT_ACK)
        {
            fail_nack(u, "STATUS", detail);
        }
        else if (u->have_status)
        {
            status_checked(u);
        }
        break;
    case PIC_UPDATE_HOLD:
        status == SML_RESULT_ACK ? go(u, PIC_UPDATE_STATUS) : fail_nack(u, "HOLD", detail);
        break;
    case PIC_UPDATE_ERASE:
        if (status == SML_RESULT_ACK)
        {
            go(u, PIC_UPDATE_DATA);
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
    case PIC_UPDATE_COMMIT:
        if (status == SML_RESULT_ACK)
        {
            go(u, PIC_UPDATE_RUN);
        }
        else if (detail == SML_ERROR_VERIFY || detail == SML_ERROR_STATE ||
                 detail == SML_ERROR_STORAGE)
        {
            char text[48];

            snprintf(text, sizeof(text), "COMMIT: NACK %s", error_name(detail));
            new_pass(u, text);
        }
        else
        {
            fail_nack(u, "COMMIT", detail);
        }
        break;
    case PIC_UPDATE_RUN:
        status == SML_RESULT_ACK ? go(u, PIC_UPDATE_WAIT_APP) : fail_nack(u, "RUN", detail);
        break;
    default:
        break;
    }
}


static void data_answered(PicUpdate *u, const uint8_t *p)
{
    uint16_t status = sml_get_u16(&p[0]);
    uint16_t detail = sml_get_u16(&p[2]);
    char text[64];

    if (sml_get_u32(&p[4]) != chunk_address(u, u->chunk))
    {
        return; // an answer to an earlier try
    }
    if (status == SML_RESULT_ACK)
    {
        if (sml_get_u32(&p[8]) != boot_crc32(u->buffer, CHUNK_BYTES))
        {
            new_pass(u, "DATA: the flash's CRC-32 differs");
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
    else if (detail == SML_ERROR_ARGUMENT)
    {
        fail(u, text);
    }
    else
    {
        new_pass(u, text);
    }
}


static void verify_answered(PicUpdate *u, const uint8_t *p)
{
    char text[64];

    if (sml_get_u32(&p[4]) != chunk_address(u, u->chunk))
    {
        return;
    }
    if (sml_get_u16(&p[0]) != SML_RESULT_ACK)
    {
        snprintf(text, sizeof(text), "VERIFY: NACK %s", error_name(sml_get_u16(&p[2])));
        fail(u, text);
        return;
    }
    if (boot_crc32(&p[12], CHUNK_BYTES) != sml_get_u32(&p[8]))
    {
        if (++u->tries < CHUNK_TRIES)
        {
            send_verify(u); // damaged on the way back: ask again
        }
        else
        {
            new_pass(u, "VERIFY: answers damaged");
        }
        return;
    }
    if (memcmp(&p[12], u->buffer, CHUNK_BYTES) != 0)
    {
        snprintf(text, sizeof(text), "VERIFY 0x%06lX: the flash differs",
                 (unsigned long)chunk_address(u, u->chunk));
        new_pass(u, text);
        return;
    }
    advance(u);
}


void pic_update_start(PicUpdate *update, const PicUpdatePort *port, const PicFileHeader *image,
                      bool force, bool in_spl, uint32_t now_ms)
{
    memset(update, 0, sizeof(*update));
    update->port = *port;
    update->image = *image;
    update->force = force;
    update->now = now_ms;
    update->next_token = 0x7000;
    update->chunks = (3U * image->length + CHUNK_BYTES - 1U) / CHUNK_BYTES;
    go(update, in_spl ? PIC_UPDATE_STATUS : PIC_UPDATE_ENTER);
}


void pic_update_frame(PicUpdate *u, uint8_t class_id, uint8_t message_id, const uint8_t *payload,
                      uint16_t length, uint32_t now_ms)
{
    u->now = now_ms;
    if (!pic_update_running(u))
    {
        return;
    }
    if (class_id == SML_CLASS_CMD && message_id == SML_CMD_RESPONSE &&
        length == SML_CMD_RESPONSE_BYTES && sml_get_u16(&payload[4]) == u->token)
    {
        uint16_t status = sml_get_u16(&payload[0]);
        uint16_t detail = sml_get_u16(&payload[2]);

        if (u->step == PIC_UPDATE_ENTER)
        {
            if (status == SML_RESULT_ACK)
            {
                go(u, PIC_UPDATE_WAIT_SPL);
            }
            else if (detail == SML_ERROR_BUSY && u->tries < BUSY_TRIES)
            {
                u->accepted = true; // waiting to ask again, not for an answer
                u->sent_ms = now_ms;
                u->timeout_ms = BUSY_WAIT_MS;
            }
            else if (detail == SML_ERROR_STATE)
            {
                fail(u, "ENTER_BOOTLOADER: NACK STATE: the PIC has no usable SPL");
            }
            else
            {
                fail_nack(u, "ENTER_BOOTLOADER", detail);
            }
        }
        return;
    }
    if (class_id == SML_CLASS_STAT && message_id == SML_STAT_SYS && length == SML_STAT_SYS_BYTES &&
        u->step == PIC_UPDATE_WAIT_APP)
    {
        uint32_t version = sml_get_u32(&payload[8]);

        if (version == u->image.version)
        {
            u->step = PIC_UPDATE_DONE;
        }
        else
        {
            char text[64];

            snprintf(text, sizeof(text), "the PIC runs %lu.%lu.%lu, not the new image",
                     (unsigned long)(version >> 16 & 0xFF), (unsigned long)(version >> 8 & 0xFF),
                     (unsigned long)(version & 0xFF));
            fail(u, text);
        }
        return;
    }
    if (class_id != SML_CLASS_BOOT)
    {
        return;
    }
    if (message_id == SML_BOOT_STATUS && length == SML_BOOT_STATUS_BYTES)
    {
        uint8_t mode = payload[5];

        if (u->step == PIC_UPDATE_STATUS)
        {
            memcpy(u->status, payload, sizeof(u->status));
            u->have_status = true;
        }
        else if (u->step == PIC_UPDATE_ENTER || u->step == PIC_UPDATE_WAIT_SPL)
        {
            if (mode != SML_BOOT_MODE_APP)
            {
                go(u, PIC_UPDATE_STATUS); // the SPL is up
            }
        }
        else if (mode == SML_BOOT_MODE_WINDOW && u->step != PIC_UPDATE_HOLD &&
                 u->step != PIC_UPDATE_WAIT_APP)
        {
            // The PIC reset mid-update: stop its window before it hands
            // over. The request in flight times out, and the pass restarts.
            uint16_t token = u->token;
            bool accepted = u->accepted;
            uint32_t sent = u->sent_ms;
            uint32_t timeout = u->timeout_ms;

            send_command(u, SML_BOOT_CMD_HOLD, 0, 0, COMMAND_MS);
            u->token = token;
            u->accepted = accepted;
            u->sent_ms = sent;
            u->timeout_ms = timeout;
        }
        return;
    }
    if (message_id == SML_BOOT_RESPONSE && length == SML_BOOT_RESPONSE_BYTES &&
        sml_get_u16(&payload[4]) == u->token)
    {
        command_answered(u, sml_get_u16(&payload[0]), sml_get_u16(&payload[2]));
        return;
    }
    if (message_id == SML_BOOT_DATA_RESPONSE && length == SML_BOOT_DATA_RESPONSE_BYTES &&
        u->step == PIC_UPDATE_DATA)
    {
        data_answered(u, payload);
        return;
    }
    if (message_id == SML_BOOT_VERIFY_RESPONSE && length == SML_BOOT_VERIFY_RESPONSE_BYTES &&
        u->step == PIC_UPDATE_VERIFY)
    {
        verify_answered(u, payload);
    }
}


void pic_update_service(PicUpdate *u, uint32_t now_ms)
{
    u->now = now_ms;
    if (!pic_update_running(u) || now_ms - u->sent_ms < u->timeout_ms)
    {
        return;
    }
    switch (u->step)
    {
    case PIC_UPDATE_ENTER:
        if (u->accepted) // BUSY: ask again
        {
            u->tries++;
            send_user(u, SML_USER_ENTER_BOOTLOADER, 0, ENTER_MS);
        }
        else if (++u->tries < ENTER_TRIES)
        {
            send_user(u, SML_USER_ENTER_BOOTLOADER, 0, ENTER_MS);
        }
        else
        {
            fail(u, "ENTER_BOOTLOADER: no answer from the application");
        }
        break;
    case PIC_UPDATE_WAIT_SPL:
        fail(u, "no BOOT-STATUS from the SPL after ENTER_BOOTLOADER");
        break;
    case PIC_UPDATE_STATUS:
    case PIC_UPDATE_HOLD:
    case PIC_UPDATE_RUN:
        if (++u->tries < COMMAND_TRIES)
        {
            uint8_t tries = u->tries;

            go(u, u->step);
            u->tries = tries;
        }
        else
        {
            char text[48];

            snprintf(text, sizeof(text), "%s: no answer", pic_update_step_name(u->step));
            fail(u, text);
        }
        break;
    case PIC_UPDATE_ERASE:
    case PIC_UPDATE_COMMIT:
    {
        char text[48];

        snprintf(text, sizeof(text), "%s: no answer%s", pic_update_step_name(u->step),
                 u->accepted ? " after ACCEPTED" : "");
        new_pass(u, text);
        break;
    }
    case PIC_UPDATE_DATA:
    case PIC_UPDATE_VERIFY:
        if (++u->tries < CHUNK_TRIES)
        {
            u->step == PIC_UPDATE_DATA ? send_data(u) : send_verify(u);
        }
        else
        {
            char text[48];

            snprintf(text, sizeof(text), "%s 0x%06lX: no answer", pic_update_step_name(u->step),
                     (unsigned long)chunk_address(u, u->chunk));
            new_pass(u, text);
        }
        break;
    case PIC_UPDATE_WAIT_APP:
        if (now_ms - u->step_ms >= WAIT_APP_MS)
        {
            fail(u, "no STAT-SYS from the new application");
        }
        else
        {
            send_user(u, SML_USER_REQUEST_MESSAGE, (uint32_t)SML_CLASS_STAT << 8 | SML_STAT_SYS,
                      ASK_SYS_MS);
        }
        break;
    default:
        break;
    }
}
