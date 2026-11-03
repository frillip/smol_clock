# ESP32 firmware (V2+)

_A network co-processor for a clock that would rather do its own timekeeping, thank you._

The ESP32 handles everything the dsPIC would prefer not to think about: WiFi, NTP, timezone lookups, the Telnet console and firmware updates. It talks to the PIC over UART using the SML protocol described in [common/SML_PROTOCOL.md](../common/SML_PROTOCOL.md), keeps its own NTP-disciplined time with a PPS output for the PIC, and passes on the PIC's console output.

The PIC remains in charge of UTC, local time and the display. The ESP supplies information and suggestions; the PIC decides what to do with them.

This firmware requires the V2+ dsPIC firmware in [dspic_src](../dspic_src). It no longer speaks the V2 0x83 protocol.

# Building

The firmware builds with the Arduino IDE 2 or arduino-cli, using the **esp32** core 3.3.x. It has been built and run with 3.3.9 to 3.3.12. Select *ESP32 Dev Module*, 16MB flash and the *Custom* partition scheme, which uses [partitions.csv](partitions.csv). The partition table can only change over USB. A clock with the older table updates its ESP over the air as before, but cannot stage the PIC's files (`PIC updates: OFF, no picfw partition`) until it is flashed once over USB. `nvs`, `otadata` and `app0` stay where they were, because the Arduino upload writes `boot_app0.bin` at 0xE000 and the application at 0x10000 whatever the table says, so the saved settings and Wi-Fi survive the change. What the old table left elsewhere is harmless: a `picfw` slot is only trusted when its CRCs pass.

The libraries are git submodules in [libraries](libraries): WiFiManager, ESPTelnet and ArduinoJson. Run `git submodule update --init`, then point the IDE's sketchbook (or `--libraries` for arduino-cli) at that folder. ezTime is no longer used, and neither is ESP32-OTA-Pull: the updater reads the same manifest itself, so that its requests connect with IPv6/IPv4 fallback (below).

The console and PIC link baud rates are set in [src/board.h](src/board.h). The PIC link rate must match the dsPIC's UART1 setting.

## Shared sources

The ESP compiles the same SML codec and protocol definitions as the dsPIC, so the two ends cannot quietly disagree about where a field is. Rather than keeping copies, the sketch uses symbolic links that mirror the repository layout:

| Link | Target |
| --- | --- |
| `src/common/enums.h`, `src/pic/sml/sml_proto.h`, `src/pic/sml/sml.c`, `src/pic/sml/sml.h` | `common/` |

`common/sml.h` includes `sml_proto.h` from its own folder, so `sml.c`, `sml.h` and `sml_proto.h` are linked side by side in `src/pic/sml/`. The links live under `src/` because the Arduino build only compiles source files in the sketch folder and in `src/`. Anything in another subfolder is copied into the build but never compiled, which produces some puzzling linker errors.

On Windows, symbolic links need Developer Mode (or an administrator prompt) and `git config core.symlinks true` before checkout. Alternatively, `mklink` in cmd from `esp_src\src`:

```bat
mklink common\enums.h      ..\..\..\common\enums.h
mklink pic\sml\sml_proto.h ..\..\..\..\common\sml_proto.h
mklink pic\sml\sml.c       ..\..\..\..\common\sml.c
mklink pic\sml\sml.h       ..\..\..\..\common\sml.h
```

Failing that, copying the four files into place also works, provided they are copied again whenever the originals change.

## Tests and formatting

The portable parts of the firmware have host tests: the clock model, NTP packet handling, tzinfo parsing (including a byte-for-byte check of the TZ-STATE frame against the protocol document), the frame and text splitter, and argument parsing. They build with gcc and run under the address and undefined-behaviour sanitisers:

```
cd tests/host && make
```

