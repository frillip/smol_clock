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

The ESP compiles the same SML codec, protocol definitions and bootloader layout as the dsPIC, so the two ends cannot quietly disagree about where a field is. The Arduino build only compiles files in the sketch folder and in `src/`, so the ESP keeps copies of them in `src/pic/sml/`: `sml.c`, `sml.h`, `sml_proto.h`, `boot_layout.h`, `boot_crc.h` and `boot_crc.c`, from `common/`. They are plain files, not symbolic links, so a checkout builds anywhere, Windows included, without special settings.

Edit the originals in `common/` only, then refresh the copies:

```
python3 tools/sync_common.py          # copies whatever differs
python3 tools/sync_common.py --check  # changes nothing; fails, listing them, if any differ
```

The ESP's host tests (`tests/commands`, `tests/pic_update`) run `--check` first, so a copy left behind fails them. The same tool keeps the helper sketches' copies of `partitions.csv` in `etc/`.

## Tests and formatting

The portable parts of the firmware have host tests: the clock model, NTP packet handling, tzinfo parsing (including a byte-for-byte check of the TZ-STATE frame against the protocol document), the frame and text splitter, and argument parsing. They build with gcc and run under the address and undefined-behaviour sanitisers:

```
cd tests/host && make
```

The console commands and the PIC link have their own, in `tests/commands`: every command against a PIC played by the test, the settings written whole and their answers, and the value parsers (`cd tests/commands && make`). The dual-stack fallback has its own too, in `tests/net`: the order addresses are tried in, a failed family held back and given back, and URLs (`cd tests/net && make`). So do the NTP servers, in `tests/ntp`: the choice between servers that agree or not, their combination, the clock's bound against a simulated crystal and server, the phase statistics, and the NTP service against fake servers that agree (combined), lie, go quiet and change (`cd tests/ntp && make`). And the settings, in `tests/settings`: kept from older firmware, and the NTP servers' defaults (`cd tests/settings && make`). The update manifest's parsing and selection are in `tests/manifest`, which needs the ArduinoJson submodule. The PIC's updates are in `tests/pic_update`: the `picfw` slots against a model of NOR flash, and the whole bootloader exchange against the real SPL core from `dspic_spl`, with frames lost and damaged, PIC resets and power cut at every step. The SPL's update runs end to end against the IPL's choice of what starts, the SPL core and the application's SPL writer: kept, rejected and put back, and power cut at the application's flash work.

[tools/format.sh](tools/format.sh) applies the same house style as the dsPIC sources.

# Layout

| Folder | Contents |
| --- | --- |
| `esp_src.ino` | `setup()` and `loop()` |
| `src/board.h`, `src/version.h` | Pin map and baud rates, firmware version |
| `src/common/` | Links to the shared protocol definitions |
| `src/pic/` | The PIC link: SML framing, the PIC's console text, its held state, commands in both directions, pairing TIME-AUTH with the epoch edge. Also the PIC state report, and the PIC's updates: its files (`pic_file`), their staging in `picfw` (`pic_store`), the bootloader exchange (`pic_update`), the SPL's update (`spl_update`) and their console glue (`pic_boot`). `sml/` holds copies of the shared SML codec and boot layout (`tools/sync_common.py`) |
| `src/timing/` | NTP client and packet handling, the clock model, ESP PPS output, PIC epoch and GNSS PPS capture |
| `src/tz/` | tzinfo lookups and cache, TZ-STATE assembly, TIME-LEAP |
| `src/net/` | WiFi, dual-stack connections with fallback, Telnet and its password, the update manifest and updates, STAT-NET |
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

Almost everything runs from `loop()`. The exceptions are the NTP exchanges, which run in a task of their own, and the timing edges, which are handled in interrupts. Blocking work such as an HTTPS request holds up `loop()` but nothing else: the ESP PPS carries on in its interrupt, and the 16KB receive buffer on the PIC link holds several seconds of traffic, or three `pic -v` printouts at full line rate. Receive overflows and line errors are counted (`pic -v`, `UART:`).

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

ezTime has been replaced by a small NTP client. Each poll sends four requests two seconds apart and keeps the exchange with the shortest round trip, since the fastest exchange has the least queueing and therefore the least asymmetry. The client uses lwIP's raw UDP API rather than a socket, to take its timestamps as close to the wire as lwIP allows: the send time once lwIP has handed the request to the Wi-Fi driver, and the receive time in lwIP's own task as the reply arrives, before the client task is woken. A socket's timestamps included lwIP's processing on the way out, and on the way in the hand-over to the client task, which was much of the ESP's own lopsidedness (about 100µs, NTP behind, on a LAN). Requests carry DSCP EF (expedited forwarding), so Wi-Fi with WMM, where the driver and access point honour it, queues them ahead of bulk traffic. Loop latency never reaches the measurement.

Server 0 is the one DHCP offers, if any (option 42; IPv4 only, the first server only, as lwIP keeps it): a router's own NTP server is usually the nearest there is, and on the test LAN a LAN server was 5–10 times closer to the truth than any on the Internet. lwIP in this core asks for it with every lease and hands it to its SNTP module, which keeps it once told to (`src/net/dhcp_ntp.cpp`; the SNTP client itself never runs). `ntp server dhcp disable|enable|prefer` (default `enable`): enabled, it is polled first and treated like any other server; preferred, it is followed alone while it agrees with the majority, the others not combined with it, as chronyd's `prefer`; if it falls out, selection carries on as usual. It is read at every poll, so a new lease's server takes over (its statistics start again). Not offered: it is not polled and shows as `? 0 dhcp`. A DHCP server is not necessarily on the LAN; it is judged like any other.

