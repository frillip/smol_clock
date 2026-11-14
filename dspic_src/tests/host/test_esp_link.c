/*
 * The ESP link (esp/esp_link.c) against a stand-in UART1 and stand-ins for
 * the modules it reports on: mode change on the first SML frame, debug text
 * framing, telemetry each epoch, and every message and command it handles.
 * The time zone module, the settings and the store are the real ones, the
 * store on a model of the flash (fake_flash).
 */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "../../drivers/pcf8563.h"
#include "../../drivers/tubes.h"
#include "../../drivers/uart1.h"
#include "../../esp/esp_link.h"
#include "../../sync/source_ntp.h"
#include "../../esp/esp_report.h"
#include "../../../common/boot_layout.h"
#include "../../platform/boot.h"
#include "../../platform/system.h"
#include "../../settings/settings.h"
#include "../../settings/store.h"
#include "../../console/console.h"
#include "../../console/debug.h"
#include "../../ui/alarm.h"
#include "../../ui/display.h"
#include "../../ui/timezone.h"
#include "fake_flash/fake_flash.h"

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

// ---- Stand-in UART1: bytes to receive, and everything sent -----------------
static uint8_t rx[4096];
static uint16_t rx_head, rx_tail;
static uint8_t tx[16384];
static uint16_t tx_length;

bool uart1_is_enabled(void)
{
    return true;
}


bool uart1_rx_pop(uint8_t *byte)
{
    if (rx_tail == rx_head)
        return false;
    *byte = rx[rx_tail++];
    return true;
}


bool uart1_rx_error_pending(void)
{
    return false;
}


uint16_t uart1_rx_recover(void)
{
    return 0;
}


uint16_t uart1_tx_free(void)
{
    return 1023;
}


uint16_t uart1_tx_pending(void)
{
    return 0;
}


bool uart1_tx_write(const uint8_t *source, uint16_t length)
{
    memcpy(&tx[tx_length], source, length);
    tx_length = (uint16_t)(tx_length + length);
    return true;
}


void uart1_tx_poll(void)
{
}


static void receive(const uint8_t *bytes, uint16_t length)
{
    memcpy(&rx[rx_head], bytes, length);
    rx_head = (uint16_t)(rx_head + length);
}


// ---- Stand-ins for the rest of the clock -----------------------------------
static ConsoleSink sink;
void console_set_sink(ConsoleSink new_sink)
{
    sink = new_sink;
}


void console_flush(void)
{
}


// console/text.c writes here: on to the sink, as the real console does in SML mode.
void console_write(const char *text, uint16_t length)
{
    if (sink != NULL)
        sink((const uint8_t *)text, length);
}


static jmp_buf reset_jump;
static int resets;
void system_reset(void)
{
    resets++;
    longjmp(reset_jump, 1);
}


// The SPL as boot_image.c would find it.
static bool spl_usable;
bool boot_image_spl_usable(void)
{
    return spl_usable;
}


extern volatile BootMailbox boot_host_mailbox; // platform/boot.c on the host

static int resyncs;
void sync_resync(void)
{
    resyncs++;
}


static UtcLeap leap_set;
static int leap_calls;
void sync_set_leap(const UtcLeap *leap)
{
    leap_set = *leap;
    leap_calls++;
}


// The NTP source, handed each TIME-NTP.
static SML_TIME_NTP_PAYLOAD ntp_handed;
static int ntp_calls;

void source_ntp_received(const SML_TIME_NTP_PAYLOAD *ntp)
{
    ntp_handed = *ntp;
    ntp_calls++;
}


// The manual brightness level and mode, as the settings apply them.
static uint16_t brightness;
static bool brightness_auto;
void brightness_set_level(uint16_t level)
{
    brightness = level;
}


void brightness_set_auto(bool automatic)
{
    brightness_auto = automatic;
}


// The beeps setting, as applied.
static bool beeps_on;
void beeps_set_enabled(bool enabled)
{
    beeps_on = enabled;
}


static DisplayMode mode_set = DISPLAY_MODE_COUNT;
void display_set_mode(DisplayMode mode)
{
    mode_set = mode;
}


static bool alarm_on;
static uint32_t alarm_at;
void alarm_set(bool enabled, uint32_t time_of_day_s)
{
    alarm_on = enabled;
    alarm_at = time_of_day_s;
}


static bool twelve_set;
void display_set_12h(bool twelve_hours)
{
    twelve_set = twelve_hours;
}


static uint32_t delta_set;
static bool delta_local_set;
void display_set_delta_epoch(uint32_t seconds, bool local)
{
    delta_set = seconds;
    delta_local_set = local;
}


void display_print_epoch(uint32_t seconds, bool local)
{
    (void)seconds;
    (void)local;
}


const char *display_mode_name(DisplayMode mode)
{
    return mode == DISPLAY_MODE_HHMM ? "HHMM" : "OTHER";
}


static int prints;
void esp_report_print_all(uint32_t now_ms, bool verbose)
{
    (void)now_ms;
    (void)verbose;
    prints++;
}


// SHOW topics other than ALL.
static uint32_t shown_topic = 0xFFFFFFFFUL;
static bool shown_verbose;
bool esp_report_show(uint32_t topic, bool verbose, uint32_t now_ms)
{
    (void)now_ms;
    shown_verbose = verbose;
    if (topic == SML_SHOW_ALL || topic >= SML_SHOW_COUNT)
    {
        return false;
    }
    shown_topic = topic;
    return true;
}


// Each payload carries a marker the test can find.
void esp_report_time_auth(SML_TIME_AUTH_PAYLOAD *p)
{
    memset(p, 0, sizeof *p);
    p->utc_seconds = 0xA0A0A0A0UL;
}


void esp_report_time_pic(const TimingSnapshot *s, const SyncUpdate *u, SML_TIME_PIC_PAYLOAD *p)
{
    memset(p, 0, sizeof *p);
    p->epoch_sequence = s->timer.sequence;
    (void)u;
}


void esp_report_time_ctrl(const TimingSnapshot *s, const SyncUpdate *u, SML_TIME_CTRL_PAYLOAD *p)
{
    memset(p, 0, sizeof *p);
    p->epoch_sequence = s->timer.sequence;
    (void)u;
}