The console commands and the PIC link have their own, in `tests/commands`: every command against a PIC played by the test, the settings written whole and their answers, and the value parsers (`cd tests/commands && make`). The dual-stack fallback has its own too, in `tests/net`: the order addresses are tried in, a failed family held back and given back, and URLs (`cd tests/net && make`). So do the NTP servers, in `tests/ntp`: the choice between servers that agree or not, and the NTP service against fake servers that agree, lie, go quiet and change (`cd tests/ntp && make`). And the settings, in `tests/settings`: kept from older firmware, and the NTP servers' defaults (`cd tests/settings && make`). The update manifest's parsing and selection are in `tests/manifest`, which needs the ArduinoJson submodule. The PIC's updates are in `tests/pic_update`: the `picfw` slots against a model of NOR flash, and the whole bootloader exchange against the real SPL core from `dspic_spl`, with frames lost and damaged, PIC resets and power cut at every step.

[tools/format.sh](tools/format.sh) applies the same house style as the dsPIC sources.

# Layout

| Folder | Contents |
| --- | --- |
| `esp_src.ino` | `setup()` and `loop()` |
| `src/board.h`, `src/version.h` | Pin map and baud rates, firmware version |
| `src/common/` | Links to the shared protocol definitions |
| `src/pic/` | The PIC link: SML framing, the PIC's console text, its held state, commands in both directions, pairing TIME-AUTH with the epoch edge. Also the PIC state report, and the PIC's updates: its files (`pic_file`), their staging in `picfw` (`pic_store`), the bootloader exchange (`pic_update`) and its console glue (`pic_boot`). `sml/` links to the shared SML codec and boot layout |
| `src/timing/` | NTP client and packet handling, the clock model, ESP PPS output, PIC epoch and GNSS PPS capture |
| `src/tz/` | tzinfo lookups and cache, TZ-STATE assembly, TIME-LEAP |
| `src/net/` | WiFi, dual-stack connections with fallback, Telnet, the update manifest and updates, STAT-NET |
| `src/console/` | Console line input and output, the command table |
| `src/settings/` | Preferences stored in NVS |
| `src/util/` | Argument parsing, time formatting, SHA-256 |

| Pin | Function |
| --- | --- |
| GPIO13 / GPIO12 | UART2 RX / TX to the PIC |
| GPIO27 | PIC epoch output, input to the ESP |
| GPIO5 | ESP PPS output, to the PIC |
| GPIO18 | GNSS PPS, only with the solder jumpers fitted |
| GPIO23 | Status LED, lit for 100ms on each ESP PPS edge while NTP is synced, two short flashes while the NTP servers disagree |

Almost everything runs from `loop()`. The exceptions are the NTP exchanges, which run in a task of their own, and the timing edges, which are handled in interrupts. Blocking work such as an HTTPS request holds up `loop()` but nothing else: the ESP PPS carries on in its interrupt, and the 4KB receive buffer on the PIC link holds several seconds of traffic.

# Network

The ESP is dual stack: IPv4 by DHCP, IPv6 by SLAAC. IPv6 is turned on before Wi-Fi connects (`WiFi.enableIPv6()`), so it has a link-local address at once and global addresses from the router's advertisements a few seconds later. DHCPv6 is not used. DNS servers come from DHCP or from the advertisements' RDNSS option: arduino-esp32 3.3.12's lwIP keeps three, and RDNSS fills the first two, so whichever arrived last is in use. lwIP does the rest:

- NTP, the tzinfo lookups and the update checks connect as any dual-stack host does (`src/net/net_connect.cpp`): the name's IPv6 and IPv4 addresses are both looked up (IPv6 only while the clock has a global IPv6 address), the preferred family's is tried first, and if it does not answer, the other's. IPv6 is preferred, unless an IPv6 connection or NTP poll failed within the last hour and IPv4 has not failed since; then IPv4 goes first. A success clears its family's failure, and NTP and HTTPS share what is learnt, so a family that does not route costs one timeout an hour rather than one per request. For NTP, a poll that gets no reply at all is followed at once by one to the other address (`NTP: no reply from 2001:db8::123, trying 192.0.2.10`); a refusal (a kiss) or a bad reply is the server's, not the family's. The first poll after Wi-Fi connects waits up to 15 seconds for SLAAC's global IPv6 address, which follows IPv4 by a few seconds, so that the frequency is learnt over the path NTP goes on using; it starts as soon as the address arrives, and an IPv4-only network costs the 15 seconds once per connection. For HTTPS the connection is made first, to the address that answers, verified against the host name, and HTTPClient then reuses it. `ntp server`, `tz server` and `update server` also take IP literals, an IPv6 one bracketed or not (it is bracketed in a URL). A failed update request says why: `Update server: 1 (HTTP request failed: connection refused)`.
- The Telnet server listens on `::`, which takes IPv4 clients too (as IPv4-mapped addresses, shown as plain IPv4).
- Pings are answered on both.

