#include "esp_link.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../console/console.h"
#include "../console/debug.h"
#include "../drivers/pcf8563.h"
#include "../drivers/uart1.h"
#include "../drivers/boot_image.h"
#include "../platform/boot.h"
#include "../platform/system.h"
#include "../platform/time_ms.h"
#include "../settings/settings.h"
#include "../sync/source_ntp.h"
#include "../sync/source_rtc.h"
#include "../sync/sync.h"
#include "../ui/timezone.h"
#include "esp_report.h"

static SmlParser parser;
static EspLinkStats stats;
static uint32_t last_rx_ms;

static uint8_t tx_frame[SML_MAX_FRAME_BYTES]; // one message at a time
static uint8_t text_frame[SML_MAX_FRAME_BYTES];
static uint16_t text_length; // console text waiting in text_frame

static bool have_epoch; // the latest epoch, for REQUEST_MESSAGE
static TimingSnapshot last_snapshot;
static SyncUpdate last_update;
static SML_TIME_MEAS_PAYLOAD last_meas; // built once per epoch: it keeps history

static bool have_ntp;
static SML_TIME_NTP_PAYLOAD ntp;
static bool have_leap;
static SML_TIME_LEAP_PAYLOAD leap;
static bool have_net;
static uint32_t net_received_ms;
static SML_STAT_NET_PAYLOAD net;
static bool have_response;
static SML_CMD_RESPONSE_PAYLOAD esp_response;
static uint16_t next_token;

// STAT-SETTINGS: the revision and flags last sent, to send again on a
// change.
static bool have_settings;
static uint32_t settings_sent_revision;
static uint16_t settings_sent_flags;

// SAVE or DEFAULTS answered ACCEPTED: the final answer follows once the
// settings are stored.
static bool save_answer_due;
static uint16_t save_token;
static uint16_t save_command;

// STAT-SOURCES: what was last sent, to send again on a change.
static bool have_sources;
static SML_STAT_SOURCES_PAYLOAD last_sources;
static uint32_t sources_sent_epoch;

// The console's debug bits are SML's.
#if DEBUG_TIMING != SML_DEBUG_TIMING || DEBUG_GNSS != SML_DEBUG_GNSS ||   \
    DEBUG_SYNC != SML_DEBUG_SYNC || DEBUG_SOURCES != SML_DEBUG_SOURCES || \
    DEBUG_NTP != SML_DEBUG_NTP || DEBUG_MINUTE != SML_DEBUG_MINUTE ||     \
    DEBUG_TUNING != SML_DEBUG_TUNING || DEBUG_RTC != SML_DEBUG_RTC ||     \
    DEBUG_EVENTS != SML_DEBUG_EVENTS || DEBUG_ALL != SML_DEBUG_ALL
#error "console/debug.h and SML_DEBUG_* disagree"
#endif

// ---- Sending --------------------------------------------------------------

// All or nothing, so frames never interleave. Waits for ring space by
// moving bytes into the UART itself, as console write() does.
static void send_frame(const uint8_t *frame, uint16_t length)
{
    bool waited = false;

    if (length == 0U || !uart1_is_enabled())
    {
        return;
    }
    while (!uart1_tx_write(frame, length))
    {
        if (length > uart1_tx_free() + uart1_tx_pending())
        {
            return; // can never fit: not possible with SML_MAX_FRAME_BYTES
        }
        waited = true;
        uart1_tx_poll();
    }
    stats.sent++;
    if (waited)
    {
        stats.tx_waits++;
    }
}


void esp_link_send(uint8_t class_id, uint8_t message_id, const void *native)
{
    send_frame(tx_frame, sml_frame_message(class_id, message_id, native, tx_frame));
}


static void flush_text(void)
{
    if (text_length == 0U)
    {
        return;
    }
    send_frame(text_frame, sml_frame(SML_CLASS_STAT, SML_STAT_DEBUG, &text_frame[SML_HEADER_BYTES],
                                     text_length, text_frame));
    stats.text_frames++;
    text_length = 0;
}


// The console sink in SML mode: text is gathered into STAT-DEBUG frames, sent
// at each line end, when full, and by esp_link_service().
static void text_sink(const uint8_t *text, uint16_t length)
{
    while (length-- != 0U)
    {
        uint8_t byte = *text++;
        text_frame[SML_HEADER_BYTES + text_length++] = byte;
        if (byte == '\n' || text_length == SML_MAX_PAYLOAD_BYTES)
        {
            flush_text();
        }
    }
}


