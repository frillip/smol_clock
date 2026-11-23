#include "settings.h"

#include <stdio.h>
#include "../platform/format.h" // our printf (format.h: the library's is slow)
#include <string.h>

#include "../../common/sml.h"
#include "../console/debug.h"
#include "../drivers/tubes.h"
#include "../platform/clock.h"
#include "../platform/utc.h"
#include "../sync/source_rtc.h"
#include "../timing/epoch_control.h"
#include "../ui/alarm.h"
#include "../ui/beeps.h"
#include "../ui/brightness.h"
#include "../ui/display.h"
#include "../ui/timezone.h"
#include "store.h"

#define FLAGS_DEFINED                                                                \
    (SML_SETTING_TZ_AUTO | SML_SETTING_DST_AUTO | SML_SETTING_DST_ACTIVE |           \
     SML_SETTING_ALARM_ENABLED | SML_SETTING_BEEP_ENABLED | SML_SETTING_FORMAT_12H | \
     SML_SETTING_BRI_AUTO | SML_SETTING_DELTA_LOCAL)
#define SECONDS_PER_DAY 86400UL
#define FOSC_LIMIT_HZ   (FCY / 1000000UL * SETTINGS_FOSC_LIMIT_PPM)
#define TICKS_PER_US    (FCY / 1000000UL) // flash_set_clock(): the capture timebase

typedef struct
{
    SML_SETTINGS_BODY body;
    uint16_t debug_mask;
} Values;

static Values running;
static Values stored; // as last stored, or the defaults while nothing is
static uint32_t revision;
static bool have_record; // a settings record is stored

static bool save_requested;
static bool oscillator_requested; // by a save: the estimate too, if it differs
static uint8_t save_error;
static bool timezone_pending;
static uint32_t timezone_changed_epoch;
static SettingsStats stats;

// The oscillator record: what is stored, and when it was written this run.
static bool have_oscillator;
static int32_t oscillator_q16;
static bool oscillator_saved;
static uint32_t oscillator_saved_epoch;

// The RTC record: what of the RTC source's it holds, and when it was
// written this run.
static uint32_t rtc_saved_changes;
static uint32_t rtc_saved_rtc_changes;
static bool rtc_saved;
static uint32_t rtc_saved_epoch;
static bool rtc_retry;
static uint32_t rtc_retry_epoch;

void settings_defaults(SML_SETTINGS_BODY *body)
{
    memset(body, 0, sizeof(*body));
    body->schema_version = SML_SETTINGS_SCHEMA;
    body->flags = SML_SETTING_TZ_AUTO | SML_SETTING_DST_AUTO | SML_SETTING_BRI_AUTO;
    body->calibration_fosc_hz = FCY;
    body->brightness = TUBES_BRIGHTNESS_DEFAULT;
    body->selected_display = SML_DISPLAY_HHMM;
}


static bool offset_ok(int32_t offset_s)
{
    return offset_s >= -TIMEZONE_OFFSET_LIMIT_S && offset_s <= TIMEZONE_OFFSET_LIMIT_S;
}


// HHMM to DELTA run in the same order in both.
typedef char display_order_check
    [DISPLAY_MODE_DELTA - DISPLAY_MODE_HHMM == SML_DISPLAY_DELTA - SML_DISPLAY_HHMM ? 1 : -1];

// A selectable SML_DISPLAY_* value (settings_check) as the display's mode.
static DisplayMode display_mode_of(uint8_t selected)
{
    return selected == SML_DISPLAY_UNCERTAINTY
               ? DISPLAY_MODE_UNCERTAINTY
               : (DisplayMode)(DISPLAY_MODE_HHMM + (selected - SML_DISPLAY_HHMM));
}


