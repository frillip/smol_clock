# Smol Clock

_A small desk clock with an unreasonable interest in what time it is._

<img width="1600" height="551" alt="image" src="https://github.com/user-attachments/assets/8335cbd0-ec97-4ee4-bd6c-85db791557a4" />

# Why?

Apparently some people don't have access to a Rubidium frequency standard? Who knew? But this doesn't make them any less deserving of a precision timekeeping solution!

# What?

## Overall design

The last PCB and associated components were somewhat sprawling for a layout that favoured debugging. This design is an evolution based on things learned from the last clock, and significantly reduces the footprint by stacking boards on top of each other. The bottom board holds most of the electronics, the top board holds the tubes and the drivers, connected via a 4x2 2.54mm header. The top board is also supported by a series of 20mm M3 standoffs. The clock is designed to function with or without a GNSS module populated on the board, as this represents a significant cost, and also many people do not want to have GNSS antennas strung around their house (what?).

## Tubes

There are 4x socketed IV-12 VFD 7-segment tubes for the digits and 2x IV-1 separator tubes for the separator colon and status information.

## Tube drivers

2x Microchip HV5812WG-G drivers provide 40 HV outputs, controlled via the SPI peripheral. The blank input is PWM driven to give brightness control, and the data is latched onto the output using both a GPIO and OC module output via a discrete OR gate.

## Microcontroller

A dsPIC33EP256GP504 runs the show with 4x input capture modules, 3x output compare modules, an I2C peripheral, an SPI peripheral and 2 UARTs. This runs off a relatively cheap 40MHz oscillator. It could be swapped out for something fancier like an OCXO or TCXO, but for the purposes of this project GNSS can more accurately characterise the existing frequency source. The flash program memory is also used to store the settings, the learnt oscillator frequency, the RTC's drift and the timezone schedule.

## ESP32

An ESP32 connects to the internet to obtain time from up to four NTP servers. It is connected to the PIC via UART for data transfer, and also has access to the PPS signals from both the PIC OC module and GNSS module (if populated). It is also connected to the I2C bus (though I2C is not enabled in software), the GNSS UART via unpopulated solder jumpers, and some other IO. There is also a PPS signal from the ESP module to the PIC for sub-second accuracy. The ESP keeps its own NTP-disciplined clock and reports a bound on it, typically around ±10ms over Wi-Fi (it covers the whole round trip). A programming and communication interface is provided on UART0 by a CP2102N USB to UART bridge.

## GNSS

GNSS duties are handled by the u-blox NEO-M9N module. This is directly integrated onto the board with an SMT SMA connector for an antenna. Both active and passive antennas are supported. There is also a second USB port connected to the GNSS module for power/configuration of the module via u-center.

## RTC

A PCF8563 RTC, battery backed with a CR1220 cell, is connected via I2C to both the PIC and ESP. It remembers the time while the clock is unplugged and provides somewhere sensible to start before GNSS or NTP becomes available.

The PCF8563 only knows the time to the second, has no trim, and the ones measured so far run about 150ppm fast. So the firmware finds where its second actually falls, learns its rate against the synchronised clock, and keeps both over a power cycle. At start-up the RTC's time is corrected for the drift while the clock was off, and comes with an honest ± bound that grows from when it was last measured, rather than an optimistic ±1s.

## Environment sensors

A BME280 temperature/pressure/humidity sensor is on the board, but the firmware does not use it. It may be possible to more accurately estimate the crystal temperature characteristics at startup with this sensor, but for now it is mostly decorative, as there is significant heating of the board from the power supply modules that render it somewhat useless as an environment sensor!

A VEML6040 is used for ambient light sensing. Its output goes through a very basic scaling to give a target brightness for the tubes out of 4000, which the tubes fade to rather than jump. The PIC then drives a third OC module using this value to get flicker-free brightness control over the tubes. The resulting PWM frequency is 10kHz, with the persistent glow of the VFD phosphor helping to smooth the brightness. This is useful for brightness control, and rather less useful when trying to show exactly when a digit changed.