Up to four more can be set (`ntp server 1..4 <host>`, `none` to clear 2 to 4). The defaults are `rubidium.darksky.io`, `time.cloudflare.com` and `ntp1.npl.co.uk`, with server 4 unset: three independent sources, so one that goes wrong is outvoted. They are `NTP_SERVER_DEFAULTS` in [src/settings/user_prefs.h](src/settings/user_prefs.h), one string per server. Each is a client of its own, polled one at a time, with its own reach (the last eight polls) and its own sample statistics, kept as chronyd keeps them (`src/timing/ntp_stats.cpp`, after chrony's `sourcestats.c` and `regress.c`), so each path is judged on its own and one server's path cannot skew another's. Its samples' raw offsets (UTC minus the ESP's timer) go through a weighted least-squares line: each sample weighted by how far its round trip is above the best (chronyd's weights), so an exchange Wi-Fi held up counts for little. The runs test on the residuals drops the oldest samples while the line is the wrong shape (the crystal's frequency has moved), down to 6. While the crystal's temperature compensation is active, never to a line shorter than 30 minutes (`NTP_STATS_MIN_SPAN_S`, not chronyd's): at 128-second polls 6 samples span 11 minutes, and a path that shifted by 200µs for a few polls had the line cut to the shifted samples and those after, its slope 0.3–0.4 ppm off, and the clock swung 600µs peak to peak for 20 minutes. Within half an hour the runs test cannot tell a shifted path from a moving crystal, and a compensated crystal moves little in that time. Replaying a day's real path errors from the GNSS clock: RMS 84 → 68µs, worst 381 → 226µs. At 512 seconds 6 samples already span 51 minutes, so nothing changes there. Without the compensation the lines are cut as chronyd cuts them: an uncompensated crystal swinging 0.5 ppm an hour would cost more (RMS 182 → 257µs at 128 s). The line gives the server's view of the crystal's frequency, a bound on it (the skew: the slope's standard error times Student's t at 99.95%, chronyd's table) and of our offset now. Its root distance is chronyd's: half the round trip and half the server's root delay, and its root dispersion, of the sample that gives the least, the skew over the time since that sample, and the samples' spread about the line. The statistics start again when the address changes, or a sample is more than 128ms off the line (the server's time jumped). As chronyd, a server is selectable only once it has a line (three samples, `?` before): without one its distance is the worst case, growing 2ms a second, so at power-on whichever server answered last looked best and the clock hopped between them. Until any server has a line, the one that set the clock is followed alone. While the lines are young their skew is large (Student's t on one or two degrees of freedom), so the drift allowed follows it up to 5 ppm rather than 2. Each name is resolved once and its address kept, as chronyd keeps a source's: a pool or anycast name gives another address almost every time, and a new address starts the server's statistics again, so a name resolved at every poll never had a line (uk.pool.ntp.org restarted 22 times in 23 polls). It is resolved again after two failed polls in a row (the address may have gone), when the network comes back, and once a week, staying on the address in use if the name still gives it; if the name will not resolve, the kept address is polled. `ntp -v` counts the lookups per server. The first address lwIP returns is used (it keeps only the first record of each family), so `pool.ntp.org` set as one server is one server: set `0.pool.ntp.org`, `1.pool.ntp.org` and so on for more. Two servers resolving to the same address are two clients all the same.

The first good sample of all sets the clock. After that, after every poll, the servers are compared as chronyd compares them: each says the clock is off by its offset, give or take its root distance, and the largest set of servers whose intervals overlap are those that agree. A server outside it is a falseticker (`x`), and of those that agree the closest is followed (`*`), the others standing by (`+`). The one followed is kept unless another is twice as close. While a server's line is young (under six samples), its closeness for this choice takes the line's skew as at most 5 ppm: a line of three to five samples has a skew of hundreds of ppm, so its distance is mostly the time since its last poll, and at power-on whichever server was polled last looked closest (an Internet server 1.3 ms off was followed over the LAN one for minutes). Agreement and combining use the full distance. A server none of whose last three polls was good, or whose latest sample is older than the sync limit below, is unusable (`?`).

The servers that agree are combined, as chronyd combines them: when the server followed has a new sample, the clock is steered to the average of the agreeing servers' lines run on to now (each against the clock as it now stands), each weighted by 1 / its root distance, and at the average of their frequencies, each weighted by 1 / its standard error squared, rather than by the followed server alone. A server whose distance is over 1.5 times the least distance among the agreeing servers (plus 100µs, chronyd's reselect distance), or whose frequency is further from the followed one's than 1.5 times their skews together, is left out (`-`, as chronyc shows it), and without a majority the followed server is used alone. chronyd's limit is 3, but with it an Internet server came in and out of a LAN server's average, moving the clock by about 200µs each time (seen against GNSS): its path's asymmetry is hundreds of µs, a LAN one's next to none. At 1.5 a LAN server is combined with LAN servers, and Internet servers with others about as far, whose asymmetries average down. As chronyd, a server left out stays out a while: each time it is left out it gains 8, up to 32, and loses one at each combining it would have joined, so one near the limit does not flit in and out. A server whose line is still short (under 6 samples) is not combined at all, and gains no count: its distance is mostly its youth (a large skew times the time since its best sample), so it fell inside the limit just after each of its polls and outside it a minute later, and the combined set turned over at every poll for the first quarter of an hour. The followed server may be young. Changes are reported: `NTP: combining 1, 2`, `NTP: combining 2 alone`. Every NTP event line is led by the clock's UTC time of day (`19:37:31Z NTP: following ntp1.npl.co.uk (139.143.5.30)`, `--:--:--Z` before the clock is set), so a session log lines up with `ntp gnss -v`. A server without a line yet (fewer than three samples) adds its offset but not its frequency. `ntp tracking` shows the combination: `Combined:  3 servers, offset -27.00 us (the followed one's 74.00 us)`. Independent servers' noise averages down, and each path's asymmetry is diluted rather than taken whole. A server joining or leaving the combination moves the average by a fraction of the paths' differences, which the clock slews out like any correction. Changing the server followed starts the clock's own sample window again (see the clock model below).

Without a majority (two servers that disagree, or two pairs), the clock keeps to the server it is following, or the first of them, rather than coast on neither: for a clock without GNSS the first server is likely the better bet than the PIC's holdover. The console says so (`NTP: servers DISAGREE, no majority`, and in `ntp` and the status), and the status LED flashes twice each second instead of once. The PIC sees one NTP source either way: this clock. But its bound says what is known: the truth is in one of the servers' intervals, not which, so while they disagree the uncertainty sent to the PIC (and shown as `+/-` in `ntp`) reaches the furthest edge of any usable server's interval from the clock. It is worked out each second from every usable server's line against the clock then, so a slew under way is in it. Two servers 20ms apart give some tens of ms, and the PIC ranks NTP accordingly. A bound of 4.29 s or more is not bounded at all as far as TIME-NTP goes: UNCERTAINTY_VALID is cleared (the PIC then does not use NTP) rather than a 4.29 s bound sent, and `ntp` shows `+/- over 4.29 s`.

One server under two numbers would be counted twice: combined at double weight, and two votes towards a majority, so a wrong server set twice could outvote a right one. A server polled at the same address as a lower-numbered one (another name for it, DHCP's offering one also set by name, a pool handing out one already listed) is set aside: not polled, selected or counted, shown with `=` and `, as server 1` in `ntp`, and taken up again when either's address changes. DHCP's server 0 is kept over the others. Both name the same path, so the kept server's `ntp server ... offset` is the one used; if the two differ, the console says so and `ntp` shows it on the set-aside row (`as server 0 (its offset +0 us used, not +120)`). The offset is not carried across, so which correction applies never depends on what DHCP happens to offer.

One host at two addresses (its IPv4 and IPv6, two names for one machine) is caught by its replies instead. Every reply carries the server's stratum, its reference ID, its root delay and its reference timestamp: the time of its own last clock update, to 2^-32 of a second. That timestamp changes at every update, so two replies can only carry the same one if they came from one host between two of its updates. The reference ID alone is not enough, as every GNSS stratum 1 says `GPS` or `PPS`. Each good poll is compared with every other server's latest, at the first and last reply of each burst. Servers are polled one after another, a few seconds apart, so a host that updates its clock every few minutes or less often is matched within a few polls. Two matches at different reference times, and the higher number is set aside as for a shared address (with three or more on one host, each is matched against the lowest, which is kept): `=` and `, same host as server 1` in `ntp`, its samples not used. It is checked every 6 hours by a single poll just after the kept server's, and polled again after three checks in a row that do not match, at once if either name is looked up again (the network came back, the weekly refresh) and gives another address, as a pool's does, if the kept server stops being reachable (the other address may be the path that works), or if the kept server's setting changes. `ntp -v` shows the checks. A server that sets its reference timestamp to whole seconds is never matched, as two such hosts could agree by chance, and nor is one that updates its clock between every two polls: those stay two servers, as before.

The limit runs from the least distance, not the followed server's: the followed one is kept until another's distance is under half of its, so it can be the furthest, and a limit from it let every server in. On a WAN test the followed server (rubidium, 11.3ms) let in one at 13.8ms whose path made it look 1.4ms ahead; from the least (7.6ms) it is out. chronyd always follows the least distance, so its limit from the selected server is the same thing.

`ntp asymmetry <us>` is added to every sample's offset (+ makes the clock later), and when it is changed every kept sample moves with it, so the lines carry on. It is for the ESP's own end of every path: its network stack and the Wi-Fi hop. Against GNSS with a PTP-disciplined server on the LAN, and against wired chrony hosts' views of the same Internet servers, the ESP's NTP was about 100µs behind on every server, LAN or Internet. On a clock with GNSS, `ntp gnss` measures it: set the negative of the long-run mean. It cannot correct a path's own asymmetry (an Internet server several hundred µs off), and the ESP never sets it itself: NTP stays a reference of its own, not trimmed by the clock's GNSS.

`ntp server <n> offset <us>` is chronyd's `offset` for one server (0 for DHCP's): added to that server's samples on top of `ntp asymmetry`, its kept samples moved with it the same way. It is for a path's own asymmetry, which differs from server to server, so servers whose paths are known can be brought into line with each other and combined. A new host for a server clears its offset, since the path is another. Both default to 0, and 0 is the right default: what they correct differs from clock to clock, and the ESP's own bias is not a reason to set one. On the test clock, the ESP's end put NTP about 120µs behind on a LAN server, while the Internet paths put it 700–800µs ahead, the way most home connections lean (the uplink is the slower, more queued direction, so the request takes longer than the reply). The ESP's bias took about 120µs off the Internet error, so `ntp asymmetry +120` would have made the LAN right and the Internet servers worse. A clock with a LAN server and GNSS to measure against can set the LAN server's offset (`ntp server 0 offset 120` for DHCP's) and leave the Internet servers at 0.

The servers are polled at startup, then after 64, 128 and 256 seconds while the crystal frequency is being learnt. After that the interval adapts between 128 seconds and `ntp.interval` (default 512) by chronyd's rule, on each sample from the server followed: if the sample is further from the server's line than half its round trip, the line is not tracking it and the score drops by log2 of the ratio; otherwise it moves by (samples in the line / 8 - 1) / 8, twice that while under 8. A score of 1 doubles the interval, one below 0 halves it. So the polls lengthen while the line grows and shorten while the runs test keeps it short. Every server is polled at that interval. A crystal that is warming up, or sitting next to a radiator, gets polled more often; a settled one leaves the servers in peace. Failed polls are retried after 60 seconds, doubling up to the interval. Lowering `ntp interval` brings polls already scheduled under the old one forward, by the ratio of the two so they keep their order, and none is left further off than the new interval.

A server may answer with a kiss-o'-death instead of the time (RFC 5905 7.4; the reply must still echo our request, so one forged from elsewhere is ignored). RATE doubles that server's own interval, up to a day, and it comes back down a step after every 8 good polls at the slower rate. DENY and RSTR stop polling that server: it is polled again when its setting changes, the network comes back, on `ntp sync`, or after a week. Other codes count as a failed poll. `ntp -v` counts each and shows a server slowed (`polled every 2048 s (RATE)`) or refused (`REFUSED (DENY), not polled`). A kiss does not count towards looking the name up again: it is the server answering, not the path failing.

512 seconds as the longest keeps the frequency estimate honest. A server's line holds at most 16 samples (chronyd keeps up to 64), about two hours at 512 seconds: long enough to measure the crystal to a fraction of a ppm through a few milliseconds of jitter, and short enough to follow it as the room warms and cools. Through a few hundred µs of Wi-Fi noise the runs test cannot see a slow wander, and a line many hours long lags it. The traffic is nothing much: each poll is four 48-byte requests, so four servers at 512 seconds come to one request every 32 seconds on average.

`ntp` shows the servers as `chronyc sources` does, and `ntp tracking` the server followed and the clock as `chronyc tracking`:

```
  # Server                   St   Poll Reach LastRx     Offset        +/-  Address
* 1 rubidium.darksky.io       1   512s   377     5m  +112.0 us   5.204 ms  2001:db8::123
+ 2 0.pool.ntp.org            2   512s   377     4m  -1.204 ms   9.877 ms  192.0.2.10
x 3 1.pool.ntp.org            2   512s   377     4m  +48.03 ms   8.102 ms  198.51.100.7
```

Offset is each server's latest sample against the clock as it then was, and +/- its root distance now. `ntp -v` adds each server's round trip, jitter (the spread about its line) and counters, the clock's frequency and slew, and each server's line, as `chronyc sourcestats` shows it:

```
  # Server                   Kept Runs   Span   Freq ppm   Skew ppm     Offset         SD
  1 rubidium.darksky.io        16    9  2h08m     +2.871      0.094  +12.00 us   41.00 us
```

Kept is the samples in the line, Runs the runs among their residuals, Span from the oldest to the latest, Freq the line's slope as the ESP's frequency error (+ = fast), Skew its bound, Offset the line now against the clock, SD the samples' spread about the line.

`ntp.interval` is the longest interval rather than a fixed one.

The servers' hostnames are resolved in `loop()`, not in the NTP task. In arduino-esp32 3.3.x, `Network.hostByName()` clears lwIP's DNS cache without taking the TCPIP core lock, which cancels any lookup in progress elsewhere and trips an lwIP assertion. With the PIC already running, the timezone lookup at startup overlapped the NTP lookup, and the ESP restarted every few seconds until power cycled. Keeping every DNS lookup in `loop()` means none is ever in progress when the cache is cleared. Any future network code should follow the same rule.

## Clock model

The ESP keeps UTC as a model against its own microsecond timer: an anchor point, a frequency estimate and a slew in progress. The first NTP sample sets the clock, and a sample more than 128ms from the model steps it. Each sample is judged against the model at the moment it was taken, but the correction starts from the moment it is applied, so the clock never jumps by what the old slew would have done in the meantime.

Corrections up to 8ms are slewed in gently: spread over about two poll intervals, and never faster than 5ppm. The next sample replaces whatever is left, so each sample's noise is only half applied, and the ESP PPS moves by a few microseconds a second at most. That is slow enough for the PIC's NTP loop to follow without re-stepping, which is the point. Anything larger is slewed in at 100ppm and flagged in TIME-NTP (`CORRECTING`) while it lasts, since the PIC cannot follow it anyway and might as well know.

Each update is steered by the servers' combined estimate (above): the correction is the combined offset now against the model, and the frequency the combined frequency. Until a server has a line, the clock falls back on its own: the followed server's latest sample, and a least-squares line through the raw offsets of its last eight samples, leaving out any whose round trip is well above the best of them (queueing adds delay in one direction, so the slow exchanges carry the most asymmetry). Two samples two minutes apart give a first estimate. `ntp tracking` shows the latest sample against the model and the correction made (`Last:      sample +85.00 us, correction +12.00 us`).

The model also reports an uncertainty bound, the tightest any of the frequency window's samples gives: how far the model, run back to that sample's time and on by what is being slewed in, stands from the sample, plus half its round trip, the server's own root delay and dispersion, and a drift allowance for the time since. The drift allowance is 50 ppm until the frequency is known and 20 ppm after one estimate. After that it is the servers' combined skew (or, before there is a line, three times the clock's own fit's standard error), plus 0.5 ppm for the crystal's wander with temperature, never more than 20 ppm (the crystal's own tolerance) while the skew's line or the fit has fewer than six samples, and 2 ppm for a fit of six or more. A line of a few samples has a skew of hundreds of ppm, and a frequency from two or three samples a minute apart can be 15 ppm out if one exchange met a Wi-Fi burst, so for the first ten minutes or so the bound grows faster between polls (about ±2–3ms on a LAN rather than ±1.5ms) and comes in as the lines fill. TIME-NTP's lock state is ACQUIRING until the server followed has a full line (six samples), then LOCKED. When the statistics of the server followed start again (another address, or a jump), the window does too, as when another server is followed. `ntp tracking` shows both (`Frequency: +0.870 ppm, skew 0.094 (...); drift allowed 0.59 ppm`). Half the round trip is most of the bound: without knowing a path's asymmetry, the truth can be anywhere in it.

In simulation (`tests/ntp`, `test_esp_clock`: a day each, every second, of a LAN or an internet server, the crystal wandering and stepping, the path lopsided at random or always fully, Wi-Fi holding a tenth of the exchanges up by up to 10ms one way) the bound always holds. Steered by the line rather than each sample, the true error's RMS where Wi-Fi holds exchanges up falls from 230–480µs to 50–350µs and the worst from 1.4–2.6ms to 0.2–1.0ms. Through steady noise with the crystal wandering slowly (0.2 ppm over six hours) the old way did better (RMS 156 against 193µs at 512 s): a line lags a curve the runs test cannot see. `ntp gnss` on a clock with GNSS is the real measure.

On a clock with GNSS, the ESP measures its NTP time against the PIC's (`src/timing/ntp_gnss.cpp`): every second the PIC is locked on GNSS within 1 µs, the NTP clock is read at the PIC's epoch edge and compared with the PIC's UTC for it. `ntp gnss` shows the latest difference (+ = NTP ahead), and the mean, standard deviation, RMS and extremes over the last hour and since the ESP started; `ntp -v` includes `ntp gnss`. `ntp gnss -v` adds the last 24 hours minute by minute (since the start, if less), each minute's mean in µs, ten to a line led by the first one's UTC time, to show the wander's shape: a sawtooth with each update, or a slow drift. Under each line, the servers steering the clock in each minute (followed and combined, `34` for servers 3 and 4, `*` if they changed within the minute, `0` for none), shown where they change and at the line's first minute, blank where they are the same:

```
  10:30    314    343    376    412    456    512    595    782    968   1025
             3                                  34*     34
``` `ntp tracking` includes the summary, and `ntp gnss reset` starts it again (to compare one set of servers with another, say). The GPIO27 capture adds a few µs of interrupt latency. Its interrupt (and GPIO18's) is at level 3, above the PPS output's timer at level 1, so it preempts the output's work: at the same level, until 2026-11-15, an epoch edge arriving in the ~28µs of the output's rising-edge work waited for its end, and while NTP was 0 to 28µs ahead every second read +28µs (runs of exactly 28 in `ntp gnss -v`).

In a 12-hour simulation with a crystal drifting from 4.9 to 2.9ppm, 12ms round trips, a few milliseconds of queueing and 3.5ms of path asymmetry, the ESP PPS wanders by 340–700µs (sd) around its asymmetry offset, against 550–1000µs for the previous model, and never moves faster than 7µs a second, against 100µs. The asymmetry offset itself, about 1.3ms here, is invisible to NTP and to any client of it.

## Temperature compensation

The ESP's crystal moves with temperature, by about -160 to -200 ppb for every degree on two V2 boards, against the board temperature the PIC's BME280 reports. NTP only learns the crystal's frequency through the servers' lines, an hour or two of samples, so when the room warms or cools the clock runs on the frequency of an hour ago: 2.5 C ahead of it is about 450 ppb, up to 230 us by the next poll at 512 seconds.

The ESP corrects for it as it goes ([src/timing/temp_comp.h](src/timing/temp_comp.h)). The board temperature (STAT-ENV) is taken as it arrives, and the crystal is taken to run that many ppb per degree from the temperature first seen since the ESP started. Only the changes matter: what the crystal does at that temperature is the lines' to find, as it always was. NTP samples have the correction taken off before they reach the lines and the clock, and the clock's time has it added back, so between polls the clock follows the temperature at once and the lines see a near enough steady crystal. In a simulated day, 8 C swings over two hours, the error fell from 536 us RMS to 85 us at 512 s polling, and with the coefficient a third out, still to 231 us.

The correction never jumps. Compensation turned off, the coefficient changed, or the temperature lost, and only its rate changes from then on. A reading not renewed for 15 seconds is held (`esp tcomp` shows `board 31.20 C (held)`): the correction and learning carry on at it for up to 5 minutes, so a PIC restart (30–45 seconds without STAT-ENV, as after an update) loses nothing. After 5 minutes without one (no BME280, or the PIC silent) the temperature is lost: no rate, and the learning window and the movement history start again when it returns.

### The coefficient, learnt

Each clock learns its own coefficient, and until it has one uses the default, -180 ppb/C. The followed server's samples, before the correction comes off, over the last 8 hours, are fitted to an offset, a frequency, a drift of that frequency and the temperature's exposure (the integral of the temperature from the reference). The drift term keeps a crystal slowly changing from being taken for temperature while the room only warms (or only cools). An estimate counts once the window spans 2 C or more with a rise and a fall of 1 C each, its standard error is within 20 ppb/C, and 4 hours have passed since the last one counted, so estimates are not the same data again. The standard error is widened for how much each poll's error follows the last's (the crystal's own wander), as without that it claimed a few ppb/C while the fits moved by a hundred. The learnt coefficient moves from the default, or from what was learnt, towards each estimate by the difference over the number of estimates (up to 8), never more than 30 ppb/C at a time. It is kept in NVS by itself (the first at once, then at most hourly), so it survives a restart and `esp defaults`. Samples with a long round trip are left out, and a change of server, or the temperature lost, starts the window again. (Field data, 2026-11-21: without these, a morning's one-way warming taught the clocks -40 and -111, and an afternoon carried one to -292, before a night of heating cycles brought them back near -150 and -180.)

The polls are shortened while it is worth it: to 128 s for the first 6 hours while nothing is learnt or set, and whenever the board temperature moves (0.5 C within 30 minutes, a window opened or the heating on) until it has moved less than 0.3 C in 30 minutes for 30 minutes. While it moves, polls are 128 s until 3 estimates are learnt (or one is set by hand), then 256 s: the compensation follows the temperature, and the polls only what it leaves. They come in at once (`NTP: short polls: temperature moving, 256 s`), and when they end the interval lengthens again by the usual rule (`NTP: short polls end`). `ntp tracking` shows why (`Interval:  256 s (adaptive, at most 512 s, short: temperature moving, 256 s)`).

`esp tcomp` shows it all:

```
esp tcomp: ON, -187 ppb/C (learnt); ACTIVE, board 32.50 C, reference 30.00 C, crystal -467 ppb, correction +1203 us
esp tcomp learning: 4 estimates, learnt -186.6 ppb/C, last -192.3 +/- 7.4 ppb/C over 4.20 C (1.80 C each way), r 0.83, 3 min ago, next in 95 min, window 180 samples over 4.20 C, moved 0.62 C in 30 min, short polls (temperature moving, 256 s)
```

The source is `default`, `learnt` or `set`. `esp tcomp off` and `on` turn it off and on, `esp tcomp -120` sets the coefficient by hand (+ = the crystal runs fast when warmer) over anything learnt, `esp tcomp learn` clears that setting again, and `esp tcomp reset` forgets what was learnt and starts learning afresh, on. The settings are saved with `esp save`, under their own key.

## ESP PPS

The ESP PPS output on GPIO5 is a 100ms pulse whose rising edge marks the ESP's UTC second. It is generated by a 1MHz hardware timer alarm. At each falling edge the interrupt arms the next rising edge, at the time `loop()` planned from the clock model, or one second on (corrected for frequency) if `loop()` has been busy. A long HTTPS request therefore delays corrections, but does not cost an edge.

The interrupt reads the time immediately after setting the pin, so the edge latency is measured rather than assumed. On the V2 board it is around 5–7µs. The rising edge's alarm is set that much early (the average latency over about 16 edges, shown in `ntp -v` as `armed n us early`), so the pin rises on the second. The PIC corrects for the edge's phase anyway (TIME-NTP's `utc_phase_ns`); this puts the pin itself right for anything else measuring it.

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
Freq:     -3.7500 ppm (TRACK), loop -3.7412 ppm
Sync:     LOCKED on GNSS, synchronised
  Source    Verdict            Bound       Phase  Last good
* GNSS      good              61 ns      +25 ns       0s
+ HOLDOVER  good              86 ns           -      42s
  NTP       good           13.70 ms   -571.0 us       0s
GNSS:     3D OK, tAcc 12 ns
Env:      board 34.41 C (ambient ~21.61 C), 1012.8 hPa, 48.21 %RH
```

`Freq` is the PIC's Kalman estimate of its oscillator, and while tracking, `loop` is that less the PI integral: what the oscillator is taken to run at. On GNSS the Kalman filter learns the frequency and the integral stays small. On NTP the Kalman estimate is frozen (an NTP epoch is too noisy to learn a frequency from) and the integral does the following, so `loop` is the figure to watch there. `Env` is the PIC's BME280 (STAT-ENV): the board's temperature, which the power supplies warm, the room roughly guessed from it, pressure and humidity.

# Updates

The updater ([src/net/updater.cpp](src/net/updater.cpp)) reads the manifest at `https://<update server><update path>`, by default `/smol_clock/v2plus/manifest.json`. A clock with the old default path (`/smol_clock/v2plus/update.json`) saved is moved to the new one when its settings load.

## Manifest

```json
{
  "format": 2,
  "issued": "2026-11-23T13:51:07Z",
  "configurations": [
    {
      "hardware": "v2",
      "config": "development",
      "devices": ["A0:B1:C2:D3:E4:F5"],
      "issued": "2026-11-23T13:51:07Z",
      "esp": {"version": "0.4.0", "url": "https://…/esp_src.bin", "size": 1252320, "sha256": "…"},
      "pic": {"version": "0.4.0", "url": "https://…/dspic_src.smolpic", "size": 177280, "sha256": "…"},
      "pic_spl": {"version": "0.1.0", "url": "https://…/dspic_spl.smolspl", "size": 9088, "sha256": "…"}
    }
  ]
}
```

Every configuration is checked before any is used, and a manifest with a malformed one is refused whole. The clock takes the first configuration whose `hardware` is `v2`, whose `config` matches `update config` (absent means the default, none), and whose `devices`, if given, include its MAC address (any case). Each component is optional, but a configuration needs at least one. Versions are dotted numbers, compared numerically (`1.10` is newer than `1.9`). A download must match the manifest's size and SHA-256, or it is discarded. `issued` (UTC) records when: the manifest's own when it was last written, each configuration's when it was last built. The clock does not read them.

[tools/make_manifest.py](../tools/make_manifest.py) writes the manifest for a set of files, reading each version from the file itself (the PIC's headers, the ESP's user agent string), and adds to or replaces a configuration in an existing one with `--merge`. The `issued` times are the time it runs, or `SOURCE_DATE_EPOCH`'s, or `--issued`:

```
make_manifest.py --base-url https://rubidium.darksky.io/smol_clock/v2plus/dev/ --esp esp_src.ino.bin --pic app.smolpic --pic-spl spl.smolspl -o manifest.json
```

[tools/make_dist.py](../tools/make_dist.py) does the whole release from the latest builds: the released IPL hex, the SPL and application files, the first-installation hex and the ESP's image, each named after the version read from it, into `dist/<folder>/`, and their configuration into `dist/manifest.json`. A version header edited after its build is reported. With no arguments it uses the MPLAB X production builds and the Arduino build folder, and writes the default configuration (no `update config`) with its files in `prod/`. `--config dev` writes the configuration an ESP set to `update config dev` takes, with its files in `dev/`. The manifest is updated, not replaced: only that configuration changes. Upload `dist/` as it is to `https://rubidium.darksky.io/smol_clock/v2plus/`:

```
python tools/make_dist.py                # prod/, the default configuration
python tools/make_dist.py --config dev   # dev/, for update config dev
python tools/make_dist.py --config dev --device EC:64:C9:06:D3:50   # dev/EC64C906D350/, that clock alone
```

Each component is compared with what the manifest offered that configuration before. A different build under the same version is warned of (`warning: ESP 0.3.69: another build under the same version: clocks at 0.3.69 keep theirs (update esp force installs it), or bump the version`), as `update install` takes only newer versions. `--check` writes nothing: it compares the builds with what the manifest offers (`--manifest` a file, by default `dist/manifest.json`, or a URL, such as the live one) and says, component by component, whether it offers them, exiting 1 if not. Run it before updating the clocks to catch a build that never reached the manifest, or a manifest not uploaded:

```
python tools/make_dist.py --check --manifest https://rubidium.darksky.io/smol_clock/v2plus/manifest.json
```

`--device` (repeatable) limits a configuration to those clocks, by the Wi-Fi MAC address `wifi -v` prints. It is a configuration of its own, put ahead of the ones without devices so those clocks take it first, but only while their `update config` matches too. Its files go in a folder named after the addresses, inside the config's, so the configuration everyone else takes keeps its own.

[tools/release.py](../tools/release.py) does all of it, and the rest, in one command on Windows or Linux: refuses a working tree with uncommitted changes, runs every host test ([tools/run_tests.py](../tools/run_tests.py)), builds the ESP with arduino-cli ([tools/build_esp.py](../tools/build_esp.py), below) and the IPL, SPL and application with XC16 alone ([tools/build_pic.py](../tools/build_pic.py), below) or, with `"pic_build": "mplabx"`, with MPLAB X's own Makefiles (headless, production, as the IDE builds them), downloads the live manifest so a new configuration is merged into what the clocks see now (not into an older copy left on this machine), runs make_dist.py, uploads with [tools/upload_dist.py](../tools/upload_dist.py) (below), downloads every file back to check it, and with `--tag v1.2.3` makes a GitHub release of the commit with the files attached (the GitHub CLI, `gh`). The upload puts the files first and the manifest last, beside the old one and renamed over it, so no clock ever reads a manifest that names a file not there yet, or half of one. It builds and releases only what is new: each component (the ESP, the PIC application, the SPL) whose version.h differs from the live manifest's entry for the configuration. The others keep their live entries and files, which are downloaded back and checked against the manifest's SHA-256 rather than rebuilt (a rebuild would change the build stamp, and every clock would take it as an update). With nothing new it says so and stops. `--allow-dirty` builds and releases everything, whatever the version headers say, and with uncommitted changes. It takes `--config` and `--device` as make_dist.py does, and `--dry-run` prints what it would do. The machine's own settings (the SSH target and key, and where arduino-cli, XC16 and the device family pack are, if not in their usual places) go in `tools/release.local.json`, which git ignores. [tools/release.sample.json](../tools/release.sample.json) is a sample to copy, with each setting explained.

[tools/build_esp.py](../tools/build_esp.py) builds the ESP with arduino-cli: `python3 tools/build_esp.py`. The board, flash size, partition scheme and libraries are set in it, and the images are exported to `build/esp32.esp32.esp32/`, where make_dist.py looks. The build is verbose, every command shown, as the PIC builds are. arduino-cli comes from `--cli`, or `arduino_cli` in the release settings, or the PATH. `--config-file` (or `arduino_config`) gives arduino-cli a configuration of its own, so the cores and toolchains for this firmware can be kept apart from the machine's others. The esp32 core is checked first: `--core` (or `esp32_core`) names the exact version wanted; without it, the version the firmware is built with is wanted, another 3.3.x gives a warning and any other series stops the build. `--print` prints the command.

[tools/build_pic.py](../tools/build_pic.py) builds a PIC project with XC16 and the device family pack alone, no MPLAB X and no make: `python3 tools/build_pic.py dspic_src dspic_spl dspic_ipl`. It reads the project's `nbproject/configurations.xml` (the device, the sources in the project tree's order, which is the link order, the linker script, and the options) and runs the commands MPLAB X 6.20's generated Makefiles run for the production image, the outputs where MPLAB puts them. Each option is turned into the flags MPLAB writes for it; an option set to something its table does not know stops the build rather than build something MPLAB would not. For all three projects the commands are checked against MPLAB's own builds ([tools/tests/test_build_pic.py](../tools/tests/test_build_pic.py)); built on Linux, the SPL and IPL hex files are the same as MPLAB's on Windows, and the application's the same apart from the order of two pairs of independent instructions in two functions (XC16's Linux and Windows builds order them differently). Windows' bin2hex also ends its lines with CR LF, which make_image.py does not mind. XC16 and the pack: `--xc16` and `--dfp`, or `xc16` and `dfp` in the release settings, or the usual places; `--print` prints the commands.

[tools/upload_dist.py](../tools/upload_dist.py) is the upload on its own, for a `dist/` made by hand with make_dist.py. It sends the files the manifest names (or the files given, or `--all` of `dist/`), then the manifest last, beside the old one and renamed over it, so a failure part way leaves the clocks on the old manifest. Files the server already has are skipped: the live manifest is read first (over SFTP), and a file it offers at the same path with the same SHA-256 is read back over SFTP and not sent again if the server's copy matches, so an ordinary release sends the new builds and the manifest. One the manifest offers but the server no longer has, or has a different copy of (cut short by an upload that failed), is sent again. A file it does not offer (an install hex given by name) always goes, and when nothing has changed, the manifest included, nothing is sent. `--force` (`release.py --force-upload`) sends everything. It uses SFTP only, never a shell or scp, so an account chrooted to `internal-sftp` will do; the folder is then the path inside the chroot (`smol@rubidium.darksky.io:/v2plus`). OpenSSH's `sftp` replaces the old manifest in a single step (OpenSSH's posix-rename extension). WinSCP does not use that extension, so the server refuses its rename over an existing file: then the old manifest is renamed aside to a name of its own (`manifest.json.old-YYYYMMDDHHMMSS`), the new one into place, and the old one removed, leaving a moment without a manifest but never a wrong one; if the second rename fails, the old one is put back. For the single step on Windows, use `"upload_method": "sftp"` with an OpenSSH key (Windows 10 and 11 have OpenSSH's `sftp`). Public-key authentication only: no password is ever asked for. On Windows it uses WinSCP's console (`WinSCP.com`) with a PuTTY `.ppk` key or Pageant, and checks the server against `hostkey` (`winscp_hostkey` also read; WinSCP does not read `known_hosts`: the `SHA256:...` that `ssh HOST` shows on a first connection, or WinSCP's form from Session > Generate Session URL/Code). Elsewhere, or on Windows without WinSCP, it uses OpenSSH's `sftp` in batch mode with an OpenSSH key or the agent. With `hostkey` set, a first connection that does nothing else learns the server's key into a `known_hosts` of its own, it is checked against `hostkey`, and only that key is trusted for the upload (`ssh-keyscan` is not used: the Windows build fails on OpenSSH 8.9's key exchange offer); without it, the server must already be in `known_hosts`, as batch mode never prompts. It reads the same `tools/release.local.json`:

```
{
  "upload": "user@rubidium.darksky.io:/var/www/smol_clock/v2plus",
  "ssh_port": 22,
  "ssh_key": "C:/Users/me/.ssh/rubidium.ppk",
  "upload_method": "auto",
  "hostkey": "SHA256:..."
}
```

```
python tools/make_dist.py
python tools/upload_dist.py --dry-run      # what would go, and how (reads the live manifest)
python tools/upload_dist.py
```

```
python tools/release.py --tag v1.0.0                                # tests, builds, prod/, upload, GitHub
python tools/release.py --config dev --device EC:64:C9:06:D3:50   # one clock's dev build, no GitHub release
```

The host tests need make, gcc and g++: on Windows from MSYS2 or WSL. Where they are not, `--skip-tests` leaves them to CI ([.github/workflows/ci.yml](../.github/workflows/ci.yml)), which runs every host test and builds the ESP on every push. The PIC's builds are not in CI: they need MPLAB X and XC16.

[tools/clock_monitor.py](../tools/clock_monitor.py) samples clocks over telnet, for long runs: every minute (`--interval`) it connects, reads the status (an empty line, resent if nothing comes), `ntp tracking`, `ntp gnss`, `osc` and `esp`, disconnects, and appends a CSV row: the PIC's frequency and lock, the oscillator measured and stored, the ESP's frequency, skew, bound and last sample, NTP against GNSS, the longest loop pass, and the BME280's board temperature, pressure and humidity. A file begun by an older version keeps its columns (new ones are left out, with a warning): start a new file for them. `--raw` keeps everything received as well, and `--temp-file` adds a temperature from the monitoring host. It needs no login (every command it uses is open), and leaves telnet free between samples. Python 3, standard library only.

```
python tools/clock_monitor.py 192.168.3.146 192.168.3.147 --out clocks.csv --raw clocks.log
```

## Order

Newer only, unless forced. The PIC goes first while the ESP is talking to it: the link is proven, so the ESP running now can update the PIC whatever a new ESP turns out to do. The ESP's update follows once the PIC's have ended, whatever their result, since the ESP can always update itself, link or not. The ESP goes first only when the PIC's updates cannot start: the PIC not heard from, or its application speaking an SML major version the running ESP does not (it checks before installing). The new ESP then installs the PIC's after its restart, from the files staged before it. `update install` and the daily check do all of this in one go.

A PIC update already retries a lost or damaged frame, and makes up to three passes. Should the PIC's updates still fail, they are tried once more, from the same staged files, as soon as the PIC answers again (its application, or its SPL), or after a minute whatever: a PIC that reset or stalled part-way is given a second go before the ESP's update. Not when trying again cannot help: the file is unreadable, made for another device, too large, needs a newer IPL, or the new SPL was rejected. Whatever the outcome, the ESP's update follows, and a PIC update that still failed is tried again at the next `update install` or daily check.

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

The application the manifest offers is staged ahead. When no slot holds it, `update check`, `update install`, `update pic` (when up to date) and the daily check download it into a slot, keeping the slot with the version the PIC runs (`PIC: staging 0.3.1, the application it runs, into slot 0`). So a clock that has been online once holds a copy of its application, and an update offered is installed without waiting for the download. The SPL offered is staged the same way, into the SPL slots.

Without the update server (no Wi-Fi, or the server down), `update pic force` installs a staged file: the version the PIC runs, else the newest. `update pic` does the same unasked when the PIC is in its SPL without a valid application. And the ESP does it by itself: a PIC that stays in its SPL without a valid application for 30 s (an update cut short, its power pulled mid-update) is given the staged file it last ran, else the newest, up to three times per ESP start.

## Installing on the PIC

The exchange with the PIC's SPL ([src/pic/pic_update.c](src/pic/pic_update.c)): ENTER_BOOTLOADER to the application, then BOOT-STATUS from the SPL, HOLD, ERASE, the image in 192-byte BOOT-DATA chunks (blank ones skipped), each checked against the SPL's CRC, a read back of the whole image with BOOT-VERIFY, COMMIT, then RUN and STAT-SYS from the new application. A chunk or command that goes unanswered is sent again, and an error that leaves the PIC's state unclear starts a new pass from STATUS, up to three. A PIC that already runs the version and CRC offered is told to RUN, unless forced. While the PIC is in its SPL (held, without an application, or stopped by its boot-loop guard), the ESP sends it nothing but bootloader messages, and console commands for the application say so. The SPL's 1 s boot window after an ordinary reset does not count: the SPL hands over by itself, and the ESP's usual traffic brings the new application's link into SML mode straight away.

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

## Installing the PIC's SPL

The SPL is written by the running application, not by itself ([src/pic/spl_update.c](src/pic/spl_update.c), the PIC's side in `dspic_src/esp/spl_writer.c`). `update spl` stages the file like the application's, then:

- STATUS to the application: the bootloader protocol, the DEVID, the IPL new enough for the file (`min_ipl`), the test page free. The same SPL there already, intact and confirmed, ends it at once, as does a newer one, unless forced.
- Until the application has run 60 s, STATUS every 5 s.
- The running SPL is read back with BOOT-VERIFY into the other SPL slot, unless a slot holds it already: the one to go back to.
- ERASE, DATA, VERIFY and COMMIT of region SPL, as for an application. The application does the flash work in its flash window, one page erase an epoch, so each answer can take up to a second. While a request is out, the ESP sends the PIC nothing else.
- ENTER_BOOTLOADER: the IPL starts the new SPL, unconfirmed. Its BOOT-STATUS must come within 25 s.
- Through the new SPL, a test: ERASE, four chunks of DATA, VERIFY and COMMIT of the test page, then ERASE again. Then RUN.
- Once the application has run 10 s again (`APP_CONFIRM_S`), CONFIRM. The IPL trusts the SPL from then on. An older application refuses it (STATE) until 60 s: the ESP asks again then.

```
SPL update: to 0.2.0, 1800 instructions
SPL update: keeping the running SPL
SPL update: erasing
SPL update: writing 29 chunks
…
SPL update: committing
SPL update: starting the new SPL
SPL update: testing the new SPL
SPL update: back to the application
SPL update: confirming once the application has run 10 s
SPL update: DONE, 0.2.0 confirmed, <seconds> s
```

A failure before COMMIT starts a new pass from STATUS, up to three. Meanwhile the IPL starts the application directly, as there is no intact SPL. A new SPL that never comes up (the IPL gives up on it after its trials and starts the application, which rejects it) or fails its test ends `REJECTED`, and the ESP installs the SPL it kept, forced, by itself. If that one is rejected too, or none was kept, the clock carries on with the application started directly by the IPL until an SPL is installed.

`update install` and the daily check first do what `update check` does: what is offered is said, and the PIC's application and SPL offered are staged. Then everything due, in one go. An ESP update restarts the ESP at once, with a record in NVS (namespace `updater`) of the PIC's files staged for it. After the restart, as soon as the PIC's application answers, the ESP installs those from picfw, without the network. Should they have gone from picfw, it fetches the manifest again once the network is up, every 30 s up to five times. The SPL comes first when it is offered and not known to be current (the update's first STATUS ends it at once if it is), then the application as soon as the SPL's update is DONE. What follows the command is reported on every console. While any of this is under way, `update install` says so and does nothing. Without the update server, `update spl force` installs the newest staged SPL.

## The SPL's state, and its repair

The ESP asks the application for BOOT-STATUS 5 s after the PIC is first heard and after each restart of it, then hourly. `pic boot` shows the answer (`SPL: confirmed (1.0.0, asked 12 s ago)`). An SPL that is missing, rejected, or committed but never confirmed (an SPL update or its confirmation cut short, by a power cut say) is installed again from picfw, forced, by itself: an unconfirmed one from its own file, else the newest staged SPL not rejected since the ESP started. At most three times per ESP start. `update pic` does the same first, then the application, so it is not refused for want of an SPL.

On the PIC's side, an application started by an unconfirmed SPL that runs healthily for 30 s zeroes the SPL's trials, so ordinary resets before the confirmation (`pic reset`, a power cut) cannot spend them and have a working SPL rejected.

`pic boot` prints what the SPL reports (its mode, versions, whether the application is valid) when the PIC is in it, or else the application's version, then how the last update went. `pic boot run` and `pic boot hold` send RUN and HOLD to the SPL. After RUN, or when the SPL has sent no BOOT-STATUS for 5 s with no update running, the ESP stops treating the PIC as in its SPL and its link's traffic resumes, which brings the new application's link into SML mode and its STAT-SYS. `update` lists the staged files (`-v` also the SPL slots), and `update erase pic|spl|all` empties their slots (the sector holding each header is erased).

While a download holds up the main loop, the PIC link is still drained between reads (`pic_link_poll()`), so its 16 KB receive buffer does not overflow. A command from the PIC that arrives then is answered BUSY. Bytes are still lost while the ESP erases or writes its flash (the UART's 128-byte FIFO overruns with its interrupt held off, at 921600 baud in under 1.5 ms), so broken frames are expected then. The PIC's raw console text is printed a whole line at a time, and a line holding any byte no console text has (a broken frame's header or payload), or still unfinished when a whole frame arrives, is dropped and counted (`pic -v`, `Text:`). A line that pauses part-way, as the PIC's start-up banner does, is printed whole when it ends. The PIC's terminal controls (the clear screen at its start) are left out.

# Telnet

Telnet gets the same commands as the USB-UART console, but on each connection only those that look until `telnet login <password>`: anything that changes something answers `telnet: log in first (telnet login <password>)` and does nothing. The rule is kept in one place, `needs_login()` in `src/console/commands.cpp`: open are the status commands, a setting shown without a value, `pic stored`, `pic boot` (not `run` or `hold`), `ntp tracking`, `ntp sync`, `wifi scan`, `update check`, `debug`, `verbose` and `help`; everything else, and any command added later, is protected. `help` marks protected commands with `*`.

The password (`src/net/telnet_auth.cpp`) is kept as a SHA-256 of a random salt and the password, in NVS (namespace `telnet`); until `telnet passwd` sets one, `TELNET_PASSWORD_DEFAULT` stands, and `telnet` shows `DEFAULT PASSWORD`. Five wrong passwords in a row, then each further one locks logins for 30 s, shared by every connection: other commands run meanwhile, and a good login or a new password (from USB-UART, say) clears it. The login lasts until the connection closes.

Telnet gets a copy of all console output, the PIC's included. That copy never blocks: whatever the connection cannot take at once is dropped, and a note saying how much appears in the stream when there is room again. ESPTelnet's own `flush()` is never used: on the ESP32 it waits for `availableForWrite()` to settle above zero, which NetworkClient never reports, so it ran to its 1 s timeout on every connect and disconnect, holding up `loop()` that long (`src/net/telnet.cpp` overrides it to do nothing). A plain write to a stalled connection waits up to 10 seconds in the network stack, holding up `loop()` (and the PIC link, and TIME-NTP) meanwhile, and after three such failures the Telnet library used to drop the client. Replies to commands typed at Telnet do not block either: what the connection cannot take at once is queued (up to 48 KB, from the heap, freed once sent) and sent as it takes it, and the next command is read once the reply is out. Written straight to the connection, `ntp gnss -v` (about 25 KB) held up `loop()` for seconds: the ESP's PPS ran on by itself, but the TIME-NTP reports and the status LED stopped. The serial console still writes its replies normally.

The status shows the ESP's uptime and why it last started. A restart through the EN pin, which is what a USB-UART's DTR and RTS lines do when a computer opens or closes the port, reports as "power-on or EN pin", so an ESP that restarted while the PIC kept counting epochs was most likely reset that way.

# Settings

Settings are stored in NVS. NTP servers 2 to 4 are kept under a key of their own. The settings carry a header, `SMOL` (`0x534D4F4C`), and anything saved under another, including by V2 and by V2+ up to 0.3.0 (`0xC0FFEEEE`), is ignored: the ESP starts from its defaults and saves them. Wi-Fi credentials are kept by WiFiManager apart from these, so a clean start of the settings does not lose the network. Each field is checked on load. An invalid field reverts to its default and the rest are kept, rather than one bad value resetting everything. Changes made from the console take effect immediately and persist after `save` (or `esp save`). `defaults` (or `esp defaults`) puts the defaults in force without saving them.

The full command list is in the repository [README](../README.md#console-commands), and `help` prints it on the console, `help <command>` the settings and actions of one. The table of commands is in [src/console/commands.cpp](src/console/commands.cpp): each command lists its settings and actions with the kind of value each takes, and the parsing, the showing of a setting given no value, the error messages and the help all come from that table. `wifi` never prints the saved WiFi passphrase.
