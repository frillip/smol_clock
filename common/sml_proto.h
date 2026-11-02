/*
 * SML protocol -- UBX-inspired framing for Smol Clock.
 * Draft for manual review. Payloads other than TIME-AUTH are provisional.
 * Specification: common/SML_PROTOCOL.md. This header is the single shared
 * copy for the dsPIC and the ESP32; do not fork it.
 * 2026-11-02 (2.5): class 0xF0 BOOT defined: BOOT-STATUS, BOOT-COMMAND,
 * BOOT-RESPONSE, BOOT-DATA, BOOT-DATA-RESPONSE, BOOT-VERIFY,
 * BOOT-VERIFY-RESPONSE. SML_ERROR_CRC and SML_ERROR_NOT_BLANK.
 * Layouts may hold byte strings (sml.h). No existing layout changes.
 * 2026-10-28 (2.3): SML_MENU_* menu_state values: the V2+ menu.
 * 2026-10-28 (2.2): SML_SETTING_DELTA_LOCAL and SML_UI_DELTA_LOCAL: the delta
 * epoch as a local wall-clock time.
 * 2026-10-27 (2.1): SML_DISPLAY_UNCERTAINTY (10), selectable: the time's bound.
 * 2026-10-27 (2.0): CMD-USER codes rationalised (breaking): the SET_* setting
 * commands are gone (settings are written with CMD-SETTINGS), INFO,
 * SHOW_CONFIG, SHOW_SETTINGS and SHOW_SOURCES become SHOW with an
 * SML_SHOW_* topic (| SML_SHOW_VERBOSE for everything) and REQUEST_MESSAGE;
 * the remaining codes renumbered, the ESP's renamed after the console's
 * words. DEFAULTS no longer saves. SML_DEBUG_RTC and SML_DEBUG_EVENTS.
 * 2026-10-21 (1.3): STAT-SOURCES added (the PIC's time sources, ranked);
 * SML_SOURCE_HOLDOVER; TIME-PIC second_source (was reserved); TIME-NTP
 * CORRECTING flag; SML_USER_SET_DEBUG with SML_DEBUG_* subsystem bits, and
 * SML_USER_SHOW_SOURCES.
 * 2026-10-19 (1.2): TIME-CTRL made lossless (exact Kalman/servo fixed-point
 * state); TIME-MEAS added (raw per-epoch measurements for statistics/ADEV);
 * STAT-NET added (ESP network/service status, ESP -> PIC); NTP WIFI_CONNECTED
 * moved to STAT-NET; CMD-USER/CMD-RESPONSE both ways (codes 0x8000+ run on the
 * ESP); TZ: the PIC always holds the latest TZ-STATE, TZ_AUTO selects the base
 * offset and DST_AUTO the DST state from it; STAT-UI TZ_HELD; RTC 3 = RV-3028-C7.
 * 2026-10-19 (1.1): wire mappings frozen for TIME-PIC sync_state/last_sync_cause,
 * TIME-CTRL controller_state, STAT-UI display/button and settings display
 * (SML_SYNC_*, SML_CAUSE_*, SML_CONTROL_*, SML_DISPLAY_*, SML_BUTTON_*,
 * SML_MENU_NONE); TIME-AUTH uncertainty defined as a +/- bound; payload
 * length constants (SML_*_BYTES) for every message. No wire layout change.
 * 2026-10-14: TZ-CURRENT + TZ-FUTURE merged into one atomic SML-TZ-STATE;
 * per-entry DST flag; horizon; sub-second offset for special timebases;
 * SML-TIME-LEAP added; GNSS leap fields added to TIME-GNSS; transition table
 * removed from the settings body (TZ state is persisted separately by the PIC).
 *
 * Wire: B5 73 CLASS ID LENGTH_H LENGTH_L PAYLOAD CRC_H CRC_L.
 * All multibyte fields are big-endian; signed integers use two's complement.
 * Packed structs specify field order/size, NOT endian conversion. Encode/decode
 * explicitly; do not send or cast native structs directly.
 * CRC-16/IBM-3740: poly=1021 init=FFFF refin=false refout=false xorout=0000.
 * Check("123456789")=29B1. Coverage includes magic through final payload byte.
 *
 * Normal payload maximum 240. Normal state payload minimum 8. Commands,
 * responses and debug text may be shorter.
 * All age_ms fields are uint16_t, saturating at 65535 ms (never wrapping).
 * Validity follows existing timeouts; saturation does not imply validity.
 * NTP update age can saturate normally for long polling intervals; last_update
 * UTC retains full elapsed-time information.
 * Reserved fields/bits transmit zero. Receivers ignore reserved bits.
 * No floats, time_t, native enums, C bitfields, pointers or unions on wire.
 */
#ifndef SML_PROTO_H
#define SML_PROTO_H
#include <stdint.h>
#include <stddef.h>

#define SML_MAGIC_1           0xB5U
#define SML_MAGIC_2           0x73U
#define SML_HEADER_BYTES      6U
#define SML_CRC_BYTES         2U
#define SML_MIN_STATE_BYTES   8U
#define SML_MAX_PAYLOAD_BYTES 240U
#define SML_MAX_FRAME_BYTES   248U
#define SML_TZ_TRANSITION_MAX 8U
#define SML_SETTINGS_SCHEMA   1U
#define SML_PROTOCOL_MAJOR    2U
#define SML_PROTOCOL_MINOR    5U

#define SML_CLASS_TIME 0x10U
#define SML_CLASS_TZ   0x20U
#define SML_CLASS_STAT 0x30U
#define SML_CLASS_CMD  0x80U
#define SML_CLASS_BOOT 0xF0U

#define SML_TIME_AUTH   0x01U // SML-TIME-AUTH
#define SML_TIME_PIC    0x10U // SML-TIME-PIC
#define SML_TIME_CTRL   0x11U // SML-TIME-CTRL
#define SML_TIME_VCTCXO 0x12U // SML-TIME-VCTCXO
#define SML_TIME_MEAS   0x13U // SML-TIME-MEAS
#define SML_TIME_GNSS   0x20U // SML-TIME-GNSS
#define SML_TIME_NTP    0x30U // SML-TIME-NTP
#define SML_TIME_RTC    0x40U // SML-TIME-RTC
#define SML_TIME_LEAP   0x50U // SML-TIME-LEAP
#define SML_TZ_STATE    0x01U // SML-TZ-STATE (replaces TZ-CURRENT)
// 0x10 (former SML-TZ-FUTURE) retired; do not reuse.
#define SML_STAT_UI       0x01U // SML-STAT-UI
#define SML_STAT_ENV      0x10U // SML-STAT-ENV: sensors only
#define SML_STAT_SUPPLY   0x20U // SML-STAT-SUPPLY
#define SML_STAT_SETTINGS 0x30U // SML-STAT-SETTINGS
#define SML_STAT_NET      0x40U // SML-STAT-NET (ESP -> PIC)
#define SML_STAT_SOURCES  0x50U // SML-STAT-SOURCES: the PIC's time sources, ranked
#define SML_STAT_SYS      0x60U // SML-STAT-SYS: the PIC's firmware, uptime, reset cause
#define SML_STAT_DEBUG    0xF0U // SML-STAT-DEBUG
#define SML_CMD_USER      0x10U // SML-CMD-USER
#define SML_CMD_RESPONSE  0x20U // SML-CMD-RESPONSE
#define SML_CMD_SETTINGS  0x30U // SML-CMD-SETTINGS
#define SML_BOOT_STATUS          0x01U // SML-BOOT-STATUS
#define SML_BOOT_COMMAND         0x10U // SML-BOOT-COMMAND
#define SML_BOOT_RESPONSE        0x20U // SML-BOOT-RESPONSE
#define SML_BOOT_DATA            0x50U // SML-BOOT-DATA
#define SML_BOOT_DATA_RESPONSE   0x60U // SML-BOOT-DATARESP
#define SML_BOOT_VERIFY          0x90U // SML-BOOT-VERIFY
#define SML_BOOT_VERIFY_RESPONSE 0xA0U // SML-BOOT-VERIFYRESP

// Protocol source values deliberately differ from existing CLOCK_SOURCE.
#define SML_SOURCE_INIT        0U
#define SML_SOURCE_NONE        1U
#define SML_SOURCE_RTC         2U
#define SML_SOURCE_ESP         3U // ESP holdover/internal time, not a fresh NTP fix
#define SML_SOURCE_NTP         4U
#define SML_SOURCE_GNSS        5U
#define SML_SOURCE_HOLDOVER    6U // the PIC's own oscillator, held: STAT-SOURCES, TIME-PIC second
#define SML_LOCK_UNINITIALISED 0U
#define SML_LOCK_ACQUIRING     1U
#define SML_LOCK_LOCKED        2U
#define SML_LOCK_HOLDOVER      3U
#define SML_LOCK_FREE_RUNNING  4U
#define SML_TZ_SOURCE_NONE     0U
#define SML_TZ_SOURCE_CACHE    1U
#define SML_TZ_SOURCE_GEOIP    2U
#define SML_TZ_SOURCE_GNSS     3U
#define SML_TZ_SOURCE_MANUAL   4U // rule/offsets entered by the user on the ESP (tzinfo off)
#define SML_TZ_SOURCE_PINNED   5U // zone pinned by name; rules automatic
#define SML_TZ_SOURCE_SPECIAL  6U // on-device special timebase (GMT, DMT, TAI, ...)
#define SML_LEAP_SOURCE_NONE   0U
#define SML_LEAP_SOURCE_SERVER 1U // tzinfo leap object / release artifact
#define SML_LEAP_SOURCE_GNSS   2U // UBX-NAV-TIMELS via PIC
#define SML_RTC_NONE           0U
#define SML_RTC_DS1307         1U
#define SML_RTC_PCF8563        2U
#define SML_RTC_RV3028         3U // Micro Crystal RV-3028-C7 (V3)

