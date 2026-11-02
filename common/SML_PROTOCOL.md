# SML: Smol Clock serial link protocol

_Two microcontrollers, one UART, and a strong preference for agreeing on where each byte goes._

SML is the link between the dsPIC33EP256GP504 timekeeper (the **PIC**) and the ESP32 network co-processor (the **ESP**). It replaces the 0x83-magic structs used up to V2. The framing borrows heavily from u-blox UBX: a two-byte sync word, a class and an ID, a 16-bit length, the payload, and a CRC.

The PIC keeps UTC and disciplines the clock's epoch. The ESP supplies what the PIC cannot get on its own: network time, timezone schedules, leap-second data and user commands. In return the ESP receives the PIC's state for display, logging and statistics, and tells the PIC about its network and services.

This document describes protocol version 2.5 (`SML_PROTOCOL_MAJOR` 2, `SML_PROTOCOL_MINOR` 5) with settings schema 1 (`SML_SETTINGS_SCHEMA`). The definitions live in [sml_proto.h](sml_proto.h), and the reference codec in [sml.c](sml.c) and [sml.h](sml.h), all in `common/` and shared by both firmwares. TIME-AUTH is stable. Every other payload remains provisional until both ends implement it.

# Physical link

| | PIC (V2) | ESP32 (V2) |
| --- | --- | --- |
| UART | UART1: RX RC6 (`ESP_UART1_RX`), TX RC7 (`ESP_UART1_TX`) | UART2: RX GPIO13, TX GPIO12 |
| Format | 921600 baud, 8N1, no flow control | Same |
| PIC epoch to ESP | RB10, the epoch output (also the display latch) | GPIO27 |
| ESP PPS to PIC | RB15 (`ESP_PPS`) | GPIO5 |

At 921600 baud a byte takes about 10.9µs, so even a full 248-byte frame is on the wire for under 3ms. The PIC's divider gives 909091 baud, 1.36% low, which is comfortably inside what both UARTs tolerate. 1000000 baud would be exact on both ends, should the margin ever matter.

On the PIC, the UART1 receive and transmit rings hold 1KB each, and the receive ring is serviced every 10ms.

# Frame format

```
offset  0     1     2      3   4      5      6 ... 6+N-1   6+N    6+N+1
       +-----+-----+------+----+------+------+-----------+------+------+
       | B5  | 73  |CLASS | ID |LEN_H |LEN_L |  PAYLOAD  |CRC_H |CRC_L |
       +-----+-----+------+----+------+------+-----------+------+------+
```