Wi-Fi power save is off. With it on (arduino-esp32's default, modem sleep), the radio sleeps between the access point's beacons, and a frame for the clock waits at the access point until the radio next wakes: tens to hundreds of milliseconds, on the way in only. That lands in the round trip and the asymmetry of every NTP exchange. The cost is the radio's receive current all the time, some tens of milliamps, from a clock on mains power. It is set before Wi-Fi starts and checked on every new connection, and `wifi -v` shows it (`Sleep:  OFF`).

`wifi -v` shows which family is preferred and any recent failures: `Prefer: IPv4 first; IPv6 failed 00:12:05 ago`.

Where the clock can be reached is printed with both addresses: `Wi-Fi: 192.168.1.20, 2001:db8:1::20` whenever they change (the IPv6 one usually a few seconds after connecting), in the status line, and Telnet's `192.168.1.20:23, [2001:db8:1::20]:23`. `wifi -v` lists every IPv6 address with its kind (GLOBAL, UNIQUE_LOCAL, LINK_LOCAL) and the DNS servers. What is connected or resolved is printed as used: the NTP server's address (`pool.example (2001:db8::123)`), the tzinfo server's (`tz -v`), the update server's (after `Update server: …`), a Telnet client's.

STAT-NET still carries the IPv4 address only; the PIC does not use it (to be brought in line later).

# Time

## NTP

ezTime has been replaced by a small NTP client. Each poll sends four requests two seconds apart and keeps the exchange with the shortest round trip, since the fastest exchange has the least queueing and therefore the least asymmetry. The send and receive timestamps are taken immediately around the socket calls in the client task, so loop latency never reaches the measurement.

Up to four servers can be set (`ntp server 1..4 <host>`, `none` to clear 2 to 4). The defaults are `rubidium.darksky.io`, `time.cloudflare.com` and `ntp1.npl.co.uk`, with server 4 unset: three independent sources, so one that goes wrong is outvoted. They are `NTP_SERVER_DEFAULTS` in [src/settings/user_prefs.h](src/settings/user_prefs.h), one string per server. Each is a client of its own, polled one at a time, with its own reach (the last eight polls), samples, jitter and root distance: half the round trip and half the server's root delay, its root dispersion, its jitter (the spread of its last eight samples about a straight line), and the drift allowed since its latest sample. Each name is resolved at every poll, and the first address lwIP returns is used (it keeps only the first record of each family), so `pool.ntp.org` set as one server is one server: set `0.pool.ntp.org`, `1.pool.ntp.org` and so on for more. Two servers resolving to the same address are two clients all the same.

The first good sample of all sets the clock. After that, after every poll, the servers are compared as chronyd compares them: each says the clock is off by its offset, give or take its root distance, and the largest set of servers whose intervals overlap are those that agree. A server outside it is a falseticker (`x`), and of those that agree the closest is followed (`*`), the others standing by (`+`). The one followed is kept unless another is twice as close. A server none of whose last three polls was good, or whose latest sample is older than the sync limit below, is unusable (`?`). Only the server followed disciplines the clock, and changing server starts the frequency estimate's window again, as two servers' raw offsets differ by their paths' asymmetry.

Without a majority (two servers that disagree, or two pairs), the clock keeps to the server it is following, or the first of them, rather than coast on neither: for a clock without GNSS the first server is likely the better bet than the PIC's holdover. The console says so (`NTP: servers DISAGREE, no majority`, and in `ntp` and the status), and the status LED flashes twice each second instead of once. The PIC sees one NTP source either way: this clock.

The servers are polled at startup, then after 64, 128 and 256 seconds while the crystal frequency is being learnt. After that the interval adapts between 128 seconds and `ntp.interval` (default 512): it doubles after four corrections in a row under 500µs from the server followed, and halves after one over 2ms. Every server is polled at that interval. A crystal that is warming up, or sitting next to a radiator, gets polled more often; a settled one leaves the servers in peace. Failed polls are retried after 60 seconds, doubling up to the interval.

512 seconds as the longest keeps the frequency estimate honest. Its window of eight samples then spans about an hour, long enough to measure the crystal to a fraction of a ppm through a few milliseconds of jitter, and short enough to follow it as the room warms and cools. The traffic is nothing much: each poll is four 48-byte requests, so four servers at 512 seconds come to one request every 32 seconds on average.

`ntp` shows the servers as `chronyc sources` does, and `ntp tracking` the server followed and the clock as `chronyc tracking`:

```
  # Server                   St   Poll Reach LastRx     Offset        +/-  Address
* 1 rubidium.darksky.io       1   512s   377     5m  +112.0 us   5.204 ms  2001:db8::123
+ 2 0.pool.ntp.org            2   512s   377     4m  -1.204 ms   9.877 ms  192.0.2.10
x 3 1.pool.ntp.org            2   512s   377     4m  +48.03 ms   8.102 ms  198.51.100.7
```

Offset is each server's latest sample against the clock as it then was, and +/- its root distance now. `ntp -v` adds each server's round trip, jitter and counters, and the clock's frequency and slew.

`ntp.interval` is the longest interval rather than a fixed one.

The servers' hostnames are resolved in `loop()`, not in the NTP task. In arduino-esp32 3.3.x, `Network.hostByName()` clears lwIP's DNS cache without taking the TCPIP core lock, which cancels any lookup in progress elsewhere and trips an lwIP assertion. With the PIC already running, the timezone lookup at startup overlapped the NTP lookup, and the ESP restarted every few seconds until power cycled. Keeping every DNS lookup in `loop()` means none is ever in progress when the cache is cleared. Any future network code should follow the same rule.

## Clock model

The ESP keeps UTC as a model against its own microsecond timer: an anchor point, a frequency estimate and a slew in progress. The first NTP sample sets the clock, and a sample more than 128ms from the model steps it. Each sample is judged against the model at the moment it was taken, but the correction starts from the moment it is applied, so the clock never jumps by what the old slew would have done in the meantime.

Corrections up to 8ms are slewed in gently: spread over about two poll intervals, and never faster than 5ppm. The next sample replaces whatever is left, so each sample's noise is only half applied, and the ESP PPS moves by a few microseconds a second at most. That is slow enough for the PIC's NTP loop to follow without re-stepping, which is the point. Anything larger is slewed in at 100ppm and flagged in TIME-NTP (`CORRECTING`) while it lasts, since the PIC cannot follow it anyway and might as well know.

The frequency comes from a least-squares line through the raw offsets of the last eight samples, leaving out any whose round trip is well above the best of them: queueing adds delay in one direction, so the slow exchanges carry the most asymmetry. Two samples two minutes apart give a first estimate.

The model also reports an uncertainty bound: half the round trip, the server's own root delay and dispersion, any correction not yet slewed in, and a drift allowance that shrinks as the frequency becomes known.

In a 12-hour simulation with a crystal drifting from 4.9 to 2.9ppm, 12ms round trips, a few milliseconds of queueing and 3.5ms of path asymmetry, the ESP PPS wanders by 340–700µs (sd) around its asymmetry offset, against 550–1000µs for the previous model, and never moves faster than 7µs a second, against 100µs. The asymmetry offset itself, about 1.3ms here, is invisible to NTP and to any client of it.

## ESP PPS

The ESP PPS output on GPIO5 is a 100ms pulse whose rising edge marks the ESP's UTC second. It is generated by a 1MHz hardware timer alarm. At each falling edge the interrupt arms the next rising edge, at the time `loop()` planned from the clock model, or one second on (corrected for frequency) if `loop()` has been busy. A long HTTPS request therefore delays corrections, but does not cost an edge.

The interrupt reads the time immediately after setting the pin, so the edge latency is measured rather than assumed. On the V2 board it is around 5–7µs.

After each edge the ESP sends SML-TIME-NTP, labelling that edge with its UTC second, phase, uncertainty and the details of the last NTP update. The PIC uses this as a time source when GNSS is unavailable.

## PIC epoch

The PIC's epoch output on GPIO27 is timestamped in an interrupt and paired with the SML-TIME-AUTH message that labels it. This gives the ESP a UTC reference before NTP is available, and allows a direct comparison of the PIC's epoch against NTP in the diagnostics.

# Timezone

## tzinfo lookups

With tzinfo enabled, the ESP asks the timezone service ([protocol v1](../tzinfo_service/API.md), served by [tzinfo_service](../tzinfo_service)) where it is and how local time works there:

```
GET https://<tzinfo.server><tzinfo.path>[?lat=...&lon=...]
```

The default is `rubidium.darksky.io/smol_clock/api/v1/tzinfo`.

tzinfo is enabled by default. Clocks with saved settings keep whatever they had; `tz remote on` and `save` turn it on.

The position comes from the PIC's GNSS fix, rounded to `tzinfo.gnss_accuracy` decimal places using integer arithmetic before it leaves the ESP. With the setting at `-1` (`tz precision none`), or with no fix after 60 seconds, no position is sent and the server falls back to GeoIP.

The ESP looks up the timezone at startup, as soon as tzinfo is enabled, at the configured interval (or sooner if the server says so), after moving more than a whole rounding step, and on request. The 60-second wait for a fix at startup is skipped when the PIC reports no GNSS receiver at all, and when nothing is cached: a clock with no schedule would otherwise show UTC for a minute for no good reason. In that case GeoIP goes first, and the GNSS position refines it once there is a fix. Failed lookups are retried after 5 minutes, doubling up to an hour, or after 30 seconds, doubling up to 5 minutes, while there is no schedule at all. The latest result is cached in NVS and used at startup until a fresh lookup replaces it.

## Schedule horizon

The v1 response gives the current state and up to eight future transitions, but no horizon. The ESP infers one:

| Transitions received | Horizon |
| --- | --- |
| More than 8 | The ninth transition, exactly |
| Exactly 8 | One second after the last, since the server may have stopped there |
| Fewer than 8 | Open-ended, as the server's five-year window held no more |

## Sending to the PIC

The schedule goes to the PIC as SML-TZ-STATE when it changes, whenever the PIC reports that it holds no schedule (after a PIC reset), and every 10 minutes. Transitions already in force are folded into the current entry, so the PIC always receives as many future transitions as possible. Nothing is sent while tzinfo is disabled, and the PIC uses its manual offsets.

SML-TIME-LEAP is built from the PIC's own GNSS leap-second data and sent back to it, so the information survives the receiver losing its fix. The v1 service has no leap-second data of its own.

# PIC link

At startup the ESP sends CMD-USER `SHOW` (everything), which moves the PIC into SML mode and prompts it to print its status, and asks for STAT-SETTINGS. STAT-NET every 10 seconds and TIME-NTP every second bring the PIC back into SML mode after it resets. The PIC then sends STAT-SYS (SML 1.4) every epoch: its firmware version, uptime and why it last reset. The ESP announces the PIC when it first hears from it, and again whenever the uptime drops.

Anything on the link outside a valid frame is treated as the PIC's console output and printed, as is STAT-DEBUG text. Both go to the USB-UART console and to Telnet.

The console's PIC actions (`pic save`, `rtc set`, `debug`, the PIC's part of `tz` or `display`, and so on) are sent as CMD-USER requests, and the PIC's answer is printed on whichever console the command came from. Where the command asked to see something, what the PIC prints is the answer, so its ACK only shows with `-v`. Commands from the PIC to the ESP (codes from 0x8000, for example from the PIC's menu) are carried out and acknowledged.