## Inputs

There is a single push button and a toggle switch. The toggle switch is used to arm/disarm the alarm feature. A short press of the button shows the next display mode for 20 seconds, and a long press opens the menu, which is navigated via a series of long/short presses. Currently it is possible to set:

- Timezone offset, automatic or manual, and remote timezone lookups on or off
- Daylight saving time automatic/manual, on/off and offset
- Alarm on/off and time
- Beeps on/off
- 12/24 hour display format
- Display mode (HHMM/MMSS/SSmm/YYYY/MMDD/delta/uncertainty)
- Brightness, automatic or manual
- The time, only while the clock has no source but the RTC (an offline clock has to be told once)
- Reset WiFi settings, clock settings, or everything

A value is only changed when a long press confirms it, holding the button longer steps through values instead. If left to time out, the setting is left as it was. Changed settings are saved when the menu closes. Everything in the menu can also be set from the console. The details are in the [dsPIC README](dspic_src/README.md#menu).

## Buzzer

A self-oscillating 5V buzzer beeps at semi-regular intervals. If enabled, every hour it beeps 1-12 times, corresponding to the hour, in groups of 4. It also beeps once at 15, 30 and 45 minutes past the hour, and clicks for each button press. It also provides the alarm.

## Power

Power is provided through either the USB-C port connected to the ESP or the USB-C connected to the GNSS module. This goes through a TPS2113A power mux chip that provides current limiting, inrush current control, and power selection/prioritization. It also prevents backfeeding from one USB source to the other if both are connected. An LM3671MFX based buck converter provides 3.3V for the GNSS module, PIC, and ESP32. Tube voltages are generated using a TPS55340 boost converter IC to give 34V for the grid voltage, and an LM2831Z based buck converter provides the lower 1.65V filament voltage. Future hardware revisions will simplify and commonise much of the power supplies to help reduce complexity and cost.

# How?

The firmware for both chips is in this repository: [dspic_src](dspic_src) for the PIC and [esp_src](esp_src) for the ESP. They talk over a serial protocol of their own, SML, described in [common/SML_PROTOCOL.md](common/SML_PROTOCOL.md). The PIC keeps the time and runs the display, the ESP fetches things from the network and makes suggestions. The [dsPIC README](dspic_src/README.md) and [ESP README](esp_src/README.md) describe each in detail, what follows is the outline.

## One master epoch

Everything is built around a master epoch owned by the dsPIC, using Timer2/3 as the underlying tick counter. Calendar time, sub-second time and display scheduling are derived from that common timebase. The generated PPS becomes an output of the timekeeping model, rather than the thing around which the rest of the clock has to organise itself.

GNSS PPS is measured against the epoch by comparing captured reference and output edges. The generated PPS is captured too, so the clock can check when its output actually happened. A calibrated phase offset accounts for the intended relationship between the reference edge and the output edge. Knowing what time it is and knowing when a pin changed are related problems, but they still need checking separately.

The tubes hang off the same edge. Each second's frame is rendered a second ahead, shifted into the tube drivers 500ms into the second before, and latched by the epoch pulse itself, so the display changes exactly on the second, with no interrupt latency in the way.

### Silicon errata and free-running captures

It would be convenient to synchronise all the peripheral counters and have them agree about where zero is. Unfortunately, the silicon has some qualifications. The two capture faults observed on the dsPIC33EP256GP504 are described more clearly in [Microchip's GM3XX/6XX/7XX-family errata, DS80000577Q](https://ww1.microchip.com/downloads/aemDocuments/documents/MCU16/ProductDocuments/Errata/dsPIC33EPXXXGM3XX_6XX_7XX-Family-Silicon-Errata-and-Data-Sheet-Clarification-DS80000577.pdf). The capture blocks are shared between these families, and the GM descriptions match the behaviour seen here, the document's formal device/revision tables remain specific to the GM family.

| Issue | Detailed behaviour | Handling |
| --- | --- | --- |
| 5 — Synchronous cascaded Input Capture | With `IC32 = 1`, `ICTRIG = 0` and a non-zero `SYNCSEL`, a synchronisation event resets only the odd, lower-word timer. The even, upper-word timer does not reset, leaving the combined 32-bit count inconsistent. | Establish the capture-counter alignment at startup, then run the IC pairs freely rather than depend on recurring synchronous resets. |
| 31 — Cascaded Input Capture rollover | When the lower word rolls from `0xFFFF` to `0x0000`, the upper word increments one timer cycle late. A capture on that boundary can contain the new lower word with the old upper word. | Retain the rollover correction when constructing a timestamp whose captured lower word is exactly `0x0000`, before calculating periods or phase differences. |

Microchip lists no workaround for either capture issue. The clock handles them in its timing architecture and timestamp processing. Free-running counters avoid the reset fault, but do not remove the rollover fault: at that boundary, the stale upper word makes the assembled timestamp 65536 ticks low. The firmware's correction accounts for the missing carry before that value reaches the frequency estimator or phase controller. Otherwise, a perfectly ordinary PPS edge can look like a rather exciting timing excursion.

The arrangement keeps Timer2/3 in charge of the epoch, with the Timer3 interrupt re-arming OC1 for the next epoch pulse. IC1/2 captures GNSS PPS and IC3/4 captures the OC1 output, using free-running 32-bit capture counters. These are the IC modules' own counters, not direct snapshots of Timer2/3. After accounting for counter alignment and the calibrated phase offset, corrected, wrap-safe timestamp differences provide the signed phase error used to discipline the master epoch. The captures measure timing, they do not independently advance the clock.

During development, starting Timer2/3 could clear the cascaded capture counter, but subsequent 32-bit periods did not provide the recurring reset required. A reset that works once is not particularly useful for a clock. The free-running arrangement avoids relying on that behaviour, while preserving the separate rollover correction.

The same GM document also describes the one-shot OC interrupt hazard in issue 24: with a timer timebase and prescaler other than 1:1, `OCxIF` can precede the pin transition. Its documented workaround is to wait at least twice the prescaler factor in CPU cycles before clearing `OCM`. This is another reason to verify the generated edge by capture instead of assuming the interrupt flag means the pin has already changed.

## Persuading the oscillator gently

The PIC disciplines the epoch with a PI servo on top of a Kalman estimate of the oscillator's frequency, all in fixed point. The aim is to follow the oscillator's drift without treating every noisy measurement as a reason to move the clock. GNSS gets a tight profile that also learns the oscillator's frequency, NTP a gentle one that averages out the jitter on the ESP's PPS.

Acquisition, tracking and holdover are handled explicitly. Getting the clock aligned at startup, maintaining that alignment, and carrying on when the reference disappears need different behaviour. Holdover still depends on the local oscillator, no amount of software will turn it into a Rubidium standard. The learnt frequency is kept in flash, so a clock that starts without a reference keeps time at it rather than at nominal.

On a V2 board against the NEO-M9N, the PIC measures its epoch at 0 or +1 tick (25ns) from the GNSS PPS in 90–98% of seconds, and the receiver's own timing of the epoch edge has a standard deviation of 12–15ns.

## Time sources

GNSS, NTP, holdover on the local oscillator and the RTC are all treated as time sources, each with a ± bound on its time error that grows or shrinks with what is known about it, and the clock uses whichever is best. A source must be twice as good as the one in use to take over, so the clock does not flit between them. When GNSS is lost the clock holds over, and only moves to NTP once NTP's bound is half of holdover's. The bound is what `sources` shows, and what the `uncertainty` display mode puts on the tubes.

At startup the RTC gives the time of day and places the epoch on its second, corrected for its drift, until GNSS or NTP takes over. A clock with nothing but the RTC to go on can be given the time from the menu.

## Less work in interrupts

Input-capture interrupts record the timing measurements, with the heavier processing kept out of the interrupt handlers. Display data is prepared ahead of its intended epoch, then committed at the corresponding boundary. This keeps the timekeeping, measurement and display work tied to the same model without asking an interrupt to do everything at once.

## Timezones and daylight saving

Fixed offsets and a simple automatic-DST setting are manageable until the clock encounters somewhere with more interesting rules. The timezone service keeps the base timezone offset and DST offset distinct and supplies upcoming transitions as UTC epochs, and the ESP passes the schedule on to the dsPIC as it is.

The split is that the ESP handles timezone lookup and fetching the rules, while the dsPIC applies the supplied offsets and transitions against its UTC master epoch, each at its UTC second. Local display time is derived from UTC, changing the timezone should not move the underlying clock. This also avoids teaching the dsPIC the entire world's timezone legislation, which seems an unreasonable thing to ask of a desk clock.

# ESP32 network features
The ESP does more than fetch the time. It also provides a network console, pulls its own firmware updates, and can ask a remote timezone service ([tzinfo_service](tzinfo_service), also in the repository) for the current timezone offset and upcoming DST changes.

The ESP is dual stack: IPv4 by DHCP, IPv6 by SLAAC. NTP, timezone lookups and update checks try the preferred address family first and fall back to the other, so a network where one of them does not route still works. The details are in the [ESP README](esp_src/README.md#network).

## Telnet console

The same console commands can be issued through the ESP USB-UART connection or over Telnet. This saves having to find a USB cable just to see what the clock is doing. Telnet is disabled by default and uses port 23 unless changed with `telnet port`. Connect to the clock's IPv4 or IPv6 address on the configured port. `quit` or `exit` closes the session.

There are two separate controls: whether Telnet should start on boot, and whether the listener is running now. `telnet on` enables startup and starts the listener when changing from disabled to enabled. `telnet off` disables startup but leaves a running listener alone. `telnet start` and `telnet stop` control the listener without changing the startup preference. Stopping it leaves an existing session open, so exit that session as well if required. Save the startup preference and port with `save`.

The current console has no login and uses plain Telnet, so anyone with access to it can issue the commands. `wifi` never prints the saved WiFi passphrase, but the console can still change every setting. This is a convenience console for a trusted local network, with USB-UART available when Telnet is disabled. Some form of authentication and general hardening is on the long list of things to be added eventually.

## OTA firmware updates

The [ESP updater](esp_src/src/net/updater.cpp) pulls firmware over WiFi from an HTTPS update manifest, `/smol_clock/v2plus/manifest.json` by default. The hostname and path of the manifest are configurable. An optional configuration string selects an update profile, for example `development`, with `default` clearing that selection. The manifest names the ESP's image and the PIC's application for each configuration, each with its version, size and SHA-256 (the [ESP README](esp_src/README.md#updates) has the format).

`update check` checks what is available for both chips without installing anything. `update install` installs the next update due: the ESP's if it is newer, leaving the ESP running until `esp reset` completes the change, otherwise the PIC's application. Each part also has its own command: `update esp` (newer only, finished by `esp reset`), `update esp force` (any other version, and an immediate restart: useful when testing, though somewhat more committed than merely checking), `update pic` and `update pic force`, and `update spl`, which is not supported yet.

The PIC's application goes in through its bootloader: the ESP stages the file in its own flash, puts the PIC in its bootloader, writes and reads back the image, and only then commits it. A failed or interrupted update can leave the PIC in its bootloader without an application. After 30 s the ESP installs the staged file again by itself, without the network. The ESP also stages the application the manifest offers, fetched by `update check` when no slot holds it, so `update pic force` can reinstall it offline. `update` lists the staged files and `update erase pic|spl|all` empties them. `pic boot` shows what the bootloader reports, `pic boot run` starts the application, and `pic boot hold` keeps the PIC in its bootloader.

Automatic updates are enabled by default. The ESP checks once a day at the start of a chosen local hour, defaulting to 04:00. It installs an ESP update and reboots, or else installs the PIC's. An ESP that starts within that hour skips that day's check, in case it has just installed the update. `update auto 4` turns it on for that hour, save changes with `save`. They can be disabled with `update auto off`.

The PIC's application is only installed by an ESP at or past the ESP version its configuration names, so the ESP always goes first. Updating the PIC's bootloader itself is not supported yet.

## Remote timezone information

The [tzinfo client](esp_src/src/tz/tzinfo.cpp) asks a separate Python service, the [tzinfo service](tzinfo_service) behind `rubidium.darksky.io`, for the timezone, base UTC offset, DST state and offset, and up to eight upcoming transitions as UTC epochs. The ESP passes this schedule to the dsPIC over UART, which keeps it in flash and changes the local time at each transition by itself, whether or not the ESP is still around. NTP still supplies UTC time, whilst tzinfo supplies the rules for displaying local time. The clock's automatic timezone and DST settings decide whether the schedule's offsets are used, or the manual ones.

The client is enabled by default, with a daily refresh interval. The hostname, path and interval are configurable, so the service can also be self-hosted: [tzinfo_service/INSTALL.md](tzinfo_service/INSTALL.md) says how, and [API.md](tzinfo_service/API.md) describes the protocol. `tz lookup check` fetches and prints the response without using it. `tz lookup` fetches it and, with lookups on, sends the schedule to the PIC.

## Location privacy

A timezone lookup does not need to know which corner of the desk the clock is on. `tz precision` controls how many decimal places of latitude and longitude are sent, rounding the coordinates before the request leaves the ESP. The default is one decimal place. Increasing this may help near a timezone boundary, but also sends a more precise location.

| Setting | Coordinate grid spacing in latitude | Effect |
| --- | --- | --- |
| `none` | No GNSS location supplied | Sends no coordinates at all |
| `0` | About 111km | Whole-degree coordinates |
| `1` | About 11.1km | Default precision |
| `2` | About 1.11km | Two decimal places |
| `3` | About 111m | Three decimal places |
| `4` | About 11.1m | Four decimal places |
| `5` | About 1.11m | Five decimal places |
| `6` | About 0.111m | Maximum configured precision |

These are grid spacings, not claims about GNSS accuracy. Longitude spacing also depends on latitude. Setting `none` suppresses GNSS location sharing with this service and leaves IP-based GeoIP lookup to the server, as does a clock without a GNSS fix. It does not disable local GNSS timekeeping. The server still sees the public IP address of requests, and rounding coordinates does not make them anonymous.

`tz remote off` turns off periodic timezone requests and forwarding of remote timezone information. An explicit `tz lookup` or `tz lookup check` still makes a request even while the feature is disabled. Automatic OTA requests are controlled separately with `update auto off`, and NTP has its own server setting. Save preferences with `save`.

# Console commands

Commands can be issued via the ESP USB-UART console or through Telnet when running. One command per line. Pressing return on an empty line prints a brief status. `help` lists the commands, and `help <command>` what each one takes.

Every command follows the same pattern:

| Form | Does |
| --- | --- |
| `<command>` | Show it, `-v` at the end of any command that shows something shows everything, counters and internals included |
| `<command> <setting>` | Show that setting |
| `<command> <setting> <value>` | Change it |
| `<command> <action> [option]` | Do something |

A command that is itself a setting takes its value directly, as in `brightness auto` or `beeps on`. Changes take effect at once, on whichever chip holds them, and are kept over a reset once saved with `save`. `defaults` puts the default settings in force without saving them, so a restart brings back what was saved, should the defaults turn out to be less charming than expected.

Values are written as people write them: on/off (or 1/0, yes/no), offsets as `+5:30` or `-1`, times of day as `07:30` or seconds past midnight (`27000`), intervals as `1024`, `17m`, `1h` or `1d`, and moments as Unix seconds or ISO 8601, loosely: `2026-10-27T15:00Z`, `2026-10-27 16:00+01:00`, `2026-10-27 16:00:00 +0100`, `20261027T150000Z` or just `2026-10-27`, with T or spaces between the date and the time and any fraction of a second dropped. A moment without a zone is local time.

## Both chips

| Command | Description |
| --- | --- |
| Enter on an empty line | A brief status: time, sources, NTP, time zone and links (everything with `verbose on`) |
| `status [-v]` | The same, `-v` prints everything |
| `help [command]` | The commands, or what one takes |
| `save` | Save the settings of both chips |
| `defaults` | The default settings on both chips, in force but not saved |
| `reset` | Reset the PIC and restart the ESP |
| `verbose on\|off` | Print everything by default, until restart |
| `quit` / `exit` | Disconnect the current Telnet session, Telnet only |

## The clock

| Command | Description |
| --- | --- |
| `tz [-v]` | The time zone: the offset in force and where it comes from, DST, the schedule, leap seconds and lookups |
| `tz offset auto\|±hh:mm` | Base offset from the time zone, or a manual one |
| `tz dst auto\|on\|off` | DST from the time zone, or manually on or off |
| `tz dst-offset ±hh:mm` | The manual DST amount |
| `tz lookup [check]` | Look the time zone up now and use it, `check` only prints the answer |
| `tz remote on\|off` | Periodic lookups from the tzinfo service |
| `tz server <host>`, `tz path <path>` | The tzinfo service |
| `tz interval <time>` | Time between lookups, 15m–7d, default 1d |
| `tz precision none\|0..6` | Decimal places of position sent, see [Location privacy](#location-privacy) |
| `display [-v]` | The display mode, 12/24 hours, brightness, light sensor, button and switch |
| `display hhmm\|mmss\|ssmm\|yyyy\|mmdd\|delta\|uncertainty` | The display mode, the button steps through them for 20 seconds. `uncertainty` shows the time's ± bound (` 89n`, ` 1.5u`, `12.6m`) |
| `display hours 12\|24` | 12- or 24-hour clock |
| `display epoch <moment>` | The reference for the delta mode: a date and time without a zone is local time (`2027-01-01 00:00`), with `Z`, an offset or as Unix seconds, an exact instant |
| `brightness auto\|0..4000` | Brightness from the light sensor, or a manual level: below 200 the tubes are off, above 3800 fully on |
| `beeps on\|off` | Quarter-hour chimes and button clicks |
| `alarm off\|on\|hh:mm[:ss]\|<seconds>` | The alarm, in local time, a time, or seconds past midnight, sets it and turns it on. The switch arms it, and `alarm` shows whether it is ARMED, DISARMED or SOUNDING |
| `rtc` | The RTC: its time, its second and rate, its last setting and next check, and its bound and predicted offset from UTC |
| `rtc set now\|<Unix seconds>` | Set the RTC: the ESP's NTP time, or Unix seconds (from 2000). Not a date, deliberately: a wrong time takes a conversion first |
| `osc` | The oscillator: the baseline setting, the loop's measured frequency, and what is stored |
| `osc baseline <Hz>` | The oscillator baseline setting, within 1000ppm of 40MHz |
| `gnss` | The GNSS receiver |
| `sources [-v]` | The PIC's time sources, ranked |
| `debug [names\|+name -name]` | The PIC's debug output: show it, set exactly these, or add and remove. Names are `timing`, `gnss`, `sync`, `sources`, `ntp`, `minute`, `tuning`, `rtc`, `events`, `all` and `none`, or a mask such as `0x1f` |
| `pic [-v]` | Everything the PIC reports |
| `pic stored` | What the PIC has in flash |
| `pic save`, `pic defaults`, `pic reset` | Save, defaults or reset for the PIC alone |
| `pic resync` | Judge every time source again from scratch and choose again, holdover dropped. The time of day carries on |
| `pic bootloader` | Enter the PIC's bootloader |
| `pic boot [run\|hold]` | The PIC's bootloader. `run` starts the application, `hold` keeps the PIC in its bootloader |

## The ESP

| Command | Description |
| --- | --- |
| `ntp [-v]` | NTP status and the servers, as `chronyc sources`: which is followed (`*`), which agree (`+`), which are outvoted (`x`) or unusable (`?`), with stratum, poll interval, reach, offset and root distance. `-v` adds round trips, jitter, counters, frequency and slew |
| `ntp tracking` | The server followed and the clock, as `chronyc tracking` |
| `ntp server 1..4 <host>\|none` | Up to four NTP servers, names or IPv4 or IPv6 addresses, a new one polled at once. 2 to 4 are optional, and `ntp server` lists them. By default `rubidium.darksky.io`, `time.cloudflare.com` and `ntp1.npl.co.uk` |
| `ntp interval <time>` | The longest poll interval, 128s–1d, default 512s |
| `ntp sync` | Poll now |
| `wifi [-v]` | Connection status, SSID, IPv4 and IPv6 addresses and signal, `-v` adds every IPv6 address, gateway, DNS servers, which address family is preferred, channel, power save (always off, for timing) and MAC |
| `wifi scan` | Scan nearby networks |
| `wifi connect`, `wifi disconnect` | Reconnect, or disconnect (the reconnect task may reconnect later) |
| `wifi setup` | Open the WiFiManager setup access point |
| `wifi forget` | Forget the network and restart |
| `telnet` | Telnet status |
| `telnet on\|off` | Start Telnet at start-up (on also starts it now, off leaves a running listener alone) |
| `telnet start`, `telnet stop` | Start or stop the listener now, without changing the start-up setting |
| `telnet port <n>` | The port, 1–65535 |
| `update [-v]` | The running version, the staged files (`-v` the SPL's too), the update server and the automatic update setting |
| `update check` | What is offered for the ESP and the PIC. Also stages the PIC's application offered if no slot holds it |
| `update install` | The next update due: the ESP's if newer (finished by `esp reset`), else the PIC's |
| `update esp [force]` | The ESP's image if newer (finished by `esp reset`). `force` installs any other version and restarts |
| `update pic [force]` | The PIC's application if newer. `force` any version, from the staged file when the update server cannot be reached |
| `update spl [force]` | The PIC's SPL: not supported yet |
| `update erase pic\|spl\|all` | Empty the staged files' slots |
| `update server <host>`, `update path <path>` | The update manifest |
| `update config <name>` | An update profile such as `development`, `default` clears it |
| `update auto off\|<hour>` | Daily automatic updates in that local hour, 0–23 |
| `esp` | Version, uptime, reset cause and settings |
| `esp save`, `esp defaults`, `esp reset` | Save, defaults or restart for the ESP alone |
| `esp factory` | Default ESP settings saved, Wi-Fi forgotten, and restart |

# Future hardware improvements

A better oscillator is still the most direct hardware improvement: an OCXO or TCXO gives the software a better starting point, especially during warm-up or loss of the reference. With some additional circuitry, a VCXO/VCTCXO or even a DCTCXO could also be used.

Temperature compensation is another possibility, but not with the BME280, which is a decorative witness to the power supplies warming up. Its position and relationship to the oscillator temperature matter more than simply having a temperature reading available, which is why a temperature sensor right beside the oscillator is the better plan.

Using a dual core dsPIC33CH device, with the secondary core as a dedicated 'timing domain', while the main core handles the tedious tasks such as user input and UART communication, is another avenue. The increased clock speed from 40MIPS to 100MIPS will also reduce quantisation uncertainty. Both are going into V3 hardware, now in design: a dsPIC33CH, a VCTCXO with an NTC beside it, and supplies the PIC can switch and measure. SML already has room for it.

However good the timekeeping, the 'real' limit of displayed accuracy here is still the persistent afterglow of the VFD tubes, hence why [really fancy clocks use Nixie tubes](https://www.daliborfarny.com/project/calibration-display-for-nasa/).

<img width="1329" height="573" alt="image" src="https://github.com/user-attachments/assets/7ac8c3ad-8a94-4397-b105-5bd9c55a9ebd" />