uint8_t settings_check(const SML_SETTINGS_BODY *body)
{
    if (body->schema_version != SML_SETTINGS_SCHEMA)
    {
        return SML_ERROR_SCHEMA;
    }
    if (!SML_DISPLAY_SELECTABLE(body->selected_display) ||
        (body->flags & (uint16_t)~FLAGS_DEFINED) != 0U || body->reserved != 0U ||
        !offset_ok(body->base_offset_s) || !offset_ok(body->dst_offset_s) ||
        body->calibration_fosc_hz < FCY - FOSC_LIMIT_HZ ||
        body->calibration_fosc_hz > FCY + FOSC_LIMIT_HZ ||
        body->alarm_offset_s >= SECONDS_PER_DAY || body->brightness > TUBES_BRIGHTNESS_FULL)
    {
        return SML_ERROR_ARGUMENT;
    }
    return SML_ERROR_NONE;
}


// Each value to the module it belongs to.
static void apply(void)
{
    const SML_SETTINGS_BODY *body = &running.body;

    timezone_set_auto((body->flags & SML_SETTING_TZ_AUTO) != 0U);
    timezone_set_dst_auto((body->flags & SML_SETTING_DST_AUTO) != 0U);
    timezone_set_manual(body->base_offset_s, body->dst_offset_s,
                        (body->flags & SML_SETTING_DST_ACTIVE) != 0U);
    brightness_set_level(body->brightness);
    brightness_set_auto((body->flags & SML_SETTING_BRI_AUTO) != 0U);
    beeps_set_enabled((body->flags & SML_SETTING_BEEP_ENABLED) != 0U);
    alarm_set((body->flags & SML_SETTING_ALARM_ENABLED) != 0U, body->alarm_offset_s);
    display_set_mode(display_mode_of(body->selected_display));
    display_set_12h((body->flags & SML_SETTING_FORMAT_12H) != 0U);
    display_set_delta_epoch(body->delta_epoch_utc, (body->flags & SML_SETTING_DELTA_LOCAL) != 0U);
}


static bool same_body(const SML_SETTINGS_BODY *a, const SML_SETTINGS_BODY *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}


// The record's bytes, from values and a revision.
static void encode_record(const Values *values, uint32_t record_revision, uint8_t *record)
{
    SML_STAT_SETTINGS_PAYLOAD payload;

    memset(&payload, 0, sizeof(payload));
    payload.revision = record_revision;
    payload.settings = values->body;
    memset(record, 0, SETTINGS_RECORD_BYTES);
    sml_put_u16(&record[0], SETTINGS_RECORD_VERSION);
    (void)sml_encode(sml_message(SML_CLASS_STAT, SML_STAT_SETTINGS)->layout, &payload, &record[2]);
    sml_put_u16(&record[2U + SML_STAT_SETTINGS_BYTES], values->debug_mask);
}


// The settings record in the store, if there is one and it can be used.
static bool read_record(Values *values, uint32_t *record_revision, bool *rejected)
{
    uint8_t record[SETTINGS_RECORD_BYTES];
    SML_STAT_SETTINGS_PAYLOAD payload;
    uint16_t length;

    *rejected = false;
    if (!store_read(SETTINGS_RECORD_SETTINGS, record, sizeof(record), &length))
    {
        return false;
    }
    if (length != SETTINGS_RECORD_BYTES || sml_get_u16(&record[0]) != SETTINGS_RECORD_VERSION)
    {
        *rejected = true;
        return false;
    }
    (void)sml_decode(sml_message(SML_CLASS_STAT, SML_STAT_SETTINGS)->layout, &record[2], &payload);
    if (settings_check(&payload.settings) != SML_ERROR_NONE)
    {
        *rejected = true;
        return false;
    }
    memset(values, 0, sizeof(*values));
    values->body = payload.settings;
    values->debug_mask = sml_get_u16(&record[2U + SML_STAT_SETTINGS_BYTES]) & DEBUG_ALL;
    *record_revision = payload.revision;
    return true;
}


