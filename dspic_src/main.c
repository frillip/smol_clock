/*
 * Smol Clock V2+: disciplines the T2/T3 epoch timer, and with it the OC1
 * epoch output, to the best available time source, shows local time on the
 * tubes and reports every second to the ESP32 and the console.
 *
 * Where things are: the task list below and timing/scheduler.h; source selection
 * and time of day in sync/ (sync/sync.h); the timing hardware and control
 * loop in timing/ (timing/timing_config.h); the GNSS receiver link in gnss/
 * (gnss/gnss.h); the ESP32 link in esp/ (esp/esp_link.h, SML protocol); the
 * tubes in ui/display.h and local time in ui/timezone.h; the I2C bus and the
 * RTC in drivers/ (drivers/i2c1.h, drivers/pcf8563.h), and the RTC as a time
 * source, kept set, in sync/source_rtc.h; the light sensor in
 * drivers/veml6040.h and automatic brightness in ui/brightness.h; the settings, and keeping them in
 * flash, in settings/ (settings/settings.h, drivers/flash.h).
 */
#include <stdbool.h>
#include <stdio.h>

#include "console/console.h"
#include "console/debug.h"
#include "drivers/buzzer.h"
#include "drivers/flash.h"
#include "drivers/i2c1.h"
#include "drivers/pcf8563.h"
#include "drivers/veml6040.h"
#include "esp/esp_link.h"
#include "esp/esp_report.h"
#include "gnss/gnss.h"
#include "menu/menu.h"
#include "platform/boot.h"
#include "platform/fault.h"
#include "platform/system.h"
#include "settings/settings.h"
#include "sync/source_ntp.h"
#include "sync/source_rtc.h"
#include "sync/sync.h"
#include "sync/sync_report.h"
#include "sync/sync_tuning.h"
#include "timing/epoch_control.h"
#include "timing/scheduler.h"
#include "timing/timing.h"
#include "timing/timing_report.h"
#include "ui/alarm.h"
#include "ui/beeps.h"
#include "ui/brightness.h"
#include "ui/controls.h"
#include "ui/display.h"
#include "ui/timezone.h"

// Full diagnostics, once a minute each, in different epochs, late in the
// second: they use printf, which costs ~1.5 ms a call here (70-80 ms a
// report), so they stay clear of the epoch work and the GNSS burst that
// follows each navigation solution. The per-epoch lines use console/text.h.
// Which are printed is up to console/debug.h.
#define REPORT_SLOT            600U
#define FULL_REPORT_EPOCHS     60U
#define SCHEDULER_REPORT_EPOCH 15U // ... at this epoch of each minute
#define GNSS_REPORT_EPOCH      30U
#define SYNC_REPORT_EPOCH      45U
#define TUNING_REPORT_EPOCH    55U

// ---- The task list --------------------------------------------------------
// Periodic tasks keep their phase on the epoch timeline; slot tasks run at a
// fixed point in every epoch. See timing/scheduler.h.

// The UART2 ring holds ~44 ms of GNSS data at 115200, the UART1 ring ~22 ms
// of ESP data.
static SchedulerTask every_10ms = SCHEDULER_EVERY("10 ms", 10U);
// After the epoch pulse has closed the tube drivers' latch.
static SchedulerTask display_load = SCHEDULER_AT_SLOT("display load", DISPLAY_LOAD_SLOT);
// The full reports, clear of the epoch work and the GNSS burst.
static SchedulerTask minute_report = SCHEDULER_AT_SLOT("minute report", REPORT_SLOT);
// Settings and the time zone schedule to flash, late in the second.
static SchedulerTask settings_store = SCHEDULER_AT_SLOT("settings store", SETTINGS_STORE_SLOT);
// The epoch's work, started by timing_snapshot(): timed only.
static SchedulerTask epoch_work = SCHEDULER_SECTION("epoch");

