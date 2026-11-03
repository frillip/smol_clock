#include "esp_report.h"
#include "spl_writer.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../drivers/boot_image.h"
#include "../drivers/i2c1.h"
#include "../drivers/pcf8563.h"
#include "../drivers/tubes.h"
#include "../drivers/veml6040.h"
#include "../gnss/gnss.h"
#include "../menu/menu.h"
#include "../platform/boot.h"
#include "../platform/fault.h"
#include "../platform/system.h"
#include "../platform/time_ms.h"
#include "../platform/version.h"
#include "../settings/settings.h"
#include "../sync/source_rtc.h"
#include "../sync/sync_report.h"
#include "../timing/q16.h"
#include "../timing/scheduler.h"
#include "../timing/timing_config.h"
#include "../ui/alarm.h"
#include "../ui/brightness.h"
#include "../ui/controls.h"
#include "../ui/timezone.h"

#define NS_PER_TICK  25L // 1 / FCY
#define AGE_LIMIT_MS 0xFFFFUL

// ---- Mappings -------------------------------------------------------------

// Nothing processed yet counts as uninitialised whatever the state says.
uint8_t esp_report_lock(SyncState state, uint32_t epochs)
{
    static const uint8_t codes[] = {SML_LOCK_FREE_RUNNING, SML_LOCK_ACQUIRING, SML_LOCK_LOCKED,
                                    SML_LOCK_HOLDOVER};
    return epochs == 0U || (unsigned)state >= sizeof(codes) ? SML_LOCK_UNINITIALISED : codes[state];
}


uint8_t esp_report_sync(SyncState state, uint32_t epochs)
{
    static const uint8_t codes[] = {SML_SYNC_FREE_RUN, SML_SYNC_ACQUIRE, SML_SYNC_LOCKED,
                                    SML_SYNC_HOLDOVER};
    return epochs == 0U || (unsigned)state >= sizeof(codes) ? SML_SYNC_INIT : codes[state];
}


uint8_t esp_report_cause(uint8_t reason)
{
    static const uint8_t codes[] = {
        SML_CAUSE_QUALIFIED,     SML_CAUSE_TRACKING,    SML_CAUSE_PHASE_LOST,
        SML_CAUSE_SOURCE_LOST,   SML_CAUSE_SOURCE_BACK, SML_CAUSE_ALTERNATIVE,
        SML_CAUSE_BETTER_SOURCE, SML_CAUSE_EXPIRED,     SML_CAUSE_RESYNC};
    return reason < sizeof(codes) ? codes[reason] : SML_CAUSE_NONE;
}


uint8_t esp_report_source(uint8_t source, uint32_t epochs)
{
    static const uint8_t codes[SYNC_SOURCE_COUNT] = {SML_SOURCE_GNSS, SML_SOURCE_NTP,
                                                     SML_SOURCE_RTC};
    if (epochs == 0U)
    {
        return SML_SOURCE_INIT;
    }
    return source < SYNC_SOURCE_COUNT ? codes[source] : SML_SOURCE_NONE;
}


uint8_t esp_report_candidate(uint8_t candidate)
{
    if (candidate == SYNC_CANDIDATE_HOLDOVER)
    {
        return SML_SOURCE_HOLDOVER;
    }
    return esp_report_source(candidate, 1U);
}


uint8_t esp_report_verdict(SyncCheck check)
{
    static const uint8_t codes[SYNC_CHECK_COUNT] = {
        SML_VERDICT_ABSENT,     SML_VERDICT_NO_TIME,    SML_VERDICT_NO_EDGE,
        SML_VERDICT_UNLABELLED, SML_VERDICT_INACCURATE, SML_VERDICT_INCONSISTENT,
        SML_VERDICT_GOOD};
    return (unsigned)check < SYNC_CHECK_COUNT ? codes[check] : SML_VERDICT_NONE;
}


uint8_t esp_report_control(EpochSyncState state)
{
    static const uint8_t codes[] = {SML_CONTROL_ACQUIRE, SML_CONTROL_STEP, SML_CONTROL_TRACK};
    return (unsigned)state < sizeof(codes) ? codes[state] : SML_CONTROL_INIT;
}