/* Frozen wire mappings (2026-10-19). Firmware maps its own enums onto these
 * with explicit tables; application enum order never reaches the wire.
 * Zero always means "not known yet / none", so a zero-filled payload is
 * never mistaken for a state.
 *
 * TIME-PIC sync_state / previous_sync_state: the PIC's source arbitration
 * state machine (V2+ sync/). lock_state is the coarse summary of the same. */
#define SML_SYNC_INIT     0U // no epoch processed yet
#define SML_SYNC_FREE_RUN 1U // no source: last frequency estimate, time counted on
#define SML_SYNC_ACQUIRE  2U // source qualified: learning frequency, stepping onto edge
#define SML_SYNC_LOCKED   3U // loop tracks the selected source
#define SML_SYNC_HOLDOVER 4U // selected source failed: frequency held, within limit
// TIME-PIC last_sync_cause: why the latest sync_state change happened.
#define SML_CAUSE_NONE          0U // no change yet
#define SML_CAUSE_QUALIFIED     1U // a source qualified with none selected
#define SML_CAUSE_TRACKING      2U // the loop is tracking
#define SML_CAUSE_PHASE_LOST    3U // the loop lost phase and re-acquires
#define SML_CAUSE_SOURCE_LOST   4U // the selected source failed
#define SML_CAUSE_SOURCE_BACK   5U // ... and is good again
#define SML_CAUSE_ALTERNATIVE   6U // another source took over a failed one
#define SML_CAUSE_BETTER_SOURCE 7U // a better-ranked source took over
#define SML_CAUSE_EXPIRED       8U // holdover limit reached
#define SML_CAUSE_RESYNC        9U // RESYNC: every source judged again (2.4)
// TIME-CTRL / TIME-VCTCXO controller_state: the epoch control loop.
#define SML_CONTROL_INIT    0U
#define SML_CONTROL_ACQUIRE 1U // waiting for reference periods before the step
#define SML_CONTROL_STEP    2U // phase step loaded, following it
#define SML_CONTROL_TRACK   3U // PI servo on the Kalman frequency estimate
// STAT-UI current_display / selected_display, settings selected_display.
// Values match the V2 UI_DISPLAY_STATE order, so V2 EEPROM values carry over.
// DASHES, TEMP and MENU are runtime-only and never selectable; UNCERTAINTY
// (2.1, new in V2+) is selectable, numbered after them.
// SML_DISPLAY_SELECTABLE(): a value the settings may select.
#define SML_DISPLAY_INIT   0U
#define SML_DISPLAY_HHMM   1U
#define SML_DISPLAY_MMSS   2U
#define SML_DISPLAY_SSMM   3U
#define SML_DISPLAY_YYYY   4U
#define SML_DISPLAY_MMDD   5U
#define SML_DISPLAY_DELTA  6U
#define SML_DISPLAY_DASHES 7U
#define SML_DISPLAY_TEMP   8U
#define SML_DISPLAY_MENU   9U
#define SML_DISPLAY_UNCERTAINTY 10U // the time's +/- bound
#define SML_DISPLAY_SELECTABLE(value)                                   \
    (((value) >= SML_DISPLAY_HHMM && (value) <= SML_DISPLAY_DELTA) || \
     (value) == SML_DISPLAY_UNCERTAINTY)
// STAT-UI button_state.
#define SML_BUTTON_NONE  0U
#define SML_BUTTON_SHORT 1U
#define SML_BUTTON_LONG  2U
#define SML_BUTTON_HOLD  3U
// STAT-UI menu_state (2.3): 0 outside the menu; else the section (bits 4-6),
// the item within it (bits 0-3, from 0, as the PIC's menu lists them, hidden
// ones included) and SML_MENU_EDITING while a value is edited. While the
// menu is open, current_display is SML_DISPLAY_MENU.
#define SML_MENU_NONE         0U
#define SML_MENU_TOP          0x10U // MENU tZ dSt ALrM bEEP dISP tIME rSt EXIt
#define SML_MENU_TZ           0x20U // AUtO/MAN, offset, remote, bACK
#define SML_MENU_DST          0x30U // AUtO/MAN, ON/OFF, offset, bACK
#define SML_MENU_ALARM        0x40U // ON/OFF, time, bACK
#define SML_MENU_BEEP         0x50U // ON/OFF, bACK
#define SML_MENU_DISPLAY      0x60U // 12/24, mode, brightness, bACK
#define SML_MENU_RESET        0x70U // WIFI, SEt, ALL, bACK
#define SML_MENU_SECTION_MASK 0x70U
#define SML_MENU_ITEM_MASK    0x0FU
#define SML_MENU_EDITING      0x80U
// STAT-SOURCES verdict: the PIC's judgement of a source's latest report, GOOD
// or the first reason it is not.
/* STAT-SYS reset causes. The positions are the dsPIC33's RCON bits, so the
 * PIC sends RCON's cause bits as they are. */
#define SML_RESET_POR    0x0001U // power-on
#define SML_RESET_BOR    0x0002U // brown-out (also set at power-on)
#define SML_RESET_WDTO   0x0010U // watchdog
#define SML_RESET_SWR    0x0040U // RESET instruction (a command, or a trap without a handler)
#define SML_RESET_EXTR   0x0080U // MCLR pin
#define SML_RESET_CM     0x0200U // configuration mismatch
#define SML_RESET_IOPUWR 0x4000U // illegal opcode or uninitialised W register
#define SML_RESET_TRAPR  0x8000U // trap conflict
#define SML_RESET_ALL    0xC2D3U

#define SML_VERDICT_NONE         0U // not judged yet
#define SML_VERDICT_ABSENT       1U // not answering
#define SML_VERDICT_NO_TIME      2U // does not know UTC (no fix, NTP unsynced)
#define SML_VERDICT_NO_EDGE      3U // no edge seen this epoch
#define SML_VERDICT_UNLABELLED   4U // an edge, but its UTC second is unknown
#define SML_VERDICT_INACCURATE   5U // its own uncertainty is above the PIC's limit
#define SML_VERDICT_INCONSISTENT 6U // label does not follow on from the last one
#define SML_VERDICT_GOOD         7U
// SML_USER_SET_DEBUG: the PIC's console diagnostics, by subsystem. Unasked,
// the PIC prints only its start-up line and faults; these gate the rest.
#define SML_DEBUG_TIMING  0x0001U // per epoch: captures, loop, servo, receiver's view
#define SML_DEBUG_GNSS    0x0002U // per epoch: the GNSS edge and its label
#define SML_DEBUG_SYNC    0x0004U // per epoch: the synchroniser's state
#define SML_DEBUG_SOURCES 0x0008U // per epoch: every candidate, ranked
#define SML_DEBUG_NTP     0x0010U // per epoch: the latest TIME-NTP and its pairing
#define SML_DEBUG_MINUTE  0x0020U // each minute: scheduler, GNSS and sync reports
#define SML_DEBUG_TUNING  0x0040U // each minute: loop and source statistics
#define SML_DEBUG_RTC     0x0080U // as they happen: RTC set, second search, release
#define SML_DEBUG_EVENTS  0x0100U // as they happen: sync state changes, controls, sensor
#define SML_DEBUG_ALL     0x01FFU