static void restore_timezone(void)
{
    uint8_t record[SML_TZ_STATE_BYTES];
    SML_TZ_STATE_PAYLOAD state;
    uint16_t length;

    if (!store_read(SETTINGS_RECORD_TIMEZONE, record, sizeof(record), &length) ||
        length != SML_TZ_STATE_BYTES)
    {
        return;
    }
    (void)sml_decode(sml_message(SML_CLASS_TZ, SML_TZ_STATE)->layout, record, &state);
    stats.timezone_restored = timezone_apply(&state) == TIMEZONE_APPLIED;
    (void)timezone_take_changed(); // as stored: nothing to write
}


// The oscillator record: version, flags (0), the Kalman estimate (period -
// nominal, Q16 ticks), and how many epochs it had tracked for.
static void encode_oscillator(int32_t offset_q16, uint32_t epochs, uint8_t *record)
{
    memset(record, 0, SETTINGS_OSCILLATOR_BYTES);
    sml_put_u16(&record[0], SETTINGS_OSCILLATOR_VERSION);
    sml_put_u32(&record[4], (uint32_t)offset_q16);
    sml_put_u32(&record[8], epochs);
}


static bool read_oscillator(int32_t *offset_q16, uint32_t *epochs)
{
    uint8_t record[SETTINGS_OSCILLATOR_BYTES];
    uint16_t length;

    if (!store_read(SETTINGS_RECORD_OSCILLATOR, record, sizeof(record), &length) ||
        length != SETTINGS_OSCILLATOR_BYTES ||
        sml_get_u16(&record[0]) != SETTINGS_OSCILLATOR_VERSION)
    {
        return false;
    }
    *offset_q16 = (int32_t)sml_get_u32(&record[4]);
    *epochs = sml_get_u32(&record[8]);
    return *offset_q16 >= -SETTINGS_OSCILLATOR_LIMIT_Q16 &&
           *offset_q16 <= SETTINGS_OSCILLATOR_LIMIT_Q16;
}


// The loop starts from the stored estimate rather than from nominal.
static void restore_oscillator(void)
{
    uint32_t epochs;

    have_oscillator = read_oscillator(&oscillator_q16, &epochs);
    oscillator_saved = false;
    if (have_oscillator)
    {
        epoch_control_seed(oscillator_q16);
        stats.oscillator_restored = true;
    }
}


static void encode_rtc(const SourceRtcKept *kept, uint8_t *record)
{
    const SourceRtcAnchor *anchor = &kept->anchor;
    int64_t seconds = anchor->offset_ns >= 0 ? anchor->offset_ns / 1000000000LL
                                             : -((-anchor->offset_ns + 999999999LL) / 1000000000LL);
    uint16_t flags = 0;

    if (anchor->valid)
    {
        flags |= SETTINGS_RTC_ANCHOR;
    }
    if (anchor->aligned)
    {
        flags |= SETTINGS_RTC_ALIGNED;
    }
    if (kept->rate_valid)
    {
        flags |= SETTINGS_RTC_RATE;
    }
    if (kept->last_write_valid)
    {
        flags |= SETTINGS_RTC_WRITE;
    }
    if (kept->last_write_aligned)
    {
        flags |= SETTINGS_RTC_WRITE_ALIGNED;
    }
    memset(record, 0, SETTINGS_RTC_BYTES);
    sml_put_u16(&record[0], SETTINGS_RTC_VERSION);
    sml_put_u16(&record[2], flags);
    sml_put_u32(&record[4], anchor->utc_seconds);
    sml_put_u32(&record[8], (uint32_t)(int32_t)seconds);
    sml_put_u32(&record[12], (uint32_t)(anchor->offset_ns - seconds * 1000000000LL));
    sml_put_u32(&record[16], anchor->bound_ns);
    sml_put_u32(&record[20], (uint32_t)kept->rate_q8);
    sml_put_u32(&record[24], kept->last_write_utc);
}