uint8_t esp_report_display(DisplayMode mode, bool time_known)
{
    static const uint8_t codes[DISPLAY_MODE_COUNT] = {
        SML_DISPLAY_HHMM, SML_DISPLAY_MMSS,  SML_DISPLAY_SSMM,       SML_DISPLAY_YYYY,
        SML_DISPLAY_MMDD, SML_DISPLAY_DELTA, SML_DISPLAY_UNCERTAINTY};
    if (!time_known)
    {
        return SML_DISPLAY_DASHES;
    }
    return (unsigned)mode < DISPLAY_MODE_COUNT ? codes[mode] : SML_DISPLAY_INIT;
}


static uint16_t age_ms(uint32_t now_ms, uint32_t then_ms)
{
    uint32_t age = time_ms_elapsed(now_ms, then_ms);
    return (uint16_t)(age > AGE_LIMIT_MS ? AGE_LIMIT_MS : age);
}


// ---- Payloads -------------------------------------------------------------

void esp_report_time_auth(SML_TIME_AUTH_PAYLOAD *payload)
{
    const SyncStatus *status = sync_status();
    uint32_t uncertainty_ns;

    memset(payload, 0, sizeof(*payload));
    uint8_t source = status->source;

    // With nothing selected, the time from a time-of-day source (the RTC at
    // start-up) names it; SOURCE_VALID still says whether one is selected.
    if (source >= SYNC_SOURCE_COUNT && status->time.known &&
        status->time.source < SYNC_SOURCE_COUNT && !sync_source_steers(status->time.source))
    {
        source = status->time.source;
    }
    payload->source = esp_report_source(source, status->epochs);
    payload->lock_state = esp_report_lock(status->state, status->epochs);
    if (status->source < SYNC_SOURCE_COUNT)
    {
        payload->flags |= SML_AUTH_SOURCE_VALID;
    }
    if (!status->time.known)
    {
        return;
    }
    // An inserted leap second is sent as 23:59:59 again (Unix seconds).
    payload->flags |= SML_AUTH_UTC_VALID;
    payload->utc_seconds = status->time.utc_seconds;
    if (status->phase_valid && status->state != SYNC_HOLDOVER)
    {
        payload->utc_phase_ns = status->phase_ticks * NS_PER_TICK;
    }
    if (sync_uncertainty(&uncertainty_ns))
    {
        payload->flags |= SML_AUTH_UNCERTAINTY_VALID;
        payload->uncertainty_ns = uncertainty_ns;
    }
}


void esp_report_time_pic(const TimingSnapshot *snapshot, const SyncUpdate *update,
                         SML_TIME_PIC_PAYLOAD *payload)
{
    const SyncStatus *status = sync_status();
    const SyncTransition *transition = sync_last_transition();
    const EpochControl *control = epoch_control_state();

    memset(payload, 0, sizeof(*payload));
    payload->source = esp_report_source(status->source, status->epochs);
    payload->lock_state = esp_report_lock(status->state, status->epochs);
    payload->sync_state = esp_report_sync(status->state, status->epochs);
    payload->second_source = esp_report_candidate(status->second);
    if (transition != NULL)
    {
        payload->previous_sync_state = esp_report_sync(transition->from, 1U);
        payload->last_sync_cause = esp_report_cause(transition->reason);
    }
    payload->epoch_sequence = snapshot->timer.sequence;
    if (update->loop.phase_valid)
    {
        payload->flags |= SML_PIC_PHASE_VALID;
        payload->phase_cycles = update->loop.utc_offset_ticks;
    }
    if (snapshot->capture.reference_period_valid)
    {
        payload->flags |= SML_PIC_PERIOD_VALID;
        payload->pps_period_cycles = snapshot->capture.reference_period;
    }
    if (control->pps_kf.initialized)
    {
        // The timer clock as measured against the reference: FCY ticks per
        // reference second (fosc_hz keeps its V2 meaning, FCY not Fosc).
        payload->flags |= SML_PIC_FREQUENCY_VALID;
        payload->fosc_hz = control->filtered_pps_period;
    }
    if (snapshot->capture.reference_capture_valid)
    {
        payload->flags |= SML_PIC_PPS_VALID;
    }
    payload->applied_adjustment_cycles =
        (int32_t)(control->commanded_period - (uint32_t)TIMING_NOMINAL_TICKS);
    payload->missing_pps_events = status->sources[SYNC_SOURCE_GNSS].verdicts[SYNC_CHECK_NO_EDGE];
    payload->sync_events = status->transitions;
    payload->reference_age_ms =
        (uint16_t)(control->holdover_epochs > 65U ? AGE_LIMIT_MS
                                                  : control->holdover_epochs * 1000U);
}