#define SML_AUTH_UTC_VALID          0x0001U
#define SML_AUTH_UNCERTAINTY_VALID  0x0002U
#define SML_AUTH_SOURCE_VALID       0x0004U
#define SML_PIC_PHASE_VALID         0x0001U
#define SML_PIC_PERIOD_VALID        0x0002U
#define SML_PIC_FREQUENCY_VALID     0x0004U
#define SML_PIC_PPS_VALID           0x0008U
#define SML_CTRL_ESTIMATE_VALID     0x0001U
#define SML_CTRL_PI_ENABLED         0x0002U
#define SML_CTRL_SATURATED          0x0004U
#define SML_CTRL_MEASUREMENT_USED   0x0008U
#define SML_CTRL_GATE_REJECTED      0x0010U
#define SML_MEAS_LABEL_VALID        0x0001U
#define SML_MEAS_PHASE_VALID        0x0002U
#define SML_MEAS_PERIOD_VALID       0x0004U
#define SML_MEAS_EPOCH_PERIOD_VALID 0x0008U
#define SML_MEAS_MARK_VALID         0x0010U
#define SML_MEAS_QERR_VALID         0x0020U
#define SML_MEAS_MEASUREMENT_USED   0x0040U
#define SML_VCT_PRESENT             0x0001U
#define SML_VCT_ENABLED             0x0002U
#define SML_VCT_SATURATED           0x0004U
#define SML_GNSS_DETECTED           0x0001U
#define SML_GNSS_FIX_OK             0x0002U
#define SML_GNSS_UTC_VALID          0x0004U
#define SML_GNSS_PPS_VALID          0x0008U
#define SML_GNSS_POSITION_VALID     0x0010U
#define SML_GNSS_LEAP_VALID         0x0020U
#define SML_GNSS_LEAP_EVENT_VALID   0x0040U
// 0x0001 (former SML_NTP_WIFI_CONNECTED) moved to STAT-NET; reserved.
#define SML_NTP_TIME_VALID        0x0002U
#define SML_NTP_UPDATE_VALID      0x0004U
#define SML_NTP_PPS_SYNC          0x0008U
#define SML_NTP_SCHEDULER_SYNC    0x0010U
#define SML_NTP_UNCERTAINTY_VALID 0x0020U
#define SML_NTP_CORRECTING        0x0040U // a step or fast slew is moving the edge
// The fastest the ESP slews a routine correction; anything faster is flagged
// SML_NTP_CORRECTING while it lasts.
#define SML_NTP_GENTLE_SLEW_PPM      5U
#define SML_TZ_VALID                 0x0001U // single gating flag for the whole state
#define SML_TZ_AMBIGUOUS             0x0002U // informational: best guess near a boundary
#define SML_TZ_ENTRY_DST_ACTIVE      0x01U   // per-entry flag; includes negative DST
#define SML_LEAP_VALID               0x0001U
#define SML_LEAP_EVENT_PENDING       0x0002U
#define SML_UI_LOCAL_VALID           0x0001U
#define SML_UI_ALARM_ENABLED         0x0002U
#define SML_UI_ALARM_ACTIVE          0x0004U
#define SML_UI_BRIGHTNESS_AUTO       0x0008U
#define SML_UI_POWER_ON              0x0010U
#define SML_UI_SWITCH                0x0020U
#define SML_UI_BUTTON                0x0040U
#define SML_UI_UPDATE_PENDING        0x0080U
#define SML_UI_FORMAT_12H            0x0100U
#define SML_UI_DST_ACTIVE            0x0200U
#define SML_UI_TZ_STALE              0x0400U // now >= horizon_end_utc of applied TZ state
#define SML_UI_TZ_AMBIGUOUS          0x0800U
#define SML_UI_TZ_HELD               0x1000U // a TZ-STATE schedule is held
#define SML_UI_DELTA_LOCAL           0x2000U // delta_epoch_utc is a local wall-clock time
#define SML_RTC_PRESENT              0x0001U
#define SML_RTC_UTC_VALID            0x0002U
#define SML_RTC_SYNCED               0x0004U
#define SML_RTC_LAST_READ_VALID      0x0008U
#define SML_RTC_LAST_WRITE_VALID     0x0010U
#define SML_RTC_PPS_PRESENT          0x0020U
#define SML_RTC_PPS_VALID            0x0040U
#define SML_RTC_TEMP_VALID           0x0080U
#define SML_RTC_UNCERTAINTY_VALID    0x0100U
#define SML_VCT_TEMP_VALID           0x0008U
#define SML_ENV_LUX_PRESENT          0x0001U
#define SML_ENV_LUX_VALID            0x0002U
#define SML_ENV_BME_PRESENT          0x0004U
#define SML_ENV_TEMP_VALID           0x0008U
#define SML_ENV_PRESSURE_VALID       0x0010U
#define SML_ENV_HUMIDITY_VALID       0x0020U
#define SML_ENV_BOARD_TEMP_VALID     0x0040U
#define SML_SUPPLY_5V0_VALID         0x0001U
#define SML_SUPPLY_3V3_VALID         0x0002U
#define SML_SUPPLY_VFIL_VALID        0x0004U
#define SML_SUPPLY_VPP_VALID         0x0008U
#define SML_SUPPLY_VFIL_ENABLED      0x0010U
#define SML_SUPPLY_VPP_ENABLED       0x0020U
#define SML_SUPPLY_DRIVER_ENABLED    0x0040U
#define SML_SUPPLY_DRIVER_FREQ_VALID 0x0080U
#define SML_NET_WIFI_CONNECTED       0x0001U
#define SML_NET_IP_VALID             0x0002U
#define SML_NET_RSSI_VALID           0x0004U
#define SML_NET_NTP_ENABLED          0x0008U
#define SML_NET_NTP_SYNCED           0x0010U
#define SML_NET_TZINFO_ENABLED       0x0020U
#define SML_NET_TZINFO_VALID         0x0040U
#define SML_NET_LOCAL_TZ_VALID       0x0080U
#define SML_NET_TELNET_ENABLED       0x0100U
#define SML_NET_TELNET_CLIENT        0x0200U
#define SML_NET_UPDATE_AUTO          0x0400U
#define SML_NET_UPDATE_AVAILABLE     0x0800U
#define SML_NET_UPDATE_IN_PROGRESS   0x1000U
#define SML_NET_SD_PRESENT           0x2000U
#define SML_WIFI_UNKNOWN             0U
#define SML_WIFI_OFF                 1U
#define SML_WIFI_NOT_CONFIGURED      2U
#define SML_WIFI_CONNECTING          3U
#define SML_WIFI_CONNECTED           4U
#define SML_WIFI_DISCONNECTED        5U // configured, link lost or failed
#define SML_WIFI_PROVISIONING        6U // setup access point active
#define SML_SETTINGS_VALID           0x0001U
#define SML_SETTINGS_DIRTY           0x0002U
#define SML_SETTINGS_DEFAULTS        0x0004U
#define SML_SETTING_TZ_AUTO          0x0001U
#define SML_SETTING_DST_AUTO         0x0002U
#define SML_SETTING_DST_ACTIVE       0x0004U
#define SML_SETTING_ALARM_ENABLED    0x0008U
#define SML_SETTING_BEEP_ENABLED     0x0010U
#define SML_SETTING_FORMAT_12H       0x0020U
#define SML_SETTING_BRI_AUTO         0x0040U
// delta_epoch_utc is local wall-clock time (UTC plus the offsets then in
// force, as a scalar), not a UTC instant: the delta is counted in local time.
#define SML_SETTING_DELTA_LOCAL      0x0080U

// Payload lengths. Fixed-length messages must arrive with exactly this
// length; anything else is answered as SML_ERROR_LENGTH (commands) or
// dropped and counted (state). STAT-DEBUG is 1..SML_MAX_PAYLOAD_BYTES.
#define SML_TIME_AUTH_BYTES     16U
#define SML_TIME_PIC_BYTES      38U
#define SML_TIME_CTRL_BYTES     56U
#define SML_TIME_MEAS_BYTES     40U
#define SML_TIME_VCTCXO_BYTES   20U
#define SML_TIME_GNSS_BYTES     54U
#define SML_TIME_NTP_BYTES      42U
#define SML_TIME_RTC_BYTES      34U
#define SML_TIME_LEAP_BYTES     16U
#define SML_TZ_ENTRY_BYTES      12U
#define SML_TZ_STATE_BYTES      124U
#define SML_STAT_UI_BYTES       32U
#define SML_STAT_ENV_BYTES      14U
#define SML_STAT_SUPPLY_BYTES   14U
#define SML_SETTINGS_BODY_BYTES 28U
#define SML_STAT_SETTINGS_BYTES 36U
#define SML_STAT_NET_BYTES      24U
#define SML_SOURCE_ENTRY_BYTES  24U
#define SML_SOURCES_ENTRIES     4U
#define SML_STAT_SOURCES_BYTES  108U
#define SML_STAT_SYS_BYTES      16U
#define SML_CMD_USER_BYTES      10U
#define SML_CMD_RESPONSE_BYTES  12U
#define SML_CMD_SETTINGS_BYTES  38U
#define SML_BOOT_STATUS_BYTES          56U
#define SML_BOOT_COMMAND_BYTES         32U
#define SML_BOOT_RESPONSE_BYTES        12U
#define SML_BOOT_CHUNK_BYTES           192U // 64 instructions, 3 bytes each
#define SML_BOOT_DATA_BYTES            204U
#define SML_BOOT_DATA_RESPONSE_BYTES   12U
#define SML_BOOT_VERIFY_BYTES          8U
#define SML_BOOT_VERIFY_RESPONSE_BYTES 204U

#pragma pack(push, 1)
typedef struct
{
    uint8_t magic_upper;
    uint8_t magic_lower;
    uint8_t class_id;
    uint8_t message_id;
    uint16_t payload_length;
} SML_HEADER;
typedef struct
{
    uint16_t crc;
} SML_CHECKSUM;

/* SML-TIME-AUTH, PIC -> ESP, 16 bytes.
 * UTC = utc_seconds + utc_phase_ns/1e9 at associated OC epoch boundary.
 * Phase = OC minus corresponding reference IC, in ns.
 * UTC invalid implies phase/uncertainty invalid. Uncertainty is a +/- bound
 * (not 1 sigma) on |UTC error| of the OC epoch: the selected source's own
 * accuracy + capture resolution + |measured phase|, growing with time spent in
 * holdover. Clear UNCERTAINTY_VALID when no bound can be given (free run). */
typedef struct
{
    /* flags:
     * bit 0: SML_AUTH_UTC_VALID
     * bit 1: SML_AUTH_UNCERTAINTY_VALID
     * bit 2: SML_AUTH_SOURCE_VALID
     * All other bits reserved; transmit zero. */
    uint16_t flags;
    uint8_t source;
    uint8_t lock_state;
    uint32_t utc_seconds;
    int32_t utc_phase_ns;
    uint32_t uncertainty_ns;
} SML_TIME_AUTH_PAYLOAD;

