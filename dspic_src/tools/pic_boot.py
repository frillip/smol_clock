"""Talks to the PIC's bootloader over SML class 0xF0 from a PC, as the ESP
will: through a serial port wired to the PIC's UART1 (the ESP running a UART
bridge, or a USB-serial adapter), or to the host simulator.

  pic_boot.py [--port P | --sim SPL_SIM --flash F [--sim-app]] COMMAND

  status              BOOT-STATUS, from the SPL (or the application, later)
  enter               CMD-USER ENTER_BOOTLOADER to the running application
  hold                wait for the boot window (reset or power-cycle the PIC)
                      and stop it with HOLD
  update FILE [--force]
                      install an application file (make_image.py app), as the
                      ESP's procedure: enter or hold, check, ERASE, DATA,
                      VERIFY, COMMIT, RUN, then wait for STAT-SYS
  verify FILE         compare the flash with an application file
  test                the SPL's test page: ERASE, DATA, VERIFY, COMMIT, ERASE
  run | reset         BOOT-COMMAND RUN or RESET
  read ADDRESS COUNT  print COUNT chunks of flash from ADDRESS (BOOT-VERIFY)

Every timeout and retry is here, as on the ESP: the PIC only answers. Needs
pyserial for --port. --sim runs dspic_spl/tests/host/spl_sim on a pipe.
"""
import argparse
import os
import queue
import struct
import subprocess
import sys
import threading
import time
import zlib

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import make_image  # noqa: E402

L = make_image.L
SYNC = b"\xB5\x73"
MAX_PAYLOAD = 240

CLASS_STAT, CLASS_CMD, CLASS_BOOT = 0x30, 0x80, 0xF0
STAT_SYS, CMD_USER, CMD_RESPONSE = 0x60, 0x10, 0x20
STATUS, COMMAND, RESPONSE, DATA, DATA_RESPONSE, VERIFY, VERIFY_RESPONSE = (
    0x01, 0x10, 0x20, 0x50, 0x60, 0x90, 0xA0)
USER_REQUEST_MESSAGE, USER_RESET, USER_ENTER_BOOTLOADER = 0x0002, 0x0031, 0x0032

CMD_STATUS, CMD_HOLD, CMD_ERASE, CMD_COMMIT, CMD_RUN, CMD_RESET = 1, 2, 3, 4, 5, 6
REGION_APP, REGION_SPL, REGION_TEST = 0, 1, 2
KEY_ERASE, KEY_COMMIT = 0x45524153, 0x434F4D4D
ACK, NACK, ACCEPTED = 0, 1, 2

MODES = ["WINDOW", "HELD", "NO_APP", "LOOP", "APP"]
FLAGS = ["RECORD", "COMMITTED", "CHECKING", "IMAGE_OK", "ERASED", "REQUESTED", "POWER_ON", "TEST_FREE"]
SPL_STATES = ["RECORD", "COMMITTED", "INTACT", "CONFIRMED", "REJECTED"]
PATHS = ["APP_DIRECT", "UNCONFIRMED", "NO_SPL", "SPL_GIVEN_UP", "LAST_RESORT"]
ERRORS = ["NONE", "UNSUPPORTED", "LENGTH", "ARGUMENT", "BUSY", "STALE_REVISION", "SCHEMA", "STORAGE",
          "VERIFY", "STATE", "CRC", "NOT_BLANK"]
CHUNK = 64  # instructions
CHUNK_BYTES = 192
CHUNK_ADDRESSES = 0x80


class BootError(Exception):
    pass


def bits(value, names):
    return " ".join(name for index, name in enumerate(names) if value >> index & 1) or "NONE"


def error_name(code):
    return ERRORS[code] if code < len(ERRORS) else str(code)


def crc16(data):
    return make_image.crc16(data)


def frame(class_id, message_id, payload):
    head = SYNC + struct.pack(">BBH", class_id, message_id, len(payload))
    return head + payload + struct.pack(">H", crc16(head + payload))


# ---- Links ---------------------------------------------------------------------------

