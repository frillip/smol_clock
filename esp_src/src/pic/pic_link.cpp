#include "pic_link.h"

#include <esp_timer.h>
#include <string.h>

#include "../board.h"
#include "../console/console.h"
#include "../timing/pps.h"
#include "pic_report.h"
#include "sml_stream.h"

#define RX_BUFFER_BYTES    4096
#define TX_BUFFER_BYTES    1024
#define FRAME_TIMEOUT_MS   100U
#define SML_TIMEOUT_MS     5000U
#define BACKLOG_MS         300U
#define PAIR_WINDOW_US     900000LL
#define TIME_TIMEOUT_US    10000000LL
#define REQUEST_TIMEOUT_MS 5000U
#define PENDING_MAX        8
#define HELD_MAX           16

typedef struct
{
    uint8_t class_id;
    uint8_t message_id;
    bool valid;
    uint32_t received_ms;
    uint8_t payload[SML_MAX_PAYLOAD_BYTES]; // native (decoded) form
} Held;

typedef struct
{
    uint16_t token;
    uint16_t command; // SML_USER_*, or SML_OPERATION_SETTINGS_WRITE
    uint32_t argument;
    Stream *reply_to;
    bool verbose; // for what is printed with the answer
    uint32_t sent_ms;
} Pending;

// The PIC's settings as last known: from STAT-SETTINGS, with our own writes
// folded in. A change made while a write is unanswered waits in body, and
// goes when the answer brings the new revision.
typedef struct
{
    bool valid;
    uint32_t revision;
    SML_SETTINGS_BODY body;
    bool writing;          // a CMD-SETTINGS is unanswered
    bool waiting;          // body holds changes not yet sent
    Stream *waiting_reply; // ... for the latest of them
} Settings;

static HardwareSerial uart(BOARD_PIC_UART);
static SmlStream stream;
static PicLinkStats stats;
static Held held[HELD_MAX];
static Pending pending[PENDING_MAX];
static PicCommandHandler command_handler;
static PicFrameHook frame_hook;
static bool in_spl;
static PicTime pic_time;
static Settings settings;
static uint16_t next_token = 1;
static uint32_t last_byte_ms;
static uint32_t last_frame_ms;
static uint32_t last_service_ms;
static bool backlog; // this service found the loop had been held up
static bool polling; // pic_link_poll(): commands from the PIC answered BUSY

static Held *find_held(uint8_t class_id, uint8_t message_id, bool create)
{
    int index;

    for (index = 0; index < HELD_MAX; index++)
    {
        if (held[index].valid && held[index].class_id == class_id &&
            held[index].message_id == message_id)
        {
            return &held[index];
        }
    }
    if (!create)
    {
        return NULL;
    }
    for (index = 0; index < HELD_MAX; index++)
    {
        if (!held[index].valid)
        {
            held[index].class_id = class_id;
            held[index].message_id = message_id;
            return &held[index];
        }
    }
    return NULL;
}

void pic_link_send(uint8_t class_id, uint8_t message_id, const void *native)
{
    uint8_t frame[SML_MAX_FRAME_BYTES];
    uint16_t length;

    if (in_spl && class_id != SML_CLASS_BOOT)
    {
        stats.held_back++;
        return;
    }
    length = sml_frame_message(class_id, message_id, native, frame);
    if (length != 0)
    {
        uart.write(frame, length);
        stats.sent++;
    }
}

static void respond(uint16_t token, uint16_t command, uint16_t result, uint16_t detail)
{
    SML_CMD_RESPONSE_PAYLOAD response;

    memset(&response, 0, sizeof(response));
    response.status = result;
    response.detail = detail;
    response.token = token;
    response.command = command;
    pic_link_send(SML_CLASS_CMD, SML_CMD_RESPONSE, &response);
}

// A slot for an answer to wait in, with the next token.
static Pending *take_slot(uint16_t command, uint32_t argument, Stream *reply_to, bool verbose)
{
    Pending *slot = &pending[0];
    int index;

    for (index = 0; index < PENDING_MAX; index++)
    {
        if (pending[index].token == 0)
        {
            slot = &pending[index];
            break;
        }
        if ((int32_t)(pending[index].sent_ms - slot->sent_ms) < 0)
        {
            slot = &pending[index]; // full: reuse the oldest
        }
    }
    slot->token = next_token;
    slot->command = command;
    slot->argument = argument;
    slot->reply_to = reply_to;
    slot->verbose = verbose;
    slot->sent_ms = millis();
    next_token = next_token == 0xFFFF ? 1 : next_token + 1;
    stats.requests++;
    return slot;
}