void esp_report_time_meas(const TimingSnapshot *s, const SyncUpdate *u, uint32_t now_ms,
                          SML_TIME_MEAS_PAYLOAD *p)
{
    memset(p, 0, sizeof *p);
    p->epoch_sequence = s->timer.sequence;
    (void)u;
    (void)now_ms;
}


void esp_report_time_gnss(uint32_t now_ms, SML_TIME_GNSS_PAYLOAD *p)
{
    memset(p, 0, sizeof *p);
    p->utc_seconds = now_ms;
}


void esp_report_time_rtc(SML_TIME_RTC_PAYLOAD *p)
{
    memset(p, 0, sizeof *p);
    p->uncertainty_ns = 777000;
}


void esp_report_stat_env(SML_STAT_ENV_PAYLOAD *p)
{
    memset(p, 0, sizeof *p);
    p->flags = SML_ENV_LUX_PRESENT | SML_ENV_LUX_VALID;
    p->lux = 321;
}


void esp_report_stat_ui(SML_STAT_UI_PAYLOAD *p)
{
    memset(p, 0, sizeof *p);
    p->brightness = 1234;
}


// STAT-SYS: the uptime is the epoch, the rest fixed.
void esp_report_stat_sys(uint32_t epoch_sequence, SML_STAT_SYS_PAYLOAD *p)
{
    memset(p, 0, sizeof *p);
    p->reset_cause = SML_RESET_POR | SML_RESET_BOR;
    p->uptime_s = epoch_sequence;
    p->pic_version = 0x000100UL;
    p->loop_max_ms = 12;
}


// The RTC, as the test sets it.
static Pcf8563Status rtc_status;
static bool rtc_busy;
static uint32_t rtc_written;
const Pcf8563Status *pcf8563_status(void)
{
    return &rtc_status;
}


bool source_rtc_set(uint32_t utc_seconds)
{
    if (rtc_busy)
    {
        return false;
    }
    rtc_written = utc_seconds;
    return true;
}


// The synchroniser's view, as the test sets it.
static uint8_t sources_verdict = SML_VERDICT_GOOD;
static int sources_built;
void esp_report_stat_sources(uint32_t epoch_sequence, SML_STAT_SOURCES_PAYLOAD *p)
{
    memset(p, 0, sizeof *p);
    p->selected = SML_SOURCE_GNSS;
    p->epoch_sequence = epoch_sequence;
    p->entries[0].source = SML_SOURCE_GNSS;
    p->entries[0].verdict = sources_verdict;
    sources_built++;
}


// ---- Reading what was sent -----------------------------------------------
typedef struct
{
    uint8_t class_id, message_id;
    uint16_t length;
    uint8_t payload[256];
} Sent;

static Sent sent[64];
static int sent_count;
static char text[4096];
static size_t text_length;

// Parses everything sent since the last call; raw text outside frames goes
// to text[] too (TEXT mode).
static void collect(void)
{
    static SmlParser parser;
    static bool started;
    SmlFrame frame;

    if (!started)
    {
        sml_parser_init(&parser);
        started = true;
    }
    sent_count = 0;
    for (uint16_t i = 0; i < tx_length; i++)
    {
        uint32_t noise = parser.stats.noise_bytes;
        if (sml_parser_push(&parser, tx[i], &frame))
        {
            do
            {
                Sent *s = &sent[sent_count++];
                s->class_id = frame.class_id;
                s->message_id = frame.message_id;
                s->length = frame.length;
                memcpy(s->payload, frame.payload, frame.length);
                if (frame.class_id == SML_CLASS_STAT && frame.message_id == SML_STAT_DEBUG)
                {
                    memcpy(&text[text_length], frame.payload, frame.length);
                    text_length += frame.length;
                }
            } while (sml_parser_next(&parser, &frame));
        }
        else if (parser.stats.noise_bytes != noise)
        {
            text[text_length++] = (char)tx[i];
        }
    }
    tx_length = 0;
}


static void send_command(uint16_t command, uint16_t token, uint32_t argument)
{
    SML_CMD_USER_PAYLOAD payload = {command, 0, token, argument};
    uint8_t frame[SML_MAX_FRAME_BYTES];
    receive(frame, sml_frame_message(SML_CLASS_CMD, SML_CMD_USER, &payload, frame));
}


// The response to the last command, decoded.
static bool response(SML_CMD_RESPONSE_PAYLOAD *r)
{
    for (int i = sent_count - 1; i >= 0; i--)
        if (sent[i].class_id == SML_CLASS_CMD && sent[i].message_id == SML_CMD_RESPONSE)
        {
            sml_decode(sml_message(SML_CLASS_CMD, SML_CMD_RESPONSE)->layout, sent[i].payload, r);
            return true;
        }
    return false;
}


static uint32_t now = 1000;

static void service(void)
{
    now += 10;
    esp_link_service(now);
    collect();
}


static bool command_result(uint16_t command, uint16_t token, uint32_t argument, uint16_t status,
                           uint16_t detail)
{
    SML_CMD_RESPONSE_PAYLOAD r;
    send_command(command, token, argument);
    service();
    return response(&r) && r.status == status && r.detail == detail && r.token == token &&
           r.command == command;
}


// CMD-SETTINGS with the revision the ESP expects; the response decoded.
static bool write_settings(const SML_SETTINGS_BODY *body, uint32_t expected, uint16_t flags,
                           uint16_t token, SML_CMD_RESPONSE_PAYLOAD *r)
{
    SML_CMD_SETTINGS_PAYLOAD write;
    uint8_t frame[SML_MAX_FRAME_BYTES];

    memset(&write, 0, sizeof write);
    write.flags = flags;
    write.token = token;
    write.expected_revision = expected;
    write.settings = *body;
    receive(frame, sml_frame_message(SML_CLASS_CMD, SML_CMD_SETTINGS, &write, frame));
    service();
    return response(r) && r->token == token && r->command == SML_OPERATION_SETTINGS_WRITE;
}


// One setting changed, as the ESP writes it: the whole body, against the
// current revision.
static bool settings_result(const SML_SETTINGS_BODY *body, uint16_t token, uint16_t status,
                            uint16_t detail)
{
    SML_CMD_RESPONSE_PAYLOAD r;
    return write_settings(body, settings_revision(), 0, token, &r) && r.status == status &&
           r.detail == detail;
}


// The epochs the settings service has seen, as main() would run it.
static uint32_t store_epoch = 1000;

