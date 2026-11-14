/*
 * The SML payloads built from the clock's state (esp/esp_report.c): the
 * frozen wire mappings, unit conversions and validity flags, against
 * hand-made sync, control loop and GNSS states.
 */
#include <stdio.h>
#include <string.h>

#include <xc.h>
#include "../../drivers/pcf8563.h"
#include "../../drivers/tubes.h"
#include "../../drivers/veml6040.h"
#include "../../menu/menu.h"
#include "../../ui/alarm.h"
#include "../../ui/brightness.h"
#include "../../ui/controls.h"
#include "../../../common/boot_layout.h"
#include "../../drivers/boot_image.h"
#include "../../esp/esp_report.h"
#include "../../platform/boot.h"
#include "../../platform/fault.h"
#include "../../gnss/gnss.h"
#include "../../platform/version.h"
#include "../../sync/source_rtc.h"
#include "../../timing/timing_config.h"
#include "../../../common/sml.h"

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

volatile unsigned _RA0 = 1, _RA7 = 0; // (pins: the report reads ui/controls.h now)

// The controls: switch off, button down, presses as the test sets them.
static uint32_t press_count;
static ControlsPress last_press = CONTROLS_NONE;
ControlsPress controls_press(uint32_t *count)
{
    *count = press_count;
    return last_press;
}


bool controls_switch_on(void)
{
    return false;
}


// Which module reports were printed, in order (esp_report_show).
static char printed[200];
static void note(const char *name)
{
    strncat(printed, name, sizeof(printed) - strlen(printed) - 2U);
    strcat(printed, " ");
}


bool controls_button_down(void)
{
    return true;
}


void controls_print(bool verbose)
{
    (void)verbose;
    note("controls");
}


// ---- Stand-ins ------------------------------------------------------------
static SyncStatus status;
static SyncTransition transition;
static bool has_transition;
static EpochControl control;
static GnssState gnss;
static GnssEpoch epoch;
static bool has_epoch;
static bool has_uncertainty;
static uint32_t uncertainty;

const SyncStatus *sync_status(void)
{
    return &status;
}


const SyncTransition *sync_last_transition(void)
{
    return has_transition ? &transition : NULL;
}


bool sync_uncertainty(uint32_t *ns)
{
    *ns = uncertainty;
    return has_uncertainty;
}


// Candidates: the bounds and ranks the test sets.
static uint32_t bounds[SYNC_CANDIDATE_HOLDOVER + 1];
static uint8_t ranks[SYNC_CANDIDATE_HOLDOVER + 1];
static bool synchronised;

uint32_t sync_candidate_bound(uint8_t candidate)
{
    return candidate <= SYNC_CANDIDATE_HOLDOVER ? bounds[candidate] : SYNC_BOUND_NONE;
}


bool sync_source_steers(uint8_t source)
{
    return source < SYNC_SOURCE_COUNT && source != SYNC_SOURCE_RTC;
}


// The RTC: the driver's status and the source's, as the test sets them.
static Pcf8563Status rtc_status;
static SourceRtcState rtc_source;
static bool rtc_time;
static uint32_t rtc_reckoning = 1792326000UL;
static uint32_t rtc_bound = 12345678UL;

const Pcf8563Status *pcf8563_status(void)
{
    return &rtc_status;
}


const SourceRtcState *source_rtc_state(void)
{
    return &rtc_source;
}


bool source_rtc_reckoning(uint32_t *utc_seconds)
{
    *utc_seconds = rtc_reckoning;
    return rtc_time;
}


bool source_rtc_bound(uint32_t *bound_ns)
{
    *bound_ns = rtc_bound;
    return rtc_time;
}


void source_rtc_print(bool verbose)
{
    (void)verbose;
    note("rtc-source");
}


uint8_t sync_candidate_rank(uint8_t candidate)
{
    return candidate <= SYNC_CANDIDATE_HOLDOVER ? ranks[candidate] : 0U;
}


bool sync_is_synchronised(void)
{
    return synchronised;
}


const EpochControl *epoch_control_state(void)
{
    return &control;
}


const GnssState *gnss_state(void)
{
    return &gnss;
}


const GnssEpoch *gnss_epoch_latest(const GnssEpochAssembler *a)
{
    (void)a;
    return has_epoch ? &epoch : NULL;
}


bool gnss_leap_seconds(uint32_t now_ms, int8_t *gps_minus_utc_s)
{
    (void)now_ms;
    *gps_minus_utc_s = 18;
    return true;
}


static UbxTimTp pulse;
static bool has_pulse, has_mark;
static int32_t mark_ns;
bool gnss_time_pulse(uint32_t pps_sequence, const UbxTimTp **time_pulse)
{
    (void)pps_sequence;
    *time_pulse = &pulse;
    return has_pulse;
}


bool gnss_mark_offset(uint32_t now_ms, int32_t *offset_ns, uint32_t *accuracy_ns)
{
    (void)now_ms;
    *offset_ns = mark_ns;
    *accuracy_ns = 7;
    return has_mark;
}


bool gnss_leap_event(uint32_t now_ms, UtcLeap *leap)
{
    (void)now_ms;
    leap->event_utc = 1798761600UL;
    leap->change = 1;
    return true;
}


uint16_t tubes_brightness(void)
{
    return 2000;
}