| Field | Size | Value |
| --- | --- | --- |
| Sync | 2 | `0xB5 0x73` (`SML_MAGIC_1`, `SML_MAGIC_2`) |
| Class | 1 | Message class, see [Message catalogue](#message-catalogue) |
| ID | 1 | Message ID within the class |
| Length | 2 | Payload length N, big-endian, 0–240 (`SML_MAX_PAYLOAD_BYTES`) |
| Payload | N | Message body |
| CRC | 2 | CRC-16/IBM-3740, big-endian, over **sync through the last payload byte** |

The header is 6 bytes (`SML_HEADER_BYTES`) and the checksum 2 bytes (`SML_CRC_BYTES`), so the largest frame is 248 bytes (`SML_MAX_FRAME_BYTES`). Unlike UBX, the sync bytes are covered by the CRC.

## CRC

SML uses CRC-16/IBM-3740, also known as CRC-16/CCITT-FALSE:

| Parameter | Value |
| --- | --- |
| Polynomial | 0x1021 |
| Initial value | 0xFFFF |
| Reflect in / out | No / no |
| Final XOR | 0x0000 |
| Check (`"123456789"`) | **0x29B1** |

The reference implementation (`sml_crc`) uses a 16-entry nibble table, which costs 32 bytes of constant data rather than the 512 a byte table would need.

## Encoding rules

Every multi-byte field is big-endian, and signed integers are two's complement.

Native types never go on the wire. Floats, `time_t`, C enums, bitfields, pointers and unions are not sent. The packed structs in `sml_proto.h` define field order and size only. They are converted field by field, never cast or copied whole, so neither end's byte order or alignment can leak into the frame (see [Codec](#codec)).

Reserved fields and flag bits are sent as zero. A receiver ignores reserved bits and does not reject a frame because they are set, which leaves room for later additions.

All times are `uint32_t` Unix seconds, valid up to 2106-02-07T06:28:15Z, which should see out the clock. An inserted leap second (23:59:60) has no Unix value of its own. It is sent as 23:59:59 a second time, and TIME-LEAP and the GNSS leap fields say when that will happen.

All `*_age_ms` fields are `uint16_t` and saturate at 65535ms rather than wrapping. A saturated age does not by itself mean the data is invalid: the validity flags and the sender's own timeouts decide that.

Each validity flag covers its group of fields. Fields whose flag is clear carry no information, and are normally zero.

Byte strings (the BOOT chunks) go as they are, in order.

Every message except STAT-DEBUG has a fixed length. State messages are at least 8 bytes (`SML_MIN_STATE_BYTES`). Commands, responses and debug text may be shorter.

## Receiving

A receiver must behave as the reference parser (`sml_parser_push`) does.

Bytes outside a frame are noise. They are counted and discarded; on a link still carrying console text, that text is the noise. A header whose length exceeds 240 is not a frame.

When a CRC fails or a header is rejected, the receiver moves on by **one byte** and rescans what it has buffered. A real frame that began inside a false header is therefore still found. The parser was fuzzed with 20000 frames buried in noise dense with `B5` and `73` bytes, and every frame was recovered.

A frame still incomplete after the line has been idle for 100ms (`ESP_LINK_FRAME_TIMEOUT_MS` on the PIC) is dropped.

Corrupt frames are never answered, since nothing in them can be trusted, the token included.

A frame with an unknown class or ID is counted and ignored, so either end can add messages without breaking the other. A known message arriving in the wrong direction is likewise counted and ignored.

A known fixed-length message with the wrong length is counted and dropped. The exception is a command (class 0x80) with a good CRC and at least 6 bytes, which is answered NACK `SML_ERROR_LENGTH` using the token at payload offset 4.

# Link modes

The PIC has to cope with an ESP that does not speak SML, so its link has two modes (`dspic_src/esp/esp_link.h`):

| Mode | PIC sends | PIC receives |
| --- | --- | --- |
| **TEXT** (start-up default) | Console text, raw | SML frames are parsed and acted on. Anything else is counted as noise |
| **SML** | Console text inside STAT-DEBUG frames, plus the per-epoch state messages | SML frames |

The PIC switches to SML mode, permanently until it resets, on the **first valid SML frame from the ESP**. An SML-speaking ESP announces itself by sending any frame soon after it starts. CMD-USER `SHOW` with `SML_SHOW_ALL` is the recommended one, because it also returns the PIC's full diagnostics. Building the PIC with `-DESP_LINK_START_SML=1` makes it start in SML mode.

Traffic from old 0x83 firmware is harmless noise to an SML receiver. The chance of one of those messages passing for an SML frame is about 2⁻¹⁶ per false sync, and it would also need the right class, ID and length to be acted on.

Class 0xF0 belongs to the bootloader (see [BOOT class](#boot-class-0xf0)). The PIC's second-stage loader (the SPL) speaks only that class, in either link mode: it ignores everything else and sends no text.

# Conventions

Directions are written `PIC→ESP` and `ESP→PIC`. Offsets are payload offsets, counted from the first payload byte. `u8`, `u16` and `u32` are unsigned and `i8`, `i16` and `i32` signed, of the given width.

The "V2+ PIC" column in the message catalogue and the command table records what the dsPIC V2+ firmware does as of this revision. It is not part of the protocol, and is expected to fill in over time.

# Message catalogue

| Class | ID | Name | Direction | Length | V2+ PIC | Rate |
| --- | --- | --- | --- | --- | --- | --- |
| 0x10 TIME | 0x01 | TIME-AUTH | PIC→ESP | 16 | ✓ Sent | Every epoch, first |
| | 0x10 | TIME-PIC | PIC→ESP | 38 | ✓ Sent | Every epoch |
| | 0x11 | TIME-CTRL | PIC→ESP | 56 | ✓ Sent | Every epoch |
| | 0x12 | TIME-VCTCXO | PIC→ESP | 20 | — V3 hardware | Every epoch (V3) |
| | 0x13 | TIME-MEAS | PIC→ESP | 40 | ✓ Sent | Every epoch |
| | 0x20 | TIME-GNSS | PIC→ESP | 54 | ✓ Sent | Every epoch |
| | 0x30 | TIME-NTP | ESP→PIC | 42 | ✓ Labels the ESP PPS for the NTP sync source; `CORRECTING` holds the PIC's frequency estimate | Every ESP PPS while NTP time is valid |
| | 0x40 | TIME-RTC | PIC→ESP | 34 | ✓ Sent | Every epoch |
| | 0x50 | TIME-LEAP | ESP→PIC | 16 | ✓ Applied | On change, and periodically |
| 0x20 TZ | 0x01 | TZ-STATE | ESP→PIC | 124 | ✓ Applied | On change, after a PIC reset, and every ~10 minutes |
| | 0x10 | _Retired: TZ-FUTURE_ | — | — | — | Never reuse |
| 0x30 STAT | 0x01 | STAT-UI | PIC→ESP | 32 | ✓ Sent | Every epoch |
| | 0x10 | STAT-ENV | PIC→ESP | 14 | ✓ Sent: light only | Every 5 seconds |
| | 0x20 | STAT-SUPPLY | PIC→ESP | 14 | — V3 hardware | Every few seconds |
| | 0x30 | STAT-SETTINGS | PIC→ESP | 36 | ✓ Sent | On change, after reset, on request |
| | 0x40 | STAT-NET | ESP→PIC | 24 | ✓ Received and held | On change, and every 10 seconds |
| | 0x50 | STAT-SOURCES | PIC→ESP | 108 | ✓ Sent | On change, every 10 seconds, and on request |
| | 0x60 | STAT-SYS | PIC→ESP | 16 | ✓ Sent | Every epoch in SML mode, and on request |
| | 0xF0 | STAT-DEBUG | PIC→ESP | 1–240 | ✓ Sent in SML mode | As text is produced |
| 0x80 CMD | 0x10 | CMD-USER | Both ways | 10 | ✓ Runs PIC codes, sends ESP codes | On demand |
| | 0x20 | CMD-RESPONSE | Both ways | 12 | ✓ | One per command |
| | 0x30 | CMD-SETTINGS | ESP→PIC | 38 | ✓ Handled | On demand |
| 0xF0 BOOT | 0x01 | BOOT-STATUS | PIC→ESP | 56 | — Not yet (0.4.0) | SPL: on entry and every second while idle. Both: before the ACK to STATUS |
| | 0x10 | BOOT-COMMAND | ESP→PIC | 32 | — Not yet | On demand, one at a time |
| | 0x20 | BOOT-RESPONSE | PIC→ESP | 12 | — Not yet | One per command, two when ACCEPTED |
| | 0x50 | BOOT-DATA | ESP→PIC | 204 | — Not yet | One chunk at a time |
| | 0x60 | BOOT-DATA-RESPONSE | PIC→ESP | 12 | — Not yet | One per chunk |
| | 0x90 | BOOT-VERIFY | ESP→PIC | 8 | — Not yet | One chunk at a time |
| | 0xA0 | BOOT-VERIFY-RESPONSE | PIC→ESP | 204 | — Not yet | One per request |

# TIME class (0x10)

## TIME-AUTH (0x01)

_The authoritative time. PIC→ESP, 16 bytes._

This is the PIC's statement of the UTC second that its latest epoch edge marks, and the only message whose meaning is frozen.

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | flags | u16 | Bit 0 `UTC_VALID`, bit 1 `UNCERTAINTY_VALID`, bit 2 `SOURCE_VALID` |
| 2 | source | u8 | `SML_SOURCE_*`, see [Sources](#sources) |
| 3 | lock_state | u8 | `SML_LOCK_*`, see [Lock state](#lock-state) |
| 4 | utc_seconds | u32 | Unix seconds labelling the epoch edge |
| 8 | utc_phase_ns | i32 | ns. UTC at the edge = utc_seconds + utc_phase_ns × 10⁻⁹ |
| 12 | uncertainty_ns | u32 | ns, a ± bound |

`utc_phase_ns` is the epoch edge minus the reference edge (OC minus IC), so a positive value means the clock's edge is late. In holdover there is no reference edge to measure, and it is 0.

`uncertainty_ns` is a ± **bound** on the UTC error of the edge, not a standard deviation. While tracking, it is the selected source's bound plus the magnitude of the measured phase. A source's bound is made conservative from its own figure: 3 × GNSS tAcc + 25ns of capture resolution, or the ESP's NTP bound + 20µs for the jitter of its PPS. In holdover it is the holdover bound, u0 + f·t + ½·D·t², grown from the bound at the last steered epoch (see `sync/sync.h` on the PIC).

Without `UTC_VALID`, the phase and uncertainty are invalid too. In free run with the time of day still known (counted on from an earlier label), `UTC_VALID` is set and `UNCERTAINTY_VALID` is clear, since there is no honest bound to give. The exception is free running on the RTC's time (at start-up, before GNSS or NTP): `source` is `SML_SOURCE_RTC` and the uncertainty is the RTC's bound (as in TIME-RTC) while that is within a second; beyond, `UNCERTAINTY_VALID` is clear. `SOURCE_VALID` means a source is selected; the RTC never is.

TIME-AUTH is sent first in each epoch, a few milliseconds after the edge it labels and long before the next. The ESP pairs it with the most recent PIC edge captured on GPIO27.

## TIME-PIC (0x10)

_Epoch timer and synchroniser. PIC→ESP, 38 bytes._

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | flags | u16 | Bit 0 `PHASE_VALID`, bit 1 `PERIOD_VALID`, bit 2 `FREQUENCY_VALID`, bit 3 `PPS_VALID` |
| 2 | source | u8 | `SML_SOURCE_*` |
| 3 | lock_state | u8 | `SML_LOCK_*` |
| 4 | sync_state | u8 | `SML_SYNC_*`, see [Synchroniser state](#synchroniser-state) |
| 5 | previous_sync_state | u8 | `SML_SYNC_*` before the latest change |
| 6 | last_sync_cause | u8 | `SML_CAUSE_*`, see [Cause of the latest change](#cause-of-the-latest-change) |
| 7 | second_source | u8 | `SML_SOURCE_*` the PIC would turn to next, `HOLDOVER` included; `NONE` if there is none (1.3; 0 from older PICs) |
| 8 | epoch_sequence | u32 | Epochs since the PIC started |
| 12 | fosc_hz | u32 | Timer clock (FCY) measured against the reference, Hz (`FREQUENCY_VALID`) |
| 16 | pps_period_cycles | u32 | Latest reference PPS period, timer cycles (`PERIOD_VALID`) |
| 20 | phase_cycles | i32 | Epoch minus reference edge, cycles (`PHASE_VALID`) |
| 24 | applied_adjustment_cycles | i32 | Period requested for the next epoch minus nominal, cycles |
| 28 | missing_pps_events | u32 | Epochs in which the GNSS source had no edge |
| 32 | sync_events | u32 | Synchroniser state changes since start |
| 36 | reference_age_ms | u16 | Time since the last usable reference edge, ms (saturates) |

`fosc_hz` keeps its V2 meaning: the timer clock, FCY, nominally 40MHz. It is not the 80MHz oscillator, despite the name. One cycle is 25ns. `PPS_VALID` means the reference edge was captured in this epoch.

## TIME-CTRL (0x11)

_The epoch control loop. PIC→ESP, 56 bytes._

The estimator and servo state, sent **losslessly** as the PIC's own fixed-point values, so the ESP sees the full-precision frequency estimate rather than a rounded one.

A **tick** is one timer cycle: 1/`nominal_period_ticks` of a second, which is 25ns on V2. **Q16.16** values are ticks × 65536, and **Q2.30** values are scaled by 2³⁰.

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | flags | u16 | Bit 0 `ESTIMATE_VALID`, 1 `PI_ENABLED`, 2 `SATURATED`, 3 `MEASUREMENT_USED`, 4 `GATE_REJECTED` |
| 2 | controller_state | u8 | `SML_CONTROL_*`, see [Controller state](#controller-state) |
| 3 | reserved | u8 | 0 |
| 4 | epoch_sequence | u32 | As TIME-PIC |
| 8 | nominal_period_ticks | u32 | Ticks in a nominal second (40000000 on V2) |
| 12 | frequency_offset_q16 | i32 | Kalman state x: reference period minus nominal, Q16.16 ticks |
| 16 | frequency_variance_q30 | u32 | Kalman P / R, Q2.30 |
| 20 | measurement_variance_q16 | u32 | Kalman R, tick² Q16.16 |
| 24 | kalman_gain_q30 | u32 | Last Kalman gain, Q2.30 |
| 28 | predicted_phase_q16 | i32 | Phase predicted for the next epoch, Q16.16 ticks |
| 32 | phase_innovation_q16 | i32 | This epoch: measured minus predicted phase, Q16.16 ticks |
| 36 | pi_proportional_q16 | i32 | PI proportional term, Q16.16 ticks of period |
| 40 | pi_integral_q16 | i32 | PI integral, Q16.16 ticks of period |
| 44 | output_offset_q16 | i32 | Steering: period requested for the next epoch minus nominal, Q16.16 ticks |
| 48 | gate_rejections | u32 | Phase gate rejections since start |
| 52 | kalman_rejections | u32 | Kalman outlier rejections since start |

`ESTIMATE_VALID` means the Kalman filter is initialised, and gates offsets 12–28. `SATURATED` means the PI correction is at its limit. `MEASUREMENT_USED` means this epoch's phase fed the loop, and `GATE_REJECTED` that the phase gate turned it away.

On the ESP, in `float` and relative to the nominal:

| Quantity | Formula |
| --- | --- |
| Fractional frequency of the oscillator (positive = fast) | `frequency_offset_q16` / 65536 / `nominal_period_ticks`. One Q16 step is 0.38 × 10⁻¹² at 40MHz |
| Measured frequency | `nominal_period_ticks` + `frequency_offset_q16` / 65536 ticks per reference second |
| Standard deviation of the frequency estimate | √(`frequency_variance_q30` / 2³⁰ × `measurement_variance_q16` / 65536) ticks |
| Phase in ns | Q16 value / 65536 × 10⁹ / `nominal_period_ticks` |

The Kalman period innovation is not sent. The ESP can form it as `reference_period_ticks` (from TIME-MEAS) minus nominal minus the previous epoch's x.

## TIME-VCTCXO (0x12)

_VCTCXO steering, V3 only. PIC→ESP, 20 bytes._

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | flags | u16 | Bit 0 `PRESENT`, bit 1 `ENABLED`, bit 2 `SATURATED`, bit 3 `TEMP_VALID` |
| 2 | controller_state | u8 | `SML_CONTROL_*` |
| 3 | reserved | u8 | 0 |
| 4 | epoch_sequence | u32 | |
| 8 | dac_code | u16 | DAC code, range depends on the hardware |
| 10 | control_uv | u32 | Control voltage, µV |
| 14 | correction_ppt | i32 | 10⁻¹² |
| 18 | vctcxo_adc_temperature_centic | i16 | 0.01°C, from the ADC |

## TIME-GNSS (0x20)

_Receiver state. PIC→ESP, 54 bytes._

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | flags | u16 | Bit 0 `DETECTED`, 1 `FIX_OK`, 2 `UTC_VALID`, 3 `PPS_VALID`, 4 `POSITION_VALID`, 5 `LEAP_VALID`, 6 `LEAP_EVENT_VALID` |
| 2 | fix_type | u8 | UBX NAV-STATUS gpsFix: 0 none, 1 DR, 2 2D, 3 3D, 4 GNSS+DR, 5 time only |
| 3 | utc_standard | u8 | UBX NAV-TIMEUTC utcStandard (upper nibble of valid) |
| 4 | utc_seconds | u32 | Unix seconds of the latest navigation epoch (`UTC_VALID`) |
| 8 | utc_accuracy_ns | u32 | NAV-TIMEUTC tAcc, ns |
| 12 | pps_sequence | u32 | PIC capture sequence number of the PPS edge it labels (`PPS_VALID`) |
| 16 | utc_age_ms | u16 | Age of the navigation epoch |
| 18 | pps_age_ms | u16 | Age of the labelled PPS association |
| 20 | position_age_ms | u16 | Age of the position |
| 22 | latitude_e7 | i32 | 10⁻⁷ degrees (`POSITION_VALID`) |
| 26 | longitude_e7 | i32 | 10⁻⁷ degrees |
| 30 | height_mm | i32 | Height above the ellipsoid, mm |
| 34 | height_msl_mm | i32 | Height above mean sea level, mm |
| 38 | horizontal_accuracy_mm | u32 | mm. The ESP's GNSS margin when deciding whether the clock has moved |
| 42 | vertical_accuracy_mm | u32 | mm |
| 46 | gps_utc_s | i16 | GPS minus UTC, s (`LEAP_VALID`). TAI minus UTC = gps_utc_s + 19 |
| 48 | leap_change | i8 | −1, 0 or +1 (`LEAP_EVENT_VALID`) |
| 49 | reserved_leap | u8 | 0 |
| 50 | leap_event_utc | u32 | First UTC second at which the new count applies (a midnight) |

`UTC_VALID` needs a usable fix, fully valid UTC, and a navigation epoch aligned to the second. `POSITION_VALID` needs NAV-POSLLH in the same epoch with `FIX_OK`. The leap fields come from UBX-NAV-TIMELS, at most 30 seconds old, and `LEAP_EVENT_VALID` is only set when the announced event lands on a UTC midnight to within 2 seconds.

NAV-CLOCK, TIM-TP and TIM-TM2 detail is available through the debug text only.

## TIME-NTP (0x30)

_ESP network time. ESP→PIC, 42 bytes._

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | flags | u16 | Bit 0 reserved (was `WIFI_CONNECTED`, now in STAT-NET), 1 `TIME_VALID`, 2 `UPDATE_VALID`, 3 `PPS_SYNC`, 4 `SCHEDULER_SYNC`, 5 `UNCERTAINTY_VALID`, 6 `CORRECTING` |
| 2 | source | u8 | `SML_SOURCE_NTP`, or `SML_SOURCE_ESP` when coasting on its own clock |
| 3 | lock_state | u8 | `SML_LOCK_*`, the ESP's view of its own time |
| 4 | utc_seconds | u32 | UTC of the **ESP PPS edge** this message labels, not of the UART arrival |
| 8 | utc_phase_ns | i32 | ESP PPS edge minus UTC, ns |
| 12 | uncertainty_ns | u32 | ± bound, ns (`UNCERTAINTY_VALID`) |
| 16 | pps_sequence | u32 | ESP PPS edge count |
| 20 | last_update_utc | u32 | UTC of the last NTP exchange (`UPDATE_VALID`) |
| 24 | update_age_ms | u16 | Age of that exchange; saturates as a matter of course with long poll intervals |
| 26 | update_interval_s | u32 | NTP poll interval in use, s (adaptive on the ESP) |
| 30 | round_trip_us | u32 | Last round trip, µs |
| 34 | last_offset_us | i32 | Last NTP offset (estimated UTC minus ESP time), µs |
| 38 | frequency_estimate_ppt | i32 | ESP clock frequency error, 10⁻¹² |

The ESP sends TIME-NTP after each of its PPS edges. That gives the PIC an edge on RB15 and a label for it, which is what an NTP sync source needs. The V2+ PIC pairs each one with the latest ESP edge it has seen (`sync/source_ntp.c`), and corrects that edge by `utc_phase_ns` when steering to it.

`last_update_utc` changes at every NTP poll, and each poll brings a correction. A routine correction is slewed in no faster than `SML_NTP_GENTLE_SLEW_PPM` (5ppm), spread over about two poll intervals, so the edge moves by at most a few microseconds from one second to the next and a receiver can simply follow it. A step, or a correction large enough to be slewed faster, sets `CORRECTING` for as long as it lasts: the edge is then moving faster than any tracking loop should believe, and a receiver may hold its frequency estimate or re-acquire.

## TIME-RTC (0x40)

_Battery-backed RTC. PIC→ESP, 34 bytes._

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | flags | u16 | Bit 0 `PRESENT`, 1 `UTC_VALID`, 2 `SYNCED`, 3 `LAST_READ_VALID`, 4 `LAST_WRITE_VALID`, 5 `PPS_PRESENT`, 6 `PPS_VALID`, 7 `TEMP_VALID`, 8 `UNCERTAINTY_VALID` |
| 2 | rtc_type | u8 | `SML_RTC_*`, see [RTC type](#rtc-type) |
| 3 | reserved | u8 | 0 |
| 4 | rtc_utc_seconds | u32 | RTC reckoning now, advanced from the last good read |
| 8 | last_read_utc | u32 | Authoritative UTC when the last read completed |
| 12 | last_write_utc | u32 | Authoritative UTC when the last write completed |
| 16 | rtc_temperature_centic | i16 | 0.01°C, 0 if unsupported |
| 18 | pps_sequence | u32 | RTC PPS edge count (V3) |
| 22 | last_pps_utc | u32 | UTC of the last RTC PPS edge (V3) |
| 26 | pps_phase_ns | i32 | PIC epoch minus paired RTC edge, ns (V3) |
| 30 | uncertainty_ns | u32 | ± bound on the RTC reckoning, ns |

RTC register contents are never sent as timestamps. The PPS fields exist on V3 only; on V2 they are zero, with their presence and validity flags clear.

On the V2+ PIC (PCF8563, no temperature):

- `UTC_VALID`: the RTC's time can be used (VL and STOP clear, a real date). `rtc_utc_seconds` is its time as it counts it: the second it last ticked, counted on from where it was found at its learnt rate, else as last read. It is the RTC's own time, not corrected.
- `SYNCED`: the RTC's second has been measured against the synchronised clock since it was last written (a check, an aligned write's confirmation, or a search after a plain write). Clear after a plain write until then, and with nothing measured at all.
- `last_read_utc`: the clock's UTC when the RTC was last read, once the clock had a time. `last_write_utc`: the second the RTC was last set to, aligned or plainly, kept over a reset.
- `uncertainty_ns`: the bound on the RTC's time as the PIC predicts it from that measurement (the anchor), corrected for its learnt rate: the anchor's bound (the clock's then plus half the search's resolution, or 999 ms for a plain write) plus 10 ppm of the seconds since, or 500 ppm with no rate learnt; 999 ms with nothing measured. Saturated at 0xFFFFFFFE (4.29 s). The predicted offset itself is not sent; `rtc` prints it.

## TIME-LEAP (0x50)

_The leap second in force. ESP→PIC, 16 bytes._

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | flags | u16 | Bit 0 `VALID`, bit 1 `EVENT_PENDING` |
| 2 | source | u8 | `SML_LEAP_SOURCE_*`, see [Leap source](#leap-source) |
| 3 | reserved | u8 | 0 |
| 4 | tai_utc_s | i16 | TAI minus UTC in force now (37 in 2026) |
| 6 | change | i8 | −1, 0 or +1 |
| 7 | reserved2 | u8 | 0 |
| 8 | event_utc | u32 | First UTC second at which TAI minus UTC becomes tai_utc_s + change (a midnight) |
| 12 | expires_utc | u32 | Expiry of the leap data, 0 if unknown |

The ESP chooses the source: GNSS (the PIC's own TIME-GNSS leap fields) when valid, otherwise leap data from its server. GNSS wins any disagreement, which is logged. TIME-LEAP is sent on change and periodically.

On the PIC, a message with `VALID` and `EVENT_PENDING` set, `change` of ±1 and `event_utc` exactly on a UTC midnight is passed to the synchroniser (`sync_set_leap`). Anything else withdraws an earlier announcement. The synchroniser uses the announcement whenever its selected source announces none, holdover and free run included, but its own GNSS NAV-TIMELS takes precedence. With +1 pending, the display shows 23:59:60 UTC (or its local equivalent) for the second before `event_utc`, which is about as exciting as a desk clock gets.

## TIME-MEAS (0x13)

_Raw measurements of one epoch. PIC→ESP, 40 bytes._

This is the logging record for statistics and Allan deviation on the ESP. Nothing in it is filtered or rounded, and each record is complete on its own, keyed by `epoch_sequence`. It is sent every epoch, after TIME-CTRL.

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | flags | u16 | Bit 0 `LABEL_VALID`, 1 `PHASE_VALID`, 2 `PERIOD_VALID`, 3 `EPOCH_PERIOD_VALID`, 4 `MARK_VALID`, 5 `QERR_VALID`, 6 `MEASUREMENT_USED` |
| 2 | source | u8 | `SML_SOURCE_*` of the reference |
| 3 | controller_state | u8 | `SML_CONTROL_*` |
| 4 | epoch_sequence | u32 | |
| 8 | utc_seconds | u32 | UTC label of this epoch (`LABEL_VALID`) |
| 12 | reference_sequence | u32 | Reference edge count; a gap means missed edges |
| 16 | reference_period_ticks | u32 | Reference PPS-to-PPS interval, ticks (`PERIOD_VALID`) |
| 20 | phase_ticks | i32 | Epoch capture minus reference capture, whole ticks (`PHASE_VALID`) |
| 24 | epoch_period_ticks | u32 | Length of the second that ended at this epoch, as loaded into the epoch timer (`EPOCH_PERIOD_VALID`) |
| 28 | mark_offset_ns | i32 | TIM-TM2: the receiver's measurement of the **previous** epoch's edge (epoch_sequence − 1) against its second; positive = late (`MARK_VALID`) |
| 32 | mark_accuracy_ns | u32 | TIM-TM2 accuracy estimate |
| 36 | qerr_ps | i32 | TIM-TP quantisation error of this epoch's reference edge (`QERR_VALID`) |

`phase_ticks` is taken before the half-tick calibration. `reference_period_ticks` is measured on the free-running capture timebase, so it shows the oscillator against the reference, unsteered. Each TIM-TM2 mark is reported once only, in the record of the epoch after the edge it measured, so a mark is never paired with the wrong second. What each series measures is covered under [Statistics and Allan deviation](#statistics-and-allan-deviation).

# TZ class (0x20)

## TZ-STATE (0x01)

_The local time schedule. ESP→PIC, 124 bytes._

The whole schedule is replaced atomically. It holds the entry in force now, followed by up to eight future entries in ascending UTC order.

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | flags | u16 | Bit 0 `VALID` (the single gating flag), bit 1 `AMBIGUOUS` (informational) |
| 2 | source | u8 | `SML_TZ_SOURCE_*`, see [Timezone source](#timezone-source) |
| 3 | count | u8 | Future entries used, 0–8 |
| 4 | issued_utc | u32 | ESP time when assembled; orders updates |
| 8 | horizon_end_utc | u32 | Exclusive end of coverage: the first transition **not** included. `0xFFFFFFFF` = no further transitions known |
| 12 | subsecond_offset_ns | i32 | Applies to every entry; 0 except for special timebases |
| 16 | current | entry | The entry in force at issue |
| 28 | next[0–7] | entry × 8 | Ascending UTC; unused entries are zero |

Each entry is 12 bytes, and entry *k* of `next` starts at offset 28 + 12k.

| Entry offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | utc_seconds | u32 | In force from this second; 0 = since before the data |
| 4 | base_offset_s | i32 | Standard offset from UTC, s |
| 8 | dst_offset_s | i16 | The zone's DST amount, s. Sent even when inactive, and may be negative |
| 10 | flags | u8 | Bit 0 `DST_ACTIVE` |
| 11 | reserved | u8 | 0 |

**Local time = UTC + base_offset_s + (DST_ACTIVE ? dst_offset_s : 0) + subsecond_offset_ns.**

Keeping the DST amount separate from the base offset, and sending it even when inactive, is what lets Dublin's negative winter DST be described without contortions.

## What the PIC takes from it

Two independent settings decide which parts of the schedule the PIC uses:

| Setting | Set | Clear |
| --- | --- | --- |
| `SML_SETTING_TZ_AUTO` | Base offset (and sub-second offset) from the schedule | The manual `base_offset_s` |
| `SML_SETTING_DST_AUTO` | DST amount and DST state from the schedule | The manual `dst_offset_s` and `SML_SETTING_DST_ACTIVE` |

| TZ_AUTO | DST_AUTO | Typical use |
| --- | --- | --- |
| On | On | Normal: everything from the ESP |
| Off | On | A fixed base offset chosen by the user, with DST still following the ESP's zone |
| On | Off | The zone's base offset, with DST overridden by hand |
| Off | Off | Fully manual: the ESP is ignored |

While no schedule is held (`SML_UI_TZ_HELD` clear), both settings fall back to the manual values. `STALE` and `AMBIGUOUS` are reported whenever any part of the offset comes from the schedule.

Where the schedule comes from is the ESP's business, not the PIC's. It may be built from tzinfo lookups (GNSS, GeoIP or a pinned zone), or, with tzinfo disabled, from a zone or rule configured on the ESP itself (`SML_TZ_SOURCE_MANUAL` or `PINNED`). The ESP therefore always sends its best schedule and never needs to know the PIC's settings.

## Applying the schedule

The PIC applies TZ-STATE in `dspic_src/ui/timezone.c`:

| Rule | Behaviour |
| --- | --- |
| `VALID` clear | Ignored. The schedule held is kept |
| Always held | A valid, well-formed, newer schedule is stored **whatever the PIC's settings**. The settings only choose which parts are used |
| Well-formed | Count at most 8, future entries strictly ascending after `current`, every base offset within ±86399s, \|subsecond_offset_ns\| below 10⁹, and the horizon later than the last entry. Otherwise the message is rejected and counted |
| Ordering | Applied if `issued_utc` is at least the applied one's, **or** the source differs, or nothing is held yet |
| Atomicity | All entries are replaced together. The schedule is only read in the foreground, so a half-applied schedule is never seen |
| Each second | The entry in force is the last one whose `utc_seconds` is at or before UTC. Transitions take effect exactly at their UTC second, and the frame for that second is already rendered in the new offset |
| Horizon | From `horizon_end_utc`, the last entry is held and `SML_UI_TZ_STALE` is set. The PIC never falls back to UTC on its own |
| Sub-second | Rounded to the nearest second, as the display has no sub-second phase shift yet. Mecca time, +02:39:18.3, is sent as base 9558s and 300000000ns, and shown as +02:39:18 |
| Persistence | Keyed on a CRC of the schedule **without** `issued_utc`, so periodic resends never count as changes (`timezone_take_changed()`). It will be written by the settings store once that is ported, and never by settings revisions |
| No acknowledgement | STAT-UI reports the offsets actually in use |

The ESP sends TZ-STATE when the schedule changes, whenever STAT-UI shows `TZ_HELD` clear (after a PIC reset, for example), and every ~10 minutes.

## Example: London

As of October 2026, with `count` 8, `horizon_end_utc` 1919293200 and `subsecond_offset_ns` 0:

| | utc_seconds | base | dst | DST_ACTIVE |
| --- | --- | --- | --- | --- |
| current | 1774746000 | 0 | 3600 | 1 |
| next[0] | 1792890000 | 0 | 3600 | 0 |
| next[1] | 1806195600 | 0 | 3600 | 1 |
| … | … | … | … | … |
| next[7] | 1901149200 | 0 | 3600 | 1 |

The complete frame is under [Golden frames](#golden-frames).

# STAT class (0x30)

## STAT-UI (0x01)

_What the clock shows. PIC→ESP, 32 bytes._

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | flags | u16 | See below |
| 2 | current_display | u8 | `SML_DISPLAY_*` shown now, see [Display, button and menu](#display-button-and-menu) |
| 3 | selected_display | u8 | `SML_DISPLAY_*` selected by the user |
| 4 | menu_state | u8 | `SML_MENU_NONE` outside the menu, else section, item and editing (2.3), see [Display, button and menu](#display-button-and-menu) |
| 5 | button_state | u8 | `SML_BUTTON_*` |
| 6 | brightness | u16 | Tube PWM level now (V2: 200–3800 of 4000) |
| 8 | brightness_target | u16 | Level being faded to |
| 10 | reserved | u16 | 0 |
| 12 | local_seconds | u32 | UTC plus the applied offsets: a civil-time scalar, **not** a UTC timestamp (`LOCAL_VALID`) |
| 16 | base_offset_s | i32 | Base offset in use, s |
| 20 | dst_offset_s | i32 | DST amount of the entry in use, s, even when inactive (as in TZ-STATE) |
| 24 | alarm_offset_s | u32 | Alarm, seconds after local midnight, 0–86399 |
| 28 | delta_epoch_utc | u32 | The configured delta-mode epoch: UTC, or a local wall-clock time with `DELTA_LOCAL` |

| Bit | Flag | Bit | Flag |
| --- | --- | --- | --- |
| 0 | `LOCAL_VALID` | 7 | `UPDATE_PENDING` |
| 1 | `ALARM_ENABLED` | 8 | `FORMAT_12H` |
| 2 | `ALARM_ACTIVE` | 9 | `DST_ACTIVE` |
| 3 | `BRIGHTNESS_AUTO` | 10 | `TZ_STALE` |
| 4 | `POWER_ON` | 11 | `TZ_AMBIGUOUS` |
| 5 | `SWITCH` | 12 | `TZ_HELD`: a TZ-STATE schedule is held |
| 6 | `BUTTON` | 13 | `DELTA_LOCAL`: `delta_epoch_utc` is local wall-clock time (2.2) |

On the V2+ PIC `current_display` is the mode shown (another for 20 seconds after a press of the button, `DASHES` while the time is unknown) and `selected_display` the setting. `ALARM_ENABLED` is the alarm setting, `ALARM_ACTIVE` set while it sounds, and `alarm_offset_s` its time; the switch (`SWITCH`) arms it. While the menu is open, `current_display` is `MENU` and `menu_state` says where in it the PIC is. `SWITCH` and `BUTTON` are the debounced levels. `button_state` is the latest press (`SML_BUTTON_SHORT` or `LONG`) if there has been one since the previous STAT-UI, otherwise `NONE`, so each press is reported once. `HOLD` is reserved.

## STAT-ENV (0x10)

_Sensors. PIC→ESP, 14 bytes._

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | flags | u16 | Bit 0 `LUX_PRESENT`, 1 `LUX_VALID`, 2 `BME_PRESENT`, 3 `TEMP_VALID`, 4 `PRESSURE_VALID`, 5 `HUMIDITY_VALID`, 6 `BOARD_TEMP_VALID` |
| 2 | reserved | u16 | 0 |
| 4 | lux | u16 | Whole lux, 0–65535 |
| 6 | ambient_temperature_centic | i16 | 0.01°C |
| 8 | board_temperature_centic | i16 | 0.01°C |
| 10 | pressure_deca_pa | u16 | 10Pa per count, 0–655350Pa |
| 12 | humidity_centipercent | u16 | 0.01% RH, 0–10000 |

A value out of range clears its validity flag rather than wrapping. There are no sentinel values and no sample ages.

The V2+ PIC sends STAT-ENV in every epoch numbered a multiple of 5, and on request. Only the light is filled in so far, from the VEML6040: `LUX_PRESENT` while it answers, `LUX_VALID` while its reading is neither saturated nor too dark to measure, the lux rounded to whole lux.

## STAT-SUPPLY (0x20)

_Power rails, V3 only. PIC→ESP, 14 bytes._

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | flags | u16 | Bit 0 `5V0_VALID`, 1 `3V3_VALID`, 2 `VFIL_VALID`, 3 `VPP_VALID`, 4 `VFIL_ENABLED`, 5 `VPP_ENABLED`, 6 `DRIVER_ENABLED`, 7 `DRIVER_FREQ_VALID` |
| 2 | reserved | u16 | 0 |
| 4 | rail_5v0_mv | u16 | mV |
| 6 | rail_3v3_mv | u16 | mV |
| 8 | filament_mv | u16 | mV |
| 10 | grid_vpp_mv | u16 | mV |
| 12 | vfil_driver_frequency_hz | u16 | Actual switching frequency, Hz, below 50000 |

## STAT-SETTINGS (0x30)

_Stored settings. PIC→ESP, 36 bytes._

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | flags | u16 | Bit 0 `VALID`, 1 `DIRTY` (not yet persisted), 2 `DEFAULTS` (factory values in use) |
| 2 | reserved | u16 | 0 |
| 4 | revision | u32 | Incremented by every applied change, from either end |
| 8 | settings | body | 28 bytes, below |

The settings body is shared with CMD-SETTINGS, and its offsets are relative to the start of the body.

| Body offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | schema_version | u16 | `SML_SETTINGS_SCHEMA` (1) |
| 2 | flags | u16 | Bit 0 `TZ_AUTO` (base from TZ-STATE), 1 `DST_AUTO` (DST from TZ-STATE), 2 `DST_ACTIVE` (manual DST state), 3 `ALARM_ENABLED`, 4 `BEEP_ENABLED`, 5 `FORMAT_12H`, 6 `BRI_AUTO` (brightness from the light sensor), 7 `DELTA_LOCAL` (2.2: `delta_epoch_utc` is a local wall-clock time, below) |
| 4 | base_offset_s | i32 | **Manual** base offset, s |
| 8 | dst_offset_s | i32 | **Manual** DST amount, s |
| 12 | calibration_fosc_hz | u32 | Stored oscillator baseline (FCY), Hz; not live telemetry |
| 16 | alarm_offset_s | u32 | 0–86399 |
| 20 | delta_epoch_utc | u32 | The delta mode's epoch: a UTC instant, or with `DELTA_LOCAL` a local wall-clock time |
| 24 | brightness | u16 | Manual level; range checked by the PIC |
| 26 | selected_display | u8 | `SML_DISPLAY_*`, a selectable value only (`SML_DISPLAY_SELECTABLE()`: HHMM to DELTA, and UNCERTAINTY) |
| 27 | reserved | u8 | 0 |

`delta_epoch_utc` is normally a UTC instant. With `DELTA_LOCAL` it is a local wall-clock time, as a scalar like UTC's (2027-01-01T00:00:00 local is 1798761600), and the PIC counts the delta against its own local time rather than UTC. No time zone data is needed for it, which matters for a past date, or a future one beyond the schedule held: the PIC has no way to know the offset in force then. The cost is that a DST change between now and the epoch moves the delta by the change, as it moves the clock. The ESP sets it for a date and time typed without a zone; one with `Z`, an offset, or Unix seconds is an instant. A later tzinfo could resolve a local time to an exact instant and write it back as UTC.

The automatic timezone schedule is not part of the settings. It is persisted separately, so routine schedule updates from the ESP never change the revision or collide with edits made from the menu.

## STAT-DEBUG (0xF0)

_Console text. PIC→ESP, 1–240 bytes._

The payload is raw text: no NUL terminator, no flags and no struct. Longer output is split across frames; the V2+ PIC sends a frame at each line end, whenever 240 bytes are pending, and otherwise within 10ms. The ESP concatenates the payloads and passes them to its USB-UART console and Telnet, replacing the raw "bridge" text of the old firmware.

## STAT-NET (0x40)

_The ESP's network and services. ESP→PIC, 24 bytes._

STAT-NET carries the network and service status for the PIC's menu and status display. NTP timing stays in TIME-NTP.

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | flags | u16 | See below |
| 2 | wifi_state | u8 | `SML_WIFI_*`, see [WiFi state](#wifi-state) |
| 3 | rssi_dbm | i8 | WiFi signal, dBm (`RSSI_VALID`) |
| 4 | ipv4 | u32 | Address, a.b.c.d = a << 24 \| b << 16 \| c << 8 \| d (`IP_VALID`) |
| 8 | uptime_s | u32 | ESP uptime, s. A drop means the ESP restarted |
| 12 | tzinfo_updated_utc | u32 | Last successful timezone lookup (`TZINFO_VALID`) |
| 16 | tzinfo_refresh_utc | u32 | Next lookup due (`TZINFO_ENABLED`) |
| 20 | esp_version | u32 | ESP firmware: major << 16 \| minor << 8 \| patch |

| Bit | Flag | Meaning |
| --- | --- | --- |
| 0 | `WIFI_CONNECTED` | Associated and up |
| 1 | `IP_VALID` | `ipv4` is set |
| 2 | `RSSI_VALID` | `rssi_dbm` is set |
| 3 | `NTP_ENABLED` | NTP configured |
| 4 | `NTP_SYNCED` | NTP time valid by the ESP's own policy |
| 5 | `TZINFO_ENABLED` | Remote timezone lookup enabled |
| 6 | `TZINFO_VALID` | The last lookup succeeded |
| 7 | `LOCAL_TZ_VALID` | A zone or rule is configured on the ESP itself, for when tzinfo is off |
| 8 | `TELNET_ENABLED` | |
| 9 | `TELNET_CLIENT` | A Telnet client is connected |
| 10 | `UPDATE_AUTO` | Automatic firmware updates enabled |
| 11 | `UPDATE_AVAILABLE` | |
| 12 | `UPDATE_IN_PROGRESS` | The PIC can show it, and expect the ESP to restart |
| 13 | `SD_PRESENT` | V3 |

The ESP's IPv6 addresses are not carried: `ipv4` and `IP_VALID` are the IPv4 address only, which is all the PIC uses so far.

STAT-NET is sent on change and every 10 seconds. The PIC treats the network state as unknown after 30 seconds without one (`ESP_LINK_NET_TIMEOUT_MS`).

## STAT-SOURCES (0x50)

_The PIC's time sources, ranked. PIC→ESP, 108 bytes._

STAT-SOURCES shows the synchroniser's view of every candidate, holdover included: how each was judged, the bound it is ranked by, which is in use and which would come next. It is structured rather than text, so it works with the debug output off.

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | flags | u16 | Bit 0 `SYNCHRONISED` (the PIC counts itself synchronised), bit 1 `FALLBACK` (the source in use was taken as a fallback, and is kept while INACCURATE) |
| 2 | sync_state | u8 | `SML_SYNC_*` |
| 3 | selected | u8 | `SML_SOURCE_*` in use: `HOLDOVER` while holding over, `NONE` in free run |
| 4 | second | u8 | The best other candidate, or `SML_SOURCE_NONE` |
| 5 | challenger | u8 | A candidate on its way to taking over, or `SML_SOURCE_NONE` |
| 6 | challenge_run | u16 | Epochs it has been better by the switching margin |
| 8 | epoch_sequence | u32 | As TIME-PIC |
| 12 | entries[0–3] | entry × 4 | One per candidate, in no particular order; `rank` orders them |

Each entry is 24 bytes, and entry *k* starts at offset 12 + 24k.

| Entry offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | source | u8 | `SML_SOURCE_GNSS`, `NTP`, `RTC` or `HOLDOVER`; `NONE` marks an unused entry |
| 1 | verdict | u8 | `SML_VERDICT_*`, see [Verdicts](#verdicts) |
| 2 | rank | u8 | 1 is best; 0 means not a candidate |
| 3 | flags | u8 | Bit 0 `PRESENT`, 1 `TIME_VALID`, 2 `EDGE` (seen this epoch), 3 `CAPTURED` (by the capture hardware, so it can steer), 4 `LABELLED`, 5 `PHASE_VALID`, 6 `BOUND_VALID` |
| 4 | good_run | u16 | Consecutive GOOD epochs, saturating |
| 6 | usable_run | u16 | Consecutive GOOD or INACCURATE epochs, saturating |
| 8 | bound_ns | u32 | The ± bound the PIC ranks it by, ns (`BOUND_VALID`). The RTC, which only gives the time of day and is never ranked (rank 0), still shows its bound (as in TIME-RTC) |
| 12 | uncertainty_ns | u32 | The source's own figure, ns (GNSS tAcc, the ESP's NTP bound); for holdover, the bound when it was last steered (u0) |
| 16 | phase_ns | i32 | The PIC's epoch minus the source's UTC second, ns (`PHASE_VALID`) |
| 20 | age_s | u32 | Seconds since its last GOOD epoch; for holdover, since the loop last steered. 0xFFFFFFFF: never |

The PIC sends STAT-SOURCES when the selection, the second choice, the sync state or any verdict changes (at most once an epoch), every 10 seconds, and in answer to `REQUEST_MESSAGE` for it, just before the ACK. On the ESP, `sources` asks for it and prints it as a table.

## STAT-SYS (0x60)

_The PIC's firmware, uptime and reset cause. PIC→ESP, 16 bytes._

STAT-SYS is the PIC's counterpart to the uptime and version in STAT-NET: what the ESP needs to notice that the PIC has restarted, and to say why.

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | flags | u16 | Reserved, zero |
| 2 | reset_cause | u16 | `SML_RESET_*` bits as found at start-up, see [Reset causes](#reset-causes). More than one may be set |
| 4 | uptime_s | u32 | Seconds since start-up, counted in epochs; it starts under a second after reset. A drop means the PIC restarted |
| 8 | pic_version | u32 | PIC firmware: major << 16 \| minor << 8 \| patch, as `esp_version` |
| 12 | loop_max_ms | u16 | The longest pass of the PIC's main loop since the previous STAT-SYS (normally the last second), ms: anything that held the rest up |
| 14 | reserved | u16 | Zero |

The PIC sends STAT-SYS every epoch in SML mode, last of the epoch's messages, so the uptime the ESP shows keeps up and a restart is heard about straight away; and on `REQUEST_MESSAGE`. The ESP shows the version, uptime and reset cause in its status, and prints a line when the uptime drops.

# CMD class (0x80)

## CMD-USER (0x10)

_One command. Both ways, 10 bytes._

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | command | u16 | `SML_USER_*` |
| 2 | flags | u16 | Reserved, 0 |
| 4 | token | u16 | Chosen by the sender, echoed in the response |
| 6 | argument | u32 | Per command; signed values in two's complement |

Codes below 0x8000 run on the **PIC** and are sent by the ESP. Codes from 0x8000 (`SML_USER_ESP_FIRST`) run on the **ESP** and are sent by the PIC, from its menu for example. A receiver given a code from the other half answers NACK `UNSUPPORTED`. Where the two halves do the same thing, they share the low byte: `SAVE` is 0x0010 and `ESP_SAVE` 0x8010.

Settings are not commands. Each end changes the PIC's settings by writing them whole with [CMD-SETTINGS](#cmd-settings-0x30), against the revision it last saw, so a change from the menu and one from the console cannot overwrite each other unseen.

Commands run by the PIC:

| Code | Command | Argument | V2+ PIC |
| --- | --- | --- | --- |
| 0x0001 | `SHOW` | `SML_SHOW_*` topic, \| `SML_SHOW_VERBOSE` (0x0100) | ✓ Prints the topic as STAT-DEBUG, then ACK: what each module is doing, in words, or with `SML_SHOW_VERBOSE` its counters and internals too. An unknown topic, or another bit set, is NACK `ARGUMENT` |
| 0x0002 | `REQUEST_MESSAGE` | class << 8 \| ID (upper 16 bits zero) | ✓ For TIME-AUTH, TIME-PIC, TIME-CTRL, TIME-MEAS, TIME-GNSS, STAT-UI, STAT-ENV, STAT-SOURCES, STAT-SETTINGS and STAT-SYS: the message, then ACK. Others: NACK UNSUPPORTED |
| 0x0010 | `SAVE` | — | ✓ ACCEPTED, then ACK once the settings (and the debug mask, and the oscillator) are stored, within a second or so, or NACK `STORAGE` / `VERIFY`. NACK `BUSY` while one is under way |
| 0x0011 | `DEFAULTS` | — | ✓ Applies the factory settings, and the debug mask none, to the running settings, then ACK. Nothing is stored until `SAVE` |
| 0x0020 | `SET_RTC` | UTC seconds | ✓ Writes the PCF8563 to the nearest second (2000 onwards; else NACK ARGUMENT), ACK once started; NACK STATE with no RTC, BUSY while the RTC source is searching or setting it. The RTC is printed once read back and its second found again. The clock's own time is not changed, but while it is synchronised its next check (every 6 hours) sets the RTC back to the clock's time |
| 0x0021 | `SET_DEBUG` | clear << 16 \| set, `SML_DEBUG_*` bits | ✓ See below. Setting a bit outside `SML_DEBUG_ALL` is NACK `ARGUMENT`; clearing any is fine (0xFFFF clears all) |
| 0x0030 | `RESYNC` | — | ✓ (2.4) Every source judged again and the choice made again, at the next epoch: each source's record starts afresh, holdover is dropped, and the clock free runs on its learnt frequency, counting the time of day on, until a source qualifies again (3 epochs). The best by bound is then taken up as at start-up, slewed onto if the loop was tracking and its edge is close, else stepped. TIME-PIC `last_sync_cause` RESYNC. ACK at once |
| 0x0031 | `RESET` | — | ✓ ACK, flush, then software reset |
| 0x0032 | `ENTER_BOOTLOADER` | — | ACK, then a reset into the SPL, which starts `HELD` ([Entering the SPL](#entering-the-spl)). Not yet implemented: NACK UNSUPPORTED until 0.4.0 |

`SHOW` topics:

| Topic | Name | The V2+ PIC prints |
| --- | --- | --- |
| 0 | `SML_SHOW_ALL` | Every module: system, synchroniser, GNSS, display, time zone, settings, RTC, light sensor, brightness, controls, I2C and the ESP link. The ESP sends this at start |
| 1 | `SML_SHOW_STORED` | What is in flash: the settings, the time zone schedule, the oscillator, the store and the flash timings |
| 2 | `SML_SHOW_TZ` | The offset in force, where it comes from, and the schedule held |
| 3 | `SML_SHOW_DISPLAY` | The display mode, brightness, light sensor, button and switch |
| 4 | `SML_SHOW_RTC` | The RTC chip, and the RTC source: its second, rate and checks |
| 5 | `SML_SHOW_OSC` | The oscillator: the stored baseline, the frequency the loop has measured, and what is in flash |
| 6 | `SML_SHOW_GNSS` | The receiver |

Running settings, written by CMD-SETTINGS or from the menu, take effect at once and are kept over a reset only once saved (`SAVE`, or CMD-SETTINGS with `SML_WRITE_PERSIST`). STAT-SETTINGS shows `DIRTY` until then. The debug mask (`SET_DEBUG`) is saved with them, though it is not in the settings body and does not change the revision.

`SET_DEBUG` changes which of the PIC's diagnostics are printed: the new mask is (old & ~clear) | set, using the bits under [Debug subsystems](#debug-subsystems). An argument of 0 changes nothing. Either way the PIC answers ACK and prints the resulting mask, by name, as STAT-DEBUG text, whatever the mask is. Unasked, the PIC prints only its start-up line and faults.

Commands run by the ESP:

| Code | Command | Argument | Purpose |
| --- | --- | --- | --- |
| 0x8010 | `ESP_SAVE` | — | Save the ESP's settings |
| 0x8011 | `ESP_DEFAULTS` | — | The ESP's default settings, running only, until `ESP_SAVE` |
| 0x8031 | `ESP_RESET` | — | Restart the ESP, after the ACK |
| 0x8040 | `ESP_NTP_SYNC` | — | Poll NTP now |
| 0x8050 | `ESP_TZ_REMOTE` | 0/1 | Remote time zone lookups on or off (running, until `ESP_SAVE`) |
| 0x8051 | `ESP_TZ_LOOKUP` | — | Look the time zone up now, and use it. NACK `STATE` if the lookup fails |
| 0x8060 | `ESP_WIFI_FORGET` | — | Forget the WiFi credentials, and restart after the ACK (V2 menu: reset WiFi) |
| 0x8061 | `ESP_WIFI_SETUP` | — | Start the setup access point |
| 0x8070 | `ESP_UPDATE_CHECK` | — | Check for a firmware update |

The V2+ PIC sends these with `esp_link_request()`, which numbers the tokens itself (never 0), and keeps the ESP's latest CMD-RESPONSE for `esp_link_response()`. Nothing is sent in TEXT mode, where the ESP would not understand it.

## CMD-RESPONSE (0x20)

_The answer. Both ways, 12 bytes._

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | status | u16 | `SML_RESULT_ACK` 0, `NACK` 1, `ACCEPTED` 2 |
| 2 | detail | u16 | `SML_ERROR_*`, see [Results and errors](#results-and-errors) |
| 4 | token | u16 | The request's token |
| 6 | command | u16 | The request's command, or `SML_OPERATION_SETTINGS_WRITE` (0x0100) for CMD-SETTINGS |
| 8 | settings_revision | u32 | PIC answering: the current settings revision, rejections included. ESP answering: 0 |

ACK means the command has completed. ACCEPTED means it has been queued, and a final ACK or NACK with the same token follows later; tokens must not be reused while their command is pending. Unknown command codes get NACK `UNSUPPORTED`.

## CMD-SETTINGS (0x30)

_Write every setting. ESP→PIC, 38 bytes._

| Offset | Field | Type | Unit / values |
| --- | --- | --- | --- |
| 0 | flags | u16 | Bit 0 `SML_WRITE_PERSIST`: also persist |
| 2 | reserved | u16 | 0 |
| 4 | token | u16 | |
| 6 | expected_revision | u32 | The revision the ESP last saw |
| 10 | settings | body | As [STAT-SETTINGS](#stat-settings-0x30) |

The PIC validates every field, then applies them all together and increments the revision. A failure on any field rejects the whole write. A write whose `expected_revision` is not the current revision is rejected with `STALE_REVISION`, so the ESP never overwrites a change it has not seen; on ACK the response carries the new revision.

When the flash is written is up to the PIC, and periodic snapshots never drive it. The V2+ PIC checks every field: offsets within ±86399s, the oscillator baseline within 1000ppm of 40MHz, the alarm below 86400s, brightness 0–4000, and a selectable display mode (anything else is `ARGUMENT`). A reserved bit or field set is `ARGUMENT`, another schema `SCHEMA`. The alarm sounds at its local time while the switch arms it. On `SML_WRITE_PERSIST` it writes its flash at the next quiet moment, within a second or so. The ACK does not wait for that write.

The ESP keeps the latest STAT-SETTINGS, and changes one setting by writing that body back with the one field changed. A change made while its previous write is unanswered is folded into the same body and sent once that answer arrives, with the new revision it carries, so a burst of changes never meets `STALE_REVISION`. After a NACK, or no answer, the ESP asks for STAT-SETTINGS again (`REQUEST_MESSAGE`) and drops any change still waiting.

# BOOT class (0xF0)

The bootloader's messages. The PIC boots in two stages: a frozen initial loader in page 0 (the IPL), then a second-stage loader (the **SPL**, pages 1–3) or, when the SPL is unusable, the application directly. The SPL writes the application. The application writes the SPL. Neither writes both, so an interrupted update of either leaves the other to repeat it. The flash layout, the records and the RAM mailbox are in [boot_layout.h](boot_layout.h), and the checksums in [boot_crc.c](boot_crc.c).

## Conventions

- **Who answers.** The SPL answers BOOT messages for the regions `APP` and `TEST`, and ignores every other class. The application answers them for the region `SPL`, alongside everything else. Each refuses the other's region (NACK `ARGUMENT`).
- **One request at a time.** The ESP sends nothing more until the answer arrives or its timeout passes. The PIC cannot receive while flash is erased or written (the CPU stalls), and its receive FIFO holds 4 bytes. The application writes flash only in its quiet window, so it may answer up to about a second later.
- **Fixed lengths.** A BOOT frame of the wrong length is dropped, unanswered.
- **Regions** (`SML_BOOT_REGION_*`):

| Region | Value | Flash | Record | Written by |
| --- | --- | --- | --- | --- |
| `APP` | 0 | 0x002000–0x028FFF | 0x029000 | The SPL |
| `SPL` | 1 | 0x000800–0x001F7F | 0x001F80 | The application |
| `TEST` | 2 | 0x028800–0x028F7F | 0x028F80 | The SPL, while the application does not reach it |

- **Chunk**: 64 instructions, 0x80 program address units, 192 bytes **packed**: each 24-bit instruction as 3 bytes, least significant first (the Intel HEX order without the phantom byte). A chunk address is a multiple of 0x80 within the region. Record rows are never sent: the writer builds the record from `COMMIT`.
- **CRC-32**: IEEE 802.3 (zlib's `crc32`), over packed bytes, for each image and each chunk. The frame's CRC-16 stays as well.

## BOOT-STATUS (0x01)

_PIC→ESP, 56 bytes._ The SPL sends it on entry, every second while idle, and before the ACK to `STATUS`. The application sends it only before the ACK to `STATUS`, with `mode` `APP`.

| Offset | Field | Type | Meaning |
| --- | --- | --- | --- |
| 0 | `spl_version` | u32 | From the SPL record (major << 16 \| minor << 8 \| patch), 0 without one |
| 4 | `protocol` | u8 | `SML_BOOT_PROTOCOL`, 1 |
| 5 | `mode` | u8 | `WINDOW` 0, `HELD` 1, `NO_APP` 2, `LOOP` 3, `APP` 4 |
| 6 | `flags` | u16 | `RECORD` 0x0001, `COMMITTED` 0x0002, `CHECKING` 0x0004, `IMAGE_OK` 0x0008, `ERASED` 0x0010, `REQUESTED` 0x0020, `POWER_ON` 0x0040, `TEST_FREE` 0x0080 |
| 8 | `reset_cause` | u16 | RCON as found (`SML_RESET_*`) |
| 10 | `window_ms` | u16 | Left of the boot window (`WINDOW`), else 0 |
| 12 | `device_id` | u32 | DEVID |
| 16 | `region_start` | u32 | 0x002000 |
| 20 | `region_end` | u32 | 0x029000, exclusive |
| 24 | `app_version` | u32 | From the application record (`RECORD`), else 0 |
| 28 | `app_length` | u32 | Instructions |
| 32 | `app_crc32` | u32 | From the record |
| 36 | `crc32_computed` | u32 | Over `app_length` instructions, unless `CHECKING`. In `APP`, the record's |
| 40 | `build_time` | u32 | From the record |
| 44 | `attempts` | u8 | Hand-overs since the application last reported healthy |
| 45 | `boot` | u8 | How this start came about: `APP_DIRECT` 0x01, `UNCONFIRMED` 0x02, `NO_SPL` 0x04, `SPL_GIVEN_UP` 0x08, `LAST_RESORT` 0x10 |
| 46 | `ipl_version` | u16 | major << 8 \| minor |
| 48 | `spl_state` | u8 | `RECORD` 0x01, `COMMITTED` 0x02, `INTACT` 0x04, `CONFIRMED` 0x08, `REJECTED` 0x10 |
| 49 | `trials` | u8 | Starts of the SPL while unconfirmed |
| 50 | `healthy_s` | u16 | The application: seconds healthy since its start, saturating. The SPL: 0 |
| 52 | `spl_crc32` | u32 | From the SPL record |

## BOOT-COMMAND (0x10) and BOOT-RESPONSE (0x20)

**BOOT-COMMAND**, _ESP→PIC, 32 bytes:_

| Offset | Field | Type | Meaning |
| --- | --- | --- | --- |
| 0 | `token` | u16 | The ESP's, echoed |
| 2 | `command` | u8 | Below |
| 3 | `region` | u8 | `SML_BOOT_REGION_*`, 0 for a command without one |
| 4 | `key` | u32 | `0x45524153` ("ERAS") for `ERASE`, `0x434F4D4D` ("COMM") for `COMMIT` and `CONFIRM`, else 0 |
| 8 | `version` | u32 | `COMMIT`: the image's version. Else 0, as are the rest |
| 12 | `start` | u32 | `COMMIT`: 0x002000 (`APP`), 0x028800 (`TEST`), 0x000800 (`SPL`) |
| 16 | `length` | u32 | `COMMIT`: instructions, even |
| 20 | `crc32` | u32 | `COMMIT`: of the packed image |
| 24 | `build_time` | u32 | `COMMIT`: Unix seconds |
| 28 | `extra` | u16 | `COMMIT`: `APP` and `TEST`, the application's SML version (major << 8 \| minor). `SPL`, its bootloader protocol |
| 30 | reserved | u16 | 0 |

**BOOT-RESPONSE**, _PIC→ESP, 12 bytes:_ `status` u16 (`SML_RESULT_*`), `detail` u16 (`SML_ERROR_*`), `token` u16, `command` u8, `region` u8, `value` u32.

| Code | Command | Answered by | Answer |
| --- | --- | --- | --- |
| 0x01 | `STATUS` | SPL, application | BOOT-STATUS, then ACK |
| 0x02 | `HOLD` | SPL | ACK. No boot window, no hand-over until `RUN` |
| 0x03 | `ERASE` | SPL (`APP`, `TEST`), application (`SPL`) | ACCEPTED, then ACK with `value` the pages erased, or NACK `STORAGE`. The record's page first, then the rest, then a blank check |
| 0x04 | `COMMIT` | SPL (`APP`, `TEST`), application (`SPL`) | ACCEPTED, then ACK. `ARGUMENT` for a start or length outside the region, `STATE` without an `ERASE` in this session, `VERIFY` (with `value` the CRC-32 computed) when the flash does not match `crc32`. Then the record, then its `commit` words alone |
| 0x05 | `RUN` | SPL | ACK, then the application if its image is valid, else NACK `STATE` |
| 0x06 | `RESET` | SPL | ACK, then a software reset |
| 0x07 | `CONFIRM` | Application (`SPL`) | ACK once the SPL record's `confirm` words are written, NACK `STATE` unless started by an unconfirmed SPL |

`ERASE` and `COMMIT` without the right key are NACK `ARGUMENT`. Commands an answerer does not take are NACK `UNSUPPORTED`, and anything arriving while ACCEPTED work is under way NACK `BUSY`. The application also refuses an SPL update for its first 60 healthy seconds, and the SPL refuses to erase a valid application while it is itself unconfirmed (both NACK `STATE`).

## BOOT-DATA (0x50) and BOOT-DATA-RESPONSE (0x60)

**BOOT-DATA**, _ESP→PIC, 204 bytes:_ `address` u32, `region` u8, reserved u8[3], `crc32` u32 (of `data`), `data` u8[192] (one chunk, packed).

The writer checks the region and address (`ARGUMENT`), that this session erased (`STATE`) and the chunk's CRC-32 (`CRC`, send it again). Flash identical to `data` is a retransmission whose ACK was lost, and is ACKed again. Anything else must be erased (`NOT_BLANK`, start again from `ERASE`). It writes each double word that is not erased already (`STORAGE` on a write error), reads the chunk back (`VERIFY`), and answers.

**BOOT-DATA-RESPONSE**, _PIC→ESP, 12 bytes:_ `status` u16, `detail` u16, `address` u32, `crc32` u32 of the chunk as now in flash (on ACK).

Chunks may come in any order, and all-0xFFFFFF chunks may be skipped: the image's CRC-32 still covers them.

## BOOT-VERIFY (0x90) and BOOT-VERIFY-RESPONSE (0xA0)

**BOOT-VERIFY**, _ESP→PIC, 8 bytes:_ `address` u32, reserved u32.

**BOOT-VERIFY-RESPONSE**, _PIC→ESP, 204 bytes:_ `status` u16 (ACK, or NACK `ARGUMENT`), `detail` u16, `address` u32, `crc32` u32 of `data`, `data` u8[192]: the chunk read from flash.

Any chunk below 0x029800, at any time, from the SPL and the application alike. It only reads. The ESP compares `data` with its image, and a mismatch abandons the update and starts again from `ERASE`.

## Entering the SPL

CMD-USER `ENTER_BOOTLOADER` (0x0032), to the running application: ACK, then a reset with a request left in RAM, and the SPL starts `HELD` with `REQUESTED` set. After any other reset the SPL waits 1 s (`WINDOW`) for `HOLD` before it starts a valid application. While the PIC is in the SPL, the ESP sends it nothing but BOOT messages, and goes back to normal on the first STAT-SYS.

# Implementation notes

## Codec

The codec in [sml.c](sml.c) and [sml.h](sml.h) is portable C99 with no hardware dependency, and both firmwares build the same file.

Each message has an entry in a table with its class, ID, fixed length (0 for STAT-DEBUG), direction and **layout**. The layout is the list of field sizes in wire order. TIME-AUTH, for example, is `{2, 1, 1, 4, 4, 4}`. Sizes 2 and 4 are big-endian integers, and any other size is that many bytes, copied as they are: BOOT-DATA's chunk is one field of 192. The structs are packed, so a field's offset in the struct equals its offset on the wire, and one layout describes both.

`sml_encode` and `sml_decode` convert between a packed native struct and the wire one field at a time, through a variable of that field's width, so native byte order and alignment never matter. `sml_frame` builds a complete frame around a payload, and `sml_frame_message` encodes straight from the native struct. `sml_parser_push`, `sml_parser_next` and `sml_parser_timeout` together implement the receiving rules. The parser buffers at most one frame (248 bytes), so it cannot overflow.

## Packing

`#pragma pack(push, 1)` is required on the ESP32, where `uint32_t` is 4-byte aligned. On XC16 every payload has the same layout without packing anyway, because the compiler aligns to at most 2 bytes and every 32-bit field sits at an even offset. The compile-time size checks at the end of `sml_proto.h` confirm the sizes on every compiler.

## dsPIC V2+ modules

| Module | Role |
| --- | --- |
| `common/sml.c`, `common/sml.h` | The codec |
| `esp/esp_link.c`, `.h` | UART1 link: mode, receive and dispatch, commands, debug text, per-epoch messages |
| `esp/esp_report.c`, `.h` | Payload builders and the frozen wire mappings from firmware enums |
| `ui/timezone.c`, `.h` | TZ-STATE application, the manual offsets, stale and ambiguous flags |
| `settings/settings.c`, `.h` | The settings body, its checks and revision, STAT-SETTINGS, CMD-SETTINGS; saving them and the TZ-STATE schedule |
| `settings/store.c`, `.h`, `drivers/flash.c` | The flash journal the settings and the schedule are kept in, in two pages reserved apart from the application |
| `sync/sync.c` | `sync_set_leap()` (TIME-LEAP) and `sync_uncertainty()` (the TIME-AUTH bound) |
| `menu/menu.c`, `.h` | STAT-UI `menu_state`; ESP commands from the menu (`ESP_TZ_REMOTE`, `ESP_SAVE`, `ESP_DEFAULTS`, `ESP_WIFI_FORGET`) and its remote time zone setting from STAT-NET |
| `sync/source_rtc.c`, `.h` | TIME-RTC's values: the RTC's time, when it was last read and set, its anchor and bound |
| `console/console.c` | `console_set_sink()`: routes printf into STAT-DEBUG in SML mode |

`esp_link_service()` runs every 10ms. `esp_link_epoch()` runs right after `sync_update()`, before the display is prepared and before the console reports, so TIME-AUTH leaves first. The eight per-epoch messages (TIME-AUTH, TIME-PIC, TIME-CTRL, TIME-MEAS, TIME-GNSS, TIME-RTC, STAT-UI and STAT-SYS) come to 350 bytes a second, a few percent of the link even at 115200 baud. The console report adds roughly 0.6–1KB a second.

## ESP

The ESP side is described in the [ESP firmware README](../esp_src/README.md). In outline, it announces SML mode with CMD-USER `SHOW` (`SML_SHOW_ALL`) at start, treats STAT-DEBUG and any text outside frames as console output, pairs each TIME-AUTH with the epoch edge captured on GPIO27, sends TIME-NTP after every ESP PPS edge, STAT-NET on change and every 10 seconds, TZ-STATE and TIME-LEAP as described above, and answers the PIC's `SML_USER_ESP_*` commands.

The ESP's NTP clock slews each routine correction in at 5ppm or less over about two poll intervals, and polls more often while its corrections are large (from 128 seconds up to `ntp.interval`). A step or a faster slew sets TIME-NTP `CORRECTING`.

The ESP links to `common/sml.c`, `sml.h`, `sml_proto.h` and `enums.h` from inside its sketch, rather than keeping copies.

## Statistics and Allan deviation

TIME-MEAS gives three phase series, and TIME-CTRL adds the loop's own view of them.

| Series | From | Measures | Resolution |
| --- | --- | --- | --- |
| Output against GNSS, fine | `mark_offset_ns` (labelled epoch − 1) | The disciplined epoch output, timed by the receiver: the clock's actual performance | Receiver timebase, about 1ns, with `mark_accuracy_ns` |
| Output against reference, coarse | `phase_ticks` | The same, by the PIC's own captures | 25ns |
| Oscillator against reference | Σ (`reference_period_ticks` − nominal) | The free-running oscillator, unsteered: the basis of holdover and of the Kalman model | 25ns per edge, better with `qerr_ps` |

Output phase x_k is `mark_offset_ns`, or `phase_ticks` × 25ns. Oscillator phase is the running sum x_k = x_(k−1) + (`reference_period_ticks` − `nominal_period_ticks`) × 25ns. ADEV(τ) then follows from the second differences of x at τ = n seconds.

qErr corrects the receiver's sawtooth on the reference edge. Its sign is still to be confirmed on hardware, so correct one edge by hand before relying on it.

A series should restart at a gap in `epoch_sequence` or `reference_sequence`, at a change of `source`, and, for the output series, whenever `controller_state` is not TRACK or `MEASUREMENT_USED` is clear. Phase steps only happen in STEP, which is outside TRACK.

The Kalman state (`frequency_offset_q16`) is a filtered estimate. It is the right thing for monitoring the model, and the wrong thing for ADEV at short τ, where the filter suppresses exactly the noise being measured.

A TIME-MEAS record plus the TIME-CTRL fields worth keeping comes to about 60 bytes a second, or 5MB a day. That fits on the V3 SD card without a second thought. On V2, decimate, or keep running ADEV accumulators instead.

# Send policy

| Message | Sender | When |
| --- | --- | --- |
| TIME-AUTH, TIME-PIC, TIME-CTRL, TIME-MEAS, TIME-GNSS, STAT-UI | PIC | Every epoch, in this order, within a few ms of the edge (SML mode only) |
| STAT-ENV | PIC | Every epoch numbered a multiple of 5, after STAT-SOURCES |
| TIME-RTC | PIC | Every epoch, after TIME-GNSS (SML mode only), and before the ACK to a `REQUEST_MESSAGE` for it |
| STAT-SUPPLY, TIME-VCTCXO | PIC | Once ported (V3): the VCTCXO every epoch, the supply every few seconds |
| STAT-SETTINGS | PIC | First epoch in SML mode, whenever the revision or the flags change (at most once an epoch, before STAT-SYS), and before the ACK to a `REQUEST_MESSAGE` for it |
| STAT-DEBUG | PIC | As text is produced |
| STAT-SOURCES | PIC | On change (at most once an epoch), every 10 seconds, and before the ACK to a `REQUEST_MESSAGE` for it |
| STAT-SYS | PIC | Every epoch in SML mode, after STAT-SOURCES |
| CMD-RESPONSE | Both | One per CMD-USER or CMD-SETTINGS received, plus a final one for each ACCEPTED |
| CMD-USER (ESP codes) | PIC | On demand, from the menu |
| STAT-NET | ESP | On change, and every 10 seconds |
| TIME-NTP | ESP | After each ESP PPS edge while it has time |
| TIME-LEAP | ESP | On change, and every ~10 minutes |
| TZ-STATE | ESP | On change, whenever STAT-UI shows `TZ_HELD` clear, and every ~10 minutes |
| CMD-USER | ESP | On demand; `SHOW` (`SML_SHOW_ALL`) once at start |

# Constants

## Sources

`SML_SOURCE_*`. These values deliberately differ from the retired `CLOCK_SOURCE` enum.

| Value | Name | Meaning |
| --- | --- | --- |
| 0 | INIT | Not yet determined (start-up) |
| 1 | NONE | No source |
| 2 | RTC | Battery-backed RTC |
| 3 | ESP | ESP holdover or internal time, not a fresh NTP fix |
| 4 | NTP | NTP via the ESP |
| 5 | GNSS | GNSS receiver |
| 6 | HOLDOVER | The PIC's own oscillator, held: a candidate in STAT-SOURCES, and a possible TIME-PIC `second_source` (1.3) |

## Lock state

`SML_LOCK_*`.

| Value | Name | V2+ synchroniser state |
| --- | --- | --- |
| 0 | UNINITIALISED | No epoch processed yet |
| 1 | ACQUIRING | ACQUIRE |
| 2 | LOCKED | LOCKED |
| 3 | HOLDOVER | HOLDOVER, within its 1-hour limit |
| 4 | FREE_RUNNING | FREE_RUN |

## Synchroniser state

`SML_SYNC_*`, used in TIME-PIC. Frozen 2026-10-19.

| Value | Name | Meaning |
| --- | --- | --- |
| 0 | INIT | No epoch processed yet |
| 1 | FREE_RUN | No source: last frequency estimate, time of day counted on |
| 2 | ACQUIRE | Source qualified: learning frequency, stepping onto its edge |
| 3 | LOCKED | Loop tracks the selected source |
| 4 | HOLDOVER | Selected source failed: frequency held, within the limit |

## Cause of the latest change

`SML_CAUSE_*`, used in TIME-PIC. Frozen 2026-10-19.

| Value | Name | Meaning |
| --- | --- | --- |
| 0 | NONE | No change yet |
| 1 | QUALIFIED | A source qualified with none selected |
| 2 | TRACKING | The loop is tracking |
| 3 | PHASE_LOST | The loop lost phase and re-acquires |
| 4 | SOURCE_LOST | The selected source failed |
| 5 | SOURCE_BACK | … and is good again |
| 6 | ALTERNATIVE | Another source took over a failed one |
| 7 | BETTER_SOURCE | A better-ranked source took over |
| 8 | EXPIRED | Holdover limit reached |
| 9 | RESYNC | RESYNC: every source judged again (2.4) |

## Controller state

`SML_CONTROL_*`, used in TIME-CTRL and TIME-VCTCXO. Frozen 2026-10-19.

| Value | Name | Meaning |
| --- | --- | --- |
| 0 | INIT | Not running |
| 1 | ACQUIRE | Waiting for reference periods before the step |
| 2 | STEP | Phase step loaded, being followed |
| 3 | TRACK | PI servo on the Kalman frequency estimate |

## Timezone source

`SML_TZ_SOURCE_*`.

| Value | Name | Meaning |
| --- | --- | --- |
| 0 | NONE | Nothing (`VALID` clear) |
| 1 | CACHE | The ESP's cached state |
| 2 | GEOIP | Server lookup by IP address |
| 3 | GNSS | Lookup by GNSS position |
| 4 | MANUAL | A rule or offsets entered by the user on the ESP (tzinfo off) |
| 5 | PINNED | Zone pinned by name, rules automatic |
| 6 | SPECIAL | On-device special timebase (GMT, DMT, TAI, Mecca, …) |

## Leap source

`SML_LEAP_SOURCE_*`.

| Value | Name | Meaning |
| --- | --- | --- |
| 0 | NONE | None |
| 1 | SERVER | tzinfo leap object or release artifact |
| 2 | GNSS | UBX-NAV-TIMELS, via the PIC |

## RTC type

`SML_RTC_*`.

| Value | Name |
| --- | --- |
| 0 | NONE |
| 1 | DS1307 |
| 2 | PCF8563 |
| 3 | RV3028: Micro Crystal RV-3028-C7 (V3) |

## Display, button and menu

Used in STAT-UI and the settings. Frozen 2026-10-19. The display values match the V2 `UI_DISPLAY_STATE` order, so V2 EEPROM values carry over; UNCERTAINTY (2.1) is new in V2+, numbered after them.

| Value | `SML_DISPLAY_*` | Selectable |
| --- | --- | --- |
| 0 | INIT | No |
| 1 | HHMM | Yes |
| 2 | MMSS | Yes |
| 3 | SSMM | Yes |
| 4 | YYYY | Yes |
| 5 | MMDD | Yes |
| 6 | DELTA | Yes |
| 7 | DASHES | No: shown while the time is unknown |
| 8 | TEMP | No: runtime only |
| 9 | MENU | No: runtime only |
| 10 | UNCERTAINTY | Yes: the time's ± bound, ` 89n` to `999m` (2.1) |

The V2+ PIC shows every selectable mode; TEMP is not ported.

The button states are `SML_BUTTON_NONE` 0, `SHORT` 1, `LONG` 2 and `HOLD` 3. `menu_state` (2.3) is `SML_MENU_NONE` 0 outside the menu. Inside it, the high bit (`SML_MENU_EDITING` 0x80) is set while a value is being edited, bits 4–6 (`SML_MENU_SECTION_MASK` 0x70) give the section, and bits 0–3 (`SML_MENU_ITEM_MASK` 0x0F) the item's place in that section's list on the PIC, counting from 0 and including items hidden at the time:

| Value | `SML_MENU_*` | Items |
| --- | --- | --- |
| 0x10 | TOP | MENU, tZ, dSt, ALrM, bEEP, dISP, tIME, rSt, EXIt |
| 0x20 | TZ | AUtO/MAN, offset, remote, bACK |
| 0x30 | DST | AUtO/MAN, ON/OFF, offset, bACK |
| 0x40 | ALARM | ON/OFF, time, bACK |
| 0x50 | BEEP | ON/OFF, bACK |
| 0x60 | DISPLAY | 12/24, mode, brightness, bACK |
| 0x70 | RESET | WIFI, SEt, ALL, bACK |

So 0x16 is tIME on the top level, and 0xA1 the time zone's offset being edited.

## WiFi state

`SML_WIFI_*`, used in STAT-NET.

| Value | Name | Meaning |
| --- | --- | --- |
| 0 | UNKNOWN | Not reported yet |
| 1 | OFF | WiFi disabled |
| 2 | NOT_CONFIGURED | No credentials |
| 3 | CONNECTING | |
| 4 | CONNECTED | |
| 5 | DISCONNECTED | Configured, but the link is lost or failed |
| 6 | PROVISIONING | Setup access point active |

## Verdicts

`SML_VERDICT_*`, used in STAT-SOURCES: the PIC's judgement of a source's latest report, GOOD or the first reason it is not.

| Value | Name | Meaning |
| --- | --- | --- |
| 0 | NONE | Not judged yet |
| 1 | ABSENT | Not answering |
| 2 | NO_TIME | Does not know UTC (no fix, NTP not synced) |
| 3 | NO_EDGE | No edge seen this epoch |
| 4 | UNLABELLED | An edge, but its UTC second is unknown |
| 5 | INACCURATE | Its own uncertainty is above the PIC's limit |
| 6 | INCONSISTENT | Its label does not follow on from the last one |
| 7 | GOOD | |

## Debug subsystems

`SML_DEBUG_*`, the bits of `SET_DEBUG`. Each gates console output on the PIC; unasked, it prints only its start-up line and faults.

| Bit | Name | Output |
| --- | --- | --- |
| 0x0001 | TIMING | Per epoch: captures, loop, servo, the receiver's view |
| 0x0002 | GNSS | Per epoch: the GNSS edge and its label |
| 0x0004 | SYNC | Per epoch: the synchroniser's state |
| 0x0008 | SOURCES | Per epoch: every candidate, ranked |
| 0x0010 | NTP | Per epoch: the latest TIME-NTP and its pairing |
| 0x0020 | MINUTE | Each minute: scheduler, GNSS and sync reports |
| 0x0040 | TUNING | Each minute: loop and source statistics |
| 0x0080 | RTC | As they happen: the RTC set, its second searched for, a stopped RTC released |
| 0x0100 | EVENTS | As they happen: sync state changes, button and switch, the light sensor found |

## Reset causes

`SML_RESET_*`, the bits of STAT-SYS `reset_cause`. They sit where the dsPIC33's RCON register keeps them, so the PIC sends RCON's cause bits unchanged. A power-on sets POR and BOR; a trap with no handler resets through the RESET instruction, so it shows as SWR, with TRAPR too if it was a trap conflict. The ESP names the most telling one, in this order.

| Bit | Name | Cause |
| --- | --- | --- |
| 0x0001 | POR | Power-on |
| 0x0002 | BOR | Brown-out |
| 0x0010 | WDTO | Watchdog timeout |
| 0x8000 | TRAPR | Trap conflict |
| 0x4000 | IOPUWR | Illegal opcode, or an uninitialised W register used as an address |
| 0x0200 | CM | Configuration mismatch |
| 0x0080 | EXTR | MCLR pin |
| 0x0040 | SWR | RESET instruction: the `RESET` command, or a trap with no handler |

## Results and errors

| Status | Value | | Detail | Value |
| --- | --- | --- | --- | --- |
| `SML_RESULT_ACK` | 0 | | `SML_ERROR_NONE` | 0 |
| `SML_RESULT_NACK` | 1 | | `SML_ERROR_UNSUPPORTED` | 1 |
| `SML_RESULT_ACCEPTED` | 2 | | `SML_ERROR_LENGTH` | 2 |
| | | | `SML_ERROR_ARGUMENT` | 3 |
| | | | `SML_ERROR_BUSY` | 4 |
| | | | `SML_ERROR_STALE_REVISION` | 5 |
| | | | `SML_ERROR_SCHEMA` | 6 |
| | | | `SML_ERROR_STORAGE` | 7 |
| | | | `SML_ERROR_VERIFY` | 8 |
| | | | `SML_ERROR_STATE` | 9 |
| | | | `SML_ERROR_CRC` | 10 |
| | | | `SML_ERROR_NOT_BLANK` | 11 |

# Golden frames

These frames are produced by the reference codec. Any implementation should reproduce them byte for byte, which is a quicker way to find a byte-order mistake than staring at a clock that disagrees with itself.

**TIME-AUTH**: locked to GNSS, 2026-10-18T12:34:56Z (1792326896), edge 150ns late, ±75ns.

```
B5 73 10 01 00 10 00 07 05 02 6A D4 BC F0 FF FF FF 6A 00 00 00 4B 0F F3
```

**CMD-USER**: `SET_RTC` 2026-10-18T12:34:56Z (1792326896), token 0x1234.

```
B5 73 80 10 00 0A 00 20 00 00 12 34 6A D4 BC F0 F9 54
```

**BOOT-COMMAND**: `ERASE` the application region, token 0x1234.

```
B5 73 F0 10 00 20 12 34 03 00 45 52 41 53 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 8D 92
```

**STAT-DEBUG**: the text `"Hi\r\n"`.

```
B5 73 30 F0 00 04 48 69 0D 0A 35 88
```

**TZ-STATE**: London as in the [example](#example-london), issued 1791911413.

```
B5 73 20 01 00 7C 00 01 03 08 6A CE 65 F5 72 66
17 10 00 00 00 00 69 C8 79 90 00 00 00 00 0E 10
01 00 6A DD 54 90 00 00 00 00 0E 10 00 00 6B A8
5B 90 00 00 00 00 0E 10 01 00 6C C6 71 10 00 00
00 00 0E 10 00 00 6D 88 3D 90 00 00 00 00 0E 10
01 00 6E A6 53 10 00 00 00 00 0E 10 00 00 6F 68
1F 90 00 00 00 00 0E 10 01 00 70 86 35 10 00 00
00 00 0E 10 00 00 71 51 3C 10 00 00 00 00 0E 10
01 00 8F 3D
```

# Open points

NTP is a sync source on the PIC, with its loop profile, jitter allowance and holdover model set from bench data. STAT-SOURCES, `SET_DEBUG` and TIME-PIC `second_source` are implemented at both ends. While TIME-NTP has `CORRECTING` set, the PIC holds its frequency estimate (the servo integral) and works the phase off with its proportional term alone; routine gentle corrections are simply followed.

The BOOT class is defined, and nothing implements it yet. The SPL, the application's half (region SPL, `CONFIRM`) and the ESP's procedures come with firmware 0.4.0.

The sub-second timezone offset is rounded to the second. Applying it as a display phase shift, by delaying the latch for that zone, is possible but not planned. Mecca time enthusiasts will have to make do with the nearest second.

STAT-NET has no field for the ESP's IPv6 address. One is to be added when the PIC has a use for it.

The sign of qErr is to be confirmed against captures on hardware before qErr is used in the phase measurement or in the ESP's statistics.

The timezone architecture document should follow the model described under [TZ-STATE](#tz-state-0x01): the ESP always sends its best schedule without needing the PIC's TZ auto setting, `SML_TZ_SOURCE_MANUAL` now means a rule or offsets entered on the ESP, and the "manual offsets, the ESP does not override" row is replaced by the PIC's TZ_AUTO and DST_AUTO settings.

# Changes

| Date | Change |
| --- | --- |
| 2026-11-02 (2.5) | BOOT class (0xF0) defined: BOOT-STATUS (0x01, new), BOOT-COMMAND, BOOT-RESPONSE, BOOT-DATA, BOOT-DATA-RESPONSE, BOOT-VERIFY, BOOT-VERIFY-RESPONSE, with their regions, commands, keys, modes and flags. `SML_ERROR_CRC` (10) and `SML_ERROR_NOT_BLANK` (11). A layout field of any size but 2 or 4 is a byte string. Not implemented yet. No existing layout changes |
| 2026-10-30 (2.4) | `RESYNC` defined and implemented on the V2+ PIC: every source judged again and chosen again, holdover dropped. `SML_CAUSE_RESYNC` (9). No layout change |
| 2026-10-29 | Document only, no wire change. The V2+ PIC sends TIME-RTC every epoch after TIME-GNSS, with its fields defined for the PCF8563; the RTC's bound grows from its last measurement against the synchronised clock, kept over a reset, so the TIME-AUTH uncertainty while free running on the RTC and the STAT-SOURCES bound for it are no longer a fixed ±999ms |
| 2026-10-28 (2.3) | `SML_MENU_*` values for STAT-UI `menu_state`: the V2+ menu's section, item and `EDITING`. The V2+ PIC reports `current_display` `MENU` while the menu is open. No layout change |
| 2026-10-28 (2.2) | Settings flag bit 7 `DELTA_LOCAL` and STAT-UI flag bit 13 `DELTA_LOCAL`: the delta epoch as a local wall-clock time, counted against local time. No layout change |
| 2026-10-27 (2.1) | `SML_DISPLAY_UNCERTAINTY` (10), selectable: the time's bound on the tubes. `SML_DISPLAY_SELECTABLE()`. The V2+ PIC now shows every selectable mode (TEMP is not ported), and fills STAT-UI `selected_display`, `FORMAT_12H` and `delta_epoch_utc`. A display mode the PIC cannot show is `ARGUMENT`, no longer `UNSUPPORTED`. No layout change |
| 2026-10-27 (2.0) | CMD-USER rationalised, a breaking change at both ends. The `SET_*` setting commands are gone: settings are written whole with CMD-SETTINGS. `INFO`, `SHOW_CONFIG` and `SHOW_SETTINGS` become `SHOW` with an `SML_SHOW_*` topic; `SHOW_SOURCES` becomes `REQUEST_MESSAGE` for STAT-SOURCES. `SHOW` prints briefly, or everything with `SML_SHOW_VERBOSE`. The remaining codes are renumbered, the two halves sharing the low byte where they do the same thing. The ESP's codes are renamed after the console's words (`ESP_NTP_SYNC`, `ESP_TZ_REMOTE`, `ESP_TZ_LOOKUP`, `ESP_WIFI_FORGET`), with `ESP_SAVE` added. `DEFAULTS` and `ESP_DEFAULTS` no longer save, and `ESP_TZ_REMOTE` no longer saves: `SAVE` and `ESP_SAVE` do. `SML_DEBUG_RTC` (0x0080) and `SML_DEBUG_EVENTS` (0x0100): the PIC's RTC and state-change messages, printed until now whatever the mask. Golden CMD-USER frame replaced |
| 2026-10-26 | STAT-UI flag bit 3 is `BRIGHTNESS_AUTO` (was `BRIGHTNESS_MANUAL`), and STAT-SETTINGS / CMD-SETTINGS settings flag bit 6 `BRI_AUTO` (was `BRI_MANUAL`): the same bits, the opposite sense, automatic brightness being the default. `SET_BRIGHTNESS` takes 0–4000 and turns auto off; `SET_BRI_AUTO` turns it on. The V2+ PIC sends STAT-ENV (light only) |
| 2026-10-23 | Document only, no wire change. The V2+ PIC implements STAT-SETTINGS, CMD-SETTINGS, `SAVE`, `DEFAULTS`, `SHOW_SETTINGS`, `SHOW_CONFIG` and `SET_FOSC`, and CMD-RESPONSE `settings_revision`. The TZ-STATE schedule is kept over a reset |
| 2026-10-21 (1.4) | STAT-SYS (0x60, PIC→ESP, 16 bytes): firmware version, uptime, reset cause (`SML_RESET_*`) and the longest main-loop pass. No existing layout changes |
| 2026-10-21 (1.3) | STAT-SOURCES (0x50, PIC→ESP, 108 bytes): the PIC's time sources, ranked. `SML_SOURCE_HOLDOVER` (6). TIME-PIC offset 7 becomes `second_source` (was reserved). TIME-NTP flag bit 6 `CORRECTING`, and `SML_NTP_GENTLE_SLEW_PPM`. CMD-USER `SHOW_SOURCES` (0x0074) and `SET_DEBUG` (0x0090) with `SML_DEBUG_*` bits. `SML_VERDICT_*`. No existing layout changes length |
| 2026-10-20 | Document only, no wire change. Rewritten in the style of the repository README. Definitions and codec now in `common/` (`sml_proto.h`, `sml.c`, `sml.h`). Link speed 921600 baud. Bootloader references reduced to the reserved class. The ESP implementation is complete and described in its own README |
| 2026-10-19 (1.2) | TIME-CTRL redefined as lossless Kalman and servo fixed-point state (28 → 56 bytes). TIME-MEAS (0x13) added: raw per-epoch measurements for statistics and ADEV. STAT-NET (0x40, ESP→PIC) added; TIME-NTP flag bit 0 (`WIFI_CONNECTED`) retired into it. CMD-USER and CMD-RESPONSE both ways: codes 0x8000 and up run on the ESP (`SML_USER_ESP_*`). Timezone: the PIC always holds the latest TZ-STATE; `TZ_AUTO` selects the base offset and `DST_AUTO` the DST state from it, independently; `SML_UI_TZ_HELD`; `SML_TZ_SOURCE_MANUAL` redefined as ESP-side manual configuration. `SML_RTC_*` 3 is the RV-3028-C7 |
| 2026-10-19 (1.1) | Frozen wire mappings: `SML_SYNC_*`, `SML_CAUSE_*`, `SML_CONTROL_*`, `SML_DISPLAY_*`, `SML_BUTTON_*`, `SML_MENU_NONE`. TIME-AUTH uncertainty defined as a ± bound. `SML_*_BYTES` length constants for every message. Link modes and receiver rules specified. `common/enums.h` loses the enums SML now defines. No wire layout change |
| 2026-10-14 | Minor version 1: TZ-CURRENT and TZ-FUTURE merged into the atomic TZ-STATE (ID 0x10 retired). Per-entry DST-active flag. `horizon_end_utc`. `subsecond_offset_ns`. `AMBIGUOUS` flag. TIME-LEAP added. GNSS leap fields in TIME-GNSS. `TZ_STALE` and `TZ_AMBIGUOUS` in STAT-UI. Transition table removed from the settings body (88 → 28 bytes) |