class SerialLink:
    def __init__(self, port, baud):
        try:
            import serial
        except ImportError:
            raise BootError("pyserial is needed for --port: pip install pyserial")
        # The timeout is set once: changing it reconfigures the port, which
        # on Windows costs tens of milliseconds a time.
        self.port = serial.Serial(port, baud, timeout=0.02)

    def write(self, data):
        self.port.write(data)

    def read(self, timeout):
        """What has arrived, as soon as anything has: up to 20 ms for the
        first byte, then whatever else is waiting."""
        waiting = self.port.in_waiting
        if waiting:
            return self.port.read(waiting)
        data = self.port.read(1)
        if data and self.port.in_waiting:
            data += self.port.read(self.port.in_waiting)
        return data

    def close(self):
        self.port.close()


class SimLink:
    def __init__(self, program, flash, app, verbose):
        args = [program, flash] + (["--app"] if app else [])
        self.process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0,
                                        stderr=None if verbose else subprocess.DEVNULL)
        self.queue = queue.Queue()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        while True:
            data = self.process.stdout.read1(4096) if hasattr(self.process.stdout, "read1") else \
                os.read(self.process.stdout.fileno(), 4096)
            if not data:
                break
            self.queue.put(data)

    def write(self, data):
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def read(self, timeout):
        try:
            return self.queue.get(timeout=max(timeout, 0.001))
        except queue.Empty:
            return b""

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=10)


# ---- Frames ----------------------------------------------------------------------------

class Port:
    """Frames in and out. Text and frames nobody waits for are dropped,
    text shown with --verbose."""

    def __init__(self, link, verbose=False):
        self.link = link
        self.buffer = bytearray()
        self.verbose = verbose
        self.token = 0x4000

    def send(self, class_id, message_id, payload):
        self.link.write(frame(class_id, message_id, payload))

    def _frames(self):
        buf = self.buffer
        while True:
            start = buf.find(SYNC)
            if start < 0:
                keep = 1 if buf.endswith(SYNC[:1]) else 0
                self._noise(buf[:len(buf) - keep])
                del buf[:len(buf) - keep]
                return
            if start:
                self._noise(buf[:start])
                del buf[:start]
            if len(buf) < 6:
                return
            length = struct.unpack(">H", buf[4:6])[0]
            if length > MAX_PAYLOAD:
                del buf[:1]
                continue
            if len(buf) < 8 + length:
                return
            if crc16(bytes(buf[:6 + length])) != struct.unpack(">H", buf[6 + length:8 + length])[0]:
                del buf[:1]
                continue
            found = (buf[2], buf[3], bytes(buf[6:6 + length]))
            del buf[:8 + length]  # before the yield: the caller may stop at this one
            yield found

    def _noise(self, data):
        if self.verbose and data:
            sys.stderr.write(data.decode("ascii", "replace"))

    def wait(self, want, timeout):
        """The first frame for which want(class, id, payload) is true, or
        None at the timeout."""
        end = time.monotonic() + timeout
        while True:
            for found in self._frames():
                if want(*found):
                    return found
            left = end - time.monotonic()
            if left <= 0:
                return None
            self.buffer += self.link.read(min(left, 0.05))

    def next_token(self):
        self.token = (self.token + 1) & 0xFFFF
        return self.token


# ---- Messages ---------------------------------------------------------------------------

STATUS_FIELDS = ("spl_version", "protocol", "mode", "flags", "reset_cause", "window_ms", "device_id",
                 "region_start", "region_end", "app_version", "app_length", "app_crc32", "crc32_computed",
                 "build_time", "attempts", "boot", "ipl_version", "spl_state", "trials", "healthy_s",
                 "spl_crc32")


def parse_status(payload):
    return dict(zip(STATUS_FIELDS, struct.unpack(">IBBHHHIIIIIIIIBBHBBHI", payload)))


