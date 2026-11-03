# dsPIC firmware (V2+)

_The part of the clock that actually knows what time it is, and would like that noted._

The dsPIC33EP256GP504 keeps UTC, disciplines the clock's one-second epoch to the best available time source, and shows local time on the tubes. Each epoch edge is produced by the timer hardware, not by software, and everything else is arranged around it: the tubes latch on it, the GNSS receiver timestamps it, and the ESP is told which second it was.

The ESP32 supplies network time, timezone schedules, leap second data and user commands over UART, using the SML protocol described in [common/SML_PROTOCOL.md](../common/SML_PROTOCOL.md). It also carries the PIC's console output. The PIC listens, but it makes its own decisions.

This firmware replaces the V2 firmware and the MCC-generated code it was built on. It needs the V2+ ESP firmware in [esp_src](../esp_src) for SML, although it also runs alongside the V2 ESP firmware, which just passes its console output through.

# Building

The project is an MPLAB X project in [nbproject](nbproject), built with XC16 2.10 for the dsPIC33EP256GP504. No MCC is involved: the clock set-up and peripheral set-up are all in the source.

The application runs behind the bootloader: the IPL in page 0 ([dspic_ipl](../dspic_ipl)) and, once it exists, the SPL in pages 1–3. It is linked from 0x002000 with [app.gld](app.gld), generated from Microchip's script for the device by [tools/gen_app_gld.py](tools/gen_app_gld.py): its entry at 0x002000, then a forwarding table of one `GOTO` per interrupt vector, which the IPL's interrupt table points at. [platform/app_vectors.s](platform/app_vectors.s) keeps the linker's own interrupt table out of the hex and the version-record page free. The configuration words are the IPL's, programmed with it, so the application's hex has none.

