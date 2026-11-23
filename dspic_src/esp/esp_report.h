/*
 * The clock's state as SML payloads for the ESP: one builder per PIC -> ESP
 * message, filling the native struct that sml_encode() puts on the wire
 * (common/SML_PROTOCOL.md has each field's meaning).
 *
 * Firmware enums never reach the wire as they are: the mapping functions
 * below translate them to the frozen SML_* codes.
 */
#ifndef ESP_REPORT_H
#define ESP_REPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "../sync/sync.h"
#include "../timing/epoch_control.h"
#include "../ui/display.h"
#include "../../common/sml_proto.h"

// Mappings to wire codes.
uint8_t esp_report_lock(SyncState state, uint32_t epochs);
uint8_t esp_report_sync(SyncState state, uint32_t epochs);
uint8_t esp_report_cause(uint8_t reason);                   // SyncReason
uint8_t esp_report_source(uint8_t source, uint32_t epochs); // SyncSourceId
// A candidate: a SyncSourceId, SYNC_CANDIDATE_HOLDOVER or SYNC_SOURCE_NONE.
uint8_t esp_report_candidate(uint8_t candidate);
uint8_t esp_report_verdict(SyncCheck check);
uint8_t esp_report_control(EpochSyncState state);
uint8_t esp_report_display(DisplayMode mode, bool time_known);

// From the latest epoch: snapshot and update are those of sync_update().
void esp_report_time_auth(SML_TIME_AUTH_PAYLOAD *payload);
void esp_report_time_pic(const TimingSnapshot *snapshot, const SyncUpdate *update,
                         SML_TIME_PIC_PAYLOAD *payload);
void esp_report_time_ctrl(const TimingSnapshot *snapshot, const SyncUpdate *update,
                          SML_TIME_CTRL_PAYLOAD *payload);
void esp_report_time_meas(const TimingSnapshot *snapshot, const SyncUpdate *update, uint32_t now_ms,
                          SML_TIME_MEAS_PAYLOAD *payload);
void esp_report_time_gnss(uint32_t now_ms, SML_TIME_GNSS_PAYLOAD *payload);
// The RTC: its time as it counts it, when it was last read and set by our
// clock, and its bound (sync/source_rtc.h). No PPS or temperature on V2.
void esp_report_time_rtc(SML_TIME_RTC_PAYLOAD *payload);
void esp_report_stat_ui(SML_STAT_UI_PAYLOAD *payload);
// The sensors: the ambient light, and the BME280's board temperature (and
// the room guessed from it), pressure and humidity.
void esp_report_stat_env(SML_STAT_ENV_PAYLOAD *payload);
// The synchroniser's candidates. epoch_sequence: the latest epoch's.
void esp_report_stat_sources(uint32_t epoch_sequence, SML_STAT_SOURCES_PAYLOAD *payload);

// An SML_SHOW_* topic queued for printing a piece at a time
// (console/print_queue.h), the caller having checked for a free place.
// SML_SHOW_ALL is every module: what each is doing, or with verbose
// (SML_SHOW_VERBOSE) its counters and internals too. False for a topic it
// does not know, or the queue full.
bool esp_report_queue(uint32_t topic, bool verbose);
// One SML_SHOW_* topic other than SML_SHOW_ALL, printed. False for a topic
// it does not know.
bool esp_report_show(uint32_t topic, bool verbose, uint32_t now_ms);

// STAT-SYS: firmware version, uptime in epochs, reset cause, and the
// longest main-loop pass since the previous one (which this restarts).
void esp_report_stat_sys(uint32_t epoch_sequence, SML_STAT_SYS_PAYLOAD *payload);
// "PIC 0.1.0, up 00:20:34, last reset: power-on": up in epochs. verbose:
// RCON too.
void esp_report_print_system(bool verbose);
// The most telling cause in SML_RESET_* bits, as text.
const char *esp_report_reset_name(uint16_t cause);

#endif // ESP_REPORT_H