static bool read_rtc(SourceRtcKept *kept)
{
    uint8_t record[SETTINGS_RTC_BYTES];
    uint16_t length;
    uint16_t flags;
    uint32_t fraction;

    if (!store_read(SETTINGS_RECORD_RTC, record, sizeof(record), &length) ||
        length != SETTINGS_RTC_BYTES || sml_get_u16(&record[0]) != SETTINGS_RTC_VERSION)
    {
        return false;
    }
    flags = sml_get_u16(&record[2]);
    fraction = sml_get_u32(&record[12]);
    if (fraction >= 1000000000UL)
    {
        return false;
    }
    memset(kept, 0, sizeof(*kept));
    kept->anchor.valid = (flags & SETTINGS_RTC_ANCHOR) != 0U;
    kept->anchor.aligned = (flags & SETTINGS_RTC_ALIGNED) != 0U;
    kept->anchor.utc_seconds = sml_get_u32(&record[4]);
    kept->anchor.offset_ns =
        (int64_t)(int32_t)sml_get_u32(&record[8]) * 1000000000LL + (int64_t)fraction;
    kept->anchor.bound_ns = sml_get_u32(&record[16]);
    kept->rate_valid = (flags & SETTINGS_RTC_RATE) != 0U;
    kept->rate_q8 = (int32_t)sml_get_u32(&record[20]);
    kept->last_write_valid = (flags & SETTINGS_RTC_WRITE) != 0U;
    kept->last_write_aligned = (flags & SETTINGS_RTC_WRITE_ALIGNED) != 0U;
    kept->last_write_utc = sml_get_u32(&record[24]);
    return true;
}


// The RTC source picks up where it was: its time and bound from the last
// measurement, at the rate learnt.
static void restore_rtc(void)
{
    SourceRtcKept kept;

    rtc_saved = false;
    rtc_retry = false;
    if (read_rtc(&kept))
    {
        source_rtc_restore(&kept);
        stats.rtc_restored = true;
    }
    rtc_saved_changes = source_rtc_state()->kept_changes;
    rtc_saved_rtc_changes = source_rtc_state()->rtc_changes;
}


void settings_init(void)
{
    memset(&stats, 0, sizeof(stats));
    memset(&running, 0, sizeof(running));
    save_requested = false;
    save_error = SML_ERROR_NONE;
    timezone_pending = false;
    revision = 0;

    store_init();
    settings_defaults(&running.body);
    running.debug_mask = DEBUG_NONE;
    stats.loaded = read_record(&running, &revision, &stats.load_rejected);
    have_record = stats.loaded;
    stored = running;
    apply();
    // A build for the bench (DEBUG_DEFAULT) adds its own on top.
    debug_set((uint16_t)(running.debug_mask | DEBUG_DEFAULT));
    restore_timezone();
    restore_oscillator();
    restore_rtc();
}


void settings_body(SML_SETTINGS_BODY *body)
{
    *body = running.body;
}


uint16_t settings_debug_mask(void)
{
    return running.debug_mask;
}


uint32_t settings_revision(void)
{
    return revision;
}


uint16_t settings_flags(void)
{
    SML_SETTINGS_BODY defaults;
    uint16_t flags = SML_SETTINGS_VALID;

    settings_defaults(&defaults);
    if (!same_body(&running.body, &stored.body) || running.debug_mask != stored.debug_mask)
    {
        flags |= SML_SETTINGS_DIRTY;
    }
    if (same_body(&running.body, &defaults))
    {
        flags |= SML_SETTINGS_DEFAULTS;
    }
    return flags;
}


void settings_stat(SML_STAT_SETTINGS_PAYLOAD *payload)
{
    memset(payload, 0, sizeof(*payload));
    payload->flags = settings_flags();
    payload->revision = revision;
    payload->settings = running.body;
}


uint8_t settings_write(const SML_SETTINGS_BODY *body)
{
    uint8_t error = settings_check(body);

    if (error != SML_ERROR_NONE)
    {
        return error;
    }
    if (!same_body(body, &running.body))
    {
        running.body = *body;
        revision++;
        apply();
    }
    return SML_ERROR_NONE;
}