The PIC's settings (time zone, DST, display, brightness, beeps, alarm, oscillator baseline) are not commands. The ESP keeps the latest STAT-SETTINGS, and a console command changes one setting by writing the whole body back with CMD-SETTINGS, against the revision it last saw. A change typed while the previous write is unanswered waits, and goes with it as soon as the answer brings the new revision, so pasting several settings at once works. If the PIC refuses a write (`PIC settings: NACK (argument)`, say, for a value out of range), or does not answer, the ESP asks for STAT-SETTINGS again and drops any change still waiting. The PIC's menu writes the same settings, and a write from either end that has not seen the other's is refused rather than overwriting it.

`sources [-v]` prints the PIC's time sources as a table: verdict, bound, phase and when each was last good; `*` marks the one in use, `+` the one that would come next, `x` one discounted (its time beyond the accuracy limit, or inconsistent) and `?` one unusable (absent, or without time, pulse or label), as `chronyc sources` does. The table also appears in the status (an empty line on the console) once the PIC sends STAT-SOURCES. `debug` shows or changes which of the PIC's diagnostics are printed: `debug gnss sync` exactly those, `debug +ntp -sync` adding and removing, `debug -all` or `debug none` nothing. The names are `timing`, `gnss`, `sync`, `sources`, `ntp`, `minute`, `tuning`, `rtc` and `events`, or `all`, or a mask such as `0x1f`. With none set the PIC prints nothing unasked but its start-up line and faults.