// SML-TIME-PIC, PIC -> ESP. Period/phase/adjustment are timer cycles.
// sync_state/previous_sync_state: SML_SYNC_*; last_sync_cause: SML_CAUSE_*.
// Extended capture/accumulation diagnostics belong in debug output.
typedef struct
{
    /* flags:
     * bit 0: SML_PIC_PHASE_VALID
     * bit 1: SML_PIC_PERIOD_VALID
     * bit 2: SML_PIC_FREQUENCY_VALID
     * bit 3: SML_PIC_PPS_VALID
     * All other bits reserved; transmit zero. */
    uint16_t flags;
    uint8_t source;
    uint8_t lock_state;
    uint8_t sync_state;
    uint8_t previous_sync_state;
    uint8_t last_sync_cause;
    uint8_t second_source; // SML_SOURCE_*, HOLDOVER included; NONE: no second choice
    uint32_t epoch_sequence;
    uint32_t fosc_hz;
    uint32_t pps_period_cycles;
    int32_t phase_cycles;
    int32_t applied_adjustment_cycles;
    uint32_t missing_pps_events;
    uint32_t sync_events;
    uint16_t reference_age_ms;
} SML_TIME_PIC_PAYLOAD;

/* SML-TIME-CTRL, PIC -> ESP. The estimator and servo state, lossless: the
 * PIC's own fixed-point values, so the ESP sees the full-precision frequency
 * estimate, not a rounded one. Units: timer ticks (1 / nominal_period_ticks of
 * a second; 25 ns on V2), Q16.16 = ticks x 65536, Q2.30 = x 2^30.
 * frequency_offset_q16 is the Kalman state: reference period minus nominal,
 * i.e. the oscillator is fast by frequency_offset_q16 / 65536 /
 * nominal_period_ticks (fraction). output_offset_q16 is the steering: the
 * period requested for the next epoch minus nominal. P = frequency_variance_q30
 * x R (tick^2). controller_state: SML_CONTROL_*. */
typedef struct
{
    /* flags:
     * bit 0: SML_CTRL_ESTIMATE_VALID (Kalman initialised)
     * bit 1: SML_CTRL_PI_ENABLED
     * bit 2: SML_CTRL_SATURATED (PI correction at its limit)
     * bit 3: SML_CTRL_MEASUREMENT_USED (this epoch's phase fed the loop)
     * bit 4: SML_CTRL_GATE_REJECTED (this epoch's phase rejected by the gate)
     * All other bits reserved; transmit zero. */
    uint16_t flags;
    uint8_t controller_state;
    uint8_t reserved;
    uint32_t epoch_sequence;
    uint32_t nominal_period_ticks;     // ticks per nominal second (40000000 on V2)
    int32_t frequency_offset_q16;      // Kalman x, Q16.16 ticks per reference second
    uint32_t frequency_variance_q30;   // Kalman P / R, Q2.30
    uint32_t measurement_variance_q16; // Kalman R, tick^2 Q16.16
    uint32_t kalman_gain_q30;          // last gain, Q2.30
    int32_t predicted_phase_q16;       // phase predicted for the next epoch, Q16.16 ticks
    int32_t phase_innovation_q16;      // this epoch: measured - predicted phase, Q16.16 ticks
    int32_t pi_proportional_q16;       // Q16.16 ticks of period
    int32_t pi_integral_q16;           // Q16.16 ticks of period
    int32_t output_offset_q16;         // requested period - nominal, Q16.16 ticks
    uint32_t gate_rejections;          // phase gate, since start
    uint32_t kalman_rejections;        // Kalman outlier rejections, since start
} SML_TIME_CTRL_PAYLOAD;

/* SML-TIME-MEAS, PIC -> ESP. The raw measurements of one epoch, for logging,
 * statistics and Allan deviation on the ESP. Nothing filtered or rounded;
 * every record is complete in itself, keyed by epoch_sequence.
 * - phase_ticks: epoch capture minus reference capture, whole ticks (before
 *   the half-tick calibration). The disciplined output against the reference.
 * - reference_period_ticks: reference PPS-to-PPS interval on the free-running
 *   capture timebase: the oscillator against the reference, unsteered.
 * - epoch_period_ticks: the length of the second that ended at this epoch, as
 *   loaded into the epoch timer (steering included).
 * - mark_offset_ns: TIM-TM2, the receiver's measurement of the PREVIOUS epoch's
 *   edge (epoch_sequence - 1) against its second; positive = epoch late.
 * - qerr_ps: TIM-TP quantisation error of this epoch's reference edge.
 * A record with controller_state != TRACK or MEASUREMENT_USED clear marks a
 * discontinuity for output statistics. */
typedef struct
{
    /* flags:
     * bit 0: SML_MEAS_LABEL_VALID (utc_seconds)
     * bit 1: SML_MEAS_PHASE_VALID
     * bit 2: SML_MEAS_PERIOD_VALID (reference_period_ticks, this epoch's edge)
     * bit 3: SML_MEAS_EPOCH_PERIOD_VALID
     * bit 4: SML_MEAS_MARK_VALID (mark_offset_ns, mark_accuracy_ns)
     * bit 5: SML_MEAS_QERR_VALID
     * bit 6: SML_MEAS_MEASUREMENT_USED (the loop learnt from this epoch)
     * All other bits reserved; transmit zero. */
    uint16_t flags;
    uint8_t source;           // SML_SOURCE_* of the reference
    uint8_t controller_state; // SML_CONTROL_*
    uint32_t epoch_sequence;
    uint32_t utc_seconds;        // label of this epoch
    uint32_t reference_sequence; // reference edge count (gaps = missed edges)
    uint32_t reference_period_ticks;
    int32_t phase_ticks;
    uint32_t epoch_period_ticks;
    int32_t mark_offset_ns;
    uint32_t mark_accuracy_ns;
    int32_t qerr_ps;
} SML_TIME_MEAS_PAYLOAD;

// SML-TIME-VCTCXO, V3 PIC -> ESP. DAC range/scaling is hardware dependent.
typedef struct
{
    /* flags:
     * bit 0: SML_VCT_PRESENT
     * bit 1: SML_VCT_ENABLED
     * bit 2: SML_VCT_SATURATED
     * bit 3: SML_VCT_TEMP_VALID
     * All other bits reserved; transmit zero. */
    uint16_t flags;
    uint8_t controller_state;
    uint8_t reserved;
    uint32_t epoch_sequence;
    uint16_t dac_code;
    uint32_t control_uv;
    int32_t correction_ppt;
    int16_t vctcxo_adc_temperature_centic; // ADC-derived temperature, 0.01 C
} SML_TIME_VCTCXO_PAYLOAD;

// SML-TIME-GNSS, PIC -> ESP. Fix values match UBX NAV-STATUS GPSFIX.
// Positions 1e-7 degrees; height mm. Each validity flag gates its field group.
// Detailed NAV-CLOCK diagnostics are available through debug text.
typedef struct
{
    /* flags:
     * bit 0: SML_GNSS_DETECTED
     * bit 1: SML_GNSS_FIX_OK
     * bit 2: SML_GNSS_UTC_VALID
     * bit 3: SML_GNSS_PPS_VALID
     * bit 4: SML_GNSS_POSITION_VALID
     * bit 5: SML_GNSS_LEAP_VALID
     * bit 6: SML_GNSS_LEAP_EVENT_VALID
     * All other bits reserved; transmit zero. */
    uint16_t flags;
    uint8_t fix_type;
    uint8_t utc_standard;
    uint32_t utc_seconds;
    uint32_t utc_accuracy_ns;
    uint32_t pps_sequence;
    uint16_t utc_age_ms;
    uint16_t pps_age_ms;
    uint16_t position_age_ms;
    int32_t latitude_e7;
    int32_t longitude_e7;
    int32_t height_mm;
    int32_t height_msl_mm;
    uint32_t horizontal_accuracy_mm;
    uint32_t vertical_accuracy_mm;
    // Leap data from UBX-NAV-TIMELS, gated by LEAP_VALID / LEAP_EVENT_VALID.
    // gps_utc_s is the current GPS-UTC leap count (TAI-UTC = gps_utc_s + 19).
    // leap_event_utc is the first UTC second at which the new count applies.
    int16_t gps_utc_s;
    int8_t leap_change; // -1, 0 or +1
    uint8_t reserved_leap;
    uint32_t leap_event_utc;
} SML_TIME_GNSS_PAYLOAD;