void settings_set_debug(uint16_t mask)
{
    running.debug_mask = mask & DEBUG_ALL;
    debug_set(running.debug_mask);
}


void settings_restore_defaults(void)
{
    SML_SETTINGS_BODY defaults;

    settings_defaults(&defaults);
    (void)settings_write(&defaults);
    settings_set_debug(DEBUG_NONE);
}


void settings_save(void)
{
    save_requested = true;
    oscillator_requested = true;
}


bool settings_save_pending(void)
{
    return save_requested;
}


uint8_t settings_save_error(void)
{
    return save_error;
}


static uint8_t store_error(StoreResult result)
{
    switch (result)
    {
        case STORE_OK: return SML_ERROR_NONE;
        case STORE_VERIFY: return SML_ERROR_VERIFY;
        default: return SML_ERROR_STORAGE;
    }
}


static void save_settings(void)
{
    uint8_t record[SETTINGS_RECORD_BYTES];

    save_requested = false;
    if ((settings_flags() & SML_SETTINGS_DIRTY) == 0U && have_record)
    {
        save_error = SML_ERROR_NONE; // already as stored
        return;
    }
    encode_record(&running, revision, record);
    save_error = store_error(store_write(SETTINGS_RECORD_SETTINGS, record, sizeof(record)));
    if (save_error == SML_ERROR_NONE)
    {
        stored = running;
        have_record = true;
        stats.saves++;
    }
    else
    {
        stats.save_failures++;
    }
}


static void save_timezone(uint32_t epoch)
{
    uint8_t record[SML_TZ_STATE_BYTES];
    SML_TZ_STATE_PAYLOAD state;

    timezone_pending = false;
    if (!timezone_state(&state))
    {
        return;
    }
    (void)sml_encode(sml_message(SML_CLASS_TZ, SML_TZ_STATE)->layout, &state, record);
    if (store_write(SETTINGS_RECORD_TIMEZONE, record, sizeof(record)) == STORE_OK)
    {
        stats.timezone_saves++;
    }
    else
    {
        // Again after the delay.
        stats.save_failures++;
        timezone_pending = true;
        timezone_changed_epoch = epoch;
    }
}


// The loop's frequency estimate is worth keeping once it has been learning
// from a reference that teaches it (GNSS) for a while, and has moved from
// what is stored; after a write this run, not again for an hour. Asked for
// (a save): whenever it differs at all.
static bool oscillator_due(uint32_t epoch, bool asked)
{
    const EpochControl *control = epoch_control_state();
    int32_t estimate;
    int32_t change;

    if (control->state != EPOCH_STATE_TRACK || !control->profile->learn_frequency ||
        !control->pps_kf.initialized ||
        control->reference_epochs < SETTINGS_OSCILLATOR_TRACK_EPOCHS)
    {
        return false;
    }
    if (!have_oscillator)
    {
        return true;
    }
    estimate = kalman_phase_q16(&control->pps_kf);
    change = estimate > oscillator_q16 ? estimate - oscillator_q16 : oscillator_q16 - estimate;
    if (asked)
    {
        return change != 0;
    }
    return change >= SETTINGS_OSCILLATOR_CHANGE_Q16 &&
           (!oscillator_saved || epoch - oscillator_saved_epoch >= SETTINGS_OSCILLATOR_EPOCHS);
}


static void save_oscillator(uint32_t epoch)
{
    const EpochControl *control = epoch_control_state();
    uint8_t record[SETTINGS_OSCILLATOR_BYTES];
    int32_t estimate = kalman_phase_q16(&control->pps_kf);

    encode_oscillator(estimate, control->reference_epochs, record);
    // Tried again in an hour if it fails.
    oscillator_saved = true;
    oscillator_saved_epoch = epoch;
    if (store_write(SETTINGS_RECORD_OSCILLATOR, record, sizeof(record)) == STORE_OK)
    {
        have_oscillator = true;
        oscillator_q16 = estimate;
        stats.oscillator_saves++;
    }
    else
    {
        stats.save_failures++;
    }
}


