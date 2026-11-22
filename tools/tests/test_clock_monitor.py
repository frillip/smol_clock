"""clock_monitor.py against a fake console replaying a real clock's output."""
import csv
import os
import socket
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import clock_monitor  # noqa: E402

STATUS = """
ESP:      0.3.64 SMOL_CLOCK_06D350, up 00:33:13
NTP:      2026-11-16T07:29:15Z +/-1.308 ms, synced
Server:   dhcp (192.168.3.1), stratum 1, updated 00:00:26 ago; 5 servers
Zone:     Europe/London (GNSS), UTC+00:00, changes 2027-03-28T01:00:00Z
Leap:     TAI-UTC 37 s (GNSS)
Wi-Fi:    Lyra's Alethiometer, 192.168.3.146, 2001:8b0:609:cf50:ee64:c9ff:fe06:d350, -51 dBm
Telnet:   RUNNING 192.168.3.146:23, start-up ON, client 192.168.3.1

PIC:      0.3.40, up 12d 08:50:59, last reset: software (reset command or unhandled trap)
UTC:      2026-11-16T07:29:15Z +/-106 ns, GNSS LOCKED
Local:    2026-11-16T07:29:15+00:00
Freq:     -1.8483 ppm (TRACK), loop -1.8391 ppm
Sync:     LOCKED on GNSS, synchronised
  Source    Verdict            Bound       Phase  Last good
* GNSS      good             106 ns      +25 ns       0s
+ HOLDOVER  good             131 ns           -       0s
  NTP       good           1.323 ms   +141.0 us       0s
  RTC       good           3.167 ms     +300 ns       0s
GNSS:     3D OK, tAcc 27 ns
Env:      board 34.41 C (ambient ~21.61 C), 1012.8 hPa, 48.21 %RH
"""

TRACKING = """Server:    0 dhcp (192.168.3.1), stratum 1; 5 servers
Clock:     2026-11-16T07:28:54Z +/-1.297 ms, synced
Updated:   2026-11-16T07:28:48Z, 00:00:05 ago
Last:      sample -159.0 us, correction -31.00 us, round trip 2.225 ms
Root:      delay 15.00 us, dispersion 15.00 us
Frequency: +1.632 ppm, skew 0.160 (14 estimates, the servers' lines); drift allowed 0.66 ppm
Slewing:   -31.00 us gently, done in 00:04:10 (0 fast)
Interval:  128 s (adaptive, at most 128 s)
Asymmetry: +0 us, server 0 +120 us (+ = clock later)
"""

NTP_GNSS = """NTP vs GNSS at 2026-11-16T07:28:58Z: latest 142.0 us (+ = NTP ahead)
  last hour: mean 34.67 us, sd 87.28 us, RMS 93.90 us, -76.00 us to 256.0 us (1942 s)
  since 2026-11-16T06:56:18Z:
             mean 34.67 us, sd 87.28 us, RMS 93.90 us, -76.00 us to 256.0 us (1942 s)
"""

OSC = """Oscillator: 39999925.476 Hz (-1.863 ppm), measured
  Stored: 39999924.369 Hz (-1.891 ppm), over 10d 17:13:32
"""

ESP = """ESP 0.3.64 (SMOL_CLOCK_06D350), up 00:33:01, last reset: software
Loop:     longest pass 19.75 s (telnet 19.75 s, "update check") at 2026-11-16T06:57:26Z; 2 of 3804011 passes over 1 s
Settings: saved
  NTP: 0: DHCP's, enable; interval 128 s
"""


def crlf(text):
    return text.replace("\n", "\r\n").encode()


class FakeClock(threading.Thread):
    """Answers like the clock: telnet negotiation first, the status only on
    the third empty line, osc's text from the PIC after the prompt."""

    def __init__(self):
        super().__init__(daemon=True)
        self.server = socket.socket()
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(1)
        self.port = self.server.getsockname()[1]
        self.lines = []

    def run(self):
        connection, _ = self.server.accept()
        connection.sendall(bytes([255, 251, 1, 255, 251, 3]) + b"Welcome\r\n> ")
        empty = 0
        buffer = b""
        while True:
            data = connection.recv(1024)
            if not data:
                break
            buffer += data
            while b"\r\n" in buffer:
                line, buffer = buffer.split(b"\r\n", 1)
                line = line.decode()
                self.lines.append(line)
                if line == "":
                    empty += 1
                    connection.sendall(crlf(STATUS) + b"> " if empty >= 3 else b"\r\n> ")
                elif line == "ntp tracking":
                    connection.sendall(crlf(TRACKING) + b"> ")
                elif line == "ntp gnss":
                    connection.sendall(crlf(NTP_GNSS) + b"> ")
                elif line == "osc":
                    connection.sendall(b"> ")
                    time.sleep(0.3)
                    connection.sendall(crlf(OSC))
                elif line == "esp":
                    connection.sendall(crlf(ESP) + b"> ")
                elif line == "esp tcomp":
                    connection.sendall(crlf(TCOMP) + b"> ")
                elif line == "quit":
                    connection.close()
                    return
                else:
                    connection.sendall(b"unknown\r\n> ")