// SML-TIME-NTP, ESP -> PIC. utc_seconds labels ESP PPS epoch, not UART arrival.
// NTP offset = estimated UTC minus local ESP time. RTT is round-trip duration.
// last_update is UTC seconds, only valid when UPDATE_VALID is set.
typedef struct
{
    /* flags:
     * bit 0: reserved (was WIFI_CONNECTED; see STAT-NET)
     * bit 1: SML_NTP_TIME_VALID
     * bit 2: SML_NTP_UPDATE_VALID
     * bit 3: SML_NTP_PPS_SYNC
     * bit 4: SML_NTP_SCHEDULER_SYNC
     * bit 5: SML_NTP_UNCERTAINTY_VALID
     * bit 6: SML_NTP_CORRECTING (a step, or a slew faster than
     *        SML_NTP_GENTLE_SLEW_PPM, is moving the edge; clear once done)
     * All other bits reserved; transmit zero. */
    uint16_t flags;
    uint8_t source;
    uint8_t lock_state;
    uint32_t utc_seconds;
    int32_t utc_phase_ns;
    uint32_t uncertainty_ns;
    uint32_t pps_sequence;
    uint32_t last_update_utc;
    uint16_t update_age_ms;
    uint32_t update_interval_s;
    uint32_t round_trip_us;
    int32_t last_offset_us;
    int32_t frequency_estimate_ppt;
} SML_TIME_NTP_PAYLOAD;

/* SML-TIME-RTC, PIC -> ESP.
 * rtc_utc_seconds is current RTC reckoning, advanced from last successful read.
 * Last read/write timestamps are authoritative UTC at successful transaction
 * completion, gated separately. Do not substitute RTC register contents.
 * PPS fields V3 only: zero and validity/presence clear on V2.
 * pps_phase_ns = local OC minus paired RTC IC, not authoritative-source phase.
 * RTC temperature is optional; zero when unsupported. */
typedef struct
{
    /* flags:
     * bit 0: SML_RTC_PRESENT
     * bit 1: SML_RTC_UTC_VALID
     * bit 2: SML_RTC_SYNCED
     * bit 3: SML_RTC_LAST_READ_VALID
     * bit 4: SML_RTC_LAST_WRITE_VALID
     * bit 5: SML_RTC_PPS_PRESENT
     * bit 6: SML_RTC_PPS_VALID
     * bit 7: SML_RTC_TEMP_VALID
     * bit 8: SML_RTC_UNCERTAINTY_VALID
     * All other bits reserved; transmit zero. */
    uint16_t flags;
    uint8_t rtc_type;
    uint8_t reserved;
    uint32_t rtc_utc_seconds;
    uint32_t last_read_utc;
    uint32_t last_write_utc;
    int16_t rtc_temperature_centic; // 0.01 C
    uint32_t pps_sequence;
    uint32_t last_pps_utc;
    int32_t pps_phase_ns;
    uint32_t uncertainty_ns; // +/- RTC reckoning uncertainty; ns for consistency
} SML_TIME_RTC_PAYLOAD;

/* SML-TIME-LEAP, ESP -> PIC, 16 bytes. The ESP selects the source (GNSS when
 * its leap flags are valid, else the server leap object) and sends the result.
 * tai_utc_s: TAI-UTC currently in force (37 in 2026). If EVENT_PENDING, at
 * event_utc TAI-UTC becomes tai_utc_s + change (UTC 23:59:60 precedes it for +1).
 * expires_utc: leap data expiry; 0 if unknown. Resent on change and periodically. */
typedef struct
{
    /* flags:
     * bit 0: SML_LEAP_VALID
     * bit 1: SML_LEAP_EVENT_PENDING
     * All other bits reserved; transmit zero. */
    uint16_t flags;
    uint8_t source; // SML_LEAP_SOURCE_*
    uint8_t reserved;
    int16_t tai_utc_s;
    int8_t change; // -1, 0 or +1
    uint8_t reserved2;
    uint32_t event_utc;
    uint32_t expires_utc;
} SML_TIME_LEAP_PAYLOAD;

/* One timezone state: valid from utc_seconds until the next entry (or horizon).
 * Effective civil offset = base_offset_s + (DST_ACTIVE ? dst_offset_s : 0)
 * (+ the state's subsecond_offset_ns). dst_offset_s is the zone's DST amount and
 * is reported even when inactive; it may be negative. No zone names: the PIC
 * needs only offsets, DST state and transition times. */
typedef struct
{
    uint32_t utc_seconds; // epoch this entry takes effect; 0 = in force since before data
    int32_t base_offset_s;
    int16_t dst_offset_s;
    // flags:
    // bit 0: SML_TZ_ENTRY_DST_ACTIVE
    // All other bits reserved; transmit zero.
    uint8_t flags;
    uint8_t reserved;
} SML_TZ_ENTRY;

/* SML-TZ-STATE, ESP -> PIC, fixed 124 bytes. Atomic replacement of the whole
 * schedule: current entry plus up to eight future entries in ascending UTC.
 * - VALID clear: PIC ignores the payload and keeps its existing schedule.
 * - horizon_end_utc: exclusive end of coverage (first transition NOT included);
 *   0xFFFFFFFF = no further transitions known. After it the PIC holds the last
 *   entry and sets SML_UI_TZ_STALE.
 * - subsecond_offset_ns applies to every entry; 0 except special timebases
 *   (e.g. Mecca +02:39:18.3 -> base 9558 s, 300000000 ns). PIC may round if
 *   unsupported.
 * - issued_utc: ESP time at assembly. PIC applies a VALID state if issued_utc
 *   >= last applied issued_utc, or source differs. Re-sent on change and periodically;
 *   no acknowledgement required (STAT-UI reflects applied offsets).
 * - Always held by the PIC, whatever its settings: SML_SETTING_TZ_AUTO takes
 *   the base offset (and sub-second offset) from it, SML_SETTING_DST_AUTO the
 *   DST amount and state; each falls back to the manual setting when clear or
 *   while no schedule is held (SML_UI_TZ_HELD clear: the ESP should resend). */
typedef struct
{
    /* flags:
     * bit 0: SML_TZ_VALID
     * bit 1: SML_TZ_AMBIGUOUS
     * All other bits reserved; transmit zero. */
    uint16_t flags;
    uint8_t source; // SML_TZ_SOURCE_*
    uint8_t count;  // future entries used, 0..SML_TZ_TRANSITION_MAX
    uint32_t issued_utc;
    uint32_t horizon_end_utc;
    int32_t subsecond_offset_ns;
    SML_TZ_ENTRY current;
    SML_TZ_ENTRY next[SML_TZ_TRANSITION_MAX];
} SML_TZ_STATE_PAYLOAD;

/* SML-STAT-UI, PIC -> ESP. local_seconds is UTC plus applied base/DST,
 * a civil-time scalar, NOT another UTC timestamp. Alarm offset is seconds
 * from local midnight (0..86399). Delta epoch is the configured UTC epoch.
 * current/selected_display: SML_DISPLAY_*; button_state: SML_BUTTON_*;
 * menu_state: SML_MENU_*. */
typedef struct
{
    /* flags:
     * bit 0: SML_UI_LOCAL_VALID
     * bit 1: SML_UI_ALARM_ENABLED
     * bit 2: SML_UI_ALARM_ACTIVE
     * bit 3: SML_UI_BRIGHTNESS_AUTO
     * bit 4: SML_UI_POWER_ON
     * bit 5: SML_UI_SWITCH
     * bit 6: SML_UI_BUTTON
     * bit 7: SML_UI_UPDATE_PENDING
     * bit 8: SML_UI_FORMAT_12H
     * bit 9: SML_UI_DST_ACTIVE
     * bit 10: SML_UI_TZ_STALE
     * bit 11: SML_UI_TZ_AMBIGUOUS
     * bit 12: SML_UI_TZ_HELD
     * bit 13: SML_UI_DELTA_LOCAL
     * All other bits reserved; transmit zero. */
    uint16_t flags;
    uint8_t current_display;
    uint8_t selected_display;
    uint8_t menu_state;
    uint8_t button_state;
    uint16_t brightness;
    uint16_t brightness_target;
    uint16_t reserved;
    uint32_t local_seconds;
    int32_t base_offset_s;
    int32_t dst_offset_s;
    uint32_t alarm_offset_s;
    uint32_t delta_epoch_utc;
} SML_STAT_UI_PAYLOAD;

/* SML-STAT-ENV, PIC -> ESP. Unsupported fields zero, validity clear.
 * BME/board fields support V2; lux supports V2/V3. No sample ages.
 * Lux whole lx (0..65535); temperatures signed centi-degrees C
 * (-327.68..327.67 C); pressure 10 Pa/count (0..655350 Pa);
 * humidity centi-percent RH (0..10000 means 0..100%).
 * Sender rounds to these units; out-of-range results clear validity rather
 * than wrapping. No sentinel values: validity flags gate each measurement. */
typedef struct
{
    /* flags:
     * bit 0: SML_ENV_LUX_PRESENT
     * bit 1: SML_ENV_LUX_VALID
     * bit 2: SML_ENV_BME_PRESENT
     * bit 3: SML_ENV_TEMP_VALID
     * bit 4: SML_ENV_PRESSURE_VALID
     * bit 5: SML_ENV_HUMIDITY_VALID
     * bit 6: SML_ENV_BOARD_TEMP_VALID
     * All other bits reserved; transmit zero. */
    uint16_t flags;
    uint16_t reserved;
    uint16_t lux;
    int16_t ambient_temperature_centic;
    int16_t board_temperature_centic;
    uint16_t pressure_deca_pa;
    uint16_t humidity_centipercent;
} SML_STAT_ENV_PAYLOAD;

/* SML-STAT-SUPPLY, V3 PIC -> ESP. Fixed measured rails; no current sensing.
 * All voltages mV. Enable flags report driven control outputs.
 * Driver frequency is actual configured switching frequency in Hz, <50000,
 * not the scheduler update rate. Validity is independent of enable state. */