// The RTC record is out of date: at once if the RTC has been written since
// (it describes another setting), else at most every SETTINGS_RTC_EPOCHS, so
// the measurement kept is never much older than the latest.
static bool rtc_due(uint32_t epoch)
{
    const SourceRtcState *rtc = source_rtc_state();

    if (rtc->kept_changes == rtc_saved_changes)
    {
        return false;
    }
    if (rtc_retry && epoch - rtc_retry_epoch < SETTINGS_RTC_RETRY_EPOCHS)
    {
        return false;
    }
    return rtc->rtc_changes != rtc_saved_rtc_changes || !rtc_saved ||
           epoch - rtc_saved_epoch >= SETTINGS_RTC_EPOCHS;
}


static void save_rtc(uint32_t epoch)
{
    const SourceRtcState *rtc = source_rtc_state();
    uint8_t record[SETTINGS_RTC_BYTES];
    SourceRtcKept kept;
    uint32_t changes = rtc->kept_changes;
    uint32_t rtc_changes = rtc->rtc_changes;

    source_rtc_kept(&kept);
    encode_rtc(&kept, record);
    if (store_write(SETTINGS_RECORD_RTC, record, sizeof(record)) == STORE_OK)
    {
        rtc_saved_changes = changes;
        rtc_saved_rtc_changes = rtc_changes;
        rtc_saved = true;
        rtc_saved_epoch = epoch;
        rtc_retry = false;
        stats.rtc_saves++;
    }
    else
    {
        rtc_retry = true;
        rtc_retry_epoch = epoch;
        stats.save_failures++;
    }
}


void settings_service(uint32_t epoch, bool quiet)
{
    if (timezone_take_changed())
    {
        timezone_pending = true;
        timezone_changed_epoch = epoch;
    }
    if (!quiet)
    {
        return;
    }
    // One record an epoch at most: each may erase a page.
    if (save_requested)
    {
        save_settings();
    }
    else if (timezone_pending && epoch - timezone_changed_epoch >= SETTINGS_TIMEZONE_DELAY_EPOCHS)
    {
        save_timezone(epoch);
    }
    else if (rtc_due(epoch))
    {
        save_rtc(epoch);
    }
    else
    {
        bool asked = oscillator_requested;

        oscillator_requested = false;
        if (oscillator_due(epoch, asked))
        {
            save_oscillator(epoch);
        }
    }
}


const SettingsStats *settings_stats(void)
{
    return &stats;
}


// The display modes a setting can select, by SML_DISPLAY_* value.
static const char *display_name(uint8_t selected)
{
    return SML_DISPLAY_SELECTABLE(selected) ? display_mode_name(display_mode_of(selected)) : "?";
}


static const char *on_off(bool on)
{
    return on ? "ON" : "OFF";
}


// Each setting: AUTO or MANUAL with the manual value kept.
static void print_values(const Values *values)
{
    const SML_SETTINGS_BODY *body = &values->body;
    uint16_t flags = body->flags;

    printf("  TZ: base %s (", (flags & SML_SETTING_TZ_AUTO) ? "AUTO" : "MANUAL");
    utc_print_offset(body->base_offset_s);
    printf("), DST %s (", (flags & SML_SETTING_DST_AUTO) ? "AUTO" : "MANUAL");
    utc_print_offset(body->dst_offset_s);
    printf(" %s)\r\n", on_off((flags & SML_SETTING_DST_ACTIVE) != 0U));
    printf("  Display: %s, %s h", display_name(body->selected_display),
           (flags & SML_SETTING_FORMAT_12H) ? "12" : "24");
    if (body->delta_epoch_utc != 0U)
    {
        printf(", delta from ");
        display_print_epoch(body->delta_epoch_utc, (flags & SML_SETTING_DELTA_LOCAL) != 0U);
    }
    printf("; brightness %s (%u); beeps %s; alarm %s (%02lu:%02lu:%02lu)\r\n",
           (flags & SML_SETTING_BRI_AUTO) ? "AUTO" : "MANUAL", body->brightness,
           on_off((flags & SML_SETTING_BEEP_ENABLED) != 0U),
           on_off((flags & SML_SETTING_ALARM_ENABLED) != 0U),
           (unsigned long)(body->alarm_offset_s / 3600U),
           (unsigned long)(body->alarm_offset_s / 60U % 60U),
           (unsigned long)(body->alarm_offset_s % 60U));
    printf("  Debug: ");
    debug_print_names(values->debug_mask);
    printf("\r\n");
}