static void respond(uint16_t token, uint16_t command, uint8_t result, uint8_t detail)
{
    SML_CMD_RESPONSE_PAYLOAD response;

    memset(&response, 0, sizeof(response));
    response.status = result;
    response.detail = detail;
    response.token = token;
    response.command = command;
    response.settings_revision = settings_revision();
    if (result == SML_RESULT_NACK)
    {
        stats.nacks++;
    }
    esp_link_send(SML_CLASS_CMD, SML_CMD_RESPONSE, &response);
}


// ---- Messages on request, and every epoch --------------------------------

static bool send_message(uint8_t class_id, uint8_t message_id, uint32_t now_ms)
{
    union
    {
        SML_TIME_AUTH_PAYLOAD auth;
        SML_TIME_PIC_PAYLOAD pic;
        SML_TIME_CTRL_PAYLOAD ctrl;
        SML_TIME_MEAS_PAYLOAD meas;
        SML_TIME_GNSS_PAYLOAD gnss;
        SML_TIME_RTC_PAYLOAD rtc;
        SML_STAT_UI_PAYLOAD ui;
        SML_STAT_SOURCES_PAYLOAD sources;
        SML_STAT_SYS_PAYLOAD sys;
        SML_STAT_SETTINGS_PAYLOAD settings;
        SML_STAT_ENV_PAYLOAD env;
    } payload;

    if (class_id == SML_CLASS_TIME && message_id == SML_TIME_AUTH)
    {
        esp_report_time_auth(&payload.auth);
    }
    else if (class_id == SML_CLASS_TIME && message_id == SML_TIME_PIC && have_epoch)
    {
        esp_report_time_pic(&last_snapshot, &last_update, &payload.pic);
    }
    else if (class_id == SML_CLASS_TIME && message_id == SML_TIME_CTRL && have_epoch)
    {
        esp_report_time_ctrl(&last_snapshot, &last_update, &payload.ctrl);
    }
    else if (class_id == SML_CLASS_TIME && message_id == SML_TIME_MEAS && have_epoch)
    {
        payload.meas = last_meas;
    }
    else if (class_id == SML_CLASS_TIME && message_id == SML_TIME_GNSS)
    {
        esp_report_time_gnss(now_ms, &payload.gnss);
    }
    else if (class_id == SML_CLASS_TIME && message_id == SML_TIME_RTC)
    {
        esp_report_time_rtc(&payload.rtc);
    }
    else if (class_id == SML_CLASS_STAT && message_id == SML_STAT_UI)
    {
        esp_report_stat_ui(&payload.ui);
    }
    else if (class_id == SML_CLASS_STAT && message_id == SML_STAT_SOURCES)
    {
        esp_report_stat_sources(last_snapshot.timer.sequence, &payload.sources);
        last_sources = payload.sources;
        have_sources = true;
        sources_sent_epoch = last_snapshot.timer.sequence;
    }
    else if (class_id == SML_CLASS_STAT && message_id == SML_STAT_SYS)
    {
        esp_report_stat_sys(last_snapshot.timer.sequence, &payload.sys);
    }
    else if (class_id == SML_CLASS_STAT && message_id == SML_STAT_ENV)
    {
        esp_report_stat_env(&payload.env);
    }
    else if (class_id == SML_CLASS_STAT && message_id == SML_STAT_SETTINGS)
    {
        settings_stat(&payload.settings);
        have_settings = true;
        settings_sent_revision = payload.settings.revision;
        settings_sent_flags = payload.settings.flags;
    }
    else
    {
        return false;
    }
    esp_link_send(class_id, message_id, &payload);
    return true;
}


// STAT-SOURCES when the selection, the second choice, the sync state or any
// verdict has changed, and every ESP_LINK_SOURCES_EPOCHS anyway.
static bool sources_changed(const SML_STAT_SOURCES_PAYLOAD *now)
{
    uint8_t index;

    if (now->selected != last_sources.selected || now->second != last_sources.second ||
        now->sync_state != last_sources.sync_state)
    {
        return true;
    }
    for (index = 0; index < SML_SOURCES_ENTRIES; index++)
    {
        if (now->entries[index].source != last_sources.entries[index].source ||
            now->entries[index].verdict != last_sources.entries[index].verdict)
        {
            return true;
        }
    }
    return false;
}