void esp_report_time_ctrl(const TimingSnapshot *snapshot, const SyncUpdate *update,
                          SML_TIME_CTRL_PAYLOAD *payload)
{
    static int32_t last_prediction_q16;
    static bool has_prediction;
    static uint32_t last_gate_rejections;
    const EpochControl *control = epoch_control_state();
    const EpochPiServo *pi = &control->pi;
    const KalmanFilter *kalman = &control->pps_kf;

    memset(payload, 0, sizeof(*payload));
    payload->controller_state = esp_report_control(control->state);
    payload->epoch_sequence = snapshot->timer.sequence;
    payload->nominal_period_ticks = TIMING_NOMINAL_TICKS;
    if (kalman->initialized)
    {
        payload->flags |= SML_CTRL_ESTIMATE_VALID;
        payload->frequency_offset_q16 = kalman->offset_q16;
        payload->frequency_variance_q30 = kalman->variance_ratio_q30;
        payload->measurement_variance_q16 = kalman->measurement_variance_q16;
        payload->kalman_gain_q30 = kalman->gain_q30;
        payload->predicted_phase_q16 = control->predicted_phase_q16;
    }
    if (pi->enabled)
    {
        payload->flags |= SML_CTRL_PI_ENABLED;
        if (pi->correction_q16 >= pi->correction_limit_q16 ||
            pi->correction_q16 <= -pi->correction_limit_q16)
        {
            payload->flags |= SML_CTRL_SATURATED;
        }
    }
    if (update->loop.kind == EPOCH_UPDATE_MEASURED)
    {
        payload->flags |= SML_CTRL_MEASUREMENT_USED;
    }
    if (control->gate.rejected != last_gate_rejections)
    {
        payload->flags |= SML_CTRL_GATE_REJECTED;
    }
    last_gate_rejections = control->gate.rejected;
    // Measured phase minus the phase predicted for this epoch one epoch ago.
    if (update->loop.phase_valid && has_prediction)
    {
        payload->phase_innovation_q16 =
            Q16_FROM_TICKS(q16_clamp(update->loop.utc_offset_ticks, Q16_TICK_LIMIT)) -
            last_prediction_q16;
    }
    last_prediction_q16 = control->predicted_phase_q16;
    has_prediction = kalman->initialized;
    payload->pi_proportional_q16 = pi->proportional_q16;
    payload->pi_integral_q16 = pi->integral_q16;
    payload->output_offset_q16 = control->commanded_offset_q16;
    payload->gate_rejections = control->gate.rejected;
    payload->kalman_rejections = kalman->stats.rejected;
}


void esp_report_time_meas(const TimingSnapshot *snapshot, const SyncUpdate *update, uint32_t now_ms,
                          SML_TIME_MEAS_PAYLOAD *payload)
{
    static bool has_previous;
    static uint32_t previous_running_period;
    static uint32_t previous_report_ms;
    const SyncStatus *status = sync_status();
    const PpsCaptureSample *capture = &snapshot->capture;
    const UbxTimTp *pulse;
    int32_t mark_ns;
    uint32_t mark_accuracy_ns;

    memset(payload, 0, sizeof(*payload));
    payload->source = esp_report_source(status->source, status->epochs);
    payload->controller_state = esp_report_control(epoch_control_state()->state);
    payload->epoch_sequence = snapshot->timer.sequence;
    if (status->time.known)
    {
        payload->flags |= SML_MEAS_LABEL_VALID;
        payload->utc_seconds = status->time.utc_seconds;
    }
    payload->reference_sequence = capture->reference_edge_sequence;
    if (update->loop.phase_valid)
    {
        payload->flags |= SML_MEAS_PHASE_VALID;
        payload->phase_ticks = update->loop.utc_offset_ticks;
    }
    if (capture->reference_capture_valid && capture->reference_period_valid)
    {
        payload->flags |= SML_MEAS_PERIOD_VALID;
        payload->reference_period_ticks = capture->reference_period;
    }
    if (has_previous)
    {
        // The period loaded at the previous epoch is the second just ended.
        payload->flags |= SML_MEAS_EPOCH_PERIOD_VALID;
        payload->epoch_period_ticks = previous_running_period;
    }
    if (update->loop.kind == EPOCH_UPDATE_MEASURED)
    {
        payload->flags |= SML_MEAS_MEASUREMENT_USED;
    }
    // The receiver's quantisation error is for its own edge: only while that
    // is the one captured.
    if (capture->reference_capture_valid && capture->reference_input == PPS_INPUT_GNSS &&
        gnss_time_pulse(snapshot->monitor.inputs[PPS_INPUT_GNSS].sequence, &pulse))
    {
        payload->flags |= SML_MEAS_QERR_VALID;
        payload->qerr_ps = pulse->quantisation_error_ps;
    }
    // A mark received during the second just ended describes the epoch that
    // began it: each is reported once, against epoch_sequence - 1.
    if (has_previous && gnss_mark_offset(now_ms, &mark_ns, &mark_accuracy_ns) &&
        time_ms_elapsed(now_ms, gnss_state()->time_mark_received_ms) <
            time_ms_elapsed(now_ms, previous_report_ms))
    {
        payload->flags |= SML_MEAS_MARK_VALID;
        payload->mark_offset_ns = mark_ns;
        payload->mark_accuracy_ns = mark_accuracy_ns;
    }
    has_previous = true;
    previous_running_period = snapshot->timer.running_period;
    previous_report_ms = now_ms;
}