def describe_status(s):
    v = make_image.version_text
    lines = ["mode %s, flags %s" % (MODES[s["mode"]] if s["mode"] < len(MODES) else s["mode"],
                                    bits(s["flags"], FLAGS)),
             "application %s, %d instructions, CRC-32 0x%08X (computed 0x%08X)" % (
                 v(s["app_version"]), s["app_length"], s["app_crc32"], s["crc32_computed"]),
             "SPL %s, %s, CRC-32 0x%08X; IPL %d.%d; protocol %d; DEVID 0x%04X" % (
                 v(s["spl_version"]), bits(s["spl_state"], SPL_STATES), s["spl_crc32"],
                 s["ipl_version"] >> 8, s["ipl_version"] & 0xFF, s["protocol"], s["device_id"]),
             "reset cause 0x%04X, boot %s, attempts %d, trials %d, window %d ms" % (
                 s["reset_cause"], bits(s["boot"], PATHS), s["attempts"], s["trials"], s["window_ms"])]
    return "\n".join(lines)


class Boot:
    def __init__(self, port, log=print, verbose=False):
        self.port = port
        self.log = log
        self.verbose = verbose

    def note(self, text):
        """A retry or a timing: shown with --verbose only."""
        if self.verbose:
            sys.stderr.write("\n%s\n" % text)

    def _boot(self, message_id):
        return lambda c, i, p: c == CLASS_BOOT and i == message_id

    def command(self, code, region=0, key=0, args=b"", timeout=1.0, final_timeout=None, retries=3):
        """A BOOT-COMMAND: (status, detail, value) of the final answer."""
        for _ in range(retries):
            token = self.port.next_token()
            payload = struct.pack(">HBBI", token, code, region, key) + args.ljust(24, b"\0")
            self.port.send(CLASS_BOOT, COMMAND, payload)
            answer = self._response(token, timeout)
            if answer is None:
                continue
            if answer[0] == ACCEPTED:
                answer = self._response(token, final_timeout or timeout)
                if answer is None:
                    raise BootError("command %d: ACCEPTED, then nothing" % code)
            return answer
        raise BootError("command %d: no answer" % code)

    def _response(self, token, timeout):
        found = self.port.wait(lambda c, i, p: c == CLASS_BOOT and i == RESPONSE and len(p) == 12 and
                               struct.unpack(">H", p[4:6])[0] == token, timeout)
        if found is None:
            return None
        status, detail, _, _, _, value = struct.unpack(">HHHBBI", found[2])
        return status, detail, value

    def status(self, timeout=0.5, retries=3):
        for _ in range(retries):
            token = self.port.next_token()
            self.port.send(CLASS_BOOT, COMMAND, struct.pack(">HBBI", token, CMD_STATUS, 0, 0) + bytes(24))
            found = self.port.wait(self._boot(STATUS), timeout)
            if found is not None and len(found[2]) == 56:
                self._response(token, timeout)
                return parse_status(found[2])
        return None

    def check(self, answer, what):
        status, detail, value = answer
        if status != ACK:
            raise BootError("%s: NACK %s (value 0x%X)" % (what, error_name(detail), value))
        return value

    def enter(self):
        """ENTER_BOOTLOADER to the application, then BOOT-STATUS HELD."""
        token = self.port.next_token()
        self.port.send(CLASS_CMD, CMD_USER, struct.pack(">HHHI", USER_ENTER_BOOTLOADER, 0, token, 0))
        found = self.port.wait(lambda c, i, p: c == CLASS_CMD and i == CMD_RESPONSE and len(p) == 12 and
                               struct.unpack(">H", p[4:6])[0] == token, 2.0)
        if found is None:
            raise BootError("ENTER_BOOTLOADER: no answer from the application")
        status, detail = struct.unpack(">HH", found[2][:4])
        if status != ACK:
            raise BootError("ENTER_BOOTLOADER: NACK %s" % error_name(detail))
        found = self.port.wait(lambda c, i, p: c == CLASS_BOOT and i == STATUS and len(p) == 56 and
                               parse_status(p)["mode"] == 1, 3.0)
        if found is None:
            raise BootError("ENTER_BOOTLOADER: no BOOT-STATUS HELD from the SPL")
        return parse_status(found[2])

    def hold(self, timeout):
        """Waits for a BOOT-STATUS in the window and answers HOLD."""
        found = self.port.wait(lambda c, i, p: c == CLASS_BOOT and i == STATUS and len(p) == 56 and
                               parse_status(p)["mode"] != 4, timeout)
        if found is None:
            raise BootError("no BOOT-STATUS: reset or power-cycle the PIC")
        self.check(self.command(CMD_HOLD), "HOLD")
        return self.status()

    def into_spl(self):
        """The SPL, held: from the application, or a running SPL."""
        s = self.status(timeout=0.3, retries=2)
        if s is None or s["mode"] == 4:
            self.log("asking the application for the bootloader")
            self.enter()
            s = self.status()  # the first BOOT-STATUS is sent before the image is checked
        elif s["mode"] == 0:
            self.check(self.command(CMD_HOLD), "HOLD")
            s = self.status()
        return s

    def application_version(self, timeout):
        """The running application's version, from STAT-SYS. A new
        application starts with its link in text mode, sending no STAT-SYS
        until a first frame puts it in SML mode (the ESP sends SHOW at its
        start), so this asks for one with REQUEST_MESSAGE every half second.
        The SPL ignores those."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.port.send(CLASS_CMD, CMD_USER, struct.pack(">HHHI", USER_REQUEST_MESSAGE, 0,
                                                            self.port.next_token(), CLASS_STAT << 8 | STAT_SYS))
            found = self.port.wait(lambda c, i, p: c == CLASS_STAT and i == STAT_SYS and len(p) == 16, 0.5)
            if found is not None:
                return struct.unpack(">I", found[2][8:12])[0]
        return None

    def erase(self, region):
        return self.check(self.command(CMD_ERASE, region, KEY_ERASE, timeout=1.0, final_timeout=10.0, retries=1),
                          "ERASE")

    def data(self, region, address, chunk):
        payload = struct.pack(">IB3xI", address, region, zlib.crc32(chunk) & 0xFFFFFFFF) + chunk
        for _ in range(5):
            self.port.send(CLASS_BOOT, DATA, payload)
            found = self.port.wait(lambda c, i, p: c == CLASS_BOOT and i == DATA_RESPONSE and len(p) == 12 and
                                   struct.unpack(">I", p[4:8])[0] == address, 0.5)
            if found is None:
                self.note("DATA 0x%06X: no answer, sending it again" % address)
                continue
            status, detail, _, crc = struct.unpack(">HHII", found[2])
            if status == ACK:
                if crc != zlib.crc32(chunk) & 0xFFFFFFFF:
                    raise BootError("DATA 0x%06X: CRC-32 of the flash 0x%08X" % (address, crc))
                return
            if detail == 10:  # CRC: send it again
                self.note("DATA 0x%06X: NACK CRC, sending it again" % address)
                continue
            raise BootError("DATA 0x%06X: NACK %s" % (address, error_name(detail)))
        raise BootError("DATA 0x%06X: no answer" % address)

    def read(self, address):
        for _ in range(5):
            self.port.send(CLASS_BOOT, VERIFY, struct.pack(">II", address, 0))
            found = self.port.wait(lambda c, i, p: c == CLASS_BOOT and i == VERIFY_RESPONSE and len(p) == 204 and
                                   struct.unpack(">I", p[4:8])[0] == address, 0.5)
            if found is None:
                self.note("VERIFY 0x%06X: no answer, asking again" % address)
                continue
            status, detail, _, crc = struct.unpack(">HHII", found[2][:12])
            data = found[2][12:]
            if status != ACK:
                raise BootError("VERIFY 0x%06X: NACK %s" % (address, error_name(detail)))
            if zlib.crc32(data) & 0xFFFFFFFF != crc:
                self.note("VERIFY 0x%06X: CRC-32 wrong, asking again" % address)
                continue
            return data
        raise BootError("VERIFY 0x%06X: no answer" % address)

    def commit(self, region, version, start, length, crc32, build_time, extra):
        args = struct.pack(">IIIIIHH", version, start, length, crc32, build_time, extra, 0)
        status, detail, value = self.command(CMD_COMMIT, region, KEY_COMMIT, args, timeout=1.0,
                                             final_timeout=3.0, retries=1)
        if status != ACK:
            raise BootError("COMMIT: NACK %s%s" % (error_name(detail),
                                                     ", flash CRC-32 0x%08X" % value if detail == 8 else ""))


def chunks(image, start):
    """(address, 192 bytes) for every chunk of a packed image, padded."""
    padded = image + b"\xFF" * (-len(image) % CHUNK_BYTES)
    for offset in range(0, len(padded), CHUNK_BYTES):
        yield start + offset // 3 * 2, padded[offset:offset + CHUNK_BYTES]


class Progress:
    """A line that counts up in place on a terminal; nothing in between
    otherwise (a pipe, a test). Ends with the time taken."""

    def __init__(self, what, total, out=None):
        self.what, self.total, self.done = what, total, 0
        self.out = out or sys.stdout
        self.live = hasattr(self.out, "isatty") and self.out.isatty()
        self.started = self.shown = time.monotonic()
        self._show(force=True)

    def _show(self, force=False):
        now = time.monotonic()
        if self.live and (force or now - self.shown >= 0.1):
            self.out.write("\r%s %d/%d" % (self.what, self.done, self.total))
            self.out.flush()
            self.shown = now

    def step(self):
        self.done += 1
        self._show()

    def end(self, data_bytes=0):
        seconds = time.monotonic() - self.started
        rate = ", %.1f KB/s" % (data_bytes / 1024.0 / seconds) if data_bytes and seconds > 0 else ""
        if self.live:
            self.out.write("\r")
        self.out.write("%s %d/%d chunks in %.1f s%s\n" % (self.what, self.done, self.total, seconds, rate))
        self.out.flush()


def write_region(boot, region, start, image, log):
    started = time.monotonic()
    pages = boot.erase(region)
    log("erased %d pages in %.1f s" % (pages, time.monotonic() - started))
    todo = [(address, chunk) for address, chunk in chunks(image, start) if chunk != b"\xFF" * CHUNK_BYTES]
    progress = Progress("written", len(todo))
    for address, chunk in todo:
        boot.data(region, address, chunk)
        progress.step()
    progress.end(len(todo) * CHUNK_BYTES)
    progress = Progress("read back", len(todo))
    for address, chunk in todo:
        if boot.read(address) != chunk:
            raise BootError("VERIFY 0x%06X: differs" % address)
        progress.step()
    progress.end(len(todo) * CHUNK_BYTES)


def update(boot, image_file, force, log):
    f = image_file
    s = boot.into_spl()
    if s["protocol"] != 1 or s["device_id"] != f["devid"] or s["region_start"] != f["start"]:
        raise BootError("this PIC does not take this file: protocol %d, DEVID 0x%04X" %
                        (s["protocol"], s["device_id"]))
    if not s["spl_state"] & 0x08 and not force:
        raise BootError("the SPL is not confirmed: it would refuse to erase (--force to try)")
    if (s["flags"] & 0x08 and s["app_version"] == f["version"] and s["app_crc32"] == f["crc32"] and
            not force):
        log("already installed: %s" % make_image.version_text(f["version"]))
        boot.check(boot.command(CMD_RUN), "RUN")
        return
    for attempt in range(3):
        try:
            started = time.monotonic()
            write_region(boot, REGION_APP, f["start"], f["image"], log)
            boot.commit(REGION_APP, f["version"], f["start"], f["length"], f["crc32"], f["build_time"],
                        f["sml_version"])
            log("committed, %.1f s from the erase" % (time.monotonic() - started))
            break
        except BootError as error:
            log("pass %d failed: %s" % (attempt + 1, error))
            if attempt == 2:
                raise
    boot.check(boot.command(CMD_RUN), "RUN")
    version = boot.application_version(10.0)
    if version is None:
        raise BootError("no STAT-SYS from the new application")
    if version != f["version"]:
        raise BootError("STAT-SYS says %s" % make_image.version_text(version))
    log("running %s" % make_image.version_text(version))


def test_page(boot, log):
    s = boot.into_spl()
    if not s["flags"] & 0x80:
        raise BootError("TEST_FREE not set: the application reaches the test page")
    words = [(index * 0x010203 + 0x5A) & 0xFFFFFE for index in range(4 * CHUNK)]
    image = b"".join(struct.pack("<I", w)[:3] for w in words)
    start = L["TEST_START"]
    write_region(boot, REGION_TEST, start, image, log)
    boot.commit(REGION_TEST, 0, start, len(words), zlib.crc32(image) & 0xFFFFFFFF, int(time.time()), 0)
    record = boot.read(L["TEST_RECORD_ADDRESS"])
    if record[:3] != bytes([L["APP_RECORD_MAGIC_VALUE"] & 0xFF, L["APP_RECORD_MAGIC_VALUE"] >> 8, 0xFF]):
        raise BootError("the test record is not there")
    boot.erase(REGION_TEST)
    log("test page: erase, write, read back, commit, erase: all good")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port")
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--sim")
    parser.add_argument("--flash")
    parser.add_argument("--sim-app", action="store_true")
    parser.add_argument("--verbose", action="store_true",
                        help="retries, and text from the PIC outside frames, on stderr")
    parser.add_argument("--wait", type=float, default=60.0, help="hold: seconds to wait for the window")
    parser.add_argument("action", choices=["status", "enter", "hold", "update", "verify", "test", "run",
                                           "reset", "read"])
    parser.add_argument("arguments", nargs="*")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args(argv)

    def log(text):
        print(text)

    link = None
    try:
        if args.sim:
            if not args.flash:
                raise BootError("--sim needs --flash")
            link = SimLink(args.sim, args.flash, args.sim_app, args.verbose)
        elif args.port:
            link = SerialLink(args.port, args.baud)
        else:
            raise BootError("--port or --sim")
        boot = Boot(Port(link, args.verbose), log, args.verbose)
        if args.action == "status":
            s = boot.status()
            if s is None:
                raise BootError("no BOOT-STATUS: the application is running (it answers STATUS only from "
                                "phase 7), or nothing is connected")
            log(describe_status(s))
        elif args.action == "enter":
            boot.enter()
            log(describe_status(boot.status()))
        elif args.action == "hold":
            log("waiting for the boot window: reset or power-cycle the PIC")
            log(describe_status(boot.hold(args.wait)))
        elif args.action in ("update", "verify"):
            if len(args.arguments) != 1:
                raise BootError("%s FILE" % args.action)
            with open(args.arguments[0], "rb") as handle:
                f = make_image.parse_file(handle.read())
            if f["kind"] != "app":
                raise BootError("not an application file")
            if args.action == "update":
                update(boot, f, args.force, log)
            else:
                bad = [a for a, c in chunks(f["image"], f["start"]) if boot.read(a) != c]
                log("%s: %s" % (args.arguments[0], "matches" if not bad else
                                "%d chunks differ, first 0x%06X" % (len(bad), bad[0])))
                if bad:
                    return 1
        elif args.action == "test":
            test_page(boot, log)
        elif args.action in ("run", "reset"):
            boot.check(boot.command(CMD_RUN if args.action == "run" else CMD_RESET), args.action.upper())
            log("ACK")
        elif args.action == "read":
            if len(args.arguments) != 2:
                raise BootError("read ADDRESS COUNT")
            address, count = int(args.arguments[0], 0), int(args.arguments[1], 0)
            for index in range(count):
                data = boot.read(address + index * CHUNK_ADDRESSES)
                for word in range(CHUNK):
                    if word % 8 == 0:
                        sys.stdout.write("%06X:" % (address + index * CHUNK_ADDRESSES + 2 * word))
                    w = data[3 * word] | data[3 * word + 1] << 8 | data[3 * word + 2] << 16
                    sys.stdout.write(" %06X" % w)
                    if word % 8 == 7:
                        sys.stdout.write("\n")
        return 0
    except (BootError, make_image.ImageError, OSError) as error:
        print("\npic_boot: %s" % error, file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\npic_boot: interrupted. The PIC stays where it was: in the SPL after an ERASE, with no "
              "application until an update completes", file=sys.stderr)
        return 130
    finally:
        if link is not None:
            link.close()


if __name__ == "__main__":
    sys.exit(main())