# Console output

Information is printed briefly by default, in the manner of chronyc: the ESP's NTP time and its server, a line each for the time zone, leap seconds, Wi-Fi and Telnet, then the PIC's firmware, uptime and last reset cause (STAT-SYS), its time, oscillator, sources and GNSS fix. Sources the PIC has never seen are left out of the table. `-v` at the end of any command that shows something prints everything: the counters, the sources' ranks, their own uncertainty figures, runs and edges, the epoch against NTP and the ESP's GNSS PPS, the PIC's reset bits and longest main-loop pass, and the link statistics. When the PIC's uptime drops, the console prints `PIC restarted: firmware 0.1.0, up 0 s, last reset: brown-out` (or whatever the cause was). `verbose on` makes that the default, an empty line included, until `verbose off` or a restart. Times are given to four significant figures, in the largest unit that leaves a whole part: `61 ns`, `571.0 us`, `13.70 ms`, `1.250 s`. A GNSS clock, for example:

```
ESP:      0.2.0 SMOL_CLOCK_A1B2C3, up 00:41:07
NTP:      2026-10-21T10:53:20Z +/-13.70 ms, synced
Server:   pool.ntp.org (192.0.2.10), stratum 2, updated 00:12:40 ago; 3 servers
Zone:     Europe/London (GNSS), UTC+01:00, changes 2026-10-25T01:00:00Z
Leap:     TAI-UTC 37 s (GNSS)
Wi-Fi:    home, 192.168.1.20, 2001:db8:1::20, -61 dBm
Telnet:   RUNNING 192.168.1.20:23, [2001:db8:1::20]:23, start-up ON, client 2001:db8:1::5

PIC:      0.1.0, up 00:40:58, last reset: power-on
UTC:      2026-10-21T10:53:20Z +/-61 ns, GNSS LOCKED
Local:    2026-10-21T11:53:20+01:00 DST
Freq:     -3.7500 ppm (TRACK)
Sync:     LOCKED on GNSS, synchronised
  Source    Verdict            Bound       Phase  Last good
* GNSS      good              61 ns      +25 ns       0s
+ HOLDOVER  good              86 ns           -      42s
  NTP       good           13.70 ms   -571.0 us       0s
GNSS:     3D OK, tAcc 12 ns
```