// Q16 ticks a second to parts per billion of FCY, to the nearest.
static int32_t ppb(int32_t offset_q16)
{
    const int64_t divisor = (int64_t)FCY * 65536LL;
    int64_t scaled = (int64_t)offset_q16 * 1000000000LL;

    scaled += scaled < 0 ? -divisor / 2 : divisor / 2;
    return (int32_t)(scaled / divisor);
}


// The oscillator's frequency for an estimate (period - nominal, Q16 ticks a
// second): "39999920.512 Hz (-1.988 ppm)".
static void print_frequency(int32_t offset_q16)
{
    // Positive throughout (FCY dwarfs any offset), so the division rounds.
    int64_t millihertz =
        ((int64_t)FCY * 65536000LL + (int64_t)offset_q16 * 1000LL + 32768LL) / 65536LL;
    int32_t parts = ppb(offset_q16);
    uint32_t magnitude = (uint32_t)(parts < 0 ? -parts : parts);

    printf("%lu.%03u Hz (%c%lu.%03u ppm)", (unsigned long)(millihertz / 1000LL),
           (unsigned)(millihertz % 1000LL), parts < 0 ? '-' : '+',
           (unsigned long)(magnitude / 1000U), (unsigned)(magnitude % 1000U));
}


void settings_print(bool verbose)
{
    uint16_t flags = settings_flags();

    printf("Settings: %s%s", (flags & SML_SETTINGS_DIRTY) ? "NOT SAVED" : "saved",
           (flags & SML_SETTINGS_DEFAULTS) ? ", DEFAULTS" : "");
    if (verbose)
    {
        printf(", revision %lu", (unsigned long)revision);
    }
    printf("\r\n");
    print_values(&running);
}


// How long erasing and writing took: the CPU is stopped meanwhile.
static void print_flash(void)
{
    const FlashStats *flash = flash_stats();

    printf("  Flash: %lu erases (last ", (unsigned long)flash->erases);
    utc_print_ns((int64_t)(flash->last_erase_ticks / TICKS_PER_US) * 1000, false);
    printf(", longest ");
    utc_print_ns((int64_t)(flash->longest_erase_ticks / TICKS_PER_US) * 1000, false);
    printf("), %lu double words written (longest ", (unsigned long)flash->writes);
    utc_print_ns((int64_t)(flash->longest_write_ticks / TICKS_PER_US) * 1000, false);
    printf("), %lu failures\r\n", (unsigned long)flash->failures);
}


static void print_stored_timezone(void)
{
    uint8_t record[SML_TZ_STATE_BYTES];
    SML_TZ_STATE_PAYLOAD state;
    uint16_t length;

    printf("Time zone: ");
    if (!store_read(SETTINGS_RECORD_TIMEZONE, record, sizeof(record), &length) ||
        length != SML_TZ_STATE_BYTES)
    {
        printf("NONE");
    }
    else
    {
        (void)sml_decode(sml_message(SML_CLASS_TZ, SML_TZ_STATE)->layout, record, &state);
        printf("%s, issued ", timezone_source_name(state.source));
        utc_print(state.issued_utc);
        printf(", %u entries", state.count);
        if (state.horizon_end_utc != UTC_NEVER)
        {
            printf(", until ");
            utc_print(state.horizon_end_utc);
        }
    }
    printf("%s\r\n", timezone_pending ? " (newer one PENDING)" : "");
}