typedef struct
{
    /* flags:
     * bit 0: SML_SUPPLY_5V0_VALID
     * bit 1: SML_SUPPLY_3V3_VALID
     * bit 2: SML_SUPPLY_VFIL_VALID
     * bit 3: SML_SUPPLY_VPP_VALID
     * bit 4: SML_SUPPLY_VFIL_ENABLED
     * bit 5: SML_SUPPLY_VPP_ENABLED
     * bit 6: SML_SUPPLY_DRIVER_ENABLED
     * bit 7: SML_SUPPLY_DRIVER_FREQ_VALID
     * All other bits reserved; transmit zero. */
    uint16_t flags;
    uint16_t reserved;
    uint16_t rail_5v0_mv;
    uint16_t rail_3v3_mv;
    uint16_t filament_mv;
    uint16_t grid_vpp_mv;
    uint16_t vfil_driver_frequency_hz;
} SML_STAT_SUPPLY_PAYLOAD;

/* SML-STAT-NET, ESP -> PIC. The ESP's network and services, for the PIC's
 * menu and status. Sent on change and every 10 s; the PIC treats it as
 * unknown after 30 s without one. NTP timing stays in TIME-NTP.
 * ipv4: address in network order (a.b.c.d = a << 24 | ... | d).
 * esp_version: major << 16 | minor << 8 | patch. */
typedef struct
{
    /* flags:
     * bit 0: SML_NET_WIFI_CONNECTED
     * bit 1: SML_NET_IP_VALID
     * bit 2: SML_NET_RSSI_VALID
     * bit 3: SML_NET_NTP_ENABLED
     * bit 4: SML_NET_NTP_SYNCED
     * bit 5: SML_NET_TZINFO_ENABLED (remote time zone lookup)
     * bit 6: SML_NET_TZINFO_VALID (last lookup succeeded)
     * bit 7: SML_NET_LOCAL_TZ_VALID (a zone configured on the ESP itself)
     * bit 8: SML_NET_TELNET_ENABLED
     * bit 9: SML_NET_TELNET_CLIENT
     * bit 10: SML_NET_UPDATE_AUTO
     * bit 11: SML_NET_UPDATE_AVAILABLE
     * bit 12: SML_NET_UPDATE_IN_PROGRESS
     * bit 13: SML_NET_SD_PRESENT (V3)
     * All other bits reserved; transmit zero. */
    uint16_t flags;
    uint8_t wifi_state; // SML_WIFI_*
    int8_t rssi_dbm;
    uint32_t ipv4;
    uint32_t uptime_s;
    uint32_t tzinfo_updated_utc; // last successful lookup (TZINFO_VALID)
    uint32_t tzinfo_refresh_utc; // next lookup due (TZINFO_ENABLED)
    uint32_t esp_version;
} SML_STAT_NET_PAYLOAD;

/* SML-STAT-SOURCES, PIC -> ESP. The PIC's time sources as its synchroniser
 * sees them: one entry per candidate, holdover included, and which is in
 * use and which would be next. Sent on change (selection, second choice,
 * sync state or any verdict; at most once an epoch), every 10 s, and in
 * answer to SML_USER_REQUEST_MESSAGE for it (before the ACK).
 * Each entry's source is SML_SOURCE_GNSS, NTP, RTC or HOLDOVER; SML_SOURCE_NONE
 * marks an unused entry. Entries are in no particular order: rank orders them
 * (1 = best, 0 = not a candidate). */
#define SML_SOURCES_SYNCHRONISED 0x0001U // the PIC counts itself synchronised
#define SML_SOURCES_FALLBACK     0x0002U // selected only as a fallback (INACCURATE kept)
#define SML_ENTRY_PRESENT        0x01U
#define SML_ENTRY_TIME_VALID     0x02U
#define SML_ENTRY_EDGE           0x04U // its edge was seen this epoch
#define SML_ENTRY_CAPTURED       0x08U // ... by the capture hardware: it can steer
#define SML_ENTRY_LABELLED       0x10U // the edge's UTC second is known
#define SML_ENTRY_PHASE_VALID    0x20U
#define SML_ENTRY_BOUND_VALID    0x40U // a candidate: bound_ns is set
typedef struct
{
    uint8_t source;  // SML_SOURCE_*
    uint8_t verdict; // SML_VERDICT_*
    uint8_t rank;    // 1 = best; 0 = not a candidate
    /* flags:
     * bit 0: SML_ENTRY_PRESENT
     * bit 1: SML_ENTRY_TIME_VALID
     * bit 2: SML_ENTRY_EDGE
     * bit 3: SML_ENTRY_CAPTURED
     * bit 4: SML_ENTRY_LABELLED
     * bit 5: SML_ENTRY_PHASE_VALID
     * bit 6: SML_ENTRY_BOUND_VALID
     * bit 7: reserved; transmit zero. */
    uint8_t flags;
    uint16_t good_run;       // consecutive GOOD epochs, saturating
    uint16_t usable_run;     // consecutive GOOD or INACCURATE epochs, saturating
    uint32_t bound_ns;       // +/- bound the PIC ranks it by (BOUND_VALID)
    uint32_t uncertainty_ns; // the source's own figure; holdover: the bound when held
    int32_t phase_ns;        // the PIC's epoch minus the source's UTC second (PHASE_VALID)
    uint32_t age_s;          // since its last GOOD epoch; holdover: since last steered; 0xFFFFFFFF never
} SML_SOURCE_ENTRY;

typedef struct
{
    /* flags:
     * bit 0: SML_SOURCES_SYNCHRONISED
     * bit 1: SML_SOURCES_FALLBACK
     * All other bits reserved; transmit zero. */
    uint16_t flags;
    uint8_t sync_state;     // SML_SYNC_*
    uint8_t selected;       // SML_SOURCE_* in use: HOLDOVER while holding; NONE
    uint8_t second;         // the best other candidate, or SML_SOURCE_NONE
    uint8_t challenger;     // a candidate on its way to taking over, or SML_SOURCE_NONE
    uint16_t challenge_run; // epochs it has been better by the switching margin
    uint32_t epoch_sequence;
    SML_SOURCE_ENTRY entries[SML_SOURCES_ENTRIES];
} SML_STAT_SOURCES_PAYLOAD;

/* SML-STAT-SYS, PIC -> ESP. The PIC's counterpart to STAT-NET's uptime and
 * version: what the ESP needs to notice and explain a PIC restart. Sent every
 * epoch, from the first after the PIC starts (or enters SML mode), and on
 * REQUEST_MESSAGE.
 * reset_cause: SML_RESET_* bits as captured at start-up (several may be set:
 * a power-on sets POR and BOR; a trap with no handler resets through the
 * RESET instruction, setting TRAPR and SWR).
 * uptime_s: seconds since start-up, counted in epochs; it starts under a
 * second after reset, and a drop means the PIC restarted.
 * pic_version: major << 16 | minor << 8 | patch, as esp_version.
 * loop_max_ms: the longest pass of the PIC's main loop since the previous
 * STAT-SYS (normally the last second): work that held everything else up. */
typedef struct
{
    uint16_t flags; // reserved; transmit zero
    uint16_t reset_cause;
    uint32_t uptime_s;
    uint32_t pic_version;
    uint16_t loop_max_ms;
    uint16_t reserved;
} SML_STAT_SYS_PAYLOAD;

/* Shared settings body. No EEPROM marker/checksum or action/reset flags.
 * Persistent selected display only; current runtime mode belongs in STAT-UI.
 * TZ_AUTO: base offset from the held TZ-STATE, else base_offset_s.
 * DST_AUTO: DST amount and state from the held TZ-STATE, else dst_offset_s and
 * DST_ACTIVE. The two are independent.
 * base/dst offsets here are the MANUAL values only. The automatic TZ schedule
 * (SML-TZ-STATE) is persisted separately by the PIC so that ESP updates never
 * bump the settings revision or collide with UI edits.
 * calibration_fosc_hz is stored baseline, NOT live estimator telemetry.
 * alarm 0..86399, brightness hardware-range validation by PIC. */
typedef struct
{
    uint16_t schema_version;
    /* flags:
     * bit 0: SML_SETTING_TZ_AUTO
     * bit 1: SML_SETTING_DST_AUTO
     * bit 2: SML_SETTING_DST_ACTIVE
     * bit 3: SML_SETTING_ALARM_ENABLED
     * bit 4: SML_SETTING_BEEP_ENABLED
     * bit 5: SML_SETTING_FORMAT_12H
     * bit 6: SML_SETTING_BRI_AUTO
     * bit 7: SML_SETTING_DELTA_LOCAL
     * All other bits reserved; transmit zero. */
    uint16_t flags;
    int32_t base_offset_s;
    int32_t dst_offset_s;
    uint32_t calibration_fosc_hz;
    uint32_t alarm_offset_s;
    uint32_t delta_epoch_utc;
    uint16_t brightness;
    uint8_t selected_display;
    uint8_t reserved;
} SML_SETTINGS_BODY;
typedef struct
{
    /* flags:
     * bit 0: SML_SETTINGS_VALID
     * bit 1: SML_SETTINGS_DIRTY
     * bit 2: SML_SETTINGS_DEFAULTS
     * All other bits reserved; transmit zero. */
    uint16_t flags;
    uint16_t reserved;
    uint32_t revision;
    SML_SETTINGS_BODY settings;
} SML_STAT_SETTINGS_PAYLOAD;

