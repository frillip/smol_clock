#!/usr/bin/env python3
"""Sample a Smol Clock V2+ over its telnet console, one CSV row a sample.

  clock_monitor.py HOST [HOST ...] [--port 23] [--interval 60] [--out FILE]
                   [--raw FILE] [--temp-file PATH] [--once]

For each host, every --interval seconds: connect, ask for the status (an
empty line, sent again if nothing comes: a fresh connection sometimes takes
a few), then `ntp tracking`, `ntp gnss`, `osc` and `esp`, then quit. The
connection is not kept: the ESP's telnet takes one client, and this leaves
it free between samples. Every command used is one that needs no login, so
no password is sent or kept. `pic -v` is not used: it holds the PIC's main
loop up for some 450 ms.

Each sample appends a row to --out (default clock_monitor.csv): the host's
UTC, the clock, and the fields below; a field not found is left empty, so
an older or newer firmware still gives what it can. --raw also appends
everything received, under a header per sample, so anything not parsed now
can be read out later. The clock's own BME280 gives its board temperature
(with pressure and humidity) from the status, from PIC 0.3.41 and ESP
0.3.65. --temp-file reads a temperature from the monitoring host
(millidegrees, as /sys/class/thermal/*/temp, or degrees) too, for a host
beside the clock.

Durations are in nanoseconds, frequencies in ppm. Python 3.8+, standard
library only (a plain socket: telnetlib went in 3.13).
"""
import argparse
import csv
import datetime
import os
import re
import socket
import sys
import time

FIELDS = [
    "utc", "host",
    # The PIC, from the status (rendered by the ESP from the PIC's messages)
    "pic_version", "pic_utc_bound_ns", "pic_source", "pic_lock", "pic_freq_ppm", "pic_freq_loop_ppm", "pic_loop",
    "pic_sync", "gnss_fix", "gnss_tacc_ns",
    # The PIC's BME280 (from the status): the board, the room guessed from it
    "board_temp_c", "ambient_temp_c", "pressure_hpa", "humidity_pct",
    # The PIC's oscillator (osc, printed by the PIC)
    "osc_measured_hz", "osc_measured_ppm", "osc_stored_hz", "osc_stored_ppm", "osc_stored_over",
    # The ESP's NTP (ntp tracking)
    "esp_version", "ntp_server", "ntp_stratum", "ntp_bound_ns", "ntp_synced",
    "ntp_last_sample_ns", "ntp_correction_ns", "ntp_round_trip_ns",
    "esp_freq_ppm", "esp_skew_ppm", "esp_estimates", "esp_drift_allowed_ppm", "ntp_interval_s",
    # NTP against GNSS (ntp gnss), on GNSS clocks
    "ntp_gnss_latest_ns", "ntp_gnss_hour_mean_ns", "ntp_gnss_hour_sd_ns", "ntp_gnss_hour_rms_ns",
    # The ESP's loop (esp)
    "loop_longest_ns", "loop_longest_by", "loop_slow_passes",
    # The monitoring host
    "host_temp_c",
]

UNITS_NS = {"ns": 1, "us": 1000, "ms": 1000000, "s": 1000000000}
NUMBER = r"[-+]?\d+(?:\.\d+)?"
DURATION = r"([-+]?\d+(?:\.\d+)?) (ns|us|ms|s)\b"


def duration_ns(value, unit):
    return round(float(value) * UNITS_NS[unit])


def find(pattern, text, flags=re.M):
    return re.search(pattern, text, flags)


# ---- Parsing ---------------------------------------------------------------------------

def parse_status(text, row):
    """The brief status: an empty line (ESP: ... NTP: ... PIC: ... UTC: ...)."""
    match = find(r"^ESP:\s+(\S+)", text)
    if match:
        row["esp_version"] = match.group(1)
    match = find(r"^PIC:\s+([^,\s]+)", text)
    if match:
        row["pic_version"] = match.group(1)
    match = find(r"^UTC:\s+\S+ \+/-" + DURATION + r", (\w+) (\w+)", text)
    if match:
        row["pic_utc_bound_ns"] = duration_ns(match.group(1), match.group(2))
        row["pic_source"] = match.group(3)
        row["pic_lock"] = match.group(4)
    match = find(r"^Freq:\s+(" + NUMBER + r") ppm \((\w+)\)", text)
    if match:
        row["pic_freq_ppm"] = match.group(1)
        row["pic_loop"] = match.group(2)
    match = find(r"^Freq:.*, loop (" + NUMBER + r") ppm", text)
    if match:
        row["pic_freq_loop_ppm"] = match.group(1)
    match = find(r"^Sync:\s+(.+?)\s*$", text)
    if match:
        row["pic_sync"] = match.group(1)
    match = find(r"^GNSS:\s+([^,]+), tAcc " + DURATION, text)
    if match:
        row["gnss_fix"] = match.group(1)
        row["gnss_tacc_ns"] = duration_ns(match.group(2), match.group(3))
    match = find(r"^Env:\s+board (" + NUMBER + r") C(.*)$", text)
    if match:
        row["board_temp_c"] = match.group(1)
        rest = match.group(2)
        for name, pattern in (("ambient_temp_c", r"ambient ~(" + NUMBER + r") C"),
                              ("pressure_hpa", r"(" + NUMBER + r") hPa"),
                              ("humidity_pct", r"(" + NUMBER + r") %RH")):
            found = re.search(pattern, rest)
            if found:
                row[name] = found.group(1)