void esp_report_time_rtc(SML_TIME_RTC_PAYLOAD *payload)
{
    const Pcf8563Status *rtc = pcf8563_status();
    const SourceRtcState *source = source_rtc_state();
    uint32_t value;

    memset(payload, 0, sizeof(*payload));
    payload->rtc_type = SML_RTC_PCF8563;
    if (rtc->present)
    {
        payload->flags |= SML_RTC_PRESENT;
    }
    if (source_rtc_reckoning(&value))
    {
        payload->flags |= SML_RTC_UTC_VALID;
        payload->rtc_utc_seconds = value;
    }
    if (source->anchor.valid && source->anchor.aligned)
    {
        payload->flags |= SML_RTC_SYNCED;
    }
    if (source->last_read_valid)
    {
        payload->flags |= SML_RTC_LAST_READ_VALID;
        payload->last_read_utc = source->last_read_utc;
    }
    if (source->last_write_valid)
    {
        payload->flags |= SML_RTC_LAST_WRITE_VALID;
        payload->last_write_utc = source->last_write_utc;
    }
    if (source_rtc_bound(&value))
    {
        payload->flags |= SML_RTC_UNCERTAINTY_VALID;
        payload->uncertainty_ns = value;
    }
}


void esp_report_time_gnss(uint32_t now_ms, SML_TIME_GNSS_PAYLOAD *payload)
{
    const GnssState *gnss = gnss_state();
    const GnssEpoch *epoch = gnss_epoch_latest(&gnss->epochs);
    UtcLeap leap;
    int8_t gps_minus_utc;

    memset(payload, 0, sizeof(*payload));
    if (gnss->detection.state == GNSS_DETECTION_CONFIRMED)
    {
        payload->flags |= SML_GNSS_DETECTED;
    }
    if (epoch != NULL)
    {
        payload->fix_type = epoch->status.fix_type;
        payload->utc_standard =
            (uint8_t)((epoch->time_utc.validity_flags & UBX_TIMEUTC_STANDARD_MASK) >>
                      UBX_TIMEUTC_STANDARD_SHIFT);
        payload->utc_age_ms = age_ms(now_ms, epoch->completed_ms);
        if (epoch->fix_ok)
        {
            payload->flags |= SML_GNSS_FIX_OK;
        }
        if (epoch->time_valid)
        {
            payload->flags |= SML_GNSS_UTC_VALID;
            payload->utc_seconds = epoch->utc_seconds;
            payload->utc_accuracy_ns = epoch->time_utc.time_accuracy_ns;
        }
        if (epoch->pps_associated)
        {
            payload->flags |= SML_GNSS_PPS_VALID;
            payload->pps_sequence = epoch->pps_sequence;
            payload->pps_age_ms = payload->utc_age_ms;
        }
        if (epoch->has_position && epoch->fix_ok)
        {
            const UbxNavPosllh *position = &epoch->position;
            payload->flags |= SML_GNSS_POSITION_VALID;
            payload->position_age_ms = payload->utc_age_ms;
            payload->latitude_e7 = position->latitude_e7;
            payload->longitude_e7 = position->longitude_e7;
            payload->height_mm = position->height_ellipsoid_mm;
            payload->height_msl_mm = position->height_msl_mm;
            payload->horizontal_accuracy_mm = position->horizontal_accuracy_mm;
            payload->vertical_accuracy_mm = position->vertical_accuracy_mm;
        }
    }
    if (gnss_leap_seconds(now_ms, &gps_minus_utc))
    {
        payload->flags |= SML_GNSS_LEAP_VALID;
        payload->gps_utc_s = gps_minus_utc;
    }
    if (gnss_leap_event(now_ms, &leap))
    {
        payload->flags |= SML_GNSS_LEAP_EVENT_VALID;
        payload->leap_change = leap.change;
        payload->leap_event_utc = leap.event_utc;
    }
}