// Fading to 2500, automatically.
static bool bright_auto = true;
uint16_t brightness_target(void)
{
    return 2500;
}


bool brightness_is_auto(void)
{
    return bright_auto;
}


void brightness_print(bool verbose)
{
    (void)verbose;
    note("brightness");
}


// The light sensor, as the test sets it.
static Veml6040Status light;
const Veml6040Status *veml6040_status(void)
{
    return &light;
}


void veml6040_print(uint32_t now_ms, bool verbose)
{
    (void)verbose;
    (void)now_ms;
    note("light");
}


DisplayMode display_mode(void)
{
    return DISPLAY_MODE_HHMM;
}


static bool menu_shown;
bool menu_is_open(void)
{
    return menu_shown;
}


uint8_t menu_state(void)
{
    return menu_shown ? (uint8_t)(SML_MENU_DISPLAY | 2U | SML_MENU_EDITING) : SML_MENU_NONE;
}


bool alarm_enabled(void)
{
    return true;
}


AlarmState alarm_state(void)
{
    return ALARM_SOUNDING;
}


uint32_t alarm_time(void)
{
    return 27000U;
}


void alarm_print(bool verbose)
{
    (void)verbose;
    note("alarm");
}


DisplayMode display_selected(void)
{
    return DISPLAY_MODE_HHMM;
}


bool display_is_12h(void)
{
    return true;
}


bool display_delta_is_local(void)
{
    return true;
}


void display_print_epoch(uint32_t seconds, bool local)
{
    (void)seconds;
    (void)local;
}


uint32_t display_delta_epoch(void)
{
    return 1800000000UL;
}


void sync_report_status(bool verbose)
{
    (void)verbose;
}


void gnss_print(uint32_t now_ms, bool verbose)
{
    (void)verbose;
    (void)now_ms;
    note("gnss");
}


void display_print(void)
{
    note("display");
}


void pcf8563_print(uint32_t now_ms, Pcf8563Reference reference, bool verbose)
{
    (void)verbose;
    (void)now_ms;
    (void)reference;
    note("pcf8563");
}


void settings_print(bool verbose)
{
    (void)verbose;
    note("settings");
}


void settings_print_stored(bool verbose)
{
    (void)verbose;
    note("stored");
}


void settings_print_oscillator(bool verbose)
{
    (void)verbose;
    note("oscillator");
}


void settings_body(SML_SETTINGS_BODY *body)
{
    memset(body, 0, sizeof(*body));
    body->delta_epoch_utc = 1800000000UL;
}


const char *display_mode_name(DisplayMode mode)
{
    (void)mode;
    return "HHMM";
}


const I2c1Stats *i2c1_stats(void)
{
    static I2c1Stats stats;
    return &stats;
}


void i2c1_print(void)
{
}


// STAT-SYS's sources: RCON at start-up, the scheduler's longest pass.
static uint16_t rcon = 0x0003; // POR and BOR, as at power-on
static uint16_t longest = 37;
uint16_t system_reset_cause(void)
{
    return rcon;
}


static bool clock_fallback;
bool system_clock_fallback(void)
{
    return clock_fallback;
}


uint16_t scheduler_take_longest_pass_ms(void)
{
    uint16_t value = longest;
    longest = 0;
    return value;
}


uint32_t scheduler_epoch(void)
{
    return 1234;
}


// The boot stages, as boot_image.c would read them from flash.
void boot_image_ipl(BootIpl *ipl)
{
    ipl->present = true;
    ipl->version = 0x000100UL;
    ipl->services = 1;
    ipl->build_time = 0;
}


void boot_image_spl(BootSpl *spl, bool check_crc)
{
    spl->state = SML_SPL_STATE_RECORD | SML_SPL_STATE_COMMITTED | SML_SPL_STATE_CONFIRMED |
                 (check_crc ? SML_SPL_STATE_INTACT : 0U);
    spl->version = 0x000102UL;
    spl->length = 1700;
    spl->crc32 = 0;
    spl->build_time = 0;
}


void boot_image_app(BootApp *app)
{
    app->committed = true;
    app->version = 0x000400UL;
    app->length = 58000;
    app->crc32 = 0;
    app->build_time = 0;
}


extern volatile BootMailbox boot_host_mailbox; // platform/boot.c on the host

// pic -v's boot lines, into a file, read back.
static void test_boot_report(void)
{
    char text[512] = {0};
    FILE *saved = stdout;
    FILE *out = tmpfile();

    boot_host_mailbox.magic = MAILBOX_MAGIC_VALUE;
    boot_host_mailbox.magic_inverse = (uint16_t)~MAILBOX_MAGIC_VALUE;
    boot_host_mailbox.boot = BOOT_PATH_APP_DIRECT | BOOT_PATH_NO_SPL;
    boot_host_mailbox.reset_cause = SML_RESET_EXTR;
    CHECK(boot_init(0) == SML_RESET_EXTR);
    stdout = out;
    esp_report_print_system(true);
    fflush(out);
    stdout = saved;
    rewind(out);
    CHECK(fread(text, 1, sizeof text - 1, out) > 0);
    fclose(out);
    CHECK(strstr(text, "Boot: IPL 0.1.0, SPL 0.1.2 committed, intact, confirmed, application "
                       "record 0.4.0, 58000 instructions\n") != NULL);
    CHECK(strstr(text,
                 "Boot: started by the IPL directly, no SPL; trials 0, attempts 0, healthy 0 s") !=
          NULL);
    printf("%s", text);

    // Through the SPL: no path bits.
    memset(text, 0, sizeof(text));
    out = tmpfile();
    boot_host_mailbox.boot = 0;
    boot_host_mailbox.attempts = 1;
    (void)boot_init(0);
    stdout = out;
    esp_report_print_system(true);
    fflush(out);
    stdout = saved;
    rewind(out);
    CHECK(fread(text, 1, sizeof text - 1, out) > 0);
    fclose(out);
    CHECK(strstr(text, "Boot: started by the SPL; trials 0, attempts 1, healthy 0 s") != NULL);
}