def parse_tracking(text, row):
    """ntp tracking (also the top of ntp -v)."""
    match = find(r"^Server:\s+\d+ (\S+)(?: \([^)]*\))?, stratum (\d+)", text)
    if match:
        row["ntp_server"] = match.group(1)
        row["ntp_stratum"] = match.group(2)
    match = find(r"^Clock:\s+\S+ \+/-\s?" + DURATION + r", (synced|NOT SYNCED)", text)
    if match:
        row["ntp_bound_ns"] = duration_ns(match.group(1), match.group(2))
        row["ntp_synced"] = 1 if match.group(3) == "synced" else 0
    elif find(r"^Clock:\s+\S+ \+/- over", text):
        row["ntp_bound_ns"] = ""
    match = find(r"^Last:\s+sample " + DURATION + r", correction " + DURATION +
                 r", round trip " + DURATION, text)
    if match:
        row["ntp_last_sample_ns"] = duration_ns(match.group(1), match.group(2))
        row["ntp_correction_ns"] = duration_ns(match.group(3), match.group(4))
        row["ntp_round_trip_ns"] = duration_ns(match.group(5), match.group(6))
    match = find(r"^Frequency:\s+(" + NUMBER + r") ppm, skew (" + NUMBER + r") \((\d+) estimates",
                 text)
    if match:
        row["esp_freq_ppm"] = match.group(1)
        row["esp_skew_ppm"] = match.group(2)
        row["esp_estimates"] = match.group(3)
    match = find(r"drift allowed (" + NUMBER + r") ppm", text)
    if match:
        row["esp_drift_allowed_ppm"] = match.group(1)
    match = find(r"^Interval:\s+(\d+) s", text)
    if match:
        row["ntp_interval_s"] = match.group(1)


def parse_ntp_gnss(text, row):
    """ntp gnss: the latest and the last hour's summary."""
    match = find(r"^NTP vs GNSS at \S+: latest " + DURATION, text)
    if match:
        row["ntp_gnss_latest_ns"] = duration_ns(match.group(1), match.group(2))
    match = find(r"last hour: mean " + DURATION + r", sd " + DURATION + r", RMS " + DURATION, text)
    if match:
        row["ntp_gnss_hour_mean_ns"] = duration_ns(match.group(1), match.group(2))
        row["ntp_gnss_hour_sd_ns"] = duration_ns(match.group(3), match.group(4))
        row["ntp_gnss_hour_rms_ns"] = duration_ns(match.group(5), match.group(6))


def parse_osc(text, row):
    """osc, as the PIC prints it."""
    match = find(r"^Oscillator: (" + NUMBER + r") Hz \((" + NUMBER + r") ppm\), measured", text)
    if match:
        row["osc_measured_hz"] = match.group(1)
        row["osc_measured_ppm"] = match.group(2)
    match = find(r"^\s+Stored: (" + NUMBER + r") Hz \((" + NUMBER + r") ppm\)(?:, over (\S+(?: \S+)?))?",
                 text)
    if match:
        row["osc_stored_hz"] = match.group(1)
        row["osc_stored_ppm"] = match.group(2)
        row["osc_stored_over"] = match.group(3) or ""


def parse_esp(text, row):
    """esp: the longest loop() pass."""
    match = find(r"^ESP (\S+) \(", text)
    if match:
        row["esp_version"] = match.group(1)
    match = find(r"^Loop:\s+longest pass " + DURATION + r"(?: \((\S+))?.*?; (\d+) of \d+ passes over 1 s",
                 text)
    if match:
        row["loop_longest_ns"] = duration_ns(match.group(1), match.group(2))
        row["loop_longest_by"] = (match.group(3) or "").rstrip(",")
        row["loop_slow_passes"] = match.group(4)


# ---- Talking to the clock --------------------------------------------------------------

IAC = 255


def strip_telnet(data):
    """Bytes as text, telnet negotiation (IAC ...) and CRs taken out."""
    out = bytearray()
    index = 0
    while index < len(data):
        byte = data[index]
        if byte == IAC and index + 1 < len(data):
            command = data[index + 1]
            if command == IAC:
                out.append(IAC)
                index += 2
            elif command in (251, 252, 253, 254):  # WILL, WONT, DO, DONT + option
                index += 3
            elif command == 250:  # SB ... SE
                end = data.find(bytes([IAC, 240]), index)
                index = len(data) if end < 0 else end + 2
            else:
                index += 2
            continue
        out.append(byte)
        index += 1
    return out.decode("utf-8", "replace").replace("\r", "")