/*
 * Settings: changed by CMD-SETTINGS, the revision and the flags,
 * STAT-SETTINGS on a change, SAVE answered ACCEPTED and then once stored,
 * DEFAULTS applied but not stored, what a restart brings back, and the time
 * zone schedule stored by itself.
 */
static void test_settings(TimingSnapshot *snapshot, SyncUpdate *update)
{
    SML_SETTINGS_BODY body;
    SML_CMD_RESPONSE_PAYLOAD r;
    uint32_t revision = settings_revision();
    uint32_t timezone_noticed = store_epoch;

    // The schedule applied earlier (India) is noticed as a change to store.
    settings_service(store_epoch++, false);

    // The writes earlier changed the manual time zone and the brightness:
    // dirty, and no longer the defaults.
    CHECK(revision > 0);
    CHECK(settings_flags() == (SML_SETTINGS_VALID | SML_SETTINGS_DIRTY));
    // Responses carry the revision.
    CHECK(command_result(SML_USER_SHOW, 50, SML_SHOW_ALL, SML_RESULT_ACK, SML_ERROR_NONE) &&
          response(&r) && r.settings_revision == revision);
    // STAT-SETTINGS goes again in the next epoch, since it changed.
    snapshot->timer.sequence++;
    esp_link_epoch(snapshot, update, now);
    collect();
    {
        int index;
        int found = -1;
        for (index = 0; index < sent_count; index++)
            if (sent[index].message_id == SML_STAT_SETTINGS &&
                sent[index].class_id == SML_CLASS_STAT)
                found = index;
        CHECK(found == sent_count - 2 && sml_get_u32(&sent[found].payload[4]) == revision &&
              sml_get_u16(&sent[found].payload[0]) == (SML_SETTINGS_VALID | SML_SETTINGS_DIRTY));
    }
    // ... and not again while nothing changes.
    snapshot->timer.sequence++;
    esp_link_epoch(snapshot, update, now);
    collect();
    CHECK(sent[sent_count - 2].message_id != SML_STAT_SETTINGS);
    // Writing the values it already has is not a change.
    settings_body(&body);
    CHECK(settings_result(&body, 51, SML_RESULT_ACK, SML_ERROR_NONE) &&
          settings_revision() == revision);

    // The oscillator baseline: within 1000 ppm of FCY.
    body.calibration_fosc_hz = 40041000UL;
    CHECK(settings_result(&body, 53, SML_RESULT_NACK, SML_ERROR_ARGUMENT));
    body.calibration_fosc_hz = 40000123UL;
    CHECK(settings_result(&body, 52, SML_RESULT_ACK, SML_ERROR_NONE));
    settings_body(&body);
    CHECK(body.calibration_fosc_hz == 40000123UL && settings_revision() == revision + 1U);
    revision = settings_revision();

    // CMD-SETTINGS: stale, then wrong, then right.
    settings_body(&body);
    body.brightness = 2500;
    CHECK(write_settings(&body, revision - 1U, 0, 60, &r) && r.status == SML_RESULT_NACK &&
          r.detail == SML_ERROR_STALE_REVISION && r.settings_revision == revision);
    body.alarm_offset_s = 86400;
    CHECK(write_settings(&body, revision, 0, 61, &r) && r.detail == SML_ERROR_ARGUMENT);
    body.alarm_offset_s = 0;
    body.schema_version = 2;
    CHECK(write_settings(&body, revision, 0, 62, &r) && r.detail == SML_ERROR_SCHEMA);
    body.schema_version = SML_SETTINGS_SCHEMA;
    body.flags |= 0x0100U;
    CHECK(write_settings(&body, revision, 0, 63, &r) && r.detail == SML_ERROR_ARGUMENT);
    body.flags &= (uint16_t)~0x0100U;
    body.selected_display = SML_DISPLAY_TEMP;
    CHECK(write_settings(&body, revision, 0, 64, &r) && r.detail == SML_ERROR_ARGUMENT);
    body.selected_display = SML_DISPLAY_HHMM;
    CHECK(write_settings(&body, revision, 0x0002U, 65, &r) && r.detail == SML_ERROR_ARGUMENT);
    CHECK(brightness == 3000 && settings_revision() == revision); // nothing applied
    body.flags |= SML_SETTING_FORMAT_12H | SML_SETTING_ALARM_ENABLED;
    body.alarm_offset_s = 7 * 3600;
    CHECK(write_settings(&body, revision, SML_WRITE_PERSIST, 66, &r) &&
          r.status == SML_RESULT_ACK && r.settings_revision == revision + 1U);
    CHECK(brightness == 2500 && settings_save_pending());
    revision = settings_revision();

    // Stored at the next quiet moment, and no longer dirty.
    settings_service(store_epoch++, false);
    CHECK(settings_save_pending());
    settings_service(store_epoch++, true);
    CHECK(!settings_save_pending() && settings_save_error() == SML_ERROR_NONE);
    CHECK(settings_flags() == SML_SETTINGS_VALID && settings_stats()->saves == 1);

    // SAVE: ACCEPTED, then the final answer once stored. One at a time.
    CHECK(command_result(SML_USER_SET_DEBUG, 67, DEBUG_GNSS, SML_RESULT_ACK, SML_ERROR_NONE));
    CHECK(settings_flags() == (SML_SETTINGS_VALID | SML_SETTINGS_DIRTY)); // the debug mask
    CHECK(settings_revision() == revision);                               // ... is not counted
    CHECK(command_result(SML_USER_SAVE, 68, 0, SML_RESULT_ACCEPTED, SML_ERROR_NONE));
    CHECK(command_result(SML_USER_SAVE, 69, 0, SML_RESULT_NACK, SML_ERROR_BUSY));
    service();
    CHECK(sent_count == 0); // not stored yet
    settings_service(store_epoch++, true);
    service();
    CHECK(response(&r) && r.token == 68 && r.command == SML_USER_SAVE &&
          r.status == SML_RESULT_ACK);
    CHECK(settings_flags() == SML_SETTINGS_VALID && settings_stats()->saves == 2);
    // A SAVE with nothing changed writes nothing.
    CHECK(command_result(SML_USER_SAVE, 70, 0, SML_RESULT_ACCEPTED, SML_ERROR_NONE));
    settings_service(store_epoch++, true);
    service();
    CHECK(response(&r) && r.token == 70 && r.status == SML_RESULT_ACK &&
          settings_stats()->saves == 2);

    // SHOW: each topic printed (by esp_report, a stand-in here), then the
    // ACK alone; an unknown topic is ARGUMENT. STAT-SETTINGS on request.
    {
        uint32_t topic;
        for (topic = SML_SHOW_STORED; topic < SML_SHOW_COUNT; topic++)
        {
            CHECK(command_result(SML_USER_SHOW, 71, topic, SML_RESULT_ACK, SML_ERROR_NONE) &&
                  shown_topic == topic && sent_count == 1);
        }
        CHECK(
            command_result(SML_USER_SHOW, 72, SML_SHOW_COUNT, SML_RESULT_NACK, SML_ERROR_ARGUMENT));
        CHECK(command_result(SML_USER_SHOW, 72, 0x10000UL, SML_RESULT_NACK, SML_ERROR_ARGUMENT));
        CHECK(command_result(SML_USER_SHOW, 72, SML_SHOW_RTC, SML_RESULT_ACK, SML_ERROR_NONE) &&
              !shown_verbose);
        CHECK(command_result(SML_USER_SHOW, 72, SML_SHOW_RTC | SML_SHOW_VERBOSE, SML_RESULT_ACK,
                             SML_ERROR_NONE) &&
              shown_verbose && shown_topic == SML_SHOW_RTC);
        CHECK(command_result(SML_USER_SHOW, 72, SML_SHOW_RTC | 0x0200U, SML_RESULT_NACK,
                             SML_ERROR_ARGUMENT));
    }
    CHECK(command_result(SML_USER_REQUEST_MESSAGE, 73, SML_CLASS_STAT << 8 | SML_STAT_SETTINGS,
                         SML_RESULT_ACK, SML_ERROR_NONE) &&
          sent[0].message_id == SML_STAT_SETTINGS);

    // The time zone schedule (India, from earlier) is stored by itself, a
    // minute after it last changed.
    CHECK(settings_stats()->timezone_saves == 0);
    settings_service(timezone_noticed + SETTINGS_TIMEZONE_DELAY_EPOCHS - 1U, true);
    CHECK(settings_stats()->timezone_saves == 0);
    settings_service(timezone_noticed + SETTINGS_TIMEZONE_DELAY_EPOCHS, true);
    CHECK(settings_stats()->timezone_saves == 1);
    store_epoch = timezone_noticed + SETTINGS_TIMEZONE_DELAY_EPOCHS + 1U;

    // A restart: everything back as saved, the schedule included, before
    // the ESP has sent anything.
    brightness = 0;
    debug_set(DEBUG_NONE);
    timezone_init();
    settings_init();
    {
        TimezoneOffset offset;
        SML_SETTINGS_BODY back;

        settings_body(&back);
        CHECK(settings_stats()->loaded && settings_stats()->timezone_restored);
        CHECK(settings_revision() == revision && settings_flags() == SML_SETTINGS_VALID);
        CHECK(brightness == 2500 && debug_mask() == DEBUG_GNSS && back.alarm_offset_s == 7 * 3600 &&
              (back.flags & SML_SETTING_FORMAT_12H) != 0U &&
              back.calibration_fosc_hz == 40000123UL);
        CHECK(!timezone_is_auto() && !timezone_is_dst_auto());
        CHECK(timezone_schedule()->held && timezone_schedule()->entries[0].base_offset_s == 19800);
        timezone_offset_at(1800000000UL, &offset);
        CHECK(offset.offset_s == -14400); // manual base -5 h, manual DST +1 h
        // The same schedule again from the ESP is not a change to store.
        settings_service(store_epoch++, true);
        CHECK(!timezone_take_changed());
    }

    // DEFAULTS: applied at once and answered ACK, but not stored: a restart
    // brings back what was saved.
    {
        uint32_t saves = settings_stats()->saves;

        CHECK(command_result(SML_USER_DEFAULTS, 74, 0, SML_RESULT_ACK, SML_ERROR_NONE));
        CHECK(timezone_is_auto() && brightness == TUBES_BRIGHTNESS_DEFAULT && debug_mask() == 0);
        CHECK(settings_flags() ==
              (SML_SETTINGS_VALID | SML_SETTINGS_DIRTY | SML_SETTINGS_DEFAULTS));
        settings_service(store_epoch++, true);
        CHECK(!settings_save_pending() && settings_stats()->saves == saves);
    }
    timezone_init();
    settings_init();
    CHECK(settings_stats()->loaded && settings_revision() == revision &&
          settings_flags() == SML_SETTINGS_VALID && brightness == 2500);
    // ... and kept once saved.
    CHECK(command_result(SML_USER_DEFAULTS, 75, 0, SML_RESULT_ACK, SML_ERROR_NONE));
    CHECK(command_result(SML_USER_SAVE, 76, 0, SML_RESULT_ACCEPTED, SML_ERROR_NONE));
    settings_service(store_epoch++, true);
    service();
    CHECK(response(&r) && r.token == 76 && r.command == SML_USER_SAVE &&
          r.status == SML_RESULT_ACK);
    CHECK(settings_flags() == (SML_SETTINGS_VALID | SML_SETTINGS_DEFAULTS));
    timezone_init();
    settings_init();
    CHECK(settings_stats()->loaded &&
          settings_flags() == (SML_SETTINGS_VALID | SML_SETTINGS_DEFAULTS));
    CHECK(fake_flash_violations == 0 && store_status()->failures == 0);
}