// The GNSS receiver link, with the latest PPS edge for labelling: as
// watched, so labelling carries on whatever the capture is routed to.
static void service_gnss(void)
{
    GnssPpsReference reference;
    PpsEdge edge;

    pps_monitor_edge(PPS_INPUT_GNSS, &edge);
    reference.sequence = edge.sequence;
    reference.observed_ms = edge.observed_ms;
    reference.valid = edge.valid;
    gnss_service(scheduler_now_ms(), reference);
}


// The clock's time, for the RTC's printed offset.
static Pcf8563Reference rtc_reference(void)
{
    const SyncStatus *status = sync_status();
    Pcf8563Reference reference;

    reference.utc_seconds = status->time.utc_seconds;
    reference.known = status->time.known;
    return reference;
}


// Writing flash stops the CPU for up to ~25 ms (drivers/flash.h): only well
// before the next epoch, with nothing on the I2C bus and the RTC source not
// timing reads or a release. The GNSS burst and the ESP's per-second
// messages are over by this slot.
static bool flash_quiet(void)
{
    return scheduler_slot() <= SETTINGS_STORE_LAST_SLOT && i2c1_idle() &&
           source_rtc_state()->activity == SOURCE_RTC_IDLE;
}


// The alarm dot follows the alarm, and its changes are printed with
// DEBUG_EVENTS.
static void report_alarm(void)
{
    static AlarmState reported = ALARM_OFF;
    AlarmState state = alarm_state();

    display_set_alarm_dot(alarm_is_set());
    if (state != reported)
    {
        reported = state;
        if (debug_enabled(DEBUG_EVENTS))
        {
            printf("Alarm: %s\r\n", alarm_state_name(state));
        }
    }
}


// The ESP, for the menu: its commands, and its remote time zone setting.
static bool menu_remote_tz(uint32_t now_ms, bool *enabled)
{
    SML_STAT_NET_PAYLOAD net;

    if (!esp_link_net(now_ms, &net))
    {
        return false;
    }
    *enabled = (net.flags & SML_NET_TZINFO_ENABLED) != 0U;
    return true;
}


static const MenuEsp menu_esp = {esp_link_request, menu_remote_tz, esp_link_is_sml};

/* The button and the switch, debounced. The switch arms the alarm, and a
 * press while it sounds stops it and does nothing else. In the menu, the
 * menu times the presses itself. On the clock, a click for each press; a
 * short one shows the next display mode, a long one opens the menu. Each
 * press and switch change is printed with DEBUG_EVENTS. */
static void service_controls(void)
{
    static uint32_t presses;
    static uint32_t switch_changes;
    static bool was_down;
    static bool spent;      // this press stopped the alarm
    static bool menu_press; // ... or began in the menu: the clock ignores it
    uint32_t now_ms = scheduler_now_ms();
    uint32_t count;
    uint32_t epoch;
    uint16_t slot;
    ControlsPress press;
    bool down;
    bool new_press;

    controls_service(now_ms, UI_BUTTON_IS_PRESSED(), UI_SWITCH_IS_ON());
    alarm_set_switch(controls_switch_on());
    down = controls_button_down();
    if (down && !was_down)
    {
        spent = alarm_press();
        menu_press = menu_is_open();
    }
    was_down = down;
    press = controls_press(&count);
    new_press = count != presses;
    presses = count;
    epoch_timer_position(&epoch, &slot);
    menu_service(now_ms, down && !spent, epoch, slot);
    if (new_press && !spent && !menu_press && !menu_is_open())
    {
        beeps_button();
        if (press == CONTROLS_SHORT)
        {
            display_cycle();
        }
        else if (press == CONTROLS_LONG)
        {
            menu_open(now_ms); // ignores this press until it is released
        }
    }
    if (new_press && debug_enabled(DEBUG_EVENTS))
    {
        printf("Button: %s\r\n", controls_press_name(press));
    }
    if (controls_switch_changes() != switch_changes)
    {
        switch_changes = controls_switch_changes();
        if (debug_enabled(DEBUG_EVENTS))
        {
            printf("Switch: %s\r\n", controls_switch_on() ? "ON" : "OFF");
        }
    }
    report_alarm();
}