static void send_sources_if_due(void)
{
    SML_STAT_SOURCES_PAYLOAD sources;

    esp_report_stat_sources(last_snapshot.timer.sequence, &sources);
    if (have_sources && !sources_changed(&sources) &&
        last_snapshot.timer.sequence - sources_sent_epoch < ESP_LINK_SOURCES_EPOCHS)
    {
        return;
    }
    esp_link_send(SML_CLASS_STAT, SML_STAT_SOURCES, &sources);
    last_sources = sources;
    have_sources = true;
    sources_sent_epoch = last_snapshot.timer.sequence;
}


void esp_link_epoch(const TimingSnapshot *snapshot, const SyncUpdate *update, uint32_t now_ms)
{
    last_snapshot = *snapshot;
    last_update = *update;
    esp_report_time_meas(snapshot, update, now_ms, &last_meas);
    have_epoch = true;
    if (!stats.sml)
    {
        return;
    }
    // TIME-AUTH first: it labels the epoch that has just begun.
    (void)send_message(SML_CLASS_TIME, SML_TIME_AUTH, now_ms);
    (void)send_message(SML_CLASS_TIME, SML_TIME_PIC, now_ms);
    (void)send_message(SML_CLASS_TIME, SML_TIME_CTRL, now_ms);
    (void)send_message(SML_CLASS_TIME, SML_TIME_MEAS, now_ms);
    (void)send_message(SML_CLASS_TIME, SML_TIME_GNSS, now_ms);
    (void)send_message(SML_CLASS_TIME, SML_TIME_RTC, now_ms);
    (void)send_message(SML_CLASS_STAT, SML_STAT_UI, now_ms);
    send_sources_if_due();
    if (last_snapshot.timer.sequence % ESP_LINK_ENV_EPOCHS == 0U)
    {
        (void)send_message(SML_CLASS_STAT, SML_STAT_ENV, now_ms);
    }
    // The first in SML mode, then on a change of revision, DIRTY or
    // DEFAULTS.
    if (!have_settings || settings_revision() != settings_sent_revision ||
        settings_flags() != settings_sent_flags)
    {
        (void)send_message(SML_CLASS_STAT, SML_STAT_SETTINGS, now_ms);
    }
    // Every epoch, so the uptime the ESP shows keeps up; the first in SML
    // mode tells it at once that the PIC (re)started.
    (void)send_message(SML_CLASS_STAT, SML_STAT_SYS, now_ms);
}


// ---- Commands -------------------------------------------------------------

// SAVE: ACCEPTED now, the final answer once stored. One at a time.
static uint8_t start_save(const SML_CMD_USER_PAYLOAD *command, bool *accepted)
{
    if (save_answer_due)
    {
        return SML_ERROR_BUSY;
    }
    settings_save();
    save_answer_due = true;
    save_token = command->token;
    save_command = command->command;
    *accepted = true;
    return SML_ERROR_NONE;
}


// SET_DEBUG: clear << 16 | set; 0 changes nothing. Shown either way. Any bit
// may be cleared (the ESP clears 0xFFFF for "all"); only the defined ones
// may be set.
static uint8_t change_debug(uint32_t argument)
{
    uint16_t set = (uint16_t)argument;
    uint16_t clear = (uint16_t)(argument >> 16);

    if ((set & (uint16_t)~DEBUG_ALL) != 0U)
    {
        return SML_ERROR_ARGUMENT;
    }
    settings_set_debug((uint16_t)((settings_debug_mask() & (uint16_t)~clear) | set));
    debug_print();
    return SML_ERROR_NONE;
}


// SET_RTC: a manual write, as V2's menu, to the nearest second; the result
// is printed once read back. The RTC source finds its second again, and its
// next check may set it to the clock's time.
static uint8_t set_rtc(uint32_t utc_seconds)
{
    if (utc_seconds < PCF8563_FIRST_UTC)
    {
        return SML_ERROR_ARGUMENT;
    }
    if (!pcf8563_status()->present)
    {
        return SML_ERROR_STATE;
    }
    return source_rtc_set(utc_seconds) ? SML_ERROR_NONE : SML_ERROR_BUSY;
}