int main(void)
{
    TimingSnapshot snapshot;
    SyncUpdate update;
    uint8_t frame[SML_MAX_FRAME_BYTES];
    uint16_t length;

    memset(&snapshot, 0, sizeof snapshot);
    memset(&update, 0, sizeof update);
    fake_flash_wipe(0xFFFFU); // never written
    timezone_init();
    settings_init();
    CHECK(!settings_stats()->loaded && brightness == TUBES_BRIGHTNESS_DEFAULT &&
          mode_set == DISPLAY_MODE_HHMM && settings_revision() == 0);
    CHECK(settings_flags() == (SML_SETTINGS_VALID | SML_SETTINGS_DEFAULTS));
    esp_link_init();

    // TEXT mode: the old firmware's messages are noise; nothing is sent.
    {
        static const uint8_t old[] = {0x83, 0xB6, 0xF0, 1, 2, 3, 4, 5, 0x83, 0xC6, 0xF7};
        receive(old, sizeof old);
        service();
        snapshot.timer.sequence = 1;
        esp_link_epoch(&snapshot, &update, now);
        collect();
        CHECK(!esp_link_is_sml() && sink == NULL && sent_count == 0);
        CHECK(esp_link_parser_stats()->noise_bytes == sizeof old);
    }

    // The first SML frame: SML mode, the console sink, and an answer.
    CHECK(command_result(SML_USER_SHOW, 7, SML_SHOW_ALL, SML_RESULT_ACK, SML_ERROR_NONE));
    CHECK(esp_link_is_sml() && sink != NULL && prints == 1);

    // Console text in STAT-DEBUG frames: a line at a time, the rest on the
    // next service.
    text_length = 0;
    sink((const uint8_t *)"hello\r\nworld", 12);
    collect();
    CHECK(sent_count == 1 && sent[0].length == 7 && text_length == 7);
    service();
    CHECK(sent_count == 1 && text_length == 12 && memcmp(text, "hello\r\nworld", 12) == 0);
    // A long line is split at the payload maximum.
    {
        char line[600];
        memset(line, 'x', sizeof line);
        text_length = 0;
        sink((const uint8_t *)line, sizeof line);
        service();
        CHECK(sent_count == 3 && sent[0].length == 240 && sent[2].length == 120);
    }

    // Every epoch: AUTH, PIC, CTRL, MEAS, GNSS, RTC, UI, in that order, and
    // STAT-SYS last; the first time, STAT-SOURCES and STAT-SETTINGS as well.
    snapshot.timer.sequence = 42;
    esp_link_epoch(&snapshot, &update, now);
    collect();
    CHECK(sent_count == 10 && sent[9].class_id == SML_CLASS_STAT &&
          sent[9].message_id == SML_STAT_SYS && sent[9].length == SML_STAT_SYS_BYTES);
    CHECK(sml_get_u16(&sent[9].payload[2]) == (SML_RESET_POR | SML_RESET_BOR) &&
          sml_get_u32(&sent[9].payload[4]) == 42 && sml_get_u32(&sent[9].payload[8]) == 0x100 &&
          sml_get_u16(&sent[9].payload[12]) == 12);
    {
        SML_STAT_SETTINGS_PAYLOAD stat;
        CHECK(sent[8].message_id == SML_STAT_SETTINGS && sent[8].length == SML_STAT_SETTINGS_BYTES);
        sml_decode(sml_message(SML_CLASS_STAT, SML_STAT_SETTINGS)->layout, sent[8].payload, &stat);
        CHECK(stat.flags == (SML_SETTINGS_VALID | SML_SETTINGS_DEFAULTS) && stat.revision == 0 &&
              stat.settings.schema_version == SML_SETTINGS_SCHEMA &&
              stat.settings.brightness == TUBES_BRIGHTNESS_DEFAULT &&
              stat.settings.flags ==
                  (SML_SETTING_TZ_AUTO | SML_SETTING_DST_AUTO | SML_SETTING_BRI_AUTO));
    }
    CHECK(sent[7].class_id == SML_CLASS_STAT && sent[7].message_id == SML_STAT_SOURCES &&
          sent[7].length == SML_STAT_SOURCES_BYTES);
    CHECK(sml_get_u32(&sent[7].payload[8]) == 42);
    CHECK(sent[0].message_id == SML_TIME_AUTH && sml_get_u32(&sent[0].payload[4]) == 0xA0A0A0A0UL);
    CHECK(sent[1].message_id == SML_TIME_PIC && sml_get_u32(&sent[1].payload[8]) == 42);
    CHECK(sent[2].message_id == SML_TIME_CTRL && sent[3].message_id == SML_TIME_MEAS);
    CHECK(sml_get_u32(&sent[3].payload[4]) == 42 && sent[4].message_id == SML_TIME_GNSS);
    CHECK(sent[5].class_id == SML_CLASS_TIME && sent[5].message_id == SML_TIME_RTC &&
          sent[5].length == SML_TIME_RTC_BYTES && sml_get_u32(&sent[5].payload[30]) == 777000);
    CHECK(sent[6].class_id == SML_CLASS_STAT && sml_get_u16(&sent[6].payload[6]) == 1234);

    // STAT-SOURCES again only on a change, or after ESP_LINK_SOURCES_EPOCHS.
    {
        uint32_t epoch;
        int sources_seen = 0;
        int sys_seen = 0;
        for (epoch = 43; epoch < 43 + ESP_LINK_SOURCES_EPOCHS; epoch++)
        {
            snapshot.timer.sequence = epoch;
            if (epoch == 45)
                sources_verdict = SML_VERDICT_NO_EDGE; // a verdict changes
            esp_link_epoch(&snapshot, &update, now);
            collect();
            for (int i = 0; i < sent_count; i++)
                if (sent[i].class_id == SML_CLASS_STAT && sent[i].message_id == SML_STAT_SOURCES)
                {
                    sources_seen++;
                    CHECK(epoch == 45 || epoch == 45 + ESP_LINK_SOURCES_EPOCHS - 0U);
                }
                else if (sent[i].class_id == SML_CLASS_STAT && sent[i].message_id == SML_STAT_SYS)
                {
                    sys_seen++;
                    CHECK(i == sent_count - 1 && sml_get_u32(&sent[i].payload[4]) == epoch);
                }
        }
        CHECK(sources_seen == 1);
        CHECK(sys_seen == (int)ESP_LINK_SOURCES_EPOCHS); // every epoch
        snapshot.timer.sequence = 45 + ESP_LINK_SOURCES_EPOCHS;
        esp_link_epoch(&snapshot, &update, now);
        collect();
        // Epoch 55: STAT-ENV too, as every fifth.
        CHECK(sent_count == 10 && sent[7].message_id == SML_STAT_SOURCES &&
              sent[8].message_id == SML_STAT_ENV && sml_get_u16(&sent[8].payload[4]) == 321 &&
              sent[9].message_id == SML_STAT_SYS);
    }

    // TZ-STATE: applied to the time zone.
    {
        SML_TZ_STATE_PAYLOAD state;
        TimezoneOffset offset;
        memset(&state, 0, sizeof state);
        state.flags = SML_TZ_VALID;
        state.source = SML_TZ_SOURCE_GEOIP;
        state.issued_utc = 100;
        state.horizon_end_utc = 0xFFFFFFFFUL;
        state.current.base_offset_s = 19800; // India
        length = sml_frame_message(SML_CLASS_TZ, SML_TZ_STATE, &state, frame);
        // Split across two services.
        receive(frame, 50);
        service();
        receive(frame + 50, (uint16_t)(length - 50));
        service();
        timezone_offset_at(1800000000UL, &offset);
        CHECK(offset.offset_s == 19800 && esp_link_stats()->tz_states == 1);
    }

    // TIME-LEAP: a +1 at a midnight is passed on; anything else withdraws.
    {
        SML_TIME_LEAP_PAYLOAD leap = {SML_LEAP_VALID | SML_LEAP_EVENT_PENDING,
                                      SML_LEAP_SOURCE_GNSS,
                                      0,
                                      37,
                                      1,
                                      0,
                                      1798761600UL,
                                      0};
        receive(frame, sml_frame_message(SML_CLASS_TIME, SML_TIME_LEAP, &leap, frame));
        service();
        CHECK(leap_calls == 1 && leap_set.change == 1 && leap_set.event_utc == 1798761600UL);
        leap.event_utc += 3600U;
        receive(frame, sml_frame_message(SML_CLASS_TIME, SML_TIME_LEAP, &leap, frame));
        service();
        CHECK(leap_calls == 2 && leap_set.change == 0);
    }

    // TIME-NTP is held, and handed to the NTP source.
    {
        SML_TIME_NTP_PAYLOAD ntp, back;
        memset(&ntp, 0, sizeof ntp);
        ntp.utc_seconds = 1800000000UL;
        ntp.round_trip_us = 12345;
        receive(frame, sml_frame_message(SML_CLASS_TIME, SML_TIME_NTP, &ntp, frame));
        service();
        CHECK(esp_link_ntp(&back) && back.round_trip_us == 12345);
        CHECK(ntp_calls == 1 && ntp_handed.utc_seconds == 1800000000UL &&
              ntp_handed.round_trip_us == 12345);
    }

    // Settings, written whole by CMD-SETTINGS as the ESP changes one at a time.
    {
        SML_SETTINGS_BODY body;
        SML_SETTINGS_BODY wrong;
        TimezoneOffset offset;

        settings_body(&body);
        body.flags &= (uint16_t)~SML_SETTING_TZ_AUTO;
        CHECK(settings_result(&body, 10, SML_RESULT_ACK, SML_ERROR_NONE) && !timezone_is_auto());
        body.base_offset_s = -18000;
        body.dst_offset_s = 3600;
        body.flags |= SML_SETTING_DST_ACTIVE;
        CHECK(settings_result(&body, 11, SML_RESULT_ACK, SML_ERROR_NONE));
        // DST still from the schedule held (India: none), base manual.
        timezone_offset_at(1800000000UL, &offset);
        CHECK(offset.offset_s == -18000 && !offset.base_automatic && offset.dst_automatic);
        body.flags &= (uint16_t)~SML_SETTING_DST_AUTO;
        CHECK(settings_result(&body, 12, SML_RESULT_ACK, SML_ERROR_NONE));
        timezone_offset_at(1800000000UL, &offset);
        CHECK(offset.offset_s == -14400 && !offset.dst_automatic);

        wrong = body;
        wrong.base_offset_s = 90000;
        CHECK(settings_result(&wrong, 14, SML_RESULT_NACK, SML_ERROR_ARGUMENT));
        // The display: every selectable mode, to the display's own; runtime-
        // only modes are not settings. 12 hours and the delta epoch passed on.
        body.selected_display = SML_DISPLAY_MMSS;
        CHECK(settings_result(&body, 15, SML_RESULT_ACK, SML_ERROR_NONE) &&
              mode_set == DISPLAY_MODE_MMSS);
        body.selected_display = SML_DISPLAY_DELTA;
        body.delta_epoch_utc = 1790000000UL;
        body.flags |= SML_SETTING_FORMAT_12H;
        CHECK(settings_result(&body, 16, SML_RESULT_ACK, SML_ERROR_NONE) &&
              mode_set == DISPLAY_MODE_DELTA && delta_set == 1790000000UL && twelve_set &&
              !delta_local_set);
        body.flags |= SML_SETTING_DELTA_LOCAL;
        CHECK(settings_result(&body, 16, SML_RESULT_ACK, SML_ERROR_NONE) && delta_local_set);
        body.flags &= (uint16_t)~SML_SETTING_DELTA_LOCAL;
        body.selected_display = SML_DISPLAY_UNCERTAINTY;
        CHECK(settings_result(&body, 17, SML_RESULT_ACK, SML_ERROR_NONE) &&
              mode_set == DISPLAY_MODE_UNCERTAINTY);
        CHECK(!alarm_on);
        body.flags |= SML_SETTING_ALARM_ENABLED;
        body.alarm_offset_s = 6U * 3600U + 45U * 60U;
        CHECK(settings_result(&body, 18, SML_RESULT_ACK, SML_ERROR_NONE) && alarm_on &&
              alarm_at == 24300U);
        body.flags &= (uint16_t)~SML_SETTING_ALARM_ENABLED;
        body.selected_display = SML_DISPLAY_HHMM;
        body.delta_epoch_utc = 0;
        body.flags &= (uint16_t)~SML_SETTING_FORMAT_12H;
        CHECK(settings_result(&body, 18, SML_RESULT_ACK, SML_ERROR_NONE) &&
              mode_set == DISPLAY_MODE_HHMM && !twelve_set);
        wrong = body;
        wrong.selected_display = SML_DISPLAY_MENU;
        CHECK(settings_result(&wrong, 18, SML_RESULT_NACK, SML_ERROR_ARGUMENT));
        wrong.selected_display = SML_DISPLAY_INIT;
        CHECK(settings_result(&wrong, 18, SML_RESULT_NACK, SML_ERROR_ARGUMENT));
        wrong.selected_display = SML_DISPLAY_DASHES;
        CHECK(settings_result(&wrong, 18, SML_RESULT_NACK, SML_ERROR_ARGUMENT));
        wrong.selected_display = SML_DISPLAY_UNCERTAINTY + 1U;
        CHECK(settings_result(&wrong, 18, SML_RESULT_NACK, SML_ERROR_ARGUMENT));
        CHECK(mode_set == DISPLAY_MODE_HHMM);

        // A level is manual brightness; auto keeps the level for when there
        // is no sensor.
        body.brightness = 3000;
        body.flags &= (uint16_t)~SML_SETTING_BRI_AUTO;
        CHECK(settings_result(&body, 19, SML_RESULT_ACK, SML_ERROR_NONE) && brightness == 3000 &&
              !brightness_auto);
        wrong = body;
        wrong.brightness = 4001;
        CHECK(settings_result(&wrong, 20, SML_RESULT_NACK, SML_ERROR_ARGUMENT));
        body.flags |= SML_SETTING_BRI_AUTO;
        CHECK(settings_result(&body, 41, SML_RESULT_ACK, SML_ERROR_NONE) && brightness_auto &&
              brightness == 3000);
        body.flags &= (uint16_t)~SML_SETTING_BRI_AUTO;
        body.brightness = 0;
        CHECK(settings_result(&body, 44, SML_RESULT_ACK, SML_ERROR_NONE) && brightness == 0);
        body.brightness = 3000;
        CHECK(settings_result(&body, 45, SML_RESULT_ACK, SML_ERROR_NONE));

        // Beeps: the chimes and clicks on and off.
        CHECK(!beeps_on);
        body.flags |= SML_SETTING_BEEP_ENABLED;
        CHECK(settings_result(&body, 46, SML_RESULT_ACK, SML_ERROR_NONE) && beeps_on);
        body.flags &= (uint16_t)~SML_SETTING_BEEP_ENABLED;
        CHECK(settings_result(&body, 48, SML_RESULT_ACK, SML_ERROR_NONE) && !beeps_on);
    }
    // ESP command codes are not for the PIC; nor are the old setting codes
    // anything now.
    CHECK(command_result(SML_USER_ESP_WIFI_FORGET, 31, 0, SML_RESULT_NACK, SML_ERROR_UNSUPPORTED));
    CHECK(command_result(0x0062, 31, 3000, SML_RESULT_NACK, SML_ERROR_UNSUPPORTED));
    // ENTER_BOOTLOADER: not without the IPL's mailbox, nor without an SPL to
    // stay in (the reset that answers it is tested at the end).
    boot_init(0);
    spl_usable = true;
    CHECK(command_result(SML_USER_ENTER_BOOTLOADER, 21, 0, SML_RESULT_NACK, SML_ERROR_STATE));
    boot_host_mailbox.magic = MAILBOX_MAGIC_VALUE;
    boot_host_mailbox.magic_inverse = (uint16_t)~MAILBOX_MAGIC_VALUE;
    boot_init(0);
    spl_usable = false;
    CHECK(command_result(SML_USER_ENTER_BOOTLOADER, 21, 0, SML_RESULT_NACK, SML_ERROR_STATE));
    CHECK(boot_host_mailbox.request == 0);
    CHECK(command_result(SML_USER_RESYNC, 21, 0, SML_RESULT_ACK, SML_ERROR_NONE) && resyncs == 1);
    // SET_RTC: needs an RTC, a time from 2000, and no request in progress.
    rtc_status.present = false;
    CHECK(command_result(SML_USER_SET_RTC, 22, 1800000000UL, SML_RESULT_NACK, SML_ERROR_STATE));
    rtc_status.present = true;
    CHECK(command_result(SML_USER_SET_RTC, 23, PCF8563_FIRST_UTC - 1UL, SML_RESULT_NACK,
                         SML_ERROR_ARGUMENT));
    rtc_busy = true;
    CHECK(command_result(SML_USER_SET_RTC, 24, 1800000000UL, SML_RESULT_NACK, SML_ERROR_BUSY));
    rtc_busy = false;
    CHECK(command_result(SML_USER_SET_RTC, 25, 1800000000UL, SML_RESULT_ACK, SML_ERROR_NONE) &&
          rtc_written == 1800000000UL);
    CHECK(command_result(0x7777, 22, 0, SML_RESULT_NACK, SML_ERROR_UNSUPPORTED));

    // REQUEST_MESSAGE: the message, then the ACK.
    CHECK(command_result(SML_USER_REQUEST_MESSAGE, 23, SML_CLASS_TIME << 8 | SML_TIME_GNSS,
                         SML_RESULT_ACK, SML_ERROR_NONE));
    CHECK(sent_count == 2 && sent[0].message_id == SML_TIME_GNSS);
    CHECK(command_result(SML_USER_REQUEST_MESSAGE, 23, SML_CLASS_STAT << 8 | SML_STAT_SYS,
                         SML_RESULT_ACK, SML_ERROR_NONE));
    CHECK(sent_count == 2 && sent[0].message_id == SML_STAT_SYS);
    CHECK(command_result(SML_USER_REQUEST_MESSAGE, 24, SML_CLASS_STAT << 8 | SML_STAT_ENV,
                         SML_RESULT_ACK, SML_ERROR_NONE) &&
          sent[0].message_id == SML_STAT_ENV);
    CHECK(command_result(SML_USER_REQUEST_MESSAGE, 24, SML_CLASS_STAT << 8 | SML_STAT_SUPPLY,
                         SML_RESULT_NACK, SML_ERROR_UNSUPPORTED));

    // The sources table: STAT-SOURCES, then the ACK.
    CHECK(command_result(SML_USER_REQUEST_MESSAGE, 33, SML_CLASS_STAT << 8 | SML_STAT_SOURCES,
                         SML_RESULT_ACK, SML_ERROR_NONE));
    CHECK(sent_count == 2 && sent[0].message_id == SML_STAT_SOURCES &&
          sent[1].message_id == SML_CMD_RESPONSE);

    // SET_DEBUG: clear << 16 | set, and the result printed whatever it is.
    debug_set(DEBUG_ALL);
    text_length = 0;
    CHECK(command_result(SML_USER_SET_DEBUG, 34, (uint32_t)DEBUG_ALL << 16 | DEBUG_SYNC,
                         SML_RESULT_ACK, SML_ERROR_NONE));
    service();
    CHECK(debug_mask() == DEBUG_SYNC);
    text[text_length] = 0;
    CHECK(strstr(text, "PIC debug: SYNC\r\n") != NULL);
    CHECK(command_result(SML_USER_SET_DEBUG, 35, DEBUG_TUNING | DEBUG_NTP, SML_RESULT_ACK,
                         SML_ERROR_NONE) &&
          debug_mask() == (DEBUG_SYNC | DEBUG_TUNING | DEBUG_NTP));
    CHECK(command_result(SML_USER_SET_DEBUG, 36, (uint32_t)DEBUG_SYNC << 16, SML_RESULT_ACK,
                         SML_ERROR_NONE) &&
          debug_mask() == (DEBUG_TUNING | DEBUG_NTP));
    text_length = 0;
    CHECK(command_result(SML_USER_SET_DEBUG, 37, 0, SML_RESULT_ACK, SML_ERROR_NONE) &&
          debug_mask() == (DEBUG_TUNING | DEBUG_NTP));
    service();
    text[text_length] = 0;
    CHECK(strstr(text, "PIC debug: NTP TUNING\r\n") != NULL);
    CHECK(command_result(SML_USER_SET_DEBUG, 38, 0x0200, SML_RESULT_NACK, SML_ERROR_ARGUMENT) &&
          debug_mask() == (DEBUG_TUNING | DEBUG_NTP));
    text_length = 0;
    CHECK(command_result(SML_USER_SET_DEBUG, 41, 0xFFFF0000UL | DEBUG_RTC | DEBUG_EVENTS,
                         SML_RESULT_ACK, SML_ERROR_NONE) &&
          debug_mask() == (DEBUG_RTC | DEBUG_EVENTS));
    service();
    text[text_length] = 0;
    CHECK(strstr(text, "PIC debug: RTC EVENTS\r\n") != NULL);
    // As the ESP sends them: "debug timing" (clear everything, set 1), then
    // "debug -all" (clear 0xFFFF).
    CHECK(command_result(SML_USER_SET_DEBUG, 40, 0xFFFF0001UL, SML_RESULT_ACK, SML_ERROR_NONE) &&
          debug_mask() == DEBUG_TIMING);
    text_length = 0;
    CHECK(command_result(SML_USER_SET_DEBUG, 39, 0xFFFF0000UL, SML_RESULT_ACK, SML_ERROR_NONE) &&
          debug_mask() == DEBUG_NONE);
    service();
    text[text_length] = 0;
    CHECK(strstr(text, "PIC debug: NONE\r\n") != NULL);

    // Wrong length: NACK LENGTH with the token; wrong direction and unknown
    // messages are counted, not answered.
    {
        uint8_t payload[8] = {0x00, 0x01, 0x00, 0x00, 0x00, 25, 0, 0};
        SML_CMD_RESPONSE_PAYLOAD r;
        receive(frame, sml_frame(SML_CLASS_CMD, SML_CMD_USER, payload, 8, frame));
        service();
        CHECK(response(&r) && r.status == SML_RESULT_NACK && r.detail == SML_ERROR_LENGTH &&
              r.token == 25);
        receive(frame, sml_frame(SML_CLASS_TIME, SML_TIME_AUTH, payload, 8, frame));
        receive(frame, sml_frame(0x55, 0x01, payload, 8, frame));
        service();
        CHECK(sent_count == 0 && esp_link_stats()->wrong_direction == 1 &&
              esp_link_stats()->unknown == 1 && esp_link_stats()->wrong_length == 1);
    }

    test_settings(&snapshot, &update);

    // A partial frame, then silence: dropped, and the link carries on.
    length = sml_frame_message(SML_CLASS_TIME, SML_TIME_NTP, &(SML_TIME_NTP_PAYLOAD){0}, frame);
    receive(frame, 20);
    for (int i = 0; i < 12; i++)
        service();
    CHECK(esp_link_parser_stats()->timeouts == 1);
    CHECK(command_result(SML_USER_SHOW, 27, SML_SHOW_ALL, SML_RESULT_ACK, SML_ERROR_NONE));

    // STAT-NET is held, and goes stale after 30 s.
    {
        SML_STAT_NET_PAYLOAD stat = {SML_NET_WIFI_CONNECTED | SML_NET_IP_VALID,
                                     SML_WIFI_CONNECTED,
                                     -61,
                                     0xC0A80117UL,
                                     3600,
                                     0,
                                     0,
                                     0x010200};
        SML_STAT_NET_PAYLOAD back;
        receive(frame, sml_frame_message(SML_CLASS_STAT, SML_STAT_NET, &stat, frame));
        service();
        CHECK(esp_link_net(now, &back) && back.rssi_dbm == -61 && back.ipv4 == 0xC0A80117UL);
        CHECK(!esp_link_net(now + 30001U, &back));
    }

    // A command for the ESP, and its answer.
    {
        SML_CMD_USER_PAYLOAD request;
        SML_CMD_RESPONSE_PAYLOAD answer = {SML_RESULT_ACK, SML_ERROR_NONE, 0, 0, 0}, back;
        uint16_t token = esp_link_request(SML_USER_ESP_WIFI_SETUP, 0);
        collect();
        CHECK(token != 0 && sent_count == 1 && sent[0].message_id == SML_CMD_USER);
        sml_decode(sml_message(SML_CLASS_CMD, SML_CMD_USER)->layout, sent[0].payload, &request);
        CHECK(request.command == SML_USER_ESP_WIFI_SETUP && request.token == token);
        answer.token = token;
        answer.command = SML_USER_ESP_WIFI_SETUP;
        receive(frame, sml_frame_message(SML_CLASS_CMD, SML_CMD_RESPONSE, &answer, frame));
        service();
        CHECK(sent_count == 0 && esp_link_response(&back) && back.token == token);
    }

    // RESET: answered first, then the reset.
    if (setjmp(reset_jump) == 0)
    {
        send_command(SML_USER_RESET, 28, 0);
        esp_link_service(now);
        CHECK(0); // not reached
    }
    collect();
    {
        SML_CMD_RESPONSE_PAYLOAD r;
        CHECK(resets == 1 && response(&r) && r.token == 28 && r.status == SML_RESULT_ACK);
    }

    // ENTER_BOOTLOADER: answered first, the request left for the SPL, then
    // the reset.
    spl_usable = true;
    if (setjmp(reset_jump) == 0)
    {
        send_command(SML_USER_ENTER_BOOTLOADER, 29, 0);
        esp_link_service(now);
        CHECK(0); // not reached
    }
    collect();
    {
        SML_CMD_RESPONSE_PAYLOAD r;
        CHECK(resets == 2 && response(&r) && r.token == 29 && r.status == SML_RESULT_ACK);
        CHECK(boot_host_mailbox.request == MAILBOX_REQUEST_STAY);
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