TCOMP = """esp tcomp: ON, -187 ppb/C (learnt); ACTIVE, board 32.50 C, reference 30.00 C, crystal -467 ppb, correction +1203 us
esp tcomp learning: 4 estimates, learnt -186.6 ppb/C, last -192.3 +/- 7.4 ppb/C over 4.20 C (1.80 C each way), r 0.83, 3 min ago, next in 95 min, window 48 samples over 4.20 C, moved 0.12 C in 30 min
"""


class ParseTest(unittest.TestCase):
    def test_fields(self):
        row = {}
        clock_monitor.parse_status(STATUS, row)
        clock_monitor.parse_tracking(TRACKING, row)
        clock_monitor.parse_ntp_gnss(NTP_GNSS, row)
        clock_monitor.parse_osc(OSC, row)
        clock_monitor.parse_esp(ESP, row)
        self.assertEqual(row["pic_freq_ppm"], "-1.8483")
        self.assertEqual(row["pic_loop"], "TRACK")
        self.assertEqual(row["pic_freq_loop_ppm"], "-1.8391")
        self.assertEqual((row["board_temp_c"], row["ambient_temp_c"], row["pressure_hpa"], row["humidity_pct"]),
                         ("34.41", "21.61", "1012.8", "48.21"))
        self.assertEqual(row["pic_utc_bound_ns"], 106)
        self.assertEqual((row["pic_source"], row["pic_lock"]), ("GNSS", "LOCKED"))
        self.assertEqual(row["pic_sync"], "LOCKED on GNSS, synchronised")
        self.assertEqual(row["gnss_tacc_ns"], 27)
        self.assertEqual(row["ntp_server"], "dhcp")
        self.assertEqual(row["ntp_stratum"], "1")
        self.assertEqual(row["ntp_bound_ns"], 1297000)
        self.assertEqual(row["ntp_synced"], 1)
        self.assertEqual(row["ntp_last_sample_ns"], -159000)
        self.assertEqual(row["ntp_correction_ns"], -31000)
        self.assertEqual(row["ntp_round_trip_ns"], 2225000)
        self.assertEqual(row["esp_freq_ppm"], "+1.632")
        self.assertEqual(row["esp_skew_ppm"], "0.160")
        self.assertEqual(row["esp_estimates"], "14")
        self.assertEqual(row["esp_drift_allowed_ppm"], "0.66")
        self.assertEqual(row["ntp_interval_s"], "128")
        self.assertEqual(row["ntp_gnss_latest_ns"], 142000)
        self.assertEqual(row["ntp_gnss_hour_mean_ns"], 34670)
        self.assertEqual(row["ntp_gnss_hour_rms_ns"], 93900)
        self.assertEqual(row["osc_measured_ppm"], "-1.863")
        self.assertEqual(row["osc_stored_ppm"], "-1.891")
        self.assertEqual(row["osc_stored_over"], "10d 17:13:32")
        self.assertEqual(row["loop_longest_ns"], 19750000000)
        self.assertEqual(row["loop_longest_by"], "telnet")
        self.assertEqual(row["loop_slow_passes"], "2")
        self.assertEqual(row["esp_version"], "0.3.64")

    def test_unbounded_and_unsynced(self):
        row = {}
        clock_monitor.parse_tracking("Clock:     2026-11-16T07:28:54Z +/- over 4.29 s, synced\n", row)
        self.assertEqual(row["ntp_bound_ns"], "")
        row = {}
        clock_monitor.parse_tracking("Clock:     2026-11-16T07:28:54Z +/-12.00 ms, NOT SYNCED\n", row)
        self.assertEqual((row["ntp_bound_ns"], row["ntp_synced"]), (12000000, 0))

    def test_tcomp(self):
        row = {}
        clock_monitor.parse_tcomp(TCOMP, row)
        self.assertEqual(row, {"tcomp_enabled": "1", "tcomp_ppb_per_c": "-187", "tcomp_state": "ACTIVE",
                               "tcomp_reference_c": "30.00", "tcomp_crystal_ppb": "-467",
                               "tcomp_correction_us": "+1203", "tcomp_source": "learnt",
                               "tcomp_estimates": "4", "tcomp_learnt_ppb_per_c": "-186.6",
                               "tcomp_last_ppb_per_c": "-192.3", "tcomp_last_se_ppb": "7.4",
                               "tcomp_last_range_c": "4.20", "tcomp_window": "48",
                               "tcomp_short_polls": "0", "tcomp_moved_c": "0.12",
                               "tcomp_short_reason": "", "tcomp_last_turn_c": "1.80",
                               "tcomp_last_rho": "0.83"})
        row = {}
        clock_monitor.parse_tcomp("esp tcomp: ON, -190 ppb/C (default); ACTIVE, board 30.10 C, reference 30.00 C, "
                                  "crystal -19 ppb, correction +2 us\n"
                                  "esp tcomp learning: 0 estimates, window 7 samples over 0.40 C, moved 0.62 C in 30 min, "
                                  "short polls (temperature moving, 256 s)\n", row)
        self.assertEqual((row["tcomp_source"], row["tcomp_estimates"], row["tcomp_window"],
                          row["tcomp_short_polls"], row["tcomp_short_reason"], row["tcomp_moved_c"],
                          "tcomp_last_ppb_per_c" in row),
                         ("default", "0", "7", "1", "temperature moving", "0.62", False))
        # Before learning: no source, no second line.
        row = {}
        clock_monitor.parse_tcomp("esp tcomp: ON, -120 ppb/C; NO TEMPERATURE, crystal +0 ppb, "
                                  "correction +0 us\n", row)
        self.assertNotIn("tcomp_source", row)
        self.assertEqual((row["tcomp_state"], row["tcomp_ppb_per_c"], row["tcomp_reference_c"]
                          if "tcomp_reference_c" in row else ""), ("NO TEMPERATURE", "-120", ""))
        row = {}
        clock_monitor.parse_tcomp("esp tcomp: OFF, +40 ppb/C; OFF, board 30.10 C, reference 29.00 C, "
                                  "crystal +0 ppb, correction -12 us\n", row)
        self.assertEqual((row["tcomp_enabled"], row["tcomp_state"], row["tcomp_correction_us"]),
                         ("0", "OFF", "-12"))

    def test_env_board_only(self):
        row = {}
        clock_monitor.parse_status("Env:      board -1.25 C\n", row)
        self.assertEqual(row, {"board_temp_c": "-1.25"})

    def test_old_file_keeps_its_columns(self):
        import contextlib
        import io
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "old.csv")
            old = [name for name in clock_monitor.FIELDS if name != "pic_freq_loop_ppm"]
            with open(path, "w", newline="") as handle:
                csv.writer(handle).writerow(old)
            with contextlib.redirect_stderr(io.StringIO()) as errors:
                clock_monitor.write_row(path, {"utc": "t", "pic_freq_ppm": "1", "pic_freq_loop_ppm": "2"})
            self.assertIn("pic_freq_loop_ppm", errors.getvalue())
            with open(path, newline="") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(list(rows[0].keys()), old)
            self.assertEqual(rows[0]["pic_freq_ppm"], "1")
            new = os.path.join(folder, "new.csv")
            clock_monitor.write_row(new, {"pic_freq_loop_ppm": "2"})
            with open(new, newline="") as handle:
                self.assertEqual(next(csv.reader(handle)), clock_monitor.FIELDS)

    def test_telnet_stripped(self):
        self.assertEqual(clock_monitor.strip_telnet(bytes([255, 251, 1]) + b"a\r\nb" +
                                                    bytes([255, 255])), "a\nb\xff".replace("\xff", "�"))


class SampleTest(unittest.TestCase):
    def test_sample_against_fake_clock(self):
        fake = FakeClock()
        fake.start()
        row, raw = clock_monitor.sample("127.0.0.1", fake.port, 5.0)
        fake.join(5)
        # The status took three empty lines; osc's text came after its prompt.
        self.assertEqual(fake.lines[:3], ["", "", ""])
        self.assertEqual(fake.lines[-1], "quit")
        self.assertEqual(row["pic_freq_ppm"], "-1.8483")
        self.assertEqual(row["osc_measured_ppm"], "-1.863")
        self.assertEqual(row["ntp_gnss_latest_ns"], 142000)
        self.assertEqual(row["loop_longest_by"], "telnet")
        self.assertEqual(row["tcomp_crystal_ppb"], "-467")
        self.assertIn("Oscillator:", raw)
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "out.csv")
            clock_monitor.write_row(path, row)
            clock_monitor.write_row(path, row)
            with open(path) as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 2)
            self.assertEqual(rows[1]["esp_freq_ppm"], "+1.632")


if __name__ == "__main__":
    unittest.main()