void pic_link_send_wire(uint8_t class_id, uint8_t message_id, const uint8_t *payload,
                        uint16_t length)
{
    uint8_t frame[SML_MAX_FRAME_BYTES];
    uint16_t bytes = sml_frame(class_id, message_id, payload, length, frame);

    if (bytes != 0)
    {
        uart.write(frame, bytes);
        stats.sent++;
    }
}


uint16_t pic_link_request(uint16_t command, uint32_t argument, Stream *reply_to, bool verbose)
{
    SML_CMD_USER_PAYLOAD request;
    Pending *slot;

    if (in_spl)
    {
        if (reply_to != NULL)
        {
            reply_to->printf("PIC %s: not sent, the PIC is in its SPL (pic boot)\n",
                             pic_report_command_name(command));
        }
        stats.held_back++;
        return 0;
    }
    slot = take_slot(command, argument, reply_to, verbose);

    memset(&request, 0, sizeof(request));
    request.command = command;
    request.token = slot->token;
    request.argument = argument;
    pic_link_send(SML_CLASS_CMD, SML_CMD_USER, &request);
    return request.token;
}

// ---- Settings ---------------------------------------------------------------

static void send_settings(Stream *reply_to)
{
    SML_CMD_SETTINGS_PAYLOAD write;
    Pending *slot = take_slot(SML_OPERATION_SETTINGS_WRITE, 0, reply_to, false);

    memset(&write, 0, sizeof(write));
    write.token = slot->token;
    write.expected_revision = settings.revision;
    write.settings = settings.body;
    pic_link_send(SML_CLASS_CMD, SML_CMD_SETTINGS, &write);
    settings.writing = true;
}

// STAT-SETTINGS, for a link (or an ESP) that has not seen it yet.
static void ask_for_settings(void)
{
    (void)pic_link_request(SML_USER_REQUEST_MESSAGE,
                           (uint32_t)SML_CLASS_STAT << 8 | SML_STAT_SETTINGS, NULL);
}

bool pic_link_settings(SML_SETTINGS_BODY *body)
{
    if (!settings.valid)
    {
        ask_for_settings(); // for next time
        return false;
    }
    *body = settings.body;
    return true;
}

bool pic_link_write_settings(const SML_SETTINGS_BODY *body, Stream *reply_to)
{
    if (!settings.valid)
    {
        ask_for_settings(); // for next time
        return false;
    }
    settings.body = *body;
    if (settings.writing)
    {
        settings.waiting = true;
        settings.waiting_reply = reply_to;
        return true;
    }
    send_settings(reply_to);
    return true;
}

// A write answered: ACK brings the revision for the next. Anything else, or
// no answer, leaves what the PIC holds unknown until it sends STAT-SETTINGS
// again, which is asked for; a change waiting is dropped.
static void settings_answered(const SML_CMD_RESPONSE_PAYLOAD *response)
{
    settings.writing = false;
    if (response != NULL && response->status == SML_RESULT_ACK)
    {
        settings.revision = response->settings_revision;
        if (settings.waiting)
        {
            settings.waiting = false;
            send_settings(settings.waiting_reply);
        }
        return;
    }
    if (settings.waiting && settings.waiting_reply != NULL)
    {
        settings.waiting_reply->printf("PIC settings: not sent, the change before was refused\n");
    }
    settings.waiting = false;
    settings.valid = false;
    ask_for_settings();
}

static void note_settings(const SML_STAT_SETTINGS_PAYLOAD *stat)
{
    if (settings.writing || (stat->flags & SML_SETTINGS_VALID) == 0)
    {
        return; // our write's answer comes first
    }
    settings.valid = true;
    settings.revision = stat->revision;
    settings.body = stat->settings;
}