// SML-STAT-DEBUG, PIC -> ESP: raw text[length], no flags, no NUL.
// 1..240 bytes/frame; split larger producer chunks. No struct required.

/* Common responses, both ways: status enum, detail enum and token.
 * ACK means completed, ACCEPTED means asynchronous operation queued.
 * A queued operation must later send final ACK/NACK with same token.
 * Tokens must not be reused while pending. Unknown code -> NACK.
 * Corrupt frames receive no response (request identity is untrusted). */
#define SML_RESULT_ACK           0U
#define SML_RESULT_NACK          1U
#define SML_RESULT_ACCEPTED      2U
#define SML_ERROR_NONE           0U
#define SML_ERROR_UNSUPPORTED    1U
#define SML_ERROR_LENGTH         2U
#define SML_ERROR_ARGUMENT       3U
#define SML_ERROR_BUSY           4U
#define SML_ERROR_STALE_REVISION 5U
#define SML_ERROR_SCHEMA         6U
#define SML_ERROR_STORAGE        7U
#define SML_ERROR_VERIFY         8U
#define SML_ERROR_STATE          9U
#define SML_ERROR_CRC            10U // BOOT-DATA: the chunk's CRC-32 is wrong; send it again
#define SML_ERROR_NOT_BLANK      11U // BOOT-DATA: flash neither erased nor identical

// SML-CMD-USER, both ways. Codes below 0x8000 run on the PIC (sent by the ESP),
// 0x8000 and up on the ESP (sent by the PIC, e.g. from its menu). The wrong
// half is answered NACK UNSUPPORTED. Argument interpreted by command; signed
// values use two's complement bits. Settings are not commands: they are
// written whole with SML-CMD-SETTINGS.
typedef struct
{
    uint16_t command;
    // flags:
    // All other bits reserved; transmit zero.
    uint16_t flags; // flags reserved zero
    uint16_t token;
    uint32_t argument;
} SML_CMD_USER_PAYLOAD;
#define SML_OPERATION_SETTINGS_WRITE 0x0100U // CMD-RESPONSE command for CMD-SETTINGS
// Run by the PIC. The two halves share the low byte where the intent is the same.
#define SML_USER_SHOW              0x0001U // arg: SML_SHOW_*; printed as STAT-DEBUG
#define SML_USER_REQUEST_MESSAGE   0x0002U // arg: class<<8 | ID; upper 16 zero
#define SML_USER_SAVE              0x0010U // ACCEPTED, then ACK once stored
#define SML_USER_DEFAULTS          0x0011U // factory settings, running only
#define SML_USER_SET_RTC           0x0020U // arg: UTC seconds
#define SML_USER_SET_DEBUG         0x0021U // arg: clear << 16 | set, SML_DEBUG_* bits
#define SML_USER_RESYNC            0x0030U
#define SML_USER_RESET             0x0031U
#define SML_USER_ENTER_BOOTLOADER  0x0032U
// Run by the ESP.
#define SML_USER_ESP_FIRST         0x8000U // codes from here run on the ESP
#define SML_USER_ESP_SAVE          0x8010U // save the ESP's settings
#define SML_USER_ESP_DEFAULTS      0x8011U // the ESP's defaults, running only
#define SML_USER_ESP_RESET         0x8031U // restart the ESP
#define SML_USER_ESP_NTP_SYNC      0x8040U // poll NTP now
#define SML_USER_ESP_TZ_REMOTE     0x8050U // arg: 0/1, remote time zone lookups
#define SML_USER_ESP_TZ_LOOKUP     0x8051U // look the time zone up now, and use it
#define SML_USER_ESP_WIFI_FORGET   0x8060U // forget the credentials, then restart
#define SML_USER_ESP_WIFI_SETUP    0x8061U // start the setup access point
#define SML_USER_ESP_UPDATE_CHECK  0x8070U // check for a firmware update

// SML_USER_SHOW topics: what the PIC prints.
#define SML_SHOW_ALL     0U // every module (the ESP sends this at start)
#define SML_SHOW_STORED  1U // the settings, schedule and oscillator in flash, and the store
#define SML_SHOW_TZ      2U // the time zone in force and the schedule held
#define SML_SHOW_DISPLAY 3U // display, brightness, light sensor, button and switch
#define SML_SHOW_RTC     4U // the RTC and the RTC source
#define SML_SHOW_OSC     5U // the oscillator: running, measured and stored
#define SML_SHOW_GNSS    6U // the receiver
#define SML_SHOW_COUNT   7U
// With a topic: everything, the counters and internals included.
#define SML_SHOW_VERBOSE 0x0100U

typedef struct
{
    uint16_t status;
    uint16_t detail;
    uint16_t token;
    uint16_t command;           // Echo operation; SML_OPERATION_SETTINGS_WRITE for bulk write
    uint32_t settings_revision; // Current revision, including stale rejection
} SML_CMD_RESPONSE_PAYLOAD;
/* SML-CMD-SETTINGS: validate all, apply atomically, increment revision.
 * expected_revision prevents overwriting unseen UI/local changes.
 * ACK returns new revision. Bit0 requests persistence; EEPROM write policy
 * remains implementation-defined and must not be driven by periodic snapshots. */
#define SML_WRITE_PERSIST 0x0001U
typedef struct
{
    // flags:
    // bit 0: SML_WRITE_PERSIST
    // All other bits reserved; transmit zero.
    uint16_t flags;
    uint16_t reserved;
    uint16_t token;
    uint32_t expected_revision;
    SML_SETTINGS_BODY settings;
} SML_CMD_SETTINGS_PAYLOAD;

/* ---- Class 0xF0 BOOT (2.5) ------------------------------------------------
 * The bootloader messages
 * Answered by the SPL for regions APP and TEST, and by the application for region
 * SPL; the SPL ignores every other class. One request at a time.
 * Flash layout, records and the mailbox: common/boot_layout.h. */
#define SML_BOOT_PROTOCOL 1U
#define SML_BOOT_CHUNK_INSTRUCTIONS 64U // 0x80 program address units
// Regions (BOOT-COMMAND, BOOT-DATA region).
#define SML_BOOT_REGION_APP  0U // 0x002000-0x028FFF and its record: the SPL writes it
#define SML_BOOT_REGION_SPL  1U // 0x000800-0x001F7F and its record: the application writes it
#define SML_BOOT_REGION_TEST 2U // 0x028800-0x028F7F, record 0x028F80: the SPL, while free
// BOOT-COMMAND command.
#define SML_BOOT_CMD_STATUS  0x01U // BOOT-STATUS, then ACK. SPL and application
#define SML_BOOT_CMD_HOLD    0x02U // ACK; stay in the SPL until RUN. SPL
#define SML_BOOT_CMD_ERASE   0x03U // key ERASE; ACCEPTED, then ACK (value: pages)
#define SML_BOOT_CMD_COMMIT  0x04U // key COMMIT; ACCEPTED, then ACK
#define SML_BOOT_CMD_RUN     0x05U // ACK, then the application. SPL
#define SML_BOOT_CMD_RESET   0x06U // ACK, then a software reset. SPL
#define SML_BOOT_CMD_CONFIRM 0x07U // key COMMIT; confirm the booted SPL. Application
#define SML_BOOT_KEY_ERASE   0x45524153UL // "ERAS"
#define SML_BOOT_KEY_COMMIT  0x434F4D4DUL // "COMM"
// BOOT-STATUS mode.
#define SML_BOOT_MODE_WINDOW 0U // the boot window: hands over unless HOLD
#define SML_BOOT_MODE_HELD   1U // stays until RUN
#define SML_BOOT_MODE_NO_APP 2U // no valid application: waits
#define SML_BOOT_MODE_LOOP   3U // the boot-loop guard: waits
#define SML_BOOT_MODE_APP    4U // the application is answering
// BOOT-STATUS flags.
#define SML_BOOT_FLAG_RECORD    0x0001U // application record present, its CRC good
#define SML_BOOT_FLAG_COMMITTED 0x0002U
#define SML_BOOT_FLAG_CHECKING  0x0004U // the image CRC-32 not computed yet
#define SML_BOOT_FLAG_IMAGE_OK  0x0008U // the image CRC-32 matches: valid
#define SML_BOOT_FLAG_ERASED    0x0010U // erased in this session, the region last erased
#define SML_BOOT_FLAG_REQUESTED 0x0020U // entered on the application's request
#define SML_BOOT_FLAG_POWER_ON  0x0040U // RCON POR or BOR
#define SML_BOOT_FLAG_TEST_FREE 0x0080U // the application does not reach the test page
// BOOT-STATUS boot: how this start came about, the mailbox's boot word
// (BOOT_PATH_* in boot_layout.h, the same bits).
#define SML_BOOT_PATH_APP_DIRECT   0x01U // the IPL started the application itself
#define SML_BOOT_PATH_UNCONFIRMED  0x02U // an unconfirmed SPL was started
#define SML_BOOT_PATH_NO_SPL       0x04U // the SPL is not intact
#define SML_BOOT_PATH_SPL_GIVEN_UP 0x08U // the SPL is rejected or out of trials
#define SML_BOOT_PATH_LAST_RESORT  0x10U // the SPL started despite that: no application
// BOOT-STATUS spl_state.
#define SML_SPL_STATE_RECORD    0x01U // record present, its CRC good
#define SML_SPL_STATE_COMMITTED 0x02U
#define SML_SPL_STATE_INTACT    0x04U // the code's CRC-32 matches
#define SML_SPL_STATE_CONFIRMED 0x08U
#define SML_SPL_STATE_REJECTED  0x10U