# Updates

The updater ([src/net/updater.cpp](src/net/updater.cpp)) reads the manifest at `https://<update server><update path>`, by default `/smol_clock/v2plus/manifest.json`. A clock with the old default path (`/smol_clock/v2plus/update.json`) saved is moved to the new one when its settings load.

## Manifest

```json
{
  "format": 2,
  "configurations": [
    {
      "hardware": "v2",
      "config": "development",
      "devices": ["A0:B1:C2:D3:E4:F5"],
      "esp": {"version": "0.4.0", "url": "https://…/esp_src.bin", "size": 1252320, "sha256": "…"},
      "pic": {"version": "0.4.0", "url": "https://…/dspic_src.smolpic", "size": 177280, "sha256": "…"},
      "pic_spl": {"version": "0.1.0", "url": "https://…/dspic_spl.smolspl", "size": 9088, "sha256": "…"}
    }
  ]
}
```

Every configuration is checked before any is used, and a manifest with a malformed one is refused whole. The clock takes the first configuration whose `hardware` is `v2`, whose `config` matches `update config` (absent means the default, none), and whose `devices`, if given, include its MAC address (any case). Each component is optional, but a configuration needs at least one. Versions are dotted numbers, compared numerically (`1.10` is newer than `1.9`). A download must match the manifest's size and SHA-256, or it is discarded. `pic_spl` is read and reported, but not yet installed.