int main(void)
{
    SML_TIME_AUTH_PAYLOAD auth;
    SML_TIME_PIC_PAYLOAD pic;
    SML_TIME_CTRL_PAYLOAD ctrl;
    SML_TIME_MEAS_PAYLOAD meas;
    SML_TIME_GNSS_PAYLOAD gnss_payload;
    SML_STAT_UI_PAYLOAD ui;
    TimingSnapshot snapshot;
    SyncUpdate update;

    // Mappings: zero is "not yet", and every firmware value has a code.
    CHECK(esp_report_lock(SYNC_LOCKED, 0) == SML_LOCK_UNINITIALISED);
    CHECK(esp_report_lock(SYNC_FREE_RUN, 1) == SML_LOCK_FREE_RUNNING);
    CHECK(esp_report_lock(SYNC_ACQUIRE, 1) == SML_LOCK_ACQUIRING);
    CHECK(esp_report_lock(SYNC_LOCKED, 1) == SML_LOCK_LOCKED);
    CHECK(esp_report_lock(SYNC_HOLDOVER, 1) == SML_LOCK_HOLDOVER);
    CHECK(esp_report_sync(SYNC_HOLDOVER, 1) == SML_SYNC_HOLDOVER);
    CHECK(esp_report_sync(SYNC_FREE_RUN, 0) == SML_SYNC_INIT);
    CHECK(esp_report_cause(SYNC_REASON_QUALIFIED) == SML_CAUSE_QUALIFIED);
    CHECK(esp_report_cause(SYNC_REASON_EXPIRED) == SML_CAUSE_EXPIRED);
    CHECK(esp_report_cause(200) == SML_CAUSE_NONE);
    CHECK(esp_report_source(SYNC_SOURCE_GNSS, 1) == SML_SOURCE_GNSS);
    CHECK(esp_report_source(SYNC_SOURCE_NTP, 1) == SML_SOURCE_NTP);
    CHECK(esp_report_source(SYNC_SOURCE_NONE, 1) == SML_SOURCE_NONE);
    CHECK(esp_report_source(SYNC_SOURCE_GNSS, 0) == SML_SOURCE_INIT);
    CHECK(esp_report_control(EPOCH_STATE_ACQUIRE) == SML_CONTROL_ACQUIRE);
    CHECK(esp_report_control(EPOCH_STATE_STEP_ACTIVE) == SML_CONTROL_STEP);
    CHECK(esp_report_control(EPOCH_STATE_TRACK) == SML_CONTROL_TRACK);
    CHECK(esp_report_display(DISPLAY_MODE_HHMM, true) == SML_DISPLAY_HHMM);
    CHECK(esp_report_display(DISPLAY_MODE_HHMM, false) == SML_DISPLAY_DASHES);
    CHECK(esp_report_display(DISPLAY_MODE_MMSS, true) == SML_DISPLAY_MMSS);
    CHECK(esp_report_display(DISPLAY_MODE_SSMM, true) == SML_DISPLAY_SSMM);
    CHECK(esp_report_display(DISPLAY_MODE_YYYY, true) == SML_DISPLAY_YYYY);
    CHECK(esp_report_display(DISPLAY_MODE_MMDD, true) == SML_DISPLAY_MMDD);
    CHECK(esp_report_display(DISPLAY_MODE_DELTA, true) == SML_DISPLAY_DELTA);
    CHECK(esp_report_display(DISPLAY_MODE_UNCERTAINTY, true) == SML_DISPLAY_UNCERTAINTY);
    CHECK(esp_report_display(DISPLAY_MODE_COUNT, true) == SML_DISPLAY_INIT);

    // TIME-AUTH, before anything: INIT, uninitialised, nothing valid.
    status.source = SYNC_SOURCE_NONE; // as sync_init() leaves it
    esp_report_time_auth(&auth);
    CHECK(auth.flags == 0 && auth.source == SML_SOURCE_INIT &&
          auth.lock_state == SML_LOCK_UNINITIALISED);
    // Locked to GNSS, 150 ns late, +/-75 ns.
    status.epochs = 100;
    status.state = SYNC_LOCKED;
    status.source = SYNC_SOURCE_GNSS;
    status.time.known = true;
    status.time.utc_seconds = 1792326896UL;
    status.phase_valid = true;
    status.phase_ticks = -6;
    has_uncertainty = true;
    uncertainty = 75;
    esp_report_time_auth(&auth);
    CHECK(auth.flags == (SML_AUTH_UTC_VALID | SML_AUTH_UNCERTAINTY_VALID | SML_AUTH_SOURCE_VALID));
    CHECK(auth.source == SML_SOURCE_GNSS && auth.lock_state == SML_LOCK_LOCKED);
    CHECK(auth.utc_seconds == 1792326896UL && auth.utc_phase_ns == -150 &&
          auth.uncertainty_ns == 75);
    // Free running on the count: UTC still valid, no bound, no source.
    status.state = SYNC_FREE_RUN;
    status.source = SYNC_SOURCE_NONE;
    has_uncertainty = false;
    esp_report_time_auth(&auth);
    CHECK(auth.flags == SML_AUTH_UTC_VALID && auth.source == SML_SOURCE_NONE &&
          auth.lock_state == SML_LOCK_FREE_RUNNING);
    // Free running on the RTC's time: its bound, and it is named.
    status.time.source = SYNC_SOURCE_RTC;
    has_uncertainty = true;
    uncertainty = 999000000UL;
    esp_report_time_auth(&auth);
    CHECK(auth.flags == (SML_AUTH_UTC_VALID | SML_AUTH_UNCERTAINTY_VALID) &&
          auth.source == SML_SOURCE_RTC && auth.lock_state == SML_LOCK_FREE_RUNNING &&
          auth.uncertainty_ns == 999000000UL);
    status.time.source = SYNC_SOURCE_GNSS;
    has_uncertainty = false;

    // TIME-PIC.
    memset(&snapshot, 0, sizeof snapshot);
    memset(&update, 0, sizeof update);
    snapshot.timer.sequence = 4242;
    snapshot.capture.reference_period_valid = true;
    snapshot.capture.reference_period = 39999936UL;
    snapshot.capture.reference_capture_valid = true;
    update.loop.phase_valid = true;
    update.loop.utc_offset_ticks = 3;
    control.pps_kf.initialized = true;
    control.filtered_pps_period = 39999936UL;
    control.commanded_period = TIMING_NOMINAL_TICKS - 64UL;
    control.holdover_epochs = 2;
    status.state = SYNC_LOCKED;
    status.source = SYNC_SOURCE_GNSS;
    status.transitions = 3;
    status.sources[SYNC_SOURCE_GNSS].verdicts[SYNC_CHECK_NO_EDGE] = 4;
    status.second = SYNC_CANDIDATE_HOLDOVER;
    transition.from = SYNC_ACQUIRE;
    transition.to = SYNC_LOCKED;
    transition.reason = SYNC_REASON_TRACKING;
    has_transition = true;
    esp_report_time_pic(&snapshot, &update, &pic);
    CHECK(pic.flags == (SML_PIC_PHASE_VALID | SML_PIC_PERIOD_VALID | SML_PIC_FREQUENCY_VALID |
                        SML_PIC_PPS_VALID));
    CHECK(pic.sync_state == SML_SYNC_LOCKED && pic.previous_sync_state == SML_SYNC_ACQUIRE &&
          pic.last_sync_cause == SML_CAUSE_TRACKING);
    CHECK(pic.epoch_sequence == 4242 && pic.fosc_hz == 39999936UL && pic.phase_cycles == 3);
    CHECK(pic.applied_adjustment_cycles == -64 && pic.missing_pps_events == 4 &&
          pic.sync_events == 3 && pic.reference_age_ms == 2000);
    CHECK(pic.second_source == SML_SOURCE_HOLDOVER);
    status.second = SYNC_SOURCE_NONE;
    esp_report_time_pic(&snapshot, &update, &pic);
    CHECK(pic.second_source == SML_SOURCE_NONE);

    // Verdicts and candidates on the wire.
    CHECK(esp_report_verdict(SYNC_CHECK_ABSENT) == SML_VERDICT_ABSENT);
    CHECK(esp_report_verdict(SYNC_CHECK_NO_TIME) == SML_VERDICT_NO_TIME);
    CHECK(esp_report_verdict(SYNC_CHECK_NO_EDGE) == SML_VERDICT_NO_EDGE);
    CHECK(esp_report_verdict(SYNC_CHECK_UNLABELLED) == SML_VERDICT_UNLABELLED);
    CHECK(esp_report_verdict(SYNC_CHECK_INACCURATE) == SML_VERDICT_INACCURATE);
    CHECK(esp_report_verdict(SYNC_CHECK_INCONSISTENT) == SML_VERDICT_INCONSISTENT);
    CHECK(esp_report_verdict(SYNC_CHECK_GOOD) == SML_VERDICT_GOOD);
    CHECK(esp_report_candidate(SYNC_SOURCE_NTP) == SML_SOURCE_NTP);
    CHECK(esp_report_candidate(SYNC_CANDIDATE_HOLDOVER) == SML_SOURCE_HOLDOVER);
    CHECK(esp_report_candidate(SYNC_SOURCE_NONE) == SML_SOURCE_NONE);

    // STAT-SOURCES: GNSS lost, holding over, NTP challenging.
    {
        SML_STAT_SOURCES_PAYLOAD sources;
        SyncSourceTracker *gnss_tracker = &status.sources[SYNC_SOURCE_GNSS];
        SyncSourceTracker *ntp_tracker = &status.sources[SYNC_SOURCE_NTP];
        uint8_t wire[SML_STAT_SOURCES_BYTES];

        status.state = SYNC_HOLDOVER;
        status.source = SYNC_SOURCE_GNSS;
        status.epochs = 5000;
        status.second = SYNC_SOURCE_NTP;
        status.challenger = SYNC_SOURCE_NTP;
        status.challenge_run = 4;
        status.fallback = false;
        synchronised = true;
        gnss_tracker->check = SYNC_CHECK_ABSENT;
        gnss_tracker->last_good_epoch = 4880;
        gnss_tracker->verdicts[SYNC_CHECK_GOOD] = 4000;
        gnss_tracker->report.present = false;
        ntp_tracker->check = SYNC_CHECK_GOOD;
        ntp_tracker->good_run = 900;
        ntp_tracker->usable_run = 950;
        ntp_tracker->last_good_epoch = 5000;
        ntp_tracker->verdicts[SYNC_CHECK_GOOD] = 900;
        ntp_tracker->report.present = true;
        ntp_tracker->report.time_valid = true;
        ntp_tracker->report.edge = true;
        ntp_tracker->report.labelled = true;
        ntp_tracker->report.uncertainty_ns = 11000000UL;
        ntp_tracker->phase_valid = true;
        ntp_tracker->phase_ticks = -4000;
        // The RTC: GOOD, never a candidate, its bound shown all the same.
        status.sources[SYNC_SOURCE_RTC].check = SYNC_CHECK_GOOD;
        status.sources[SYNC_SOURCE_RTC].bound_ns = 999000000UL;
        status.sources[SYNC_SOURCE_RTC].good_run = 400;
        status.sources[SYNC_SOURCE_RTC].verdicts[SYNC_CHECK_GOOD] = 400;
        status.sources[SYNC_SOURCE_RTC].last_good_epoch = 5000;
        status.sources[SYNC_SOURCE_RTC].report.present = true;
        status.sources[SYNC_SOURCE_RTC].report.time_valid = true;
        status.sources[SYNC_SOURCE_RTC].report.edge = true;
        status.sources[SYNC_SOURCE_RTC].report.labelled = true;
        status.sources[SYNC_SOURCE_RTC].report.uncertainty_ns = 999000000UL;
        bounds[SYNC_SOURCE_RTC] = SYNC_BOUND_NONE;
        ranks[SYNC_SOURCE_RTC] = 0;
        status.holdover.ready = true;
        status.holdover.age = 120;
        status.holdover.base_ns = 115;
        bounds[SYNC_SOURCE_GNSS] = SYNC_BOUND_NONE;
        bounds[SYNC_SOURCE_NTP] = 11020000UL;
        bounds[SYNC_CANDIDATE_HOLDOVER] = 4951UL;
        ranks[SYNC_SOURCE_GNSS] = 0;
        ranks[SYNC_SOURCE_NTP] = 2;
        ranks[SYNC_CANDIDATE_HOLDOVER] = 1;
        esp_report_stat_sources(5001, &sources);
        CHECK(sources.flags == SML_SOURCES_SYNCHRONISED && sources.sync_state == SML_SYNC_HOLDOVER);
        CHECK(sources.selected == SML_SOURCE_HOLDOVER && sources.second == SML_SOURCE_NTP &&
              sources.challenger == SML_SOURCE_NTP && sources.challenge_run == 4 &&
              sources.epoch_sequence == 5001);
        CHECK(sources.entries[0].source == SML_SOURCE_GNSS &&
              sources.entries[0].verdict == SML_VERDICT_ABSENT && sources.entries[0].rank == 0 &&
              sources.entries[0].flags == 0 && sources.entries[0].age_s == 121);
        CHECK(sources.entries[1].source == SML_SOURCE_NTP &&
              sources.entries[1].verdict == SML_VERDICT_GOOD && sources.entries[1].rank == 2);
        CHECK(sources.entries[1].flags ==
              (SML_ENTRY_PRESENT | SML_ENTRY_TIME_VALID | SML_ENTRY_EDGE | SML_ENTRY_LABELLED |
               SML_ENTRY_PHASE_VALID | SML_ENTRY_BOUND_VALID));
        CHECK(sources.entries[1].bound_ns == 11020000UL &&
              sources.entries[1].uncertainty_ns == 11000000UL &&
              sources.entries[1].phase_ns == -100000 && sources.entries[1].good_run == 900 &&
              sources.entries[1].usable_run == 950 && sources.entries[1].age_s == 1);
        CHECK(sources.entries[2].source == SML_SOURCE_RTC &&
              sources.entries[2].verdict == SML_VERDICT_GOOD && sources.entries[2].rank == 0 &&
              sources.entries[2].bound_ns == 999000000UL &&
              (sources.entries[2].flags & SML_ENTRY_BOUND_VALID) != 0 &&
              sources.entries[2].age_s == 1);
        CHECK(sources.entries[3].source == SML_SOURCE_HOLDOVER &&
              sources.entries[3].verdict == SML_VERDICT_GOOD && sources.entries[3].rank == 1 &&
              sources.entries[3].bound_ns == 4951UL && sources.entries[3].uncertainty_ns == 115 &&
              sources.entries[3].age_s == 120);
        // On the wire: 108 bytes, entry k at 12 + 24k.
        CHECK(sml_encode(sml_message(SML_CLASS_STAT, SML_STAT_SOURCES)->layout, &sources, wire) ==
              SML_STAT_SOURCES_BYTES);
        CHECK(wire[3] == SML_SOURCE_HOLDOVER && wire[12 + 24] == SML_SOURCE_NTP &&
              sml_get_u32(&wire[12 + 24 + 8]) == 11020000UL &&
              (int32_t)sml_get_u32(&wire[12 + 24 + 16]) == -100000);

        // Locked on NTP as a fallback.
        status.state = SYNC_LOCKED;
        status.source = SYNC_SOURCE_NTP;
        status.fallback = true;
        status.challenger = SYNC_SOURCE_NONE;
        synchronised = false;
        esp_report_stat_sources(5002, &sources);
        CHECK(sources.flags == SML_SOURCES_FALLBACK && sources.selected == SML_SOURCE_NTP &&
              sources.challenger == SML_SOURCE_NONE && sources.challenge_run == 0);
        status.fallback = false;
        status.second = SYNC_SOURCE_NONE;
        status.challenge_run = 0;
        status.source = SYNC_SOURCE_GNSS; // as the rest expects
    }

    // TIME-CTRL: the loop's own values, unrounded.
    control.state = EPOCH_STATE_TRACK;
    control.pps_kf.offset_q16 = -64 * 65536 - 12345; // 1.6 ppm slow and a fraction
    control.pps_kf.variance_ratio_q30 = 1234567;
    control.pps_kf.measurement_variance_q16 = 65536;
    control.pps_kf.gain_q30 = 7654321;
    control.pps_kf.stats.rejected = 2;
    control.predicted_phase_q16 = 32768;
    control.commanded_offset_q16 = -64 * 65536 + 99;
    control.pi.enabled = true;
    control.pi.correction_limit_q16 = 10 * 65536;
    control.pi.correction_q16 = 10 * 65536;
    control.pi.proportional_q16 = 111;
    control.pi.integral_q16 = -222;
    control.gate.rejected = 5;
    update.loop.kind = EPOCH_UPDATE_MEASURED;
    esp_report_time_ctrl(&snapshot, &update, &ctrl); // first: no innovation yet
    CHECK(ctrl.phase_innovation_q16 == 0 && (ctrl.flags & SML_CTRL_GATE_REJECTED));
    esp_report_time_ctrl(&snapshot, &update, &ctrl); // measured 3 ticks, predicted 0.5
    CHECK(ctrl.flags == (SML_CTRL_ESTIMATE_VALID | SML_CTRL_PI_ENABLED | SML_CTRL_SATURATED |
                         SML_CTRL_MEASUREMENT_USED));
    CHECK(ctrl.controller_state == SML_CONTROL_TRACK && ctrl.epoch_sequence == 4242);
    CHECK(ctrl.nominal_period_ticks == 40000000UL &&
          ctrl.frequency_offset_q16 == -64 * 65536 - 12345);
    CHECK(ctrl.frequency_variance_q30 == 1234567 && ctrl.measurement_variance_q16 == 65536 &&
          ctrl.kalman_gain_q30 == 7654321);
    CHECK(ctrl.predicted_phase_q16 == 32768 && ctrl.phase_innovation_q16 == 3 * 65536 - 32768);
    CHECK(ctrl.pi_proportional_q16 == 111 && ctrl.pi_integral_q16 == -222 &&
          ctrl.output_offset_q16 == -64 * 65536 + 99);
    CHECK(ctrl.gate_rejections == 5 && ctrl.kalman_rejections == 2);

    // TIME-MEAS: the first record has no previous second; a mark received
    // during the second just ended is reported once, for the previous epoch.
    snapshot.timer.running_period = 39999990UL;
    snapshot.capture.reference_edge_sequence = 900;
    snapshot.capture.reference_input = PPS_INPUT_GNSS;
    pulse.quantisation_error_ps = -1234;
    has_pulse = true;
    has_mark = true;
    mark_ns = 44;
    gnss.time_mark_received_ms = 50;
    esp_report_time_meas(&snapshot, &update, 1001, &meas);
    CHECK(meas.flags == (SML_MEAS_LABEL_VALID | SML_MEAS_PHASE_VALID | SML_MEAS_PERIOD_VALID |
                         SML_MEAS_QERR_VALID | SML_MEAS_MEASUREMENT_USED));
    CHECK(meas.source == SML_SOURCE_GNSS && meas.controller_state == SML_CONTROL_TRACK);
    CHECK(meas.epoch_sequence == 4242 && meas.utc_seconds == 1792326896UL &&
          meas.reference_sequence == 900 && meas.reference_period_ticks == 39999936UL &&
          meas.phase_ticks == 3 && meas.qerr_ps == -1234);
    snapshot.timer.sequence = 4243;
    snapshot.timer.running_period = 40000010UL;
    gnss.time_mark_received_ms = 1300; // during the second after epoch 4242
    esp_report_time_meas(&snapshot, &update, 2001, &meas);
    CHECK((meas.flags & SML_MEAS_EPOCH_PERIOD_VALID) && meas.epoch_period_ticks == 39999990UL);
    CHECK((meas.flags & SML_MEAS_MARK_VALID) && meas.mark_offset_ns == 44 &&
          meas.mark_accuracy_ns == 7);
    snapshot.timer.sequence = 4244;
    esp_report_time_meas(&snapshot, &update, 3001, &meas); // same mark: not again
    CHECK(!(meas.flags & SML_MEAS_MARK_VALID) && meas.epoch_period_ticks == 40000010UL);

    // TIME-GNSS.
    gnss.detection.state = GNSS_DETECTION_CONFIRMED;
    has_epoch = true;
    epoch.completed_ms = 9000;
    epoch.fix_ok = true;
    epoch.time_valid = true;
    epoch.utc_seconds = 1792326896UL;
    epoch.status.fix_type = 3;
    epoch.time_utc.validity_flags = 0x27; // standard 2 (USNO... per u-blox), valid
    epoch.time_utc.time_accuracy_ns = 20;
    epoch.pps_associated = true;
    epoch.pps_sequence = 77;
    epoch.has_position = true;
    epoch.position.latitude_e7 = 515000000L;
    epoch.position.longitude_e7 = -1000000L;
    epoch.position.horizontal_accuracy_mm = 1500;
    esp_report_time_gnss(9400, &gnss_payload);
    CHECK(gnss_payload.flags == 0x7F);
    CHECK(gnss_payload.fix_type == 3 && gnss_payload.utc_standard == 2 &&
          gnss_payload.utc_accuracy_ns == 20 && gnss_payload.utc_age_ms == 400);
    CHECK(gnss_payload.pps_sequence == 77 && gnss_payload.latitude_e7 == 515000000L &&
          gnss_payload.horizontal_accuracy_mm == 1500);
    CHECK(gnss_payload.gps_utc_s == 18 && gnss_payload.leap_change == 1 &&
          gnss_payload.leap_event_utc == 1798761600UL);
    esp_report_time_gnss(9000 + 70000, &gnss_payload); // age saturates
    CHECK(gnss_payload.utc_age_ms == 65535);

    // STAT-UI with a manual UTC-5 zone.
    timezone_init();
    timezone_set_auto(false);
    timezone_set_dst_auto(false);
    timezone_set_manual(-18000, 3600, true);
    esp_report_stat_ui(&ui);
    CHECK(ui.flags == (SML_UI_LOCAL_VALID | SML_UI_POWER_ON | SML_UI_BUTTON | SML_UI_DST_ACTIVE |
                       SML_UI_BRIGHTNESS_AUTO | SML_UI_FORMAT_12H | SML_UI_ALARM_ENABLED |
                       SML_UI_ALARM_ACTIVE | SML_UI_DELTA_LOCAL));
    CHECK(ui.alarm_offset_s == 27000U);
    CHECK(ui.delta_epoch_utc == 1800000000UL && ui.selected_display == SML_DISPLAY_HHMM);
    CHECK(ui.brightness_target == 2500 && ui.button_state == SML_BUTTON_NONE);
    // A press is reported once, in the next STAT-UI.
    last_press = CONTROLS_LONG;
    press_count++;
    esp_report_stat_ui(&ui);
    CHECK(ui.button_state == SML_BUTTON_LONG);
    esp_report_stat_ui(&ui);
    CHECK(ui.button_state == SML_BUTTON_NONE);
    bright_auto = false;
    esp_report_stat_ui(&ui);
    CHECK((ui.flags & SML_UI_BRIGHTNESS_AUTO) == 0U);
    {
        SML_TZ_STATE_PAYLOAD state;
        memset(&state, 0, sizeof state);
        state.flags = SML_TZ_VALID;
        state.horizon_end_utc = 0xFFFFFFFFUL;
        timezone_apply(&state);
        esp_report_stat_ui(&ui);
        CHECK(ui.flags & SML_UI_TZ_HELD);
    }
    CHECK(ui.current_display == SML_DISPLAY_HHMM && ui.brightness == 2000);
    CHECK(ui.menu_state == SML_MENU_NONE);
    menu_shown = true; // the menu open: shown, and where in it
    esp_report_stat_ui(&ui);
    CHECK(ui.current_display == SML_DISPLAY_MENU &&
          ui.menu_state == (SML_MENU_DISPLAY | 2U | SML_MENU_EDITING));
    menu_shown = false;
    CHECK(ui.base_offset_s == -18000 && ui.dst_offset_s == 3600 &&
          ui.local_seconds == 1792326896UL - 14400UL);

    // TIME-RTC: nothing without an RTC but its type; then its time, when
    // our clock last read and set it, SYNCED once measured, and its bound.
    {
        SML_TIME_RTC_PAYLOAD rtc;

        esp_report_time_rtc(&rtc);
        CHECK(rtc.flags == 0U && rtc.rtc_type == SML_RTC_PCF8563 && rtc.uncertainty_ns == 0U);
        rtc_status.present = true;
        rtc_time = true;
        rtc_source.last_read_valid = true;
        rtc_source.last_read_utc = 1792325990UL;
        rtc_source.last_write_valid = true;
        rtc_source.last_write_utc = 1792300000UL;
        rtc_source.anchor.valid = true; // a plain write: not SYNCED
        esp_report_time_rtc(&rtc);
        CHECK(rtc.flags == (SML_RTC_PRESENT | SML_RTC_UTC_VALID | SML_RTC_LAST_READ_VALID |
                            SML_RTC_LAST_WRITE_VALID | SML_RTC_UNCERTAINTY_VALID));
        CHECK(rtc.rtc_utc_seconds == 1792326000UL && rtc.last_read_utc == 1792325990UL &&
              rtc.last_write_utc == 1792300000UL && rtc.uncertainty_ns == 12345678UL &&
              rtc.pps_sequence == 0U && rtc.rtc_temperature_centic == 0);
        rtc_source.anchor.aligned = true;
        esp_report_time_rtc(&rtc);
        CHECK((rtc.flags & SML_RTC_SYNCED) != 0U);
    }

    // STAT-ENV: the light, rounded to whole lux, while valid.
    {
        SML_STAT_ENV_PAYLOAD env;

        esp_report_stat_env(&env);
        CHECK(env.flags == 0 && env.lux == 0);
        light.present = true;
        light.lux_x10 = 12345;
        esp_report_stat_env(&env);
        CHECK(env.flags == SML_ENV_LUX_PRESENT && env.lux == 0); // not valid
        light.lux_valid = true;
        esp_report_stat_env(&env);
        CHECK(env.flags == (SML_ENV_LUX_PRESENT | SML_ENV_LUX_VALID) && env.lux == 1235);
        light.lux_x10 = 20617; // full scale at 320 ms
        esp_report_stat_env(&env);
        CHECK(env.lux == 2062);
    }

    // STAT-SYS.
    {
        SML_STAT_SYS_PAYLOAD sys;

        rcon = 0x0103; // POR, BOR and VREGS (not a cause: dropped)
        esp_report_stat_sys(98765, &sys);
        CHECK(sys.flags == 0 && sys.fault == 0);
        CHECK(sys.reset_cause == (SML_RESET_POR | SML_RESET_BOR));
        CHECK(sys.uptime_s == 98765 && sys.pic_version == PIC_VERSION_NUMBER);
        CHECK(sys.loop_max_ms == 37);
        esp_report_stat_sys(98775, &sys);
        CHECK(sys.loop_max_ms == 0); // taken: since the previous STAT-SYS
        CHECK(strcmp(esp_report_reset_name(SML_RESET_POR | SML_RESET_BOR), "power-on") == 0);
        CHECK(strcmp(esp_report_reset_name(SML_RESET_BOR), "brown-out") == 0);
        CHECK(strcmp(esp_report_reset_name(SML_RESET_SWR | SML_RESET_TRAPR), "trap conflict") == 0);
        CHECK(strncmp(esp_report_reset_name(SML_RESET_SWR), "software", 8) == 0);
        CHECK(strcmp(esp_report_reset_name(SML_RESET_EXTR), "MCLR pin") == 0);
        CHECK(strcmp(esp_report_reset_name(0), "unknown") == 0);
        // A fault the PIC recorded goes with it (SML 2.6).
        fault_init(SML_RESET_WDTO);
        esp_report_stat_sys(98785, &sys);
        CHECK(sys.flags == SML_SYS_FAULT && sys.fault == SML_FAULT_WATCHDOG << 8);
        fault_init(SML_RESET_POR);
        esp_report_stat_sys(98795, &sys);
        CHECK(sys.flags == 0 && sys.fault == 0);
        // On the internal oscillator: said so (SML 2.7).
        clock_fallback = true;
        esp_report_stat_sys(98805, &sys);
        CHECK(sys.flags == SML_SYS_CLOCK_FALLBACK && sys.fault == 0);
        esp_report_print_system(false);
        clock_fallback = false;
        esp_report_print_system(false);
        esp_report_print_system(true);
    }

    // SHOW topics: each prints its modules; ALL is esp_report_print_all's,
    // and anything else is not known.
    {
        static const struct
        {
            uint32_t topic;
            const char *expected;
        } topics[] = {
            {SML_SHOW_STORED, "stored "},
            {SML_SHOW_TZ, ""},                                      // timezone.c itself, to stdout
            {SML_SHOW_DISPLAY, "brightness light controls alarm "}, // the counters: verbose
            {SML_SHOW_RTC, "pcf8563 rtc-source "},
            {SML_SHOW_OSC, "oscillator "},
            {SML_SHOW_GNSS, "gnss "},
        };
        size_t index;

        CHECK(sizeof(topics) / sizeof(topics[0]) == SML_SHOW_COUNT - 1U);
        for (index = 0; index < sizeof(topics) / sizeof(topics[0]); index++)
        {
            printed[0] = '\0';
            CHECK(esp_report_show(topics[index].topic, false, 1000) &&
                  strcmp(printed, topics[index].expected) == 0);
        }
        printed[0] = '\0';
        CHECK(esp_report_show(SML_SHOW_DISPLAY, true, 1000) &&
              strcmp(printed, "display brightness light controls alarm ") == 0);
        printed[0] = '\0';
        CHECK(!esp_report_show(SML_SHOW_ALL, false, 1000) &&
              !esp_report_show(SML_SHOW_COUNT, false, 1000) && printed[0] == '\0');
        esp_report_print_all(1000, false);
        CHECK(strstr(printed, "settings") != NULL && strstr(printed, "rtc-source") != NULL &&
              strstr(printed, "oscillator") != NULL && strstr(printed, "display") == NULL);
    }

    test_boot_report();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