// Applies a command. Returns the error code, SML_ERROR_NONE for an ACK (or,
// with accepted set, for ACCEPTED: the final answer comes later).
/* ENTER_BOOTLOADER: a request for the SPL in the mailbox, then a reset once
 * the ACK is out; the SPL starts HELD. Refused while a save is pending
 * (BUSY), and when there is no SPL to stay in (STATE): the IPL would only
 * start the application again. */
static uint8_t enter_bootloader(bool *reset)
{
    if (settings_save_pending())
    {
        return SML_ERROR_BUSY;
    }
    if (!boot_mailbox_valid() || !boot_image_spl_usable())
    {
        return SML_ERROR_STATE;
    }
    boot_request_spl();
    *reset = true;
    return SML_ERROR_NONE;
}


static uint8_t run_command(const SML_CMD_USER_PAYLOAD *command, uint32_t now_ms, bool *reset,
                           bool *accepted)
{
    uint32_t argument = command->argument;

    switch (command->command)
    {
        case SML_USER_SHOW:
        {
            bool verbose = (argument & SML_SHOW_VERBOSE) != 0U;

            argument &= ~(uint32_t)SML_SHOW_VERBOSE;
            if (argument == SML_SHOW_ALL)
            {
                esp_report_print_all(now_ms, verbose);
                if (verbose)
                {
                    esp_link_print();
                }
                return SML_ERROR_NONE;
            }
            return esp_report_show(argument, verbose, now_ms) ? SML_ERROR_NONE : SML_ERROR_ARGUMENT;
        }
        case SML_USER_REQUEST_MESSAGE:
            if ((argument >> 16) != 0U)
            {
                return SML_ERROR_ARGUMENT;
            }
            return send_message((uint8_t)(argument >> 8), (uint8_t)argument, now_ms)
                       ? SML_ERROR_NONE
                       : SML_ERROR_UNSUPPORTED;
        case SML_USER_SAVE: return start_save(command, accepted);
        case SML_USER_DEFAULTS:
            // Running only, as any other change: kept once saved.
            settings_restore_defaults();
            return SML_ERROR_NONE;
        case SML_USER_SET_RTC: return set_rtc(argument);
        case SML_USER_SET_DEBUG: return change_debug(argument);
        case SML_USER_RESYNC: sync_resync(); return SML_ERROR_NONE;
        case SML_USER_RESET: *reset = true; return SML_ERROR_NONE;
        case SML_USER_ENTER_BOOTLOADER: return enter_bootloader(reset);
        default: return SML_ERROR_UNSUPPORTED;
    }
}


static void handle_command(const SmlFrame *frame, uint32_t now_ms)
{
    SML_CMD_USER_PAYLOAD command;
    uint8_t error;
    bool reset = false;
    bool accepted = false;

    (void)sml_decode(sml_message(SML_CLASS_CMD, SML_CMD_USER)->layout, frame->payload, &command);
    stats.commands++;
    error = run_command(&command, now_ms, &reset, &accepted);
    respond(command.token, command.command,
            error != SML_ERROR_NONE ? SML_RESULT_NACK
            : accepted              ? SML_RESULT_ACCEPTED
                                    : SML_RESULT_ACK,
            error);
    if (reset)
    {
        flush_text();
        console_flush();
        system_reset();
    }
}


// ---- Receiving ------------------------------------------------------------

static void handle_leap(const SmlFrame *frame)
{
    UtcLeap announced = {0, 0};
    uint32_t midnight;

    (void)sml_decode(sml_message(SML_CLASS_TIME, SML_TIME_LEAP)->layout, frame->payload, &leap);
    have_leap = true;
    stats.leaps++;
    // Only a whole-midnight event of +/-1 s is a leap second.
    if ((leap.flags & SML_LEAP_VALID) != 0U && (leap.flags & SML_LEAP_EVENT_PENDING) != 0U &&
        (leap.change == 1 || leap.change == -1) && utc_leap_snap(leap.event_utc, 0U, &midnight))
    {
        announced.event_utc = midnight;
        announced.change = leap.change;
    }
    sync_set_leap(&announced);
}