// The chimes and the alarm, at the start of the second that has just
// begun: local time, as the display shows it.
static void sound_epoch(void)
{
    const SyncTime *time = &sync_status()->time;
    TimezoneOffset zone;
    uint32_t local;

    timezone_offset_at(time->utc_seconds, &zone);
    local = time->utc_seconds + (uint32_t)zone.offset_s;
    alarm_epoch(time->known, local);
    beeps_epoch(time->known, time->leap_second, local);
}


// The time of day for the display, as the synchroniser keeps it.
static void prepare_display(void)
{
    const SyncStatus *status = sync_status();
    DisplayClock clock;

    clock.known = status->time.known;
    clock.synchronised = sync_is_synchronised();
    clock.utc_seconds = status->time.utc_seconds;
    clock.leap_second = status->time.leap_second;
    clock.leap = status->time.leap;
    clock.uncertainty_ns = 0;
    clock.uncertainty_valid = sync_uncertainty(&clock.uncertainty_ns);
    display_prepare(&clock);
}


// Once per epoch, 1 ms after it: measure, steer, label the second, tell the
// ESP, render the next second, and report.
static void run_epoch(const TimingSnapshot *snapshot)
{
    SyncUpdate update;
    uint32_t bound_ns;

    // The watchdog fed: the epoch timer and the main loop are both going.
    fault_epoch(scheduler_epoch());
    boot_service(scheduler_epoch()); // healthy running: the boot-loop guard's count zeroed
    source_rtc_set_oscillator(kalman_phase_q16(&epoch_control_state()->pps_kf));
    update = sync_update(snapshot, epoch_timer_ms());
    sound_epoch();
    esp_link_epoch(snapshot, &update, epoch_timer_ms());
    if (!sync_uncertainty(&bound_ns))
    {
        bound_ns = SYNC_BOUND_NONE;
    }
    source_rtc_epoch(snapshot, &sync_status()->time, sync_is_synchronised(), bound_ns);
    prepare_display();
    menu_epoch();
    sync_tuning_epoch(snapshot, &update);

    if (debug_enabled(DEBUG_TIMING))
    {
        timing_report_epoch(snapshot, &update.loop);
    }
    if (debug_enabled(DEBUG_GNSS))
    {
        gnss_print_edge(snapshot->monitor.inputs[PPS_INPUT_GNSS].sequence, epoch_timer_ms());
    }
    if (debug_enabled(DEBUG_NTP))
    {
        source_ntp_print_epoch(snapshot, epoch_timer_ms());
    }
    if (debug_enabled(DEBUG_SYNC | DEBUG_EVENTS))
    {
        sync_report_transition(&update);
    }
    if (debug_enabled(DEBUG_SYNC))
    {
        sync_report_epoch(&update);
    }
    if (debug_enabled(DEBUG_SOURCES))
    {
        sync_report_sources();
    }
}


// One of the full reports, by the epoch's place in the minute.
static void report_minute(void)
{
    uint32_t minute_epoch = scheduler_epoch() % FULL_REPORT_EPOCHS;

    if (minute_epoch == TUNING_REPORT_EPOCH && debug_enabled(DEBUG_TUNING))
    {
        sync_tuning_print();
    }
    if (!debug_enabled(DEBUG_MINUTE))
    {
        return;
    }
    if (minute_epoch == SCHEDULER_REPORT_EPOCH)
    {
        scheduler_print();
    }
    if (minute_epoch == GNSS_REPORT_EPOCH)
    {
        gnss_print(epoch_timer_ms(), true);
    }
    if (minute_epoch == SYNC_REPORT_EPOCH)
    {
        // In full: these are diagnostics.
        sync_report_status(true);
        display_print();
        timezone_print(sync_status()->time.utc_seconds, true);
        pcf8563_print(epoch_timer_ms(), rtc_reference(), true);
        source_rtc_print(true);
        veml6040_print(epoch_timer_ms(), true);
        brightness_print(true);
        controls_print(true);
        alarm_print(true);
        i2c1_print();
        esp_link_print();
    }
}


