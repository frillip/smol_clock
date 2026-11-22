/*
 * Smol Clock V2 ESP32: network co-processor for the dsPIC timekeeper.
 *
 *   src/pic/       SML link to the PIC (pic_link), its state (pic_report),
 *                  its updates (pic_boot, pic_update, the files staged in
 *                  picfw: pic_store); sml/ links to common/
 *   src/timing/    NTP client and clock model, ESP PPS out, edge captures
 *   src/tz/        tzinfo v1 -> SML-TZ-STATE, TIME-LEAP
 *   src/net/       Wi-Fi, telnet, OTA updates, STAT-NET
 *   src/console/   console lines and commands (serial and telnet)
 *   src/settings/  NVS settings
 *   src/common/    links to the repository's common/
 *
 * Everything runs from loop(), except the NTP exchanges (their own task)
 * and the edges (interrupts). Blocking work (HTTP, OTA) holds up loop()
 * only: the ESP PPS runs on in its interrupt, and the PIC link's 4 KB
 * receive buffer holds several seconds of PIC traffic.
 */
#include <Arduino.h>
#include <esp_timer.h>

#include "src/board.h"
#include "src/console/commands.h"
#include "src/console/console.h"
#include "src/net/net_status.h"
#include "src/net/telnet.h"
#include "src/net/updater.h"
#include "src/net/wifi_link.h"
#include "src/pic/pic_boot.h"
#include "src/pic/pic_link.h"
#include "src/settings/user_prefs.h"
#include "src/timing/ntp.h"
#include "src/timing/ntp_gnss.h"
#include "src/timing/esp_time.h"
#include "src/timing/pps.h"
#include "src/tz/leap.h"
#include "src/tz/tzinfo.h"
#include "src/util/loop_timing.h"
#include "src/version.h"

#define LED_PULSE_MS 100U
// While the NTP servers disagree, each pulse is two short flashes.
#define LED_FLASH_MS 40U
#define LED_GAP_MS   80U
#define MINUTE_MS    60000U

static ConsoleLine serial_line;
static uint32_t led_on_ms;
static bool led_double;
static uint32_t last_minute_ms;

void setup()
{
    Serial.begin(BOARD_CONSOLE_BAUD);
    Serial.printf("\nSmol Clock ESP %s (reset: %s)\n", ESP_VERSION, commands_reset_reason());
    pinMode(BOARD_STATUS_LED_PIN, OUTPUT);
    digitalWrite(BOARD_STATUS_LED_PIN, LOW);

    user_prefs_init(&Serial);
    pps_init();
    pic_link_init();
    pic_link_set_command_handler(commands_from_pic);
    pic_boot_init(&Serial);
    wifi_link_init(&Serial);
    telnet_init(&Serial);
    ntp_init();
    tzinfo_init(&Serial);
    Serial.print("> ");
}


// Each ESP PPS edge: TIME-NTP to the PIC, and the status LED while synced:
// one flash, or two while the NTP servers disagree.
// The board's temperature, from the PIC's STAT-ENV while it is fresh, to
// the ESP crystal's temperature compensation. No reading for a while (no
// BME280, or the PIC silent) and the compensation holds (temp_comp.h).
static void service_temperature(void)
{
    SML_STAT_ENV_PAYLOAD env;

    if (pic_link_get(SML_CLASS_STAT, SML_STAT_ENV, &env, 1000U) &&
        (env.flags & SML_ENV_BOARD_TEMP_VALID))
    {
        ntp_board_temperature(env.board_temperature_centic);
    }
}

static void service_ntp_report(void)
{
    SML_TIME_NTP_PAYLOAD report;

    if (!ntp_take_report(&report))
    {
        return;
    }
    pic_link_send(SML_CLASS_TIME, SML_TIME_NTP, &report);
    if (ntp_is_synced() && wifi_link_is_up())
    {
        digitalWrite(BOARD_STATUS_LED_PIN, HIGH);
        led_on_ms = millis();
        led_double = ntp_servers_disagree();
    }
}


// Each service timed as it returns: the longest pass and its cause in esp -v.
#define TIMED(name, call)                                    \
    do                                                       \
    {                                                        \
        call;                                                \
        loop_timing_note(name, esp_timer_get_time());        \
    } while (0)

void loop()
{
    bool network_up;
    uint32_t utc = 0;

    loop_timing_start(esp_timer_get_time());
    TIMED("pic_link", pic_link_service());
    TIMED("ntp_gnss", ntp_gnss_service());
    TIMED("pic_boot", pic_boot_service());
    TIMED("wifi", wifi_link_service());
    network_up = wifi_link_is_up();
    TIMED("temperature", service_temperature());
    TIMED("ntp", ntp_service(network_up));
    TIMED("ntp_report", service_ntp_report());
    TIMED("tzinfo", tzinfo_service(network_up));
    TIMED("leap", leap_service());
    TIMED("net_status", net_status_service());
    TIMED("updater_poll", updater_poll(network_up));

    if (console_read_line(&serial_line, &Serial, true))
    {
        commands_execute(&Serial, serial_line.text);
        console_line_clear(&serial_line);
        Serial.print("> ");
    }
    loop_timing_note("serial", esp_timer_get_time());
    TIMED("telnet", telnet_service());

    if (led_on_ms != 0)
    {
        uint32_t lit_ms = millis() - led_on_ms;

        if (!led_double && lit_ms >= LED_PULSE_MS)
        {
            digitalWrite(BOARD_STATUS_LED_PIN, LOW);
            led_on_ms = 0;
        }
        else if (led_double)
        {
            bool on = lit_ms < LED_FLASH_MS || (lit_ms >= LED_FLASH_MS + LED_GAP_MS &&
                                                lit_ms < 2 * LED_FLASH_MS + LED_GAP_MS);

            digitalWrite(BOARD_STATUS_LED_PIN, on ? HIGH : LOW);
            if (lit_ms >= 2 * LED_FLASH_MS + LED_GAP_MS)
            {
                led_on_ms = 0;
            }
        }
    }
    loop_timing_note("led", esp_timer_get_time());
    if (network_up && millis() - last_minute_ms >= MINUTE_MS)
    {
        uint32_t local;
        bool have_local = tzinfo_local_now(&local);

        last_minute_ms = millis();
        TIMED("updater", updater_service(have_local, local));
    }
    loop_timing_end(esp_timer_get_time(), esp_time_utc_now(&utc), utc);
}