// ui/controls.h's presses are SML's button states (a compile-time check).
typedef char
    controls_match_sml[(CONTROLS_NONE == SML_BUTTON_NONE && CONTROLS_SHORT == SML_BUTTON_SHORT &&
                        CONTROLS_LONG == SML_BUTTON_LONG && CONTROLS_HOLD == SML_BUTTON_HOLD)
                           ? 1
                           : -1];

// The latest press, if there has been one since the previous STAT-UI.
static uint8_t new_press(void)
{
    static uint32_t reported;
    uint32_t count;
    ControlsPress press = controls_press(&count);

    if (count == reported)
    {
        return SML_BUTTON_NONE;
    }
    reported = count;
    return (uint8_t)press; // ControlsPress has SML_BUTTON_*'s values
}


void esp_report_stat_ui(SML_STAT_UI_PAYLOAD *payload)
{
    const SyncTime *time = &sync_status()->time;
    TimezoneOffset zone;

    memset(payload, 0, sizeof(*payload));
    payload->current_display =
        menu_is_open() ? SML_DISPLAY_MENU : esp_report_display(display_mode(), time->known);
    payload->selected_display = esp_report_display(display_selected(), true);
    payload->menu_state = menu_state();
    payload->button_state = new_press();
    payload->brightness = tubes_brightness();
    payload->brightness_target = brightness_target();
    if (tubes_brightness() != TUBES_BRIGHTNESS_OFF)
    {
        payload->flags |= SML_UI_POWER_ON;
    }
    if (brightness_is_auto())
    {
        payload->flags |= SML_UI_BRIGHTNESS_AUTO;
    }
    if (display_is_12h())
    {
        payload->flags |= SML_UI_FORMAT_12H;
    }
    if (alarm_enabled())
    {
        payload->flags |= SML_UI_ALARM_ENABLED;
    }
    if (alarm_state() == ALARM_SOUNDING)
    {
        payload->flags |= SML_UI_ALARM_ACTIVE;
    }
    payload->alarm_offset_s = alarm_time();
    payload->delta_epoch_utc = display_delta_epoch();
    if (display_delta_is_local())
    {
        payload->flags |= SML_UI_DELTA_LOCAL;
    }
    if (controls_switch_on())
    {
        payload->flags |= SML_UI_SWITCH;
    }
    if (controls_button_down())
    {
        payload->flags |= SML_UI_BUTTON;
    }
    timezone_offset_at(time->utc_seconds, &zone);
    payload->base_offset_s = zone.base_offset_s;
    payload->dst_offset_s = zone.dst_offset_s;
    if (zone.dst_active)
    {
        payload->flags |= SML_UI_DST_ACTIVE;
    }
    if (zone.stale)
    {
        payload->flags |= SML_UI_TZ_STALE;
    }
    if (zone.ambiguous)
    {
        payload->flags |= SML_UI_TZ_AMBIGUOUS;
    }
    if (timezone_schedule()->held)
    {
        payload->flags |= SML_UI_TZ_HELD;
    }
    if (time->known)
    {
        payload->flags |= SML_UI_LOCAL_VALID;
        payload->local_seconds = time->utc_seconds + (uint32_t)zone.offset_s;
    }
}


void esp_report_stat_env(SML_STAT_ENV_PAYLOAD *payload)
{
    const Veml6040Status *light = veml6040_status();

    memset(payload, 0, sizeof(*payload));
    if (light->present)
    {
        payload->flags |= SML_ENV_LUX_PRESENT;
    }
    if (light->present && light->lux_valid)
    {
        payload->flags |= SML_ENV_LUX_VALID;
        payload->lux = (uint16_t)(((uint32_t)light->lux_x10 + 5UL) / 10UL);
    }
}