// CMD-SETTINGS: every setting at once, if the ESP has seen the latest
// revision; persisted later if asked.
static void handle_settings(const SmlFrame *frame)
{
    SML_CMD_SETTINGS_PAYLOAD write;
    uint8_t error;

    (void)sml_decode(sml_message(SML_CLASS_CMD, SML_CMD_SETTINGS)->layout, frame->payload, &write);
    stats.settings_writes++;
    if ((write.flags & (uint16_t)~SML_WRITE_PERSIST) != 0U || write.reserved != 0U)
    {
        error = SML_ERROR_ARGUMENT;
    }
    else if (write.expected_revision != settings_revision())
    {
        error = SML_ERROR_STALE_REVISION;
    }
    else
    {
        error = settings_write(&write.settings);
    }
    if (error == SML_ERROR_NONE && (write.flags & SML_WRITE_PERSIST) != 0U)
    {
        settings_save();
    }
    respond(write.token, SML_OPERATION_SETTINGS_WRITE,
            error == SML_ERROR_NONE ? SML_RESULT_ACK : SML_RESULT_NACK, error);
}


static void handle_frame(const SmlFrame *frame, uint32_t now_ms)
{
    const SmlMessage *message = sml_message(frame->class_id, frame->message_id);

    stats.received++;
    esp_link_set_sml(); // any valid frame: the ESP speaks SML
    if (message == NULL)
    {
        stats.unknown++;
        return;
    }
    if ((message->direction & SML_FROM_ESP) == 0U)
    {
        stats.wrong_direction++;
        return;
    }
    if (frame->length != message->length)
    {
        stats.wrong_length++;
        if (frame->class_id == SML_CLASS_CMD && frame->length >= 6U)
        {
            // CRC good, so the token can be trusted.
            respond(sml_get_u16(&frame->payload[4]),
                    frame->message_id == SML_CMD_USER ? sml_get_u16(&frame->payload[0])
                                                      : SML_OPERATION_SETTINGS_WRITE,
                    SML_RESULT_NACK, SML_ERROR_LENGTH);
        }
        return;
    }
    if (frame->class_id == SML_CLASS_TZ && frame->message_id == SML_TZ_STATE)
    {
        SML_TZ_STATE_PAYLOAD state;
        (void)sml_decode(message->layout, frame->payload, &state);
        stats.tz_states++;
        (void)timezone_apply(&state);
    }
    else if (frame->class_id == SML_CLASS_TIME && frame->message_id == SML_TIME_LEAP)
    {
        handle_leap(frame);
    }
    else if (frame->class_id == SML_CLASS_TIME && frame->message_id == SML_TIME_NTP)
    {
        (void)sml_decode(message->layout, frame->payload, &ntp);
        have_ntp = true;
        stats.ntp_times++;
        source_ntp_received(&ntp);
    }
    else if (frame->class_id == SML_CLASS_STAT && frame->message_id == SML_STAT_NET)
    {
        (void)sml_decode(message->layout, frame->payload, &net);
        have_net = true;
        net_received_ms = now_ms;
        stats.net_states++;
    }
    else if (frame->class_id == SML_CLASS_CMD && frame->message_id == SML_CMD_USER)
    {
        handle_command(frame, now_ms);
    }
    else if (frame->class_id == SML_CLASS_CMD && frame->message_id == SML_CMD_RESPONSE)
    {
        (void)sml_decode(message->layout, frame->payload, &esp_response);
        have_response = true;
        stats.responses++;
    }
    else if (frame->class_id == SML_CLASS_CMD && frame->message_id == SML_CMD_SETTINGS)
    {
        handle_settings(frame);
    }
    stats.handled++;
}


void esp_link_init(void)
{
    sml_parser_init(&parser);
    memset(&stats, 0, sizeof(stats));
    text_length = 0;
    have_epoch = false;
    have_ntp = false;
    have_leap = false;
    have_net = false;
    have_response = false;
    have_settings = false;
    save_answer_due = false;
    next_token = 0;
    last_rx_ms = 0;
    if (ESP_LINK_START_SML)
    {
        esp_link_set_sml();
    }
}