To program a chip, use the hex `tools/make_image.py install` makes from the IPL's and the application's hex files (and the SPL's, once there is one), with the settings pages 0x029800–0x02A7FF preserved. Programming the application alone leaves a chip with no reset vector and erased configuration words. For debugging, add the IPL's released hex to the project's Loadables.

The SML codec and protocol definitions are shared with the ESP. They are not copied into this folder: the project includes `common/sml_proto.h`, `common/sml.c` and `common/sml.h`, along with `common/enums.h`, directly from [common](../common). The includes (`../../common/sml.h` and friends) rely on the repository layout, so keep the two folders side by side.

The ESP link runs at 921600 baud, set by `CONSOLE_BAUD_RATE` in [console/console.h](console/console.h), which must match the PIC link rate in the ESP's `src/board.h`. At 40MHz the dsPIC actually manages 909091 baud, 1.4% slow, which both ends tolerate. 1000000 baud would be exact on both sides, if it ever matters.

## GNSS receiver

The receiver must already be configured: provisioning has not been ported yet, so for now that means u-center. The firmware expects UBX on the module's UART1 at 115200 baud, with these messages enabled once per navigation solution:

| Message | Used for |
| --- | --- |
| NAV-STATUS, NAV-TIMEUTC | Fix, UTC, and the label for each PPS edge |
| NAV-CLOCK, NAV-POSLLH | Clock bias and drift, position (for timezone lookups) |
| NAV-TIMELS | Leap seconds, current and announced |
| NAV-EOE | End of each navigation epoch |
| TIM-TP | Quantisation error of the next PPS edge |
| TIM-TM2 | The receiver's own timestamp of the PIC's epoch edge |

MON-VER is polled at startup to identify the receiver. Anything else the receiver sends is counted by class and ID in the GNSS report, so a stray message is easy to spot. NAV-DOP can be turned off.

## Tests and formatting

Most of the firmware has host tests, built with gcc against stand-ins for the hardware:

- the fixed-point Kalman filter and servo against double precision;
- the epoch timer, scheduler and OC1 output against a cycle-level model;
- the control loop, closed through a model of the oscillator and receiver;
- source selection, holdover and leap seconds, and the choice between GNSS, NTP and holdover, closed through a model of both PPS inputs;
- the UART drivers, the UBX parser and the GNSS service against a simulated receiver;
- the tube wire format, bit for bit against the V2 encoder;
- the SML codec, including golden frames and a 20000-frame fuzz test;
- the ESP link and payloads, the time zone schedule, the calendar (every day from 1970 to 2106) and the report formatter;
- the I2C master, the PCF8563 driver and the RTC source: its second searched for, its rate, the anchor and bound, and setting it;
- the settings and the flash journal, through simulated power cuts;
- the light sensor and brightness, the button and switch, the menu (and setting the time from it), the alarm, beeps and buzzer;
- the bootloader's shared layout and checksums ([common/boot_layout.h](../common/boot_layout.h), [common/boot_crc.c](../common/boot_crc.c)) against its specification.

```
cd tests/host && make
```

The image tool has Python tests of its own: `python3 -m unittest discover -s tests/tools -t tests/tools`.

[tools/format.sh](tools/format.sh) applies the house style: clang-format with [.clang-format](.clang-format), then [tools/style.py](tools/style.py). Firmware sources use CRLF line endings, the host tests LF.

[tools/make_image.py](tools/make_image.py) turns production `.hex` files into the bootloader's files: the application image and the SPL image the ESP sends, and the first-installation hex for a PICkit. `info` checks a file. The bootloader itself is still to come, so for now only a build linked for it (from 0x002000) makes an application image.

# Layout

| Folder | Contents |
| --- | --- |
| `main.c` | The task list, and the order modules are started and run in |
| `platform/` | Clock set-up, the pin map, millisecond helpers, UTC and calendar arithmetic, the boot mailbox |
| `drivers/` | UART1 (the ESP link), UART2 (GNSS), the tube drivers, I2C1, the PCF8563 RTC, the VEML6040 light sensor, program flash for the settings, and what flash holds of the boot stages |
| `console/` | Console output over the ESP link, the fast text formatter, and the debug mask |
| `timing/` | PPS capture, the epoch timer and output, the control loop and its console report, and the cooperative scheduler that runs the task list |
| `sync/` | Time source evaluation and selection, holdover, and the UTC second of each epoch |
| `gnss/` | UBX parsing, decoding and encoding, receiver detection, epoch assembly, the GNSS state |
| `esp/` | The ESP link and the SML payloads built from the clock's state |
| `ui/` | Display modes and frames, brightness, the button and switch, beeps, time zones |
| `settings/` | The settings, and the store that keeps them and the time zone schedule in flash |
| `menu/` | The menu on the tubes |
| `tests/host/` | Host tests and the stand-ins they run against |

The folders are layered, from the bottom: `common/`, `platform/`, `drivers/`, `console/`, `timing/`, `gnss/`, `ui/`, `sync/`, `settings/`, `menu/`, `esp/`, then `main.c`. Each includes only from itself and the folders below it, so a module can be built and tested with what is beneath it; anything that has to go the other way is passed in, as the synchroniser's time is to the RTC source and the ESP link to the menu, or handed down, as the ESP link hands each TIME-NTP to the NTP source. [tools/layers.py](tools/layers.py) checks this, and that no two files include each other.

| Pin | Function |
| --- | --- |
| RA8 | GNSS PPS: watched always, captured by IC1/IC2 unless NTP is selected |
| RB10 | Epoch output (OC1): latches the tubes, captured by IC3/IC4, timed by the receiver |
| RB2 / RB3 | UART2 RX / TX to the GNSS receiver |
| RC6 / RC7 | UART1 RX / TX to the ESP32 |
| RB15 | ESP PPS: watched always, captured by IC1/IC2 while NTP is selected |
| RC8 / RC9 | SPI2 clock / data to the tube drivers |
| RB11 | Tube BLANK, PWM from OC3 for brightness |
| RB12 | Tube latch, ORed with the epoch output for immediate updates |
| RA10 | Status LED, lit for the 100ms epoch pulse |
| RA7 / RA0 | Push button / toggle switch |
| RB7 | Buzzer |
| RB8 / RB9 | I2C1 on its alternate pins ASCL1/ASDA1 (ALTI2C1 = ON): the PCF8563 RTC and the VEML6040 light sensor |
| RB13 | Not connected: pulsed at startup to check that the capture timebases agree |

The full pin table, with the remappable pin numbers, is at the top of [platform/board.h](platform/board.h). Code elsewhere uses the function names, never the port names.

Almost everything runs from the main loop. The scheduler there reads its time from the epoch timer, so tasks keep their place in the second:

| Task | When |
| --- | --- |
| Epoch work | 1ms after each epoch: measure, steer, label the second, send the ESP its messages, render the next second's frame, print the epoch report |
| GNSS and ESP link service, brightness, button, switch and menu | Every 10ms |
| Display load | 500ms into each second, after the latch has closed |
| Minute reports | 600ms into the second, one report per epoch at 15, 30 and 45 seconds into each minute |
| Settings store | 700ms into the second, when there is something to write and the moment is quiet |

Only the timing edges, the 1ms slot timer, the UARTs, I2C1 and the buzzer's timer use interrupts. The epoch timer interrupts run at priority 4, the PPS watch at 2, and the captures, UARTs, I2C1 and the buzzer at 1, so nothing in the foreground can delay an epoch. The scheduler report shows each task's worst lateness, its longest run and its share of the time.

# Time

## The epoch

Timers 2 and 3 form a 32-bit epoch timer clocked at 40MHz. Its period match triggers OC1, which raises RB10 for 100ms: that rising edge **is** the second, with no interrupt latency anywhere in its path. The length of each second is set by loading the next period, with fractional ticks carried forward so nothing is lost to rounding.

IC1/IC2 capture the reference PPS and IC3/IC4 capture the epoch output on a common, free-running timebase. The difference between the two is the clock's phase error, measured to one tick (25ns). At start-up Timer1 resets all four capture timers on the same cycle, and one strobe edge on RB13, captured by both pairs, proves it. The check is retried from scratch up to 8 times. If the pairs never agree but keep disagreeing by the same tick or two, that offset is taken off every reference capture instead. The reference is the GNSS PPS, or the ESP's PPS while NTP is the selected source: the capture is only ever rerouted when a source is selected.

Both PPS pins are also watched by interrupt-on-change, whichever one is captured. Each rising edge is numbered and timestamped from the same timebase, to within a microsecond or so of interrupt latency. That is enough to show a source's pulses are arriving and where they are, and it is what the edges are labelled by, so GNSS labelling carries on while the capture follows the ESP. Steering still needs the capture.

## Control loop

Each epoch, `timing/epoch_control.c` decides the length of the next second:

| State | What happens |
| --- | --- |
| ACQUIRE | Learn the reference's frequency for a few periods, then step the epoch onto its edge in one go |
| STEP | Follow the step at the estimated frequency |
| TRACK | Steer the phase with a PI servo on top of a Kalman frequency estimate, rejecting outliers |

All the arithmetic is fixed point. A period requested after epoch N only runs from N+1, so the loop acts on the phase it predicts for N+1, at its best estimate of the true second (the Kalman estimate less the servo integral). The PPS edge arrives asynchronously and its capture is rounded down, so the loop aims for an average measured phase of half a tick: an even split between 0 and +1.

How hard the loop follows its reference depends on the source, by profile:

| Profile | Used for | Servo | Gate | Kalman |
| --- | --- | --- | --- | --- |
| precise | GNSS | Kp 1/8, Ki 1/256: time constant ~8s, up to 0.4ppm of slew | 400ns | Learns the oscillator's frequency |
| gentle | NTP | Kp 1/64, Ki 1/16384: time constant ~2 minutes, up to 10ppm of slew | 300µs | Learns nothing: the ESP slews its own clock |

Both are critically damped. The NTP profile averages out the microseconds of jitter on the ESP's PPS, and corrects the ESP's edge by the offset the ESP reports for it in TIME-NTP. A clock that has never had GNSS has no frequency estimate, and the servo alone would take minutes to find the oscillator's error, a few hundred microseconds out of phase meanwhile. So while acquiring NTP it measures the ESP's average period over 16 seconds once, and starts the Kalman filter from that. Two things in TIME-NTP help further. The ESP's own estimate of each edge arrives after the PIC has measured it, and wanders by microseconds, by tens just after a correction, so the edge is corrected by its running average. And while the ESP steps or slews its clock fast (TIME-NTP `CORRECTING`), the phase jump is the ESP's time moving, so the servo works it off with its integral held rather than taking it for a frequency error. Routine corrections, slewed in at 5ppm or less, are simply followed. In the model, a 150µs correction then settles in about 3 minutes with 1.6µs of overshoot, instead of 8 minutes with 21µs.

On the V2 board against a NEO-M9N, the measured phase sits at 0 or +1 tick in 90–98% of epochs. The receiver's own timing of the epoch edge (TIM-TM2) averages around +44ns, the fixed delay through the latch logic, with a standard deviation of 12–15ns. The 40MHz oscillator is around 1.6–1.9ppm slow, which the Kalman filter takes care of.

## Sources and holdover

`sync/` decides which source the loop may steer by, and when:

| State | Meaning |
| --- | --- |
| FREE_RUN | No source: the epoch runs on the last frequency estimate, and the time of day, if known, is counted on |
| ACQUIRE | A source has been chosen: the loop is learning its frequency and stepping onto its edge, or slewing onto it if it is already close |
| LOCKED | The loop is tracking the source |
| HOLDOVER | The source has failed: the loop holds frequency, still counted as synchronised while its uncertainty is within 100ms |

The sources are GNSS and NTP, and holdover on our own oscillator is treated as a third. Each carries a ± bound on its time error, and the choice is made on those bounds alone:

| Candidate | Bound |
| --- | --- |
| GNSS | 3 × tAcc + 25ns |
| NTP | The ESP's own bound + 20µs for the jitter of its PPS |
| Holdover | u0 + f·t + ½·D·t², from the bound when the loop last steered (u0), the frequency error that source leaves (f: 40ns/s after GNSS, 1µs/s after NTP), and the oscillator's drift rate (D: 5ppt/s) |

A source becomes a candidate after 3 consecutive good epochs. A candidate takes over from the one in use when its bound is less than half of it for 10 epochs in a row. Holdover takes over at once when the source in use fails, so when GNSS is lost the clock holds over, and only moves to NTP once NTP is twice as good. With the holdover model measured on the bench, that is after about 30 minutes with NTP at ±20µs, about 9 hours with NTP at ±2ms, and about a day at the ±11ms an ESP reports over Wi-Fi. GNSS is taken up when its reported time accuracy is within 10µs, and kept while it is within 100µs. NTP is taken up while the ESP's bound is within 50ms, and kept while it is within 100ms: the ESP's bound covers the whole Wi-Fi round trip, so ~10ms is normal.

Those limits decide what competes, not whether the clock runs. In free run, with nothing within its limit, a source that meets every other requirement (present, time valid, its pulses arriving, labelled, consistent) for 3 epochs is taken as a fallback, however coarse: a receiver just after its first fix, or an ESP that only knows its time to tens of milliseconds. It is steered by and kept while it meets those requirements, and anything twice as good displaces it. Until the bound in use is within 100ms, the clock does not call itself synchronised. The time of day does not wait for any of this: it is set from the best labelled source the first epoch there is one.

Selecting a source routes the capture to it if need be, then restarts the loop with that source's profile. If the loop was tracking and the new source's edge is already within half the profile's gate of the epoch, the loop slews onto it without a step. A handover from GNSS holdover to NTP usually goes that way; one from NTP back to GNSS steps.

The GNSS holdover model comes from a night of virtual holdover trials on a V2 board (`sync/sync_tuning.c`, see [Console](#console)). Against GNSS, the held frequency drifted by 0.8µs at most after a minute, 23µs after ten, 50µs after an hour and 96µs after two (RMS 0.12, 3.8, 15 and 34µs). The bound is set at about three times the worst case, to leave room for temperature: 2.4µs, 25µs, 176µs and 420µs. Holdover after NTP is dominated by the ESP's own time: its frequency wanders by about 2ppm between its 30-minute polls, so 1µs/s.

The RTC is a source of the time of day only. On V2 it has no pulse the capture can take, so it never steers and is never a candidate, but it is judged like the others, with a bound that grows from when it was last measured against the synchronised clock (±999ms if never; see [I2C and the RTC](#i2c-and-the-rtc)). At start-up it is the first source with a time. Its second is searched for as soon as the chip has been read, and while that search is going on (up to 6 epochs), free run waits rather than take up GNSS or NTP. The time of day is taken from the RTC, and the first epoch it is good the epoch is moved onto its second as corrected for its drift, so the seconds shown change when UTC's do, as the RTC predicts them. GNSS or NTP then take over as usual.

`pic resync` (SML RESYNC) makes the choice again from scratch. At the next epoch every source's record starts afresh, holdover is dropped, and the clock free runs on its learnt frequency, the time of day counted on, until the sources have qualified again (3 epochs). The best by its bound is then taken up as at start-up, with no holdover to beat. If the loop was tracking and the new source's edge is already close, it is slewed onto rather than stepped. So a clock holding over after losing GNSS, with NTP not yet twice as good, moves to NTP at once, and a clock locked on GNSS takes it up again within a few seconds without moving.

Adding a source takes an entry in the source table, an adapter that reports its edge, its UTC label and its uncertainty, and a loop profile.

## Time of day

Times are 32-bit Unix seconds throughout, good until 2106, with no 64-bit arithmetic. Each epoch is labelled with the UTC second it marks, from the selected source, and counted on when there is none.

Leap seconds come from the receiver's NAV-TIMELS. Failing that, they come from SML-TIME-LEAP from the ESP. An inserted second is shown as 23:59:60, whether or not a source is available at the time.

# Display and time zones

The tube driver takes a logical frame (segments, separators and dots) and turns it into the V2 wire format, so the wiring lives in one place. Each frame is rendered a second ahead, shifted into the drivers at 500ms, and latched by the next epoch pulse. The tubes therefore change exactly on the second, and the display can be no later than the clock. Brightness is a 10kHz PWM on the BLANK input.

Brightness is automatic by default, as on V2. The VEML6040 is read every half second over I2C1 ([drivers/veml6040.c](drivers/veml6040.c), V2's driver ported as the PCF8563 was), its green channel giving lux, and [ui/brightness.c](ui/brightness.c) turns that into a level: 3 per 0.1 lux, between 200 and 3800 of 4000, so the brightest from about 127 lux. Too dark to measure gives the dimmest, a saturated sensor the brightest, and with no reading at all the manual level stands in. `brightness n` (0–4000) on the ESP sets the manual level and turns auto off; below 200 the tubes are off, above 3800 lit continuously. `brightness auto` turns it back on. In auto the tubes fade to a new level by 10 every 10ms rather than jumping; a manual level takes effect at once. The light goes to the ESP in STAT-ENV every five seconds, and `display`, `pic` and the minute report show it with the brightness.

The display modes are V2's, less the temperature, plus one new one. The mode selected (`display <mode>` on the ESP, kept with `save`) is the one shown; a short press of the button shows the next instead, in the order of the table, and the selected mode comes back after 20 seconds without another press, as on V2. A change of mode is drawn at once, rather than at the next second. Dashes are shown in every mode until the time of day is known.

| Mode | Shows |
| --- | --- |
| HHMM | Local hours and minutes, 24-hour (`09:05`) or 12-hour without a leading zero (` 9:05`, `display hours 12`) |
| MMSS | Minutes and seconds |
| SSMM | Seconds and hundredths: the epoch's frame is `SS:00`, then the hundredths are latched at once as each changes, from the main loop |
| YYYY | The year |
| MMDD | Month and day |
| DELTA | The time to or since the delta epoch (`display epoch <moment>`), in its two largest units: years (alone from 100), years and months, months and days, days and hours, hours and minutes, or minutes and seconds. The start line is lit while the epoch is still to come. Months are whole calendar months from the earlier moment, a day that does not exist (31 February) taken as the month's last. An epoch typed without a zone (`display epoch 2027-01-01 00:00`) is local wall-clock time and is counted against local time, so a New Year countdown reaches zero as the clock reads midnight; a DST change in between moves the delta by an hour, as it moves the clock. One with `Z`, an offset, or Unix seconds is an exact instant, counted in UTC |
| UNCERTAINTY | The time's ± bound, rounded up to three figures with the unit on the last tube: ` 89n`, ` 1.5u`, `130u`, `12.6m`, `999m`, the middle separator's lower dot as the decimal point. ` --m` without a bound (free running on a counted time) or above 999ms |

The colon blinks with the seconds while the clock is synchronised. Running free, HH:MM keeps it lit and the other modes leave it off, as V2 did. The uncertainty mode uses only the lower dot, as its decimal point. The start dot is lit in every mode while the alarm is armed or sounding, as on V2.

Local time is UTC plus offsets. The clock itself never moves. The PIC keeps the latest SML-TZ-STATE schedule from the ESP: the entry in force and up to eight future transitions. It moves to each new entry at its UTC second. Two settings decide what is taken from the schedule:

| Setting | On | Off |
| --- | --- | --- |
| TZ auto | Base offset from the schedule | Manual base offset |
| DST auto | DST offset and state from the schedule | Manual DST offset and state |

Without a schedule, both fall back to the manual values. Past the end of a schedule the last entry is held and flagged as stale, rather than quietly reverting to UTC.

# ESP link

The PIC starts in text mode, sending its console output raw, which is what the V2 ESP firmware expects. On the first valid SML frame from the ESP it switches to SML mode for good. From then on the console text travels in STAT-DEBUG frames, and each epoch the PIC sends TIME-AUTH, TIME-PIC, TIME-CTRL, TIME-MEAS, TIME-GNSS, TIME-RTC, STAT-UI and STAT-SYS, about 350 bytes a second.

STAT-SETTINGS, the settings with their revision and whether they are saved, goes in the first epoch in SML mode and whenever either changes. STAT-SOURCES, the synchroniser's candidates ranked, follows whenever the choice or a verdict changes, every 10 seconds, and on request; the ESP shows it with `sources`. STAT-SYS carries the firmware version (`platform/version.h`), the uptime, why the PIC last reset (RCON, captured and cleared first thing in `system_init()`) and the longest main-loop pass. It goes every epoch, last, so the uptime the ESP shows keeps up and it hears about a restart at once. The same version and reset cause are printed at start-up and by `pic`. TIME-AUTH goes first, within a few milliseconds of the edge it labels. TIME-MEAS carries the raw measurements of each epoch, which the ESP needs for statistics and Allan deviation.

From the ESP, the PIC acts on:

- TZ-STATE;
- TIME-LEAP;
- user commands: SHOW (a topic, printed: everything, what is stored, the time zone, the display, the RTC, the oscillator or the GNSS receiver), message requests (STAT-SOURCES for the sources table, among others), SAVE, DEFAULTS, SET_RTC, SET_DEBUG, RESYNC and RESET;
- CMD-SETTINGS, every setting at once: the only way the ESP changes a setting;
- TIME-NTP, which labels the ESP's PPS for the NTP source;
- STAT-NET, which it holds for the menu.

Commands it does not support yet are answered with a NACK, never ignored. The PIC can also send commands of its own to the ESP, such as resetting WiFi.

# Button and switch

[ui/controls.c](ui/controls.c) samples the push button (RA7) and the switch (RA0) every 10ms. A change counts once a pin has read the same for 30ms, timed from its first edge, so contact bounce is never seen. As on V2, a press released within 500ms is short, and one held for 500ms is long, reported at that moment while it is still held; its release adds nothing. A hold (held on past a long press) is reserved in the enum and in SML for later. The switch is a debounced level. The first sample sets the starting levels: a switch already on is not a change, and a button held through a reset is not a press. Each press is counted rather than consumed, so the UI and STAT-UI each see every press. The switch arms the alarm. A press while the alarm sounds stops it and does nothing else; otherwise a short press clicks and shows the next display mode, and a long press opens the menu (below). STAT-UI carries each press to the ESP, and each press and switch change is printed with `debug +events`.

# Menu

[menu/menu.c](menu/menu.c) is V2's menu on the tubes, reworked: a long press on the clock opens it, and everything in it can also be set from the ESP.

| Item | Contents |
| --- | --- |
| `MENU` | The title: a long press closes the menu |
| `tZ` | `AUtO`/`MAN`; the manual offset, `hh:mm` from −12:00 to +14:45 in 15-minute steps, the start line lit for minus (hidden while automatic); the ESP's remote time zone `r ON`/`rOFF` (hidden until the ESP has reported it) |
| `dSt` | `AUtO`/`MAN`; manual `ON`/`OFF` and the DST offset, −1 to +2 hours in 15-minute steps (both hidden while automatic) |
| `ALrM` | `ON`/`OFF`; the time, `hh:mm` (hidden while off) |
| `bEEP` | `ON`/`OFF` |
| `dISP` | `24`/`12` hours; the display mode, its name shown for two seconds and then the mode itself; brightness, `AUtO` or `b 10` to `b100` in tens of percent (400 to 4000 on the PWM), shown on the tubes as it is chosen |
| `tIME` | The time, while no source but the RTC has given it since start-up: `YYYY`, `MM-DD`, `hh:mm`, `OK` |
| `rSt` | `WIFI` (the ESP forgets its network), `SEt` (the PIC's factory settings) and `ALL` (both, and the ESP's settings), each `NO` until changed to `YES` |
| `EXIt` | Closes the menu |

Each section ends with `bACK`. Moving through the menu, a short press goes to the next item and a long press, acted on at 500ms while still held, opens a section, starts editing a value, or goes back or out. That press is then done with: however long it is held on, nothing more happens until it is let go. The value being edited flashes, lit 600ms of each second, and each press is judged as it is released:

| Press | Editing |
| --- | --- |
| Under 500ms | Next value (wrapping) |
| 500ms to 1.2s | Confirms: the next field, or the value stored. At 500ms the value blinks off for 150ms and then shows steadily, the cue that letting go will confirm |
| Over 1.2s | The value steps on five times a second while held. Letting go does not confirm, and a step less than 120ms before letting go is taken back, so a late release does not overshoot |

A value confirmed takes effect at once; one dropped (20 seconds without a press closes the menu, editing or not) leaves the setting as it was, and a brightness or display mode being previewed goes back. The settings are saved once, when the menu closes, if any changed, as are the ESP's if its remote time zone was changed. While the menu is open the display shows only the menu, and STAT-UI reports `MENU` as the current display with where in the menu it is (`menu_state`).

The clock tells the time; it is not told it. An offline clock has to be told once, though, so `tIME` appears only while the RTC is the only source to have given the time since start-up, and goes as soon as GNSS or NTP does, even from under the menu. It starts from the time shown, or 2026-01-01 00:00 if there is none. `OK` is confirmed by a long press while held: the time becomes `hh:mm:00` local at the moment the button went down. The epoch is moved to the second boundary from that moment (later by up to half a second, or earlier by up to half), so the seconds count from the press, and the RTC is written once the new time is in force.

# Buzzer

[drivers/buzzer.c](drivers/buzzer.c) plays sounds described by a pattern and a count, as V2's chimes: count beeps of a length, a gap apart, in groups with a pause between them, optionally repeating until stopped. V2 built a 100-step buffer for each sound; a pattern does the same in a few bytes. Timer5 steps it at 1ms, running only while a sound plays, so a busy main loop (a minute report, a flash erase) does not stretch a beep. One sound plays at a time, by priority: a button click never cuts into a chime, and nothing cuts into the alarm.

| Sound | Pattern | Priority |
| --- | --- | --- |
| Button | One 60ms click | Lowest |
| Chime | 100ms beeps, 100ms apart, in fours with 300ms between | Middle |
| Alarm | As the chime, 500ms between groups, repeating until stopped | Highest |

[ui/beeps.c](ui/beeps.c) decides when, with the beeps setting on (`beeps on`, kept with `save`): a chime every quarter hour of local time, starting within a millisecond of the epoch, one beep, or on the hour the hour on a 12-hour count (midnight and noon 12, in fours); and a click for each button press. Chimes need the time of day and skip a leap second. V2 counted no beeps at midnight; that is fixed. The alarm uses the alarm pattern whatever the setting (below).

# Alarm

[ui/alarm.c](ui/alarm.c) is V2's alarm: a local time of day, on or off (`alarm 07:30`, `alarm 27000` in seconds past midnight, `alarm off`, kept with `save`), armed by the switch. Armed, it sounds the buzzer's alarm pattern, four beeps and a pause, for two minutes, whatever the beeps setting. A press of the button or the switch turned off stops it; a stopped or finished alarm stays armed for the next day while the switch is on. The start dot shows it armed, and `display` (`Alarm: 07:30:00, ARMED`), `pic` and STAT-UI (`ALARM_ENABLED`, `ALARM_ACTIVE` while sounding) report it; `debug +events` prints each change.

It goes off once a local day, at the first second at or past its time. V2 compared the time of day each second, which missed a time skipped by the clocks going forward; here a time skipped (01:30 as 01:00 becomes 02:00) sounds at the change, and a time repeated as they go back sounds once. A step of the clock larger than any DST change (2 hours) is a time being set, not passing, and sounds nothing; neither does anything while the time of day is unknown.

# Console

The console output goes to the ESP, which shows it on its USB-UART console and over Telnet. What is printed is set by a mask of subsystems in [console/debug.h](console/debug.h). `DEBUG_DEFAULT` sets it at build time, nothing by default (`-DDEBUG_DEFAULT=DEBUG_ALL` for a bench session); the ESP changes it with SML `SET_DEBUG` (`debug`, `debug gnss sync`, `debug +ntp`, `debug -all`), and the PIC prints the result. It is kept over a reset once saved (`save`), with the other settings; a build with `DEBUG_DEFAULT` adds its own bits to the saved ones. Unasked, the PIC prints only its start-up line (version and reset cause) and faults.

| Bit | Name | What |
| --- | --- | --- |
| 0x01 | TIMING | Per epoch: the reference capture and frequency estimate, the epoch capture, phase and loop state, the servo terms, the receiver's view of the edge |
| 0x02 | GNSS | Per epoch: the GNSS edge and its label |
| 0x04 | SYNC | Per epoch: the synchroniser's state |
| 0x08 | SOURCES | Per epoch: every candidate, best first, with its bound, verdict and phase |
| 0x10 | NTP | Per epoch: the latest TIME-NTP and the edge it labels |
| 0x20 | MINUTE | Each minute, at 15, 30 and 45 seconds past: the scheduler, GNSS and sync reports. The sync report also covers the candidates, the display, time zone and ESP link |
| 0x40 | TUNING | Each minute, at 55 seconds past: the bench statistics below |
| 0x80 | RTC | As they happen: the RTC set and where its second landed, its second searched for (resolution, offset, rate), a stopped RTC released, and the RTC's first read and each read-back. A read-back after `rtc set` is printed anyway |
| 0x100 | EVENTS | As they happen: sync state changes (also with SYNC), button presses and switch changes, the light sensor's first reading, and at start-up where the settings came from (TIMING adds the capture timebase check) |

The sources line marks the candidate in use with `*` and the second choice with `+`, and says whether each source's phase comes from its captured edge (`cap`) or its watched one (`ioc`):

```
   SOURCES: 1*GNSS 115ns good 412 cap +1 | 2+HOLD 140ns 0s | 3 NTP 2020us good 412 ioc -1203 |
```

The tuning report gives, for the minute just ended:

- the captured reference's period and the loop's phase: mean, standard deviation and extremes;
- each PPS input's period as watched, and how far its watched timestamp lags its capture (for `PPS_MONITOR_LATENCY_TICKS`);
- each source's phase against the epoch. On the GNSS clock, the NTP line is the ESP's PPS as the PIC sees it, which sets the NTP jitter allowance and `SOURCE_NTP_PPS_DELAY_NS`.

It also gives virtual holdover trials, which build up over hours. While the loop is locked, a trial starts every 10 minutes, up to 12 at once. Each takes the period the loop would hold if its source failed at that moment, then adds up how far that period drifts from the reference over the next two hours. Nothing is actually held over. The error at 10s, 1, 5, 10, 30, 60 and 120 minutes gives f and D for the holdover model.

XC16's printf turned out to cost about 1.5ms a call, and the epoch lines took 67ms of every second. They are now built with the small formatter in [console/text.c](console/text.c), which produces identical output in about 2ms. The minute reports still use printf, so they run late in the second, clear of the epoch work and the GNSS traffic that follows each navigation solution.

# I2C and the RTC

I2C1 runs at 400kHz, as on V2, but nothing waits for it. A driver queues a transfer (bytes to write, then optionally a repeated start and bytes to read), and the master interrupt carries it through a byte at a time. The driver checks the result later from the main loop. A transfer that has not finished within 20ms is abandoned. After that, or after a bus collision, the bus is recovered: SCL is clocked by hand until any slave that was part-way through a byte releases SDA, and a stop is sent. The same recovery runs at start-up, because a PIC reset in the middle of a read leaves the RTC holding SDA low.

The PCF8563 driver is V2's, ported onto this. It reads the control registers and the time in one transfer and reports whether the time can be trusted: VL (the oscillator lost power) and STOP must be clear and the date must be real. A write clears STOP and VL and is read back to confirm it. The RTC is read at start-up, then every minute (or tried every 10s while it does not answer), and shown by `rtc`, `pic` and the minute report; with `debug +rtc` its first read and each write's read-back are printed as they happen. The RTC holds UTC, never local time, as on V2; time zones and DST are applied only on the display. Unlike V2, the century bit is honoured (1 for 2100 onwards), and the registers are converted with the clock's own UTC calendar rather than `mktime()`.

The RTC holds the time to the second, and has no pulse on V2, so `sync/source_rtc.c` finds where its second falls by polling. It reads the chip back to back, a few hundred microseconds a read, each stamped on the capture timebase as its I2C start completes, until the seconds count goes up. The increment lies between the last read before it and the first after, which places it to well within a millisecond. From there the second is counted on, and found again at every check: at start-up, after `rtc set`, and whenever the second found is more than 7 hours old.

Each search after the first, with no setting in between, shows how far the RTC's second has moved from where it was counted on to. That gives its rate, and from then on its second is counted on at that rate. The first rate comes from a search 10 minutes after each setting. The two V2 boards measured so far run about 150ppm fast, 13s a day: the PCF8563 has no trim register, so the rate can only be allowed for.

Once the clock is synchronised, the RTC is checked after a minute, then often enough that at its rate it drifts no more than 500ms from the clock (half its bound): every 6 hours for a good crystal, about every 55 minutes at 150ppm, never more often than every 10 minutes. It is also checked a minute after any correction of the clock's time. If its second is more than 10ms from ours, or its time cannot be trusted (VL or STOP set), it is set, aligned to the epoch:

1. The time is loaded with STOP set, which holds the chip's prescaler in reset.
2. STOP is released so that the chip's first increment lands on one of our epochs. On V2's PCF8563s that first increment comes 63/128s (492.2ms) after the release, as measured on the bench. NXP's PCA85063A data sheet gives 65/128s for its prescaler, the figure first used. The release is timed against the capture timebase, with the main loop spinning for the last 2ms.
3. The RTC's second is found again, which confirms the time and measures where the increment landed.

That error corrects the next release, so a part whose start delay differs from the data sheet's (a clone, say) is learnt on the second attempt. An attempt that misses its moment, because the main loop was busy or the bus was in use, is tried again the next second. After three, the time is written plainly, to the nearest second, so the RTC is never left stopped. Nothing is written unless the clock is synchronised, and never next to a leap second, which the RTC cannot hold.

## The RTC's time against UTC, and its bound

Each time the RTC's second is found while the clock is synchronised (every check, the confirmation of every aligned write, the rate search after it), where it started against UTC's is measured: an anchor, good to the clock's own bound plus half the search's resolution, a few hundred microseconds. A plain write (`rtc set`, the menu's `tIME`, or after three failed aligned writes) makes an anchor too, with nothing measured: offset zero, ±999ms. Any write begun voids the anchor until one of those replaces it, and so does finding the RTC stopped.

From the anchor and the learnt rate the RTC's time is predicted: where its second starts against UTC's is the anchor's offset plus the rate times the seconds since. The RTC source reports that as its edge's offset from the true second, whole seconds of it moving the label, so the clock takes the time of day from the RTC as corrected, and at start-up places the epoch on UTC's second as predicted rather than on the RTC's. The rate is learnt on the capture timebase, the PIC's own crystal, so the control loop's estimate of that crystal (the one kept for the next start) is taken off it: the prediction follows the RTC against true seconds. On the bench, leaving that out showed as the RTC's predicted second drifting about 2ppm, the PIC crystal's offset. The bound is the anchor's, plus 10ppm of the seconds since (the crystal at another temperature while the clock is off, and ageing), or 500ppm with no rate learnt. With no anchor at all it is ±999ms, as the RTC holds UTC to the second.

The anchor, the rate and the last setting are kept in the store (below), so all of this holds over a power cycle, which is what it is for. On the bench model, a PCF8563 150ppm fast and off for a day comes back 13s ahead; its time is predicted to within 5ms, against a bound of ±865ms. The record is written at the next quiet moment after the RTC has been written (the stored one would no longer describe it), otherwise after each new measurement, at most hourly. The bound at the next start grows from the stored measurement, so its age when the power goes counts as much as the time off: 10ppm of an hour is 36ms, of a day 864ms. The flash does not mind (below). A power cut in the second or two between the RTC being written and the record leaves the stored anchor describing the previous setting; that is the one window this does not cover.

Within a second, the RTC's bound is the time's while the clock free runs on it (TIME-AUTH, the uncertainty display). Beyond, the RTC is judged INACCURATE and the time it gives has no bound, but it still gives the time and still places the epoch: it is the best estimate there is until GNSS or NTP takes over.

`rtc`, `pic` and the minute report show where the RTC's second was last found, its offset from the clock and its rate; the last setting (aligned, with where it landed, or PLAIN) and the next check; and its bound and predicted offset from UTC's second, with the anchor's time: `Bound: +/-1.203 ms; second -12.35 ms from UTC's, from 2026-10-28T21:00:00Z` (times to four significant figures, as everywhere: `utc_print_ns`). `pic stored` shows the stored record. TIME-RTC carries the RTC's time as it counts it, when the clock last read and set it, whether it has been measured against the synchronised clock (`SYNCED`), and its bound, every second.

# Settings and flash

The settings are those of SML's STAT-SETTINGS: the manual time zone, TZ and DST auto, the brightness, the display mode, 12 or 24 hours and the delta epoch, the beeps, the stored oscillator frequency, and the alarm. The debug mask is kept with them. [settings/settings.c](settings/settings.c) holds them and applies each to its module. The ESP changes them by writing the whole body back with CMD-SETTINGS, one field changed, against the revision it last saw; there are no per-setting commands. A change runs at once and is kept once saved, as on V2: `save` on the ESP (SAVE), or CMD-SETTINGS with PERSIST. SAVE is answered ACCEPTED, then ACK once the flash has been written, or NACK STORAGE or VERIFY. `defaults` (DEFAULTS) puts the factory settings in force without saving them, like any other change. STAT-SETTINGS shows DIRTY until saved. `pic` prints the running settings with everything else, and `pic stored` what is stored, the store and the flash timings.

The oscillator's frequency, as the control loop learnt it from GNSS, is kept too: once the loop has tracked GNSS for 10 minutes, and again whenever it has moved by 50ppb, at most hourly; `save` also writes it, if it differs at all. `osc` shows the loop's estimate and the stored one, as a frequency and in ppm. At start-up the loop is seeded with it, so a clock free running before any source is found keeps time at the learnt frequency rather than nominal (a couple of ppm out on the bench boards), and an NTP-only start steps with it. GNSS refines it from there. This is not `calibration_fosc_hz`, the setting `osc baseline` changes, which nothing uses yet.

The RTC's anchor, rate and last setting are kept too (above).

The time zone schedule from the ESP is not a setting, and is kept by itself: a minute after its contents last change, so the CACHE, then GEOIP or GNSS schedules an ESP sends as it starts make one write. A resend of the same schedule is not a change. At start-up it goes back in through `timezone_apply()`, checked like one from the ESP, so local time is right before the ESP has said anything.

They live in two erase pages reserved at the top of program flash, apart from the application:

| Program addresses | |
| --- | --- |
| 0x000000–0x0297FF | The application (and the bootloader, and the application's version and CRC trailer, to come) |
| 0x029800–0x02A7FF | The settings store: two pages of 1024 instructions, 2KB of data each |
| 0x02A800–0x02AFFF | The last page: the configuration words from 0x02AFEC, and nothing else |

The rest of the last page is reserved too, empty, so no application code lands in it. Anything else there could only be rewritten by erasing the page, configuration words and all, and a reset before they were written back would start the chip on the erased defaults (watchdog on, another oscillator), which no bootloader could recover from. With the page left alone, the configuration words are programmed once with the bootloader and an update never erases them. The application's version and CRC trailer, for the bootloader to check, will go below the store, in 0x029000–0x0297FF, written last on an update.

[drivers/flash.c](drivers/flash.c) reserves the pages with `address()` and `noload`, so the linker places nothing there and the hex has nothing for them, and writes nowhere else. Reprogramming the application therefore leaves the settings alone, as long as the programmer is told to: the project sets MPLAB's "Preserve program memory" range to 0x29800–0x2A7FF, since programming erases the whole chip first. A bootloader or update scheme must leave the range out in the same way.

[settings/store.c](settings/store.c) keeps a journal in the two pages. Each page has a header with a generation number; records follow it, each with its type, length and a CRC, written a double word at a time with the CRC last. The latest intact record of each type counts, so a record cut short by a reset is passed over and the one before it stands. When a page is full, the other is erased, the latest of each record copied to it, and its header written last: until then the old page is still the one in use, whole. In the host tests a settings record of 42 bytes is written about 38 times between erases, and of 20,000 writes, 6,000 cut short by a simulated power cut at a random point, none lost a record already written. At that rate the flash's rated 10,000 erase cycles per page would take some 750,000 saves. The RTC's record is the one written often: hourly on a PCF8563 150ppm fast, which is set again at every check. About 50 fit beside the others, so each page is erased every four days or so, and lasts some 100 years.

Erasing a page stops the CPU for roughly 20–30ms, with interrupts held off throughout; writing a double word takes some 50µs. The timers, captures and epoch output carry on in hardware, but the 1ms slot interrupt is missed, so `epoch_timer_resume()` brings the slots and milliseconds back to where T2/T3 stands afterwards. UART bytes arriving while the FIFOs are full would be lost, so the store writes only at 700–900ms into the second: after the GNSS burst and the ESP's per-second messages, well before the next epoch, with the I2C bus idle and the RTC source not timing a read or a release. At most one record is written each second. The erase and write times are measured on the capture timebase and shown by `pic stored`; the scheduler report counts the slots made up.

# Bootloader

The IPL leaves a mailbox in the top 16 bytes of RAM below 0x8000 (0x7FF0–0x7FFF), which no C start-up touches ([platform/boot.c](platform/boot.c), [common/boot_layout.h](../common/boot_layout.h)). It holds why the PIC reset: the IPL clears RCON's cause bits and keeps them there, so `last reset` and STAT-SYS `reset_cause` come from the mailbox. Started without the IPL, from a debugger say, the mailbox's magic is wrong and RCON is used as before. The mailbox also says how this start came about (the SPL, or the IPL directly with no SPL or a failed one), and counts hand-overs for the boot-loop guard: the application zeroes that count once it has run for 30 s, and the SPL stays after 8 starts without that.

`ENTER_BOOTLOADER` leaves a request in the mailbox and resets once its ACK is out, and the SPL starts held. It is refused (`STATE`) without the IPL's mailbox or an intact SPL to stay in, and (`BUSY`) while a save is pending.

`pic -v` shows the boot stages ([drivers/boot_image.c](drivers/boot_image.c)): the IPL's version, the SPL's record (committed, intact, confirmed or rejected), the application's record, and how this start came about. The SPL's CRC-32 is checked through the IPL's service at 0x000200.

Every interrupt reaches its handler through the forwarding table, a `GOTO` of 4 cycles, so the PPS watch's latency is 37 ticks rather than V2's 33.

The SPL ([dspic_spl](../dspic_spl/README.md)) can be driven from a PC before the ESP's side exists: [tools/pic_boot.py](tools/pic_boot.py) installs an application file (`make_image.py app`) over any serial port wired to UART1, the ESP's UART bridge for instance, as the ESP will.

# Not yet ported

The V2 firmware did rather more than this. Still to come:

- GNSS provisioning;
- the watchdog.

The SML messages for most of these are already defined.