static void handle_response(const SML_CMD_RESPONSE_PAYLOAD *response)
{
    int index;

    stats.responses++;
    for (index = 0; index < PENDING_MAX; index++)
    {
        Pending *slot = &pending[index];

        if (slot->token == 0 || slot->token != response->token)
        {
            continue;
        }
        if (slot->reply_to != NULL)
        {
            // What is asked for (the PIC's text for SHOW, the message for
            // REQUEST_MESSAGE, the sources table) is the answer, so the ACK
            // itself only shows with -v.
            bool shown =
                (slot->command == SML_USER_SHOW || slot->command == SML_USER_REQUEST_MESSAGE) &&
                response->status == SML_RESULT_ACK;
            bool table = shown && slot->command == SML_USER_REQUEST_MESSAGE &&
                         slot->argument == ((uint32_t)SML_CLASS_STAT << 8 | SML_STAT_SOURCES);

            if (!shown || slot->verbose)
            {
                slot->reply_to->printf("PIC %s: %s", pic_report_command_name(slot->command),
                                       pic_report_result_name(response->status));
                if (response->status == SML_RESULT_NACK)
                {
                    slot->reply_to->printf(" (%s)", pic_report_error_name(response->detail));
                }
                slot->reply_to->printf("\n");
            }
            if (table)
            {
                pic_report_print_sources(slot->reply_to, slot->verbose);
            }
        }
        if (response->status != SML_RESULT_ACCEPTED)
        {
            slot->token = 0; // final answer
            if (slot->command == SML_OPERATION_SETTINGS_WRITE)
            {
                settings_answered(response);
            }
        }
        return;
    }
}

static void expire_requests(void)
{
    int index;

    for (index = 0; index < PENDING_MAX; index++)
    {
        Pending *slot = &pending[index];

        if (slot->token != 0 && millis() - slot->sent_ms > REQUEST_TIMEOUT_MS)
        {
            if (slot->reply_to != NULL)
            {
                slot->reply_to->printf("PIC %s: no response\n",
                                       pic_report_command_name(slot->command));
            }
            slot->token = 0;
            stats.unanswered++;
            if (slot->command == SML_OPERATION_SETTINGS_WRITE)
            {
                settings_answered(NULL);
            }
        }
    }
}

static void handle_command(const SML_CMD_USER_PAYLOAD *command)
{
    uint16_t detail = SML_ERROR_UNSUPPORTED;
    uint16_t result = SML_RESULT_NACK;
    bool restart = false;

    stats.commands++;
    if (polling)
    {
        respond(command->token, command->command, SML_RESULT_NACK, SML_ERROR_BUSY);
        return;
    }
    if (command->command >= SML_USER_ESP_FIRST && command_handler != NULL)
    {
        detail = SML_ERROR_NONE;
        result = command_handler(command->command, command->argument, &detail, &restart);
    }
    respond(command->token, command->command, result, detail);
    if (restart)
    {
        uart.flush();
        delay(100);
        ESP.restart();
    }
}

static void pair_time(const SML_TIME_AUTH_PAYLOAD *auth)
{
    PpsEdge edge;
    int64_t now = esp_timer_get_time();

    if (backlog || !pps_pic_latest(&edge) || now - edge.local_us > PAIR_WINDOW_US ||
        (auth->flags & SML_AUTH_UTC_VALID) == 0)
    {
        stats.unpaired++;
        return;
    }
    pic_time.valid = true;
    pic_time.edge_local_us = edge.local_us;
    pic_time.edge_utc_us = (int64_t)auth->utc_seconds * 1000000 + auth->utc_phase_ns / 1000;
    pic_time.uncertainty_ns = auth->uncertainty_ns;
    pic_time.uncertainty_valid = (auth->flags & SML_AUTH_UNCERTAINTY_VALID) != 0;
    pic_time.source = auth->source;
    pic_time.lock_state = auth->lock_state;
    stats.pairings++;
}

// STAT-SYS: announce the PIC the first time, and whenever its uptime drops
// (it restarted), with why.
static void note_system(const SML_STAT_SYS_PAYLOAD *sys)
{
    static bool seen;
    static uint32_t last_uptime_s;
    char version[16];

    if (in_spl)
    {
        in_spl = false;
        console_printf("PIC: out of its SPL\n");
    }

    pic_report_version(version, sizeof(version), sys->pic_version);
    if (!seen || sys->uptime_s < last_uptime_s)
    {
        if (seen)
        {
            stats.restarts++;
        }
        console_printf("%s: firmware %s, up %lu s, last reset: %s\n",
                       seen ? "PIC restarted" : "PIC", version, (unsigned long)sys->uptime_s,
                       pic_report_reset_name(sys->reset_cause));
    }
    seen = true;
    last_uptime_s = sys->uptime_s;
}