int main(void)
{
    PpsCaptureSync capture_sync;

    system_init();
    console_init(FCY);
    esp_link_init();
    printf("\033[2J\033[1;1H");
    esp_report_print_system(false); // the version and why the PIC (re)started, and a fault

    debug_init();
    sync_init();
    sync_tuning_init();
    timing_init(&capture_sync);
    gnss_init(FCY, epoch_timer_ms());
    // The RTC: its reads stamped on the capture timebase, so the RTC source
    // can place its second against the epoch. The light sensor on the same
    // bus.
    i2c1_init();
    i2c1_set_stamp_clock(pps_capture_timebase);
    pcf8563_init(epoch_timer_ms());
    veml6040_init();
    source_rtc_init();
    timezone_init();
    display_init();
    brightness_init();
    controls_init();
    buzzer_init();
    beeps_init();
    alarm_init();
    menu_init(&menu_esp);
    // The settings and the time zone schedule kept in flash, applied to the
    // modules above. Erases and writes are timed on the capture timebase,
    // and the epoch timer's slots put right after each.
    if (!flash_init())
    {
        printf("Flash: the settings pages are not at 0x%05lX: nothing can be saved\r\n",
               (unsigned long)FLASH_STORE_BASE);
    }
    flash_set_clock(pps_capture_timebase);
    flash_set_resume_hook(epoch_timer_resume);
    settings_init();
    // Now the saved debug mask is in force.
    timing_report_sync(&capture_sync, debug_enabled(DEBUG_TIMING));
    settings_print_start(debug_enabled(DEBUG_EVENTS));
    scheduler_set_task_hook(fault_task);
    scheduler_init();
    // From here the watchdog is fed every epoch (run_epoch()).
    watchdog_start();

    while (1)
    {
        TimingSnapshot snapshot;

        scheduler_pass();

        // Every pass. The status LED is lit for the 100 ms epoch pulse: a
        // heartbeat on the second. The epoch interrupt lights it, so its
        // rising edge also marks the interrupt's latency on a scope.
        STATUS_LED_SET(DISPLAY_LATCH_OC_GET_VALUE());

        // Every pass: the RTC's reads are taken as they finish, so a search
        // for its second (and the timed release of an aligned write) is not
        // held to the 10 ms tasks.
        (void)pcf8563_service(scheduler_now_ms());
        if (pcf8563_report_due() && (source_rtc_take_set() || debug_enabled(DEBUG_RTC)))
        {
            pcf8563_print(scheduler_now_ms(), rtc_reference(), false);
        }
        source_rtc_poll();
        // The light sensor leaves the bus to the RTC source while it is
        // searching or timing a release.
        veml6040_set_auto_read(source_rtc_state()->activity == SOURCE_RTC_IDLE);
        (void)veml6040_service(scheduler_now_ms());
        if (veml6040_report_due() && debug_enabled(DEBUG_EVENTS))
        {
            veml6040_print(scheduler_now_ms(), false);
        }

        // Every epoch, at the measurement slot (set by the T4 scheduler
        // interrupt, so the captures of the epoch are read together).
        if (timing_snapshot(&snapshot))
        {
            scheduler_section(&epoch_work);
            run_epoch(&snapshot);
        }
        // Every pass, after the epoch's frame is prepared: SSMM's hundredths.
        display_tick(epoch_timer_slot());

        if (scheduler_due(&every_10ms))
        {
            service_gnss();
            esp_link_service(scheduler_now_ms());
            source_ntp_service(scheduler_now_ms());
            i2c1_service(scheduler_now_ms());
            brightness_service();
            service_controls();
        }

        if (scheduler_due(&display_load))
        {
            display_service();
        }

        if (scheduler_due(&minute_report))
        {
            report_minute();
        }

        if (scheduler_due(&settings_store))
        {
            settings_service(scheduler_epoch(), flash_quiet());
        }
    }
}