void esp_link_service(uint32_t now_ms)
{
    uint8_t byte;
    bool received = false;
    SmlFrame frame;

    if (uart1_rx_error_pending() && uart1_rx_recover() != 0U)
    {
        stats.rx_errors++;
        (void)sml_parser_timeout(&parser);
    }
    while (uart1_rx_pop(&byte))
    {
        received = true;
        if (sml_parser_push(&parser, byte, &frame))
        {
            do
            {
                handle_frame(&frame, now_ms);
            } while (sml_parser_next(&parser, &frame));
        }
    }
    if (received)
    {
        last_rx_ms = now_ms;
    }
    else if (time_ms_has_elapsed(now_ms, last_rx_ms, ESP_LINK_FRAME_TIMEOUT_MS))
    {
        (void)sml_parser_timeout(&parser);
    }
    if (save_answer_due && !settings_save_pending())
    {
        uint8_t error = settings_save_error();

        save_answer_due = false;
        respond(save_token, save_command,
                error == SML_ERROR_NONE ? SML_RESULT_ACK : SML_RESULT_NACK, error);
    }
    flush_text();
}


bool esp_link_is_sml(void)
{
    return stats.sml;
}


void esp_link_set_sml(void)
{
    if (!stats.sml)
    {
        stats.sml = true;
        console_set_sink(text_sink);
    }
}


const EspLinkStats *esp_link_stats(void)
{
    return &stats;
}


const SmlParserStats *esp_link_parser_stats(void)
{
    return &parser.stats;
}


bool esp_link_ntp(SML_TIME_NTP_PAYLOAD *destination)
{
    if (have_ntp)
    {
        *destination = ntp;
    }
    return have_ntp;
}


bool esp_link_leap(SML_TIME_LEAP_PAYLOAD *destination)
{
    if (have_leap)
    {
        *destination = leap;
    }
    return have_leap;
}


bool esp_link_net(uint32_t now_ms, SML_STAT_NET_PAYLOAD *destination)
{
    if (!have_net || time_ms_has_elapsed(now_ms, net_received_ms, ESP_LINK_NET_TIMEOUT_MS))
    {
        return false;
    }
    *destination = net;
    return true;
}


uint16_t esp_link_request(uint16_t command, uint32_t argument)
{
    SML_CMD_USER_PAYLOAD request;

    if (!stats.sml)
    {
        return 0;
    }
    next_token = (uint16_t)(next_token + 1U == 0U ? 1U : next_token + 1U); // never 0
    request.command = command;
    request.flags = 0;
    request.token = next_token;
    request.argument = argument;
    esp_link_send(SML_CLASS_CMD, SML_CMD_USER, &request);
    stats.requests++;
    return next_token;
}


bool esp_link_response(SML_CMD_RESPONSE_PAYLOAD *destination)
{
    if (have_response)
    {
        *destination = esp_response;
    }
    return have_response;
}


void esp_link_print(void)
{
    const SmlParserStats *parsed = &parser.stats;

    printf("ESP link: %s; received %lu (%lu handled, %lu unknown, %lu wrong direction, %lu wrong "
           "length), noise %lu bytes, %lu CRC, %lu length, %lu timeouts, %lu UART errors\r\n",
           stats.sml ? "SML" : "text", (unsigned long)stats.received, (unsigned long)stats.handled,
           (unsigned long)stats.unknown, (unsigned long)stats.wrong_direction,
           (unsigned long)stats.wrong_length, (unsigned long)parsed->noise_bytes,
           (unsigned long)parsed->crc_errors, (unsigned long)parsed->length_errors,
           (unsigned long)parsed->timeouts, (unsigned long)stats.rx_errors);
    printf("  TZ-STATE %lu, TIME-LEAP %lu, TIME-NTP %lu, STAT-NET %lu, commands %lu "
           "(%lu settings writes, %lu NACK), requests %lu (%lu answered); sent %lu frames "
           "(%lu text), %lu waited\r\n",
           (unsigned long)stats.tz_states, (unsigned long)stats.leaps,
           (unsigned long)stats.ntp_times, (unsigned long)stats.net_states,
           (unsigned long)stats.commands, (unsigned long)stats.settings_writes,
           (unsigned long)stats.nacks, (unsigned long)stats.requests,
           (unsigned long)stats.responses, (unsigned long)stats.sent,
           (unsigned long)stats.text_frames, (unsigned long)stats.tx_waits);
}