// BOOT-STATUS from the SPL: the link goes quiet, until STAT-SYS.
static void note_boot_status(const SML_BOOT_STATUS_PAYLOAD *status)
{
    if (status->mode == SML_BOOT_MODE_APP || in_spl)
    {
        return;
    }
    in_spl = true;
    stats.spl_entries++;
    console_printf("PIC: in its SPL (%s)\n", pic_report_boot_mode_name(status->mode));
}


static void handle_frame(const SmlFrame *frame)
{
    const SmlMessage *message = sml_message(frame->class_id, frame->message_id);
    uint8_t native[SML_MAX_PAYLOAD_BYTES];

    stats.frames++;
    last_frame_ms = millis();
    if (frame_hook != NULL)
    {
        frame_hook(frame);
    }
    if (message == NULL)
    {
        stats.unknown++;
        return;
    }
    if ((message->direction & SML_FROM_PIC) == 0)
    {
        stats.wrong_direction++;
        return;
    }
    if (message->length == 0) // STAT-DEBUG
    {
        stats.text_frames++;
        console_write_text(frame->payload, frame->length);
        return;
    }
    if (frame->length != message->length)
    {
        stats.wrong_length++;
        return;
    }
    sml_decode(message->layout, frame->payload, native);
    if (frame->class_id == SML_CLASS_CMD)
    {
        if (frame->message_id == SML_CMD_USER)
        {
            handle_command((const SML_CMD_USER_PAYLOAD *)native);
        }
        else if (frame->message_id == SML_CMD_RESPONSE)
        {
            handle_response((const SML_CMD_RESPONSE_PAYLOAD *)native);
        }
        return;
    }
    if (frame->class_id == SML_CLASS_TIME && frame->message_id == SML_TIME_AUTH)
    {
        pair_time((const SML_TIME_AUTH_PAYLOAD *)native);
    }
    if (frame->class_id == SML_CLASS_STAT && frame->message_id == SML_STAT_SYS)
    {
        note_system((const SML_STAT_SYS_PAYLOAD *)native);
    }
    if (frame->class_id == SML_CLASS_STAT && frame->message_id == SML_STAT_SETTINGS)
    {
        note_settings((const SML_STAT_SETTINGS_PAYLOAD *)native);
    }
    if (frame->class_id == SML_CLASS_BOOT && frame->message_id == SML_BOOT_STATUS)
    {
        note_boot_status((const SML_BOOT_STATUS_PAYLOAD *)native);
    }
    Held *slot = find_held(frame->class_id, frame->message_id, true);
    if (slot != NULL)
    {
        memcpy(slot->payload, native, message->length);
        slot->received_ms = millis();
        slot->valid = true;
    }
}

// Text outside frames: the PIC's raw console output, a whole line at a time.
// A frame broken by bytes lost on the way (the UART's FIFO overruns while the
// ESP's flash is erased or written, with its interrupt held off) comes here
// too, as the parser gives its bytes back: its header (0xB5) and binary
// payload mark the line, which is then dropped, not printed. So is a line
// still unfinished when a whole frame arrives: the PIC sends its raw text only
// while it sends no frames (in TEXT mode, or just started), and a line from it
// can pause part-way (its start-up banner does, while it sets up) but is not
// interrupted by a frame.
// Terminal controls (the clear screen at the PIC's start) are left out:
// ESC, then '[' and parameters up to a final byte from 0x40 to 0x7E.
#define TEXT_LINE_BYTES 160U

static uint8_t text_line[TEXT_LINE_BYTES];
static uint16_t text_length;
static bool text_bad;     // the line held a byte no console text has
static bool text_pending; // a line is open

static void end_line(bool complete)
{
    if (text_pending && complete && !text_bad && text_length != 0U)
    {
        console_write_text(text_line, text_length);
        stats.text_bytes += text_length;
    }
    else if (text_pending)
    {
        stats.text_dropped++;
    }
    text_length = 0;
    text_bad = false;
    text_pending = false;
}