/* SML-BOOT-STATUS, PIC->ESP: the SPL on entry, every second while idle and
 * before the ACK to STATUS; the application before the ACK to STATUS only. */
typedef struct
{
    uint32_t spl_version;    // from the SPL record (major << 16 | minor << 8 | patch), else 0
    uint8_t protocol;        // SML_BOOT_PROTOCOL
    uint8_t mode;            // SML_BOOT_MODE_*
    uint16_t flags;          // SML_BOOT_FLAG_*
    uint16_t reset_cause;    // RCON as found (SML_RESET_*)
    uint16_t window_ms;      // left of the boot window (WINDOW), else 0
    uint32_t device_id;      // DEVID
    uint32_t region_start;   // 0x002000
    uint32_t region_end;     // 0x029000, exclusive
    uint32_t app_version;    // from the application record (RECORD), else 0
    uint32_t app_length;     // instructions
    uint32_t app_crc32;      // from the record
    uint32_t crc32_computed; // over app_length (not while CHECKING); the record's in APP
    uint32_t build_time;     // from the record, Unix seconds
    uint8_t attempts;        // hand-overs since the application last reported healthy
    uint8_t boot;            // SML_BOOT_PATH_*
    uint16_t ipl_version;    // major << 8 | minor
    uint8_t spl_state;       // SML_SPL_STATE_*
    uint8_t trials;          // starts of the SPL while unconfirmed
    uint16_t healthy_s;      // the application: seconds healthy, saturating; the SPL: 0
    uint32_t spl_crc32;      // from the SPL record
} SML_BOOT_STATUS_PAYLOAD;

/* SML-BOOT-COMMAND, ESP->PIC. args: COMMIT's record fields; zero otherwise.
 * For APP and TEST, version is the application's and extra its SML version
 * (major << 8 | minor); for SPL, the SPL's and its bootloader protocol. */
typedef struct
{
    uint32_t version;
    uint32_t start;  // APP 0x002000, TEST 0x028800, SPL 0x000800
    uint32_t length; // instructions, even
    uint32_t crc32;  // of the packed image
    uint32_t build_time;
    uint16_t extra;
    uint16_t reserved;
} SML_BOOT_COMMIT_ARGS;
typedef struct
{
    uint16_t token;  // the ESP's, echoed
    uint8_t command; // SML_BOOT_CMD_*
    uint8_t region;  // SML_BOOT_REGION_*; 0 for commands without one
    uint32_t key;    // SML_BOOT_KEY_ERASE, SML_BOOT_KEY_COMMIT (COMMIT, CONFIRM), else 0
    SML_BOOT_COMMIT_ARGS args;
} SML_BOOT_COMMAND_PAYLOAD;

// SML-BOOT-RESPONSE, PIC->ESP: SML_RESULT_* and SML_ERROR_*.
typedef struct
{
    uint16_t status;
    uint16_t detail;
    uint16_t token;
    uint8_t command;
    uint8_t region;
    uint32_t value; // ERASE: pages erased; COMMIT NACK VERIFY: the CRC-32 computed
} SML_BOOT_RESPONSE_PAYLOAD;

/* SML-BOOT-DATA, ESP->PIC: one chunk, written once into erased flash (an
 * identical chunk is ACKed again). data: 64 instructions, 3 bytes each,
 * least significant first. */
typedef struct
{
    uint32_t address; // a multiple of 0x80 within the region
    uint8_t region;
    uint8_t reserved[3];
    uint32_t crc32; // CRC-32 of data
    uint8_t data[SML_BOOT_CHUNK_BYTES];
} SML_BOOT_DATA_PAYLOAD;

// SML-BOOT-DATA-RESPONSE, PIC->ESP.
typedef struct
{
    uint16_t status; // ACK or NACK
    uint16_t detail;
    uint32_t address;
    uint32_t crc32; // of the chunk as now in flash (ACK)
} SML_BOOT_DATA_RESPONSE_PAYLOAD;

// SML-BOOT-VERIFY, ESP->PIC: any chunk below 0x029800, at any time.
typedef struct
{
    uint32_t address;
    uint32_t reserved;
} SML_BOOT_VERIFY_PAYLOAD;

// SML-BOOT-VERIFY-RESPONSE, PIC->ESP: the chunk read from flash.
typedef struct
{
    uint16_t status; // ACK, or NACK ARGUMENT
    uint16_t detail;
    uint32_t address;
    uint32_t crc32; // of data
    uint8_t data[SML_BOOT_CHUNK_BYTES];
} SML_BOOT_VERIFY_RESPONSE_PAYLOAD;

#pragma pack(pop)

// C11/C++11 checks; also work with older XC16 language modes.
#define SML_SIZE_CHECK(type, n) typedef char sml_size_##type[(sizeof(type) == (n)) ? 1 : -1]
SML_SIZE_CHECK(SML_HEADER, SML_HEADER_BYTES);
SML_SIZE_CHECK(SML_CHECKSUM, SML_CRC_BYTES);
SML_SIZE_CHECK(SML_TIME_AUTH_PAYLOAD, SML_TIME_AUTH_BYTES);
SML_SIZE_CHECK(SML_TIME_PIC_PAYLOAD, SML_TIME_PIC_BYTES);
SML_SIZE_CHECK(SML_TIME_CTRL_PAYLOAD, SML_TIME_CTRL_BYTES);
SML_SIZE_CHECK(SML_TIME_MEAS_PAYLOAD, SML_TIME_MEAS_BYTES);
SML_SIZE_CHECK(SML_TIME_VCTCXO_PAYLOAD, SML_TIME_VCTCXO_BYTES);
SML_SIZE_CHECK(SML_TIME_GNSS_PAYLOAD, SML_TIME_GNSS_BYTES);
SML_SIZE_CHECK(SML_TIME_NTP_PAYLOAD, SML_TIME_NTP_BYTES);
SML_SIZE_CHECK(SML_TIME_RTC_PAYLOAD, SML_TIME_RTC_BYTES);
SML_SIZE_CHECK(SML_TIME_LEAP_PAYLOAD, SML_TIME_LEAP_BYTES);
SML_SIZE_CHECK(SML_TZ_ENTRY, SML_TZ_ENTRY_BYTES);
SML_SIZE_CHECK(SML_TZ_STATE_PAYLOAD, SML_TZ_STATE_BYTES);
SML_SIZE_CHECK(SML_STAT_UI_PAYLOAD, SML_STAT_UI_BYTES);
SML_SIZE_CHECK(SML_STAT_ENV_PAYLOAD, SML_STAT_ENV_BYTES);
SML_SIZE_CHECK(SML_STAT_SUPPLY_PAYLOAD, SML_STAT_SUPPLY_BYTES);
SML_SIZE_CHECK(SML_SETTINGS_BODY, SML_SETTINGS_BODY_BYTES);
SML_SIZE_CHECK(SML_STAT_SETTINGS_PAYLOAD, SML_STAT_SETTINGS_BYTES);
SML_SIZE_CHECK(SML_STAT_NET_PAYLOAD, SML_STAT_NET_BYTES);
SML_SIZE_CHECK(SML_SOURCE_ENTRY, SML_SOURCE_ENTRY_BYTES);
SML_SIZE_CHECK(SML_STAT_SOURCES_PAYLOAD, SML_STAT_SOURCES_BYTES);
SML_SIZE_CHECK(SML_STAT_SYS_PAYLOAD, SML_STAT_SYS_BYTES);
SML_SIZE_CHECK(SML_CMD_USER_PAYLOAD, SML_CMD_USER_BYTES);
SML_SIZE_CHECK(SML_CMD_RESPONSE_PAYLOAD, SML_CMD_RESPONSE_BYTES);
SML_SIZE_CHECK(SML_CMD_SETTINGS_PAYLOAD, SML_CMD_SETTINGS_BYTES);
SML_SIZE_CHECK(SML_BOOT_STATUS_PAYLOAD, SML_BOOT_STATUS_BYTES);
SML_SIZE_CHECK(SML_BOOT_COMMAND_PAYLOAD, SML_BOOT_COMMAND_BYTES);
SML_SIZE_CHECK(SML_BOOT_RESPONSE_PAYLOAD, SML_BOOT_RESPONSE_BYTES);
SML_SIZE_CHECK(SML_BOOT_DATA_PAYLOAD, SML_BOOT_DATA_BYTES);
SML_SIZE_CHECK(SML_BOOT_DATA_RESPONSE_PAYLOAD, SML_BOOT_DATA_RESPONSE_BYTES);
SML_SIZE_CHECK(SML_BOOT_VERIFY_PAYLOAD, SML_BOOT_VERIFY_BYTES);
SML_SIZE_CHECK(SML_BOOT_VERIFY_RESPONSE_PAYLOAD, SML_BOOT_VERIFY_RESPONSE_BYTES);
#undef SML_SIZE_CHECK
#endif
