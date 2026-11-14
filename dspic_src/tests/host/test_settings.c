/*
 * The settings (settings/settings.c) at start-up and when the flash lets
 * them down: an unusable record, a build's own debug mask, a failed save,
 * and the debug mask's own rules; the oscillator's and the RTC's records. The store and the time zone module are
 * the real ones, the flash a model (fake_flash); the tubes and the display
 * are stand-ins.
 */
#include <stdio.h>
#include <string.h>

#include "../../../common/sml.h"
#include "../../drivers/tubes.h"
#include "../../settings/settings.h"
#include "../../settings/store.h"
#include "../../console/debug.h"
#include "../../ui/alarm.h"
#include "../../ui/display.h"
#include "../../ui/timezone.h"
#include "fake_control.h"
#include "fake_flash/fake_flash.h"
#include "fake_source_rtc.h"

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


void display_set_mode(DisplayMode mode)
{
    (void)mode;
}


void alarm_set(bool enabled, uint32_t time_of_day_s)
{
    (void)enabled;
    (void)time_of_day_s;
}


void display_set_12h(bool twelve_hours)
{
    (void)twelve_hours;
}


void display_set_delta_epoch(uint32_t seconds, bool local)
{
    (void)seconds;
    (void)local;
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


// console/text.c's output (debug_print).
void console_write(const char *text, uint16_t length)
{
    (void)text;
    (void)length;
}


static void restart(void)
{
    timezone_init();
    debug_init();
    settings_init();
}


int main(void)
{
    SML_SETTINGS_BODY body;
    uint8_t record[SETTINGS_RECORD_BYTES];
    uint32_t epoch = 1;

    // Nothing stored: the defaults, not dirty.
    fake_flash_wipe(0xFFFFU);
    restart();
    CHECK(!settings_stats()->loaded && !settings_stats()->load_rejected);
    CHECK(settings_flags() == (SML_SETTINGS_VALID | SML_SETTINGS_DEFAULTS));
    settings_body(&body);
    CHECK(body.brightness == TUBES_BRIGHTNESS_DEFAULT && body.calibration_fosc_hz == 40000000UL);
    // Saving the defaults still writes them: a record to come back to.
    settings_save();
    settings_service(epoch++, true);
    CHECK(settings_save_error() == SML_ERROR_NONE && settings_stats()->saves == 1);
    restart();
    CHECK(settings_stats()->loaded);

    // A record from a later firmware (another version): not used.
    memset(record, 0, sizeof record);
    sml_put_u16(record, SETTINGS_RECORD_VERSION + 1U);
    CHECK(store_write(SETTINGS_RECORD_SETTINGS, record, sizeof record) == STORE_OK);
    restart();
    CHECK(!settings_stats()->loaded && settings_stats()->load_rejected);
    CHECK(settings_flags() == (SML_SETTINGS_VALID | SML_SETTINGS_DEFAULTS));

    // A record whose values do not check out (brightness past full): not used.
    {
        SML_STAT_SETTINGS_PAYLOAD payload;
        memset(&payload, 0, sizeof payload);
        settings_defaults(&payload.settings);
        payload.settings.brightness = 4001;
        payload.revision = 9;
        sml_put_u16(record, SETTINGS_RECORD_VERSION);
        sml_encode(sml_message(SML_CLASS_STAT, SML_STAT_SETTINGS)->layout, &payload, &record[2]);
        CHECK(store_write(SETTINGS_RECORD_SETTINGS, record, sizeof record) == STORE_OK);
        restart();
        CHECK(!settings_stats()->loaded && settings_stats()->load_rejected &&
              settings_revision() == 0);
    }

    // The debug mask: kept, and a build's DEBUG_DEFAULT (none here) added.
    settings_set_debug(DEBUG_GNSS | 0x8000U);
    CHECK(settings_debug_mask() == DEBUG_GNSS && debug_mask() == DEBUG_GNSS);
    CHECK((settings_flags() & SML_SETTINGS_DIRTY) != 0U);
    settings_save();
    settings_service(epoch++, true);
    restart();
    CHECK(settings_stats()->loaded && debug_mask() == DEBUG_GNSS && settings_revision() == 0);

    // The flash failing: the save reports STORAGE, the settings stay dirty,
    // and a later save goes through.
    settings_body(&body);
    body.brightness = 1000;
    CHECK(settings_write(&body) == SML_ERROR_NONE && brightness == 1000 &&
          settings_revision() == 1);
    fake_flash_fail = 1;
    settings_save();
    settings_service(epoch++, true);
    CHECK(!settings_save_pending() && settings_save_error() == SML_ERROR_STORAGE);
    CHECK((settings_flags() & SML_SETTINGS_DIRTY) != 0U && settings_stats()->save_failures == 1);
    fake_flash_fail = 0;
    settings_save();
    settings_service(epoch++, true);
    CHECK(settings_save_error() == SML_ERROR_NONE && (settings_flags() & SML_SETTINGS_DIRTY) == 0U);
    restart();
    settings_body(&body);
    CHECK(body.brightness == 1000 && brightness == 1000 && settings_revision() == 1);

    // Not quiet: nothing is written, however long it waits.
    settings_set_debug(DEBUG_NONE);
    settings_save();
    settings_service(epoch++, false);
    CHECK(settings_save_pending());
    settings_service(epoch++, true);
    CHECK(!settings_save_pending());

    // A time zone schedule that fails to store is tried again after the
    // delay.
    {
        SML_TZ_STATE_PAYLOAD state;
        memset(&state, 0, sizeof state);
        state.flags = SML_TZ_VALID;
        state.source = SML_TZ_SOURCE_GNSS;
        state.issued_utc = 1000;
        state.horizon_end_utc = 0xFFFFFFFFUL;
        state.current.base_offset_s = 3600;
        CHECK(timezone_apply(&state) == TIMEZONE_APPLIED);
        settings_service(epoch, true);
        fake_flash_fail = 1;
        settings_service(epoch + SETTINGS_TIMEZONE_DELAY_EPOCHS, true);
        CHECK(settings_stats()->timezone_saves == 0);
        fake_flash_fail = 0;
        settings_service(epoch + SETTINGS_TIMEZONE_DELAY_EPOCHS + 1U, true);
        CHECK(settings_stats()->timezone_saves == 0); // waits the delay again
        settings_service(epoch + 2U * SETTINGS_TIMEZONE_DELAY_EPOCHS, true);
        CHECK(settings_stats()->timezone_saves == 1);
        restart();
        CHECK(settings_stats()->timezone_restored &&
              timezone_schedule()->entries[0].base_offset_s == 3600);
    }
    // The oscillator: kept once the loop has learnt it from GNSS for long
    // enough, and the next start seeded with it.
    {
        const int32_t estimate = (int32_t)(-76L * 65536L) - 12345L; // -1.9 ppm
        uint8_t record[SETTINGS_OSCILLATOR_BYTES];

        epoch = 5000;
        fake_flash_wipe(0xFFFFU);
        fake_control_seeds = 0;
        restart();
        CHECK(!settings_stats()->oscillator_restored && fake_control_seeds == 0);
        settings_service(epoch++, true); // free running: nothing
        fake_control_tracking(estimate, SETTINGS_OSCILLATOR_TRACK_EPOCHS - 1U);
        settings_service(epoch++, true); // not long enough
        CHECK(settings_stats()->oscillator_saves == 0);
        {
            // A reference that does not teach the filter (NTP): never.
            static const EpochProfile gentle = {"gentle", 0, 0, 0, 0, 0, 0, false};
            fake_control.reference_epochs = SETTINGS_OSCILLATOR_TRACK_EPOCHS;
            fake_control.profile = &gentle;
            settings_service(epoch++, true);
            CHECK(settings_stats()->oscillator_saves == 0);
            fake_control.profile = &epoch_profile_precise;
        }
        settings_service(epoch++, false); // not quiet
        CHECK(settings_stats()->oscillator_saves == 0);
        settings_service(epoch++, true);
        CHECK(settings_stats()->oscillator_saves == 1);
        // Moved by less than the threshold: not again.
        fake_control_tracking(estimate + SETTINGS_OSCILLATOR_CHANGE_Q16 - 1,
                              SETTINGS_OSCILLATOR_TRACK_EPOCHS);
        settings_service(epoch + SETTINGS_OSCILLATOR_EPOCHS, true);
        CHECK(settings_stats()->oscillator_saves == 1);
        // More, but within the hour: not yet; after it, yes.
        fake_control_tracking(estimate + SETTINGS_OSCILLATOR_CHANGE_Q16,
                              SETTINGS_OSCILLATOR_TRACK_EPOCHS);
        settings_service(epoch + SETTINGS_OSCILLATOR_EPOCHS - 2U, true);
        CHECK(settings_stats()->oscillator_saves == 1);
        settings_service(epoch + SETTINGS_OSCILLATOR_EPOCHS, true);
        CHECK(settings_stats()->oscillator_saves == 2);
        // The next start is seeded with it.
        memset(&fake_control, 0, sizeof(fake_control));
        restart();
        CHECK(settings_stats()->oscillator_restored && fake_control_seeds == 1 &&
              fake_control_seed_q16 == estimate + SETTINGS_OSCILLATOR_CHANGE_Q16);
        // ... and a first save in a run only for a change from what is stored.
        fake_control_tracking(estimate + SETTINGS_OSCILLATOR_CHANGE_Q16,
                              SETTINGS_OSCILLATOR_TRACK_EPOCHS);
        settings_service(epoch + 10U, true);
        CHECK(settings_stats()->oscillator_saves == 0);
        // A save (pic-save) writes it too, after the settings, for any
        // change at all, and not again unless asked.
        fake_control_tracking(estimate + SETTINGS_OSCILLATOR_CHANGE_Q16 + 1,
                              SETTINGS_OSCILLATOR_TRACK_EPOCHS);
        settings_service(epoch + 11U, true);
        CHECK(settings_stats()->oscillator_saves == 0);
        settings_save();
        settings_service(epoch + 12U, true); // the settings
        settings_service(epoch + 13U, true); // the oscillator
        CHECK(settings_stats()->oscillator_saves == 1);
        settings_save();
        settings_service(epoch + 14U, true);
        settings_service(epoch + 15U, true); // unchanged: nothing
        CHECK(settings_stats()->oscillator_saves == 1);
        // What osc and pic print for it (by eye: 39999925.812 Hz, -1.855 ppm).
        settings_print(false);
        settings_print_oscillator(true);
        settings_print_stored(true);
        // A stored estimate beyond 100 ppm is not ours: ignored.
        memset(record, 0, sizeof record);
        sml_put_u16(record, SETTINGS_OSCILLATOR_VERSION);
        sml_put_u32(&record[4], (uint32_t)(SETTINGS_OSCILLATOR_LIMIT_Q16 + 1));
        CHECK(store_write(SETTINGS_RECORD_OSCILLATOR, record, sizeof record) == STORE_OK);
        fake_control_seeds = 0;
        restart();
        CHECK(!settings_stats()->oscillator_restored && fake_control_seeds == 0);
    }
    // The RTC's record: written at once after the RTC was written, else at
    // most every SETTINGS_RTC_EPOCHS; given back at the next start.
    {
        SourceRtcKept kept;

        epoch = 9000;
        fake_flash_wipe(0xFFFFU);
        memset(&fake_rtc_state, 0, sizeof fake_rtc_state);
        fake_rtc_restores = 0;
        restart();
        CHECK(!settings_stats()->rtc_restored && fake_rtc_restores == 0);
        settings_service(epoch++, true); // nothing changed: nothing
        CHECK(settings_stats()->rtc_saves == 0);
        // Set aligned and measured.
        memset(&kept, 0, sizeof kept);
        kept.anchor.valid = true;
        kept.anchor.aligned = true;
        kept.anchor.utc_seconds = 1792600000UL;
        kept.anchor.offset_ns = -1234567890123LL; // 1234.6 s behind UTC: whole seconds and ns
        kept.anchor.bound_ns = 300100U;
        kept.rate_valid = true;
        kept.rate_q8 = -1536000; // 150 ppm fast
        kept.last_write_valid = true;
        kept.last_write_aligned = true;
        kept.last_write_utc = 1792599990UL;
        fake_rtc_kept = kept;
        fake_rtc_state.kept_changes = 3;
        fake_rtc_state.rtc_changes = 1;
        settings_service(epoch++, false); // not quiet
        CHECK(settings_stats()->rtc_saves == 0);
        settings_service(epoch++, true);
        CHECK(settings_stats()->rtc_saves == 1);
        // A new measurement, the RTC not written: not for six hours.
        fake_rtc_state.kept_changes = 4;
        settings_service(epoch + SETTINGS_RTC_EPOCHS - 10U, true);
        CHECK(settings_stats()->rtc_saves == 1);
        settings_service(epoch + SETTINGS_RTC_EPOCHS, true);
        CHECK(settings_stats()->rtc_saves == 2);
        // Written again: at once, and a failure tried again a minute on.
        fake_rtc_state.kept_changes = 6;
        fake_rtc_state.rtc_changes = 2;
        fake_flash_fail = 1;
        settings_service(epoch + SETTINGS_RTC_EPOCHS + 1U, true);
        fake_flash_fail = 0;
        CHECK(settings_stats()->rtc_saves == 2);
        settings_service(epoch + SETTINGS_RTC_EPOCHS + 2U, true);
        CHECK(settings_stats()->rtc_saves == 2);
        settings_service(epoch + SETTINGS_RTC_EPOCHS + 1U + SETTINGS_RTC_RETRY_EPOCHS, true);
        CHECK(settings_stats()->rtc_saves == 3);
        // The next start gets it back as it was.
        memset(&fake_rtc_restored, 0, sizeof fake_rtc_restored);
        restart();
        CHECK(settings_stats()->rtc_restored && fake_rtc_restores == 1);
        CHECK(fake_rtc_restored.anchor.valid && fake_rtc_restored.anchor.aligned &&
              fake_rtc_restored.anchor.utc_seconds == kept.anchor.utc_seconds &&
              fake_rtc_restored.anchor.offset_ns == kept.anchor.offset_ns &&
              fake_rtc_restored.anchor.bound_ns == kept.anchor.bound_ns);
        CHECK(fake_rtc_restored.rate_valid && fake_rtc_restored.rate_q8 == kept.rate_q8 &&
              fake_rtc_restored.last_write_valid && fake_rtc_restored.last_write_aligned &&
              fake_rtc_restored.last_write_utc == kept.last_write_utc);
        // Unchanged since the start: nothing written.
        settings_service(epoch + 20000U, true);
        CHECK(settings_stats()->rtc_saves == 0);
        // A plain setting, no rate, positive offset.
        kept.anchor.aligned = false;
        kept.anchor.offset_ns = 999999999LL;
        kept.rate_valid = false;
        kept.last_write_aligned = false;
        fake_rtc_kept = kept;
        fake_rtc_state.rtc_changes = 3;
        fake_rtc_state.kept_changes = 7;
        settings_service(epoch + 20001U, true);
        restart();
        CHECK(!fake_rtc_restored.anchor.aligned &&
              fake_rtc_restored.anchor.offset_ns == 999999999LL && !fake_rtc_restored.rate_valid &&
              !fake_rtc_restored.last_write_aligned);
        settings_print_stored(false); // by eye
    }
    CHECK(fake_flash_violations == 0);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