static void on_text(const uint8_t *text, uint16_t length, void *context)
{
    static uint8_t escape; // 0: none; 1: after ESC; 2: in a control sequence
    uint16_t index;

    (void)context;
    for (index = 0; index < length; index++)
    {
        uint8_t c = text[index];

        text_pending = true;
        if (c == 0x1B)
        {
            escape = 1;
            continue;
        }
        if (escape == 1)
        {
            escape = c == '[' ? 2 : 0;
            continue;
        }
        if (escape == 2)
        {
            if (c >= 0x40 && c <= 0x7E)
            {
                escape = 0;
            }
            else if (c < 0x20 || c > 0x3F)
            {
                escape = 0; // not a control sequence after all
                text_bad = true;
            }
            continue;
        }
        if ((c >= 0x20 && c < 0x7F) || c == '\r' || c == '\t' || c == '\n')
        {
            text_line[text_length++] = c;
            if (c == '\n' || text_length == TEXT_LINE_BYTES)
            {
                end_line(true);
            }
        }
        else
        {
            text_bad = true;
        }
    }
}

static void on_frame(const SmlFrame *frame, void *context)
{
    (void)context;
    if (text_pending)
    {
        text_bad = true; // a broken frame's bytes, before this one
        end_line(false);
    }
    handle_frame(frame);
}

void pic_link_init(void)
{
    uart.setRxBufferSize(RX_BUFFER_BYTES);
    uart.setTxBufferSize(TX_BUFFER_BYTES);
    uart.begin(BOARD_PIC_BAUD, SERIAL_8N1, BOARD_PIC_RX_PIN, BOARD_PIC_TX_PIN);
    sml_stream_init(&stream, on_frame, on_text, NULL);
    last_service_ms = millis();

    SML_CMD_USER_PAYLOAD show;
    memset(&show, 0, sizeof(show));
    show.command = SML_USER_SHOW;
    show.argument = SML_SHOW_ALL;
    show.token = 0;
    // Announce SML: the PIC switches modes on this frame and prints its state.
    pic_link_send(SML_CLASS_CMD, SML_CMD_USER, &show);
    // A PIC already in SML mode sends STAT-SETTINGS only on a change.
    ask_for_settings();
}

void pic_link_service(void)
{
    uint32_t now = millis();

    backlog = now - last_service_ms > BACKLOG_MS;
    if (backlog && uart.available())
    {
        stats.backlogs++;
    }
    last_service_ms = now;
    while (uart.available())
    {
        sml_stream_push(&stream, (uint8_t)uart.read());
        last_byte_ms = now;
    }
    if (now - last_byte_ms > FRAME_TIMEOUT_MS)
    {
        sml_stream_timeout(&stream);
    }
    expire_requests();
}

void pic_link_poll(void)
{
    if (polling)
    {
        return;
    }
    polling = true;
    pic_link_service();
    polling = false;
}

void pic_link_set_command_handler(PicCommandHandler handler)
{
    command_handler = handler;
}


void pic_link_set_frame_hook(PicFrameHook hook)
{
    frame_hook = hook;
}


void pic_link_left_spl(const char *why)
{
    if (in_spl)
    {
        in_spl = false;
        console_printf("PIC: out of its SPL (%s)\n", why);
    }
}


bool pic_link_in_spl(void)
{
    return in_spl;
}

bool pic_link_is_sml(void)
{
    return stats.frames != 0 && millis() - last_frame_ms < SML_TIMEOUT_MS;
}

bool pic_link_get(uint8_t class_id, uint8_t message_id, void *native, uint32_t max_age_ms)
{
    const Held *slot = find_held(class_id, message_id, false);
    const SmlMessage *message = sml_message(class_id, message_id);

    if (slot == NULL || message == NULL || millis() - slot->received_ms > max_age_ms)
    {
        return false;
    }
    if (native != NULL)
    {
        memcpy(native, slot->payload, message->length);
    }
    return true;
}

bool pic_link_time(PicTime *time)
{
    *time = pic_time;
    return pic_time.valid;
}

bool pic_link_utc_now(uint32_t *utc_seconds)
{
    int64_t elapsed = esp_timer_get_time() - pic_time.edge_local_us;

    if (!pic_time.valid || elapsed > TIME_TIMEOUT_US)
    {
        return false;
    }
    *utc_seconds = (uint32_t)((pic_time.edge_utc_us + elapsed) / 1000000);
    return true;
}

HardwareSerial *pic_link_uart(void)
{
    return &uart;
}

const PicLinkStats *pic_link_stats(void)
{
    return &stats;
}

const SmlParserStats *pic_link_parser_stats(void)
{
    return &stream.parser.stats;
}