// One source's entry in STAT-SOURCES.
static void source_entry(const SyncStatus *status, uint8_t source, uint32_t epoch_sequence,
                         SML_SOURCE_ENTRY *entry)
{
    const SyncSourceTracker *tracker = &status->sources[source];
    const SyncSourceReport *report = &tracker->report;
    uint32_t bound = sync_candidate_bound(source);

    // A time-of-day source (the RTC) is never a candidate, but has a bound.
    if (!sync_source_steers(source) && tracker->bound_ns != 0U)
    {
        bound = tracker->bound_ns;
    }

    entry->source = esp_report_candidate(source);
    entry->verdict = status->epochs == 0U ? SML_VERDICT_NONE : esp_report_verdict(tracker->check);
    entry->rank = sync_candidate_rank(source);
    entry->flags = (uint8_t)((report->present ? SML_ENTRY_PRESENT : 0U) |
                             (report->time_valid ? SML_ENTRY_TIME_VALID : 0U) |
                             (report->edge ? SML_ENTRY_EDGE : 0U) |
                             (report->captured ? SML_ENTRY_CAPTURED : 0U) |
                             (report->labelled ? SML_ENTRY_LABELLED : 0U) |
                             (tracker->phase_valid ? SML_ENTRY_PHASE_VALID : 0U) |
                             (bound != SYNC_BOUND_NONE ? SML_ENTRY_BOUND_VALID : 0U));
    entry->good_run = tracker->good_run;
    entry->usable_run = tracker->usable_run;
    entry->bound_ns = bound != SYNC_BOUND_NONE ? bound : 0U;
    entry->uncertainty_ns = report->uncertainty_ns;
    entry->phase_ns = tracker->phase_valid ? tracker->phase_ticks * NS_PER_TICK : 0;
    entry->age_s = tracker->verdicts[SYNC_CHECK_GOOD] != 0U
                       ? epoch_sequence - tracker->last_good_epoch
                       : UINT32_MAX;
}


// Holdover's entry: judged by nothing but its own bound.
static void holdover_entry(const SyncStatus *status, SML_SOURCE_ENTRY *entry)
{
    uint32_t bound = sync_candidate_bound(SYNC_CANDIDATE_HOLDOVER);

    entry->source = SML_SOURCE_HOLDOVER;
    entry->verdict = bound != SYNC_BOUND_NONE ? SML_VERDICT_GOOD : SML_VERDICT_NONE;
    entry->rank = sync_candidate_rank(SYNC_CANDIDATE_HOLDOVER);
    entry->flags = status->holdover.ready ? SML_ENTRY_PRESENT | SML_ENTRY_TIME_VALID : 0U;
    if (bound != SYNC_BOUND_NONE)
    {
        entry->flags |= SML_ENTRY_BOUND_VALID;
        entry->bound_ns = bound;
    }
    entry->uncertainty_ns = status->holdover.ready ? status->holdover.base_ns : 0U;
    entry->age_s = status->holdover.ready ? status->holdover.age : UINT32_MAX;
}


void esp_report_stat_sources(uint32_t epoch_sequence, SML_STAT_SOURCES_PAYLOAD *payload)
{
    const SyncStatus *status = sync_status();
    uint8_t index;

    memset(payload, 0, sizeof(*payload));
    if (sync_is_synchronised())
    {
        payload->flags |= SML_SOURCES_SYNCHRONISED;
    }
    if (status->fallback && (status->state == SYNC_ACQUIRE || status->state == SYNC_LOCKED))
    {
        payload->flags |= SML_SOURCES_FALLBACK;
    }
    payload->sync_state = esp_report_sync(status->state, status->epochs);
    switch (status->state)
    {
        case SYNC_HOLDOVER: payload->selected = SML_SOURCE_HOLDOVER; break;
        case SYNC_ACQUIRE:
        case SYNC_LOCKED: payload->selected = esp_report_candidate(status->source); break;
        default: payload->selected = SML_SOURCE_NONE; break;
    }
    payload->second = esp_report_candidate(status->second);
    payload->challenger = esp_report_candidate(status->challenger);
    payload->challenge_run = status->challenger != SYNC_SOURCE_NONE ? status->challenge_run : 0U;
    payload->epoch_sequence = epoch_sequence;

    // The sources, then holdover; unused entries say NONE.
    for (index = 0; index < SML_SOURCES_ENTRIES; index++)
    {
        payload->entries[index].source = SML_SOURCE_NONE;
    }
    for (index = 0; index < SYNC_SOURCE_COUNT && index < SML_SOURCES_ENTRIES - 1U; index++)
    {
        source_entry(status, index, epoch_sequence, &payload->entries[index]);
    }
    holdover_entry(status, &payload->entries[index]);
}