static void print_stored_oscillator(const char *label)
{
    int32_t offset_q16;
    uint32_t epochs;

    printf("%s", label);
    if (read_oscillator(&offset_q16, &epochs))
    {
        print_frequency(offset_q16);
        printf(", over ");
        utc_print_duration(epochs);
        printf("\r\n");
    }
    else
    {
        printf("NONE\r\n");
    }
}


static void print_stored_rtc(void)
{
    SourceRtcKept kept;

    printf("RTC: ");
    if (!read_rtc(&kept))
    {
        printf("NONE\r\n");
        return;
    }
    if (kept.anchor.valid)
    {
        printf("from ");
        utc_print(kept.anchor.utc_seconds);
        printf("%s, +/-", kept.anchor.aligned ? "" : " (PLAIN)");
        utc_print_ns(kept.anchor.bound_ns, false);
    }
    else
    {
        printf("NOT MEASURED");
    }
    if (kept.rate_valid)
    {
        int32_t ppm10 = -(kept.rate_q8 / 1024); // as source_rtc_rate_ppm10()
        int32_t size = ppm10 < 0 ? -ppm10 : ppm10;
        printf(", rate %c%ld.%ld ppm", ppm10 < 0 ? '-' : '+', (long)(size / 10), (long)(size % 10));
    }
    if (kept.last_write_valid)
    {
        printf(", set ");
        utc_print(kept.last_write_utc);
    }
    printf("\r\n");
}


void settings_print_stored(bool verbose)
{
    static const char *const record_names[STORE_TYPES] = {NULL, "settings", "time zone",
                                                          "oscillator", "RTC"};
    Values values;
    uint32_t record_revision = 0;
    bool rejected;

    if (read_record(&values, &record_revision, &rejected))
    {
        printf("Settings: stored");
        if (verbose)
        {
            printf(", revision %lu", (unsigned long)record_revision);
        }
        printf("\r\n");
        print_values(&values);
    }
    else
    {
        printf("Settings: %s\r\n", rejected ? "UNUSABLE record, DEFAULTS" : "NONE, DEFAULTS");
    }
    print_stored_timezone();
    print_stored_oscillator("Oscillator: ");
    print_stored_rtc();
    store_print(record_names, verbose);
    if (verbose)
    {
        print_flash();
    }
}


// The loop's estimate now, and what is stored for the next start.
void settings_print_oscillator(bool verbose)
{
    const EpochControl *control = epoch_control_state();

    printf("Oscillator: ");
    if (control->pps_kf.initialized)
    {
        print_frequency(kalman_phase_q16(&control->pps_kf));
        printf(", measured\r\n");
    }
    else
    {
        printf("not measured (nominal %lu Hz)\r\n", (unsigned long)FCY);
    }
    print_stored_oscillator("  Stored: ");
    if (verbose)
    {
        printf("  Baseline setting: %lu Hz (unused)\r\n",
               (unsigned long)running.body.calibration_fosc_hz);
    }
}


void settings_print_start(bool detail)
{
    const TimezoneSchedule *schedule = timezone_schedule();

    if (!detail && !stats.load_rejected)
    {
        return;
    }

    printf("Settings: %s, revision %lu",
           stats.loaded          ? "from flash"
           : stats.load_rejected ? "stored record unusable, defaults"
                                 : "defaults (nothing stored)",
           (unsigned long)revision);
    if (stats.timezone_restored)
    {
        printf("; time zone from flash (%s, %u entries)", timezone_source_name(schedule->source),
               schedule->count);
    }
    if (stats.oscillator_restored)
    {
        printf("; oscillator from flash, ");
        print_frequency(oscillator_q16);
    }
    if (stats.rtc_restored)
    {
        printf("; RTC measurement from flash");
    }
    printf("\r\n");
}