class Console:
    def __init__(self, host, port, timeout):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(0.2)
        self.received = []

    def read_until(self, wanted, timeout, quiet=0.4):
        """Text until wanted (a regex) is seen and the line has been quiet for
        quiet seconds, or until timeout. wanted None: until quiet."""
        text = ""
        deadline = time.monotonic() + timeout
        last = time.monotonic()
        seen = wanted is None
        while time.monotonic() < deadline:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                chunk = None
            if chunk == b"":
                break  # closed
            if chunk:
                text += strip_telnet(chunk)
                last = time.monotonic()
                if not seen and re.search(wanted, re.sub(r"(?m)^(?:> )+", "", text), re.M):
                    seen = True
            elif seen and time.monotonic() - last >= quiet:
                break
        self.received.append(text)
        # Text that came after a prompt (the PIC's, printed once the ESP has
        # answered) starts on the prompt's line: the prompts taken out.
        return re.sub(r"(?m)^(?:> )+", "", text)

    def send(self, line):
        self.sock.sendall(line.encode() + b"\r\n")

    def command(self, line, wanted, timeout=6.0, quiet=0.4):
        self.send(line)
        return self.read_until(wanted, timeout, quiet)

    def close(self):
        try:
            self.send("quit")
        except OSError:
            pass
        self.sock.close()


def sample(host, port, timeout):
    """One sample of host: the row, and everything received."""
    row = {field: "" for field in FIELDS}
    row["utc"] = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    row["host"] = host
    console = Console(host, port, timeout)
    try:
        console.read_until(None, 2.0)  # the banner and the first prompt
        status = ""
        # The status, for an empty line: a fresh connection sometimes needs
        # a few before it answers.
        for _ in range(4):
            status += console.command("", r"^GNSS:|^Sync:|^UTC:\s+unknown", timeout=2.0, quiet=0.6)
            if re.search(r"^PIC:", status, re.M):
                break
        parse_status(status, row)
        parse_tracking(console.command("ntp tracking", r"^Interval:|^Clock:.*NOT SYNCED"), row)
        parse_ntp_gnss(console.command("ntp gnss", r"last hour:|^NTP vs GNSS.*\n.*no", quiet=0.3), row)
        # Printed by the PIC, so it comes after the prompt: waited for.
        parse_osc(console.command("osc", r"^\s+Stored:", timeout=4.0, quiet=0.5), row)
        parse_esp(console.command("esp", r"^Settings:", quiet=0.5), row)
    finally:
        console.close()
    return row, "".join(console.received)


def host_temperature(path):
    try:
        with open(path) as handle:
            value = float(handle.read().strip())
    except (OSError, ValueError):
        return ""
    return "%.2f" % (value / 1000.0 if abs(value) > 200 else value)


def file_fields(path):
    """The columns of an existing CSV, or None for a new (or empty) one."""
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        return None
    with open(path, newline="") as handle:
        return next(csv.reader(handle), None)


def write_row(path, row):
    """Appended under the file's own header: a file begun by an older version
    of this script keeps its columns (and the new ones are left out, with a
    warning), so its rows stay aligned."""
    fields = file_fields(path)
    if fields is not None and fields != FIELDS:
        dropped = [name for name in FIELDS if name not in fields and row.get(name) not in (None, "")]
        if dropped and not getattr(write_row, "warned", False):
            print("%s has older columns: leaving out %s (start a new file for them)" %
                  (path, ", ".join(dropped)), file=sys.stderr)
            write_row.warned = True
    with open(path, "a", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields or FIELDS, extrasaction="ignore")
        if fields is None:
            writer.writeheader()
        writer.writerow(row)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("hosts", nargs="+", help="the clocks' names or addresses")
    parser.add_argument("--port", type=int, default=23)
    parser.add_argument("--interval", type=float, default=60.0, help="seconds between samples")
    parser.add_argument("--out", default="clock_monitor.csv", help="the CSV to append to")
    parser.add_argument("--raw", help="also append everything received to this file")
    parser.add_argument("--temp-file", help="the monitoring host's temperature, to sample too")
    parser.add_argument("--timeout", type=float, default=10.0, help="connection timeout, seconds")
    parser.add_argument("--once", action="store_true", help="one sample of each, then stop")
    args = parser.parse_args(argv)
    next_at = time.monotonic()
    while True:
        for host in args.hosts:
            try:
                row, raw = sample(host, args.port, args.timeout)
            except OSError as error:
                print("%s: %s" % (host, error), file=sys.stderr, flush=True)
                continue
            if args.temp_file:
                row["host_temp_c"] = host_temperature(args.temp_file)
            write_row(args.out, row)
            if args.raw:
                with open(args.raw, "a", encoding="utf-8") as handle:
                    handle.write("===== %s %s\n%s\n" % (row["utc"], host, raw))
            print("%s %s: PIC %s ppm %s, NTP vs GNSS %s ns, ESP %s ppm, board %s C" %
                  (row["utc"], host, row["pic_freq_ppm"] or "-", row["pic_loop"] or "-",
                   row["ntp_gnss_latest_ns"] or "-", row["esp_freq_ppm"] or "-",
                   row["board_temp_c"] or "-"), flush=True)
        if args.once:
            return 0
        next_at += args.interval
        time.sleep(max(0.0, next_at - time.monotonic()))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