const char *esp_report_reset_name(uint16_t cause)
{
    // Most telling first: a power-on also sets BOR, and a trap without a
    // handler resets through the RESET instruction, which sets SWR.
    if (cause & SML_RESET_POR)
    {
        return "power-on";
    }
    if (cause & SML_RESET_BOR)
    {
        return "brown-out";
    }
    if (cause & SML_RESET_WDTO)
    {
        return "watchdog";
    }
    if (cause & SML_RESET_TRAPR)
    {
        return "trap conflict";
    }
    if (cause & SML_RESET_IOPUWR)
    {
        return "illegal opcode or uninitialised W";
    }
    if (cause & SML_RESET_CM)
    {
        return "configuration mismatch";
    }
    if (cause & SML_RESET_EXTR)
    {
        return "MCLR pin";
    }
    if (cause & SML_RESET_SWR)
    {
        return "software (reset command or unhandled trap)";
    }
    return "unknown";
}


void esp_report_stat_sys(uint32_t epoch_sequence, SML_STAT_SYS_PAYLOAD *payload)
{
    memset(payload, 0, sizeof(*payload));
    payload->reset_cause = (uint16_t)(system_reset_cause() & SML_RESET_ALL);
    payload->uptime_s = epoch_sequence;
    payload->pic_version = PIC_VERSION_NUMBER;
    payload->loop_max_ms = scheduler_take_longest_pass_ms();
    if (system_clock_fallback())
    {
        payload->flags |= SML_SYS_CLOCK_FALLBACK;
    }
    if (fault_last()->kind != FAULT_NONE)
    {
        payload->flags |= SML_SYS_FAULT;
        payload->fault = (uint16_t)((uint16_t)fault_last()->kind << 8 | fault_last()->trap);
    }
}


static void print_version(uint32_t version)
{
    printf("%u.%u.%u", (unsigned)(version >> 16 & 0xFFUL), (unsigned)(version >> 8 & 0xFFUL),
           (unsigned)(version & 0xFFUL));
}


// The boot stages: the IPL, the SPL, this application's record, and how
// this start came about.
static void print_boot(void)
{
    static const char *const paths[] = {"the IPL directly", "an unconfirmed SPL", "no SPL",
                                        "the SPL given up", "the SPL as a last resort",
                                        "after the SPL hung"};
    BootIpl ipl;
    BootSpl spl;
    BootApp app;
    uint8_t path = boot_path();
    uint8_t bit;

    boot_image_ipl(&ipl);
    boot_image_spl(&spl, true);
    boot_image_app(&app);
    printf("Boot: IPL ");
    if (ipl.present)
    {
        print_version(ipl.version);
    }
    else
    {
        printf("NONE");
    }
    printf(", SPL ");
    if ((spl.state & SML_SPL_STATE_RECORD) == 0U)
    {
        printf("NONE");
    }
    else
    {
        print_version(spl.version);
        printf(" %s%s%s%s",
               (spl.state & SML_SPL_STATE_COMMITTED) != 0U ? "committed" : "UNCOMMITTED",
               (spl.state & SML_SPL_STATE_INTACT) != 0U ? ", intact" : ", NOT INTACT",
               (spl.state & SML_SPL_STATE_CONFIRMED) != 0U ? ", confirmed" : "",
               (spl.state & SML_SPL_STATE_REJECTED) != 0U ? ", REJECTED" : "");
    }
    printf(", application record ");
    if (app.committed)
    {
        print_version(app.version);
        printf(", %lu instructions", (unsigned long)app.length);
    }
    else
    {
        printf("NONE");
    }
    printf("\n");
    if (!boot_mailbox_valid())
    {
        printf("Boot: started without the IPL (no mailbox)\n");
        return;
    }
    printf("Boot: started by ");
    if ((path & (uint16_t)~SML_BOOT_PATH_SPL_HUNG) == 0U)
    {
        printf("the SPL%s", path != 0U ? ", " : "");
    }
    for (bit = 0; bit < sizeof(paths) / sizeof(paths[0]); bit++)
    {
        if ((path & (1U << bit)) != 0U)
        {
            printf("%s%s", (path & ((1U << bit) - 1U)) != 0U ? ", " : "", paths[bit]);
        }
    }
    printf("; trials %u, attempts %u, healthy %u s\n", boot_trials(), boot_attempts(),
           boot_healthy_s());
    {
        const SplWriterStatus *writer = spl_writer_status();

        if (writer->erases != 0U || writer->rejects != 0U || writer->confirms != 0U ||
            writer->reject_due || writer->session)
        {
            printf("SPL writer: %lu pages erased, %lu chunks, %lu commits, %lu confirms, %lu "
                   "rejects, %lu failures%s%s\n",
                   (unsigned long)writer->erases, (unsigned long)writer->chunks,
                   (unsigned long)writer->commits, (unsigned long)writer->confirms,
                   (unsigned long)writer->rejects, (unsigned long)writer->failures,
                   writer->session ? "; ERASED, waiting for its data" : "",
                   writer->reject_due ? "; reject due" : "");
        }
    }
}