[tools/make_manifest.py](../tools/make_manifest.py) writes the manifest for a set of files, reading each version from the file itself (the PIC's headers, the ESP's user agent string), and adds to or replaces a configuration in an existing one with `--merge`:

```
make_manifest.py --base-url https://rubidium.darksky.io/smol_clock/v2plus/dev/ --esp esp_src.ino.bin --pic app.smolpic --pic-spl spl.smolspl -o manifest.json
```

[tools/make_dist.py](../tools/make_dist.py) does the whole release from the latest builds: the released IPL hex, the SPL and application files, the first-installation hex, the ESP's image and the manifest, each named after the version read from it, into `dist/`. A version header edited after its build is reported. `python tools/make_dist.py` with no arguments uses the MPLAB X production builds and the Arduino build folder.

## Order

Newer only, unless forced. The ESP goes first: the PIC's application is only installed by an ESP at or past its configuration's ESP version, so a PIC never runs against an ESP that does not know its SML version. The ESP also checks that the application's SML major version is its own before installing it. The daily check installs the ESP's update and restarts, or else the PIC's.

## The PIC's files

The PIC's application (`.smolpic`) and SPL (`.smolspl`) files come from `tools/make_image.py`: a 64-byte header with its own CRC-32, then the image packed three bytes per instruction. They are staged in the `picfw` partition (512 KB):

| Offset | Slot |
| --- | --- |
| 0x00000 | Application 0, 240 KB |
| 0x3C000 | Application 1, 240 KB |
| 0x78000 | SPL 0, 12 KB |
| 0x7B000 | SPL 1, 12 KB |
| 0x7E000 | Spare, 8 KB |

A file is written image first, its header last, so a slot interrupted mid-write has no valid header. A slot is only trusted when its header's CRC and the image's CRC both pass, whatever else was in the flash before. A download goes to a slot with no valid file, else the one whose version is not the one the PIC runs, else the older. The slot the PIC runs from is kept, so a file already staged (same version and SHA-256) is installed again without a download.

The application the manifest offers is staged ahead. When no slot holds it, `update check`, `update install`, `update pic` (when up to date) and the daily check download it into a slot, keeping the slot with the version the PIC runs (`PIC: staging 0.3.1, the application it runs, into slot 0`). So a clock that has been online once holds a copy of its application, and an update offered is installed without waiting for the download.