void esp_report_print_system(bool verbose)
{
    uint16_t cause = system_reset_cause();

    printf("PIC %u.%u.%u, up ", PIC_VERSION_MAJOR, PIC_VERSION_MINOR, PIC_VERSION_PATCH);
    utc_print_duration(scheduler_epoch());
    printf(", last reset: %s", esp_report_reset_name((uint16_t)(cause & SML_RESET_ALL)));
    if (verbose)
    {
        printf(" (RCON 0x%04X)", cause);
    }
    printf("\n");
    fault_print();
    if (system_clock_fallback())
    {
        printf("Clock: the external clock did not start: on the internal oscillator\n");
    }
    if (verbose)
    {
        print_boot();
    }
}


// The clock's time, for the RTC's report.
static Pcf8563Reference rtc_reference(void)
{
    const SyncStatus *status = sync_status();
    Pcf8563Reference clock;

    clock.utc_seconds = status->time.utc_seconds;
    clock.known = status->time.known;
    return clock;
}


// The display as set and as running: the mode, the clock, the delta epoch.
static void print_display(bool verbose, uint32_t now_ms)
{
    printf("Display: %s", display_mode_name(display_mode()));
    if (display_mode() != display_selected())
    {
        printf(" (selected %s)", display_mode_name(display_selected()));
    }
    printf(", %s h", display_is_12h() ? "12" : "24");
    if (display_delta_epoch() != 0U)
    {
        printf(", delta from ");
        display_print_epoch(display_delta_epoch(), display_delta_is_local());
    }
    printf("\n");
    if (verbose)
    {
        printf("  ");
        display_print();
    }
    printf("  ");
    brightness_print(verbose);
    printf("  ");
    veml6040_print(now_ms, verbose);
    printf("  ");
    controls_print(verbose);
    printf("  ");
    alarm_print(verbose);
}


bool esp_report_show(uint32_t topic, bool verbose, uint32_t now_ms)
{
    switch (topic)
    {
        case SML_SHOW_STORED: settings_print_stored(verbose); return true;
        case SML_SHOW_TZ: timezone_print(sync_status()->time.utc_seconds, verbose); return true;
        case SML_SHOW_DISPLAY: print_display(verbose, now_ms); return true;
        case SML_SHOW_RTC:
            pcf8563_print(now_ms, rtc_reference(), verbose);
            source_rtc_print(verbose);
            return true;
        case SML_SHOW_OSC: settings_print_oscillator(verbose); return true;
        case SML_SHOW_GNSS: gnss_print(now_ms, verbose); return true;
        default: return false;
    }
}


void esp_report_print_all(uint32_t now_ms, bool verbose)
{
    esp_report_print_system(verbose);
    sync_report_status(verbose);
    timezone_print(sync_status()->time.utc_seconds, verbose);
    settings_print(verbose);
    (void)esp_report_show(SML_SHOW_DISPLAY, verbose, now_ms);
    (void)esp_report_show(SML_SHOW_RTC, verbose, now_ms);
    settings_print_oscillator(verbose);
    gnss_print(now_ms, verbose);
    if (verbose)
    {
        i2c1_print();
    }
    else if (i2c1_stats()->stuck != 0U)
    {
        printf("I2C: BUS STUCK\n");
    }
}