Without the update server (no Wi-Fi, or the server down), `update pic force` installs a staged file: the version the PIC runs, else the newest. `update pic` does the same unasked when the PIC is in its SPL without a valid application. And the ESP does it by itself: a PIC that stays in its SPL without a valid application for 30 s (an update cut short, its power pulled mid-update) is given the staged file it last ran, else the newest, up to three times per ESP start.

## Installing on the PIC

The exchange with the PIC's SPL ([src/pic/pic_update.c](src/pic/pic_update.c)): ENTER_BOOTLOADER to the application, then BOOT-STATUS from the SPL, HOLD, ERASE, the image in 192-byte BOOT-DATA chunks (blank ones skipped), each checked against the SPL's CRC, a read back of the whole image with BOOT-VERIFY, COMMIT, then RUN and STAT-SYS from the new application. A chunk or command that goes unanswered is sent again, and an error that leaves the PIC's state unclear starts a new pass from STATUS, up to three. A PIC that already runs the version and CRC offered is told to RUN, unless forced. While the PIC is in its SPL, the ESP sends it nothing but bootloader messages, and console commands for the application say so.

```
PIC update: to 0.4.0, 58838 instructions
PIC update: asking the application for the SPL
PIC update: erasing
PIC update: writing 920 chunks
PIC update: written 25%
…
PIC update: reading back
…
PIC update: committing
PIC update: DONE, the PIC runs it, <seconds> s
```

`pic boot` prints what the SPL reports (its mode, versions, whether the application is valid) when the PIC is in it, or else the application's version, then how the last update went. `pic boot run` and `pic boot hold` send RUN and HOLD to the SPL. `update` lists the staged files (`-v` also the SPL slots), and `update erase pic|spl|all` empties their slots (the sector holding each header is erased).

While a download holds up the main loop, the PIC link is still drained between reads (`pic_link_poll()`), so its 4 KB receive buffer does not overflow. A command from the PIC that arrives then is answered BUSY. Bytes are still lost while the ESP erases or writes its flash (the UART's 128-byte FIFO overruns with its interrupt held off, at 921600 baud in under 1.5 ms), so broken frames are expected then. The PIC's raw console text is printed a whole line at a time, and a line holding any byte no console text has (a broken frame's header or payload), or left unfinished when the link goes quiet, is dropped and counted (`pic -v`, `Text:`). The PIC's terminal controls (the clear screen at its start) are left out.

# Telnet

Telnet gets a copy of all console output, the PIC's included. That copy never blocks: whatever the connection cannot take at once is dropped, and a note saying how much appears in the stream when there is room again. A plain write to a stalled connection waits up to 10 seconds in the network stack, holding up `loop()` (and the PIC link, and TIME-NTP) meanwhile, and after three such failures the Telnet library used to drop the client. Replies to commands typed at Telnet are still written normally.

The status shows the ESP's uptime and why it last started. A restart through the EN pin, which is what a USB-UART's DTR and RTS lines do when a computer opens or closes the port, reports as "power-on or EN pin", so an ESP that restarted while the PIC kept counting epochs was most likely reset that way.

# Settings

Settings are stored in NVS. NTP servers 2 to 4 are kept under a key of their own. The settings carry a header, `SMOL` (`0x534D4F4C`), and anything saved under another, including by V2 and by V2+ up to 0.3.0 (`0xC0FFEEEE`), is ignored: the ESP starts from its defaults and saves them. Wi-Fi credentials are kept by WiFiManager apart from these, so a clean start of the settings does not lose the network. Each field is checked on load. An invalid field reverts to its default and the rest are kept, rather than one bad value resetting everything. Changes made from the console take effect immediately and persist after `save` (or `esp save`). `defaults` (or `esp defaults`) puts the defaults in force without saving them.

The full command list is in the repository [README](../README.md#console-commands), and `help` prints it on the console, `help <command>` the settings and actions of one. The table of commands is in [src/console/commands.cpp](src/console/commands.cpp): each command lists its settings and actions with the kind of value each takes, and the parsing, the showing of a setting given no value, the error messages and the help all come from that table. `wifi` never prints the saved WiFi passphrase.
