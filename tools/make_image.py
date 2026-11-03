"""Builds the SMOL bootloader's image files from MPLAB X production .hex files

  make_image.py app <app.hex> -o app.smolpic [--build-time S]
      The application file for the ESP: a 64-byte header, then the packed
      image from 0x002000. Version from platform/version.h.
  make_image.py spl <spl.hex> -o spl.smolspl [--version X.Y.Z] [--min-ipl X.Y.Z]
      The SPL file: a 64-byte header, then the packed code from 0x000800.
      Version from dspic_spl/spl_version.h unless given.
  make_image.py install --ipl <ipl.hex> [--spl <spl.hex> [--spl-version X.Y.Z]]
                        --app <app.hex> -o first.hex
      The first-installation hex, for a PICkit: the IPL with the configuration
      words, the SPL with its record committed and confirmed, the application
      with its record committed. Without an SPL, the IPL starts the
      application directly (until the SPL exists, and for testing).
  make_image.py ipl <ipl.hex> -o ipl-X.Y.Z.hex [--build-time S]
      The IPL hex to release: page 0 checked (reset, services, IVT, info
      block), the info block's build time and CRC-16 filled in.
  make_image.py info <file>
      A file's header, checked, and its image's CRC-32.

Packed: each 24-bit instruction as 3 bytes, least significant first; absent
words count as erased (0xFFFFFF). CRC-32 is zlib's. Header fields are
big-endian. --build-time defaults to SOURCE_DATE_EPOCH, else the hex file's
modification time. Standard library only.
"""
import argparse
import os
import re
import struct
import sys
import time
import zlib

TOOLS = os.path.dirname(os.path.abspath(__file__))
VERSION_HEADER = os.path.join(TOOLS, "..", "platform", "version.h")
SML_PROTO = os.path.join(TOOLS, "..", "..", "common", "sml_proto.h")
BOOT_LAYOUT = os.path.join(TOOLS, "..", "..", "common", "boot_layout.h")
SPL_VERSION_HEADER = os.path.join(TOOLS, "..", "..", "dspic_spl", "spl_version.h")

# dsPIC33EP256GP504 (silicon errata DS80000533, table 1).
DEVICE_ID = 0x1F6C

HEADER_BYTES = 64
FORMAT = 1
APP_MAGIC = b"SMOLPIC\0"
SPL_MAGIC = b"SMOLSPL\0"
ERASED = 0xFFFFFF


class ImageError(Exception):
    pass


# ---- common/boot_layout.h -------------------------------------------------------

def read_layout(path=BOOT_LAYOUT):
    """The numeric #defines of boot_layout.h, BOOT_UL() unwrapped."""
    values = {}
    with open(path) as handle:
        for line in handle:
            match = re.match(r"\s*#define\s+(\w+)\s+(?:BOOT_UL\()?(0x[0-9A-Fa-f]+|\d+)\)?", line)
            if match:
                values[match.group(1)] = int(match.group(2), 0)
    return values


L = read_layout()


def read_defines(path, names):
    found = {}
    with open(path) as handle:
        for line in handle:
            match = re.match(r"\s*#define\s+(\w+)\s+(\d+)U?\b", line)
            if match and match.group(1) in names:
                found[match.group(1)] = int(match.group(2))
    missing = [name for name in names if name not in found]
    if missing:
        raise ImageError("%s: no %s" % (path, ", ".join(missing)))
    return found


def app_version(path=VERSION_HEADER):
    d = read_defines(path, ["PIC_VERSION_MAJOR", "PIC_VERSION_MINOR", "PIC_VERSION_PATCH"])
    return version_number(d["PIC_VERSION_MAJOR"], d["PIC_VERSION_MINOR"], d["PIC_VERSION_PATCH"])


def spl_version(path=SPL_VERSION_HEADER):
    d = read_defines(path, ["SPL_VERSION_MAJOR", "SPL_VERSION_MINOR", "SPL_VERSION_PATCH"])
    return version_number(d["SPL_VERSION_MAJOR"], d["SPL_VERSION_MINOR"], d["SPL_VERSION_PATCH"])


def sml_version(path=SML_PROTO):
    d = read_defines(path, ["SML_PROTOCOL_MAJOR", "SML_PROTOCOL_MINOR"])
    return d["SML_PROTOCOL_MAJOR"] << 8 | d["SML_PROTOCOL_MINOR"]


def boot_protocol(path=SML_PROTO):
    return read_defines(path, ["SML_BOOT_PROTOCOL"])["SML_BOOT_PROTOCOL"]


def version_number(major, minor, patch):
    for part in (major, minor, patch):
        if not 0 <= part <= 255:
            raise ImageError("version part %d out of range" % part)
    return major << 16 | minor << 8 | patch


def parse_version(text):
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", text)
    if not match:
        raise ImageError("version %r: expected X.Y.Z" % text)
    return version_number(*(int(part) for part in match.groups()))


def version_text(number):
    return "%d.%d.%d" % (number >> 16 & 0xFF, number >> 8 & 0xFF, number & 0xFF)


# ---- Intel HEX --------------------------------------------------------------------

def read_hex(path):
    """Program address -> 24-bit instruction word, for every word the file
    holds (4 bytes of hex per instruction: 3 and the phantom byte)."""
    memory = {}
    upper = 0
    with open(path, "rb") as handle:
        data = handle.read()
    if data.startswith((APP_MAGIC, SPL_MAGIC)):
        raise ImageError("%s: an image file (make_image.py app or spl), not a hex: install takes the "
                         "MPLAB .hex files" % path)
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError:
        raise ImageError("%s: not Intel HEX (not text)" % path)
    for number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        if not line.startswith(":"):
            raise ImageError("%s:%d: not Intel HEX" % (path, number))
        record = bytes.fromhex(line[1:])
        if sum(record) & 0xFF:
            raise ImageError("%s:%d: checksum" % (path, number))
        length, address, kind = record[0], record[1] << 8 | record[2], record[3]
        data = record[4:4 + length]
        if kind == 0:
            for index, value in enumerate(data):
                memory[upper + address + index] = value
        elif kind == 4:
            upper = (data[0] << 8 | data[1]) << 16
        elif kind == 1:
            break
    words = {}
    for byte_address in memory:
        base = byte_address & ~3
        if base // 2 not in words:
            words[base // 2] = (memory.get(base, 0xFF) | memory.get(base + 1, 0xFF) << 8 |
                                memory.get(base + 2, 0xFF) << 16)
    return words


def write_hex(path, words):
    """Program address -> 24-bit word, as MPLAB writes it: 4 bytes an
    instruction, the phantom byte 0, 16 bytes a record."""
    memory = {}
    for address, word in words.items():
        base = address * 2
        memory[base] = word & 0xFF
        memory[base + 1] = word >> 8 & 0xFF
        memory[base + 2] = word >> 16 & 0xFF
        memory[base + 3] = 0
    lines = []
    upper = None
    addresses = sorted(memory)
    index = 0
    while index < len(addresses):
        start = addresses[index]
        if start >> 16 != upper:
            upper = start >> 16
            lines.append(bytes([2, 0, 0, 4, upper >> 8, upper & 0xFF]))
        data = [memory[start]]
        index += 1
        while (index < len(addresses) and addresses[index] == start + len(data) and len(data) < 16
               and addresses[index] >> 16 == upper):
            data.append(memory[addresses[index]])
            index += 1
        lines.append(bytes([len(data), start >> 8 & 0xFF, start & 0xFF, 0]) + bytes(data))
    lines.append(bytes([0, 0, 0, 1]))
    with open(path, "w", newline="\n") as handle:
        for record in lines:
            handle.write(":%s%02X\n" % (record.hex().upper(), -sum(record) & 0xFF))


def spans(addresses):
    out = []
    for address in sorted(addresses):
        if out and address == out[-1][1] + 2:
            out[-1][1] = address
        else:
            out.append([address, address])
    return ", ".join("0x%06X-0x%06X" % (a, b) for a, b in out)


def split_config(words):
    """The words outside program memory proper (configuration words and the
    device's ID area), and the rest."""
    config = {a: w for a, w in words.items() if a >= L["CONFIG_WORDS"]}
    code = {a: w for a, w in words.items() if a < L["CONFIG_WORDS"]}
    return code, config


def take_region(words, start, end, what):
    """The words of [start, end), refusing anything else."""
    outside = [a for a in words if not start <= a < end]
    if outside:
        raise ImageError("%s: content outside 0x%06X-0x%06X: %s" % (what, start, end - 2, spans(outside)))
    return words


def image_length(words, start):
    """Instructions from start to the last word that is not erased, even."""
    used = [a for a, w in words.items() if w != ERASED]
    if not used:
        raise ImageError("empty image")
    length = (max(used) - start) // 2 + 1
    return length + (length & 1)


def pack(words, start, length):
    out = bytearray()
    for index in range(length):
        out += struct.pack("<I", words.get(start + 2 * index, ERASED))[:3]
    return bytes(out)


def unpack(data, start):
    if len(data) % 3:
        raise ImageError("packed image not a whole number of instructions")
    return {start + 2 * (i // 3): data[i] | data[i + 1] << 8 | data[i + 2] << 16
            for i in range(0, len(data), 3)}


def goto_target(words, address):
    first, second = words.get(address), words.get(address + 2)
    if first is None or second is None or first >> 16 != 0x04 or second & 0xFFFF80:
        return None
    return (second & 0x7F) << 16 | (first & 0xFFFE)


# ---- Records --------------------------------------------------------------------

def crc16(data, crc=0xFFFF):
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = (crc << 1 ^ 0x1021) & 0xFFFF if crc & 0x8000 else crc << 1 & 0xFFFF
    return crc


def record_crc(words):
    return crc16(b"".join(struct.pack(">H", w) for w in words[:L["RECORD_CRC_WORDS"]]))


def split32(value):
    return [value >> 16 & 0xFFFF, value & 0xFFFF]


def app_record(version, start, length, crc32, build_time, sml):
    words = ([L["APP_RECORD_MAGIC_VALUE"], L["RECORD_FORMAT_1"]] + split32(version) + split32(start) +
             split32(length) + split32(crc32) + split32(build_time) + [sml, 0])
    words += [record_crc(words), 0xFFFF, L["RECORD_COMMIT_0"], L["RECORD_COMMIT_1"]]
    assert len(words) == L["APP_RECORD_WORDS"]
    return words


def spl_record(version, length, crc32, build_time, protocol, confirmed):
    words = ([L["SPL_RECORD_MAGIC_VALUE"], L["RECORD_FORMAT_1"]] + split32(version) + split32(length) +
             split32(crc32) + split32(build_time) + [protocol, 0, 0, 0])
    words += [record_crc(words), 0xFFFF, L["RECORD_COMMIT_0"], L["RECORD_COMMIT_1"]]
    if confirmed:
        words += [L["SPL_CONFIRM_0"], L["SPL_CONFIRM_1"]]
    return words


def record_instructions(address, words):
    """A record as flash holds it: a word per instruction, upper byte 0xFF.
    Words not given stay erased."""
    return {address + 2 * index: 0xFF0000 | word for index, word in enumerate(words)}


# ---- Files ------------------------------------------------------------------------

def header_crc(header60):
    return zlib.crc32(header60) & 0xFFFFFFFF


def app_header(length, version, crc32, build_time, sml):
    head = struct.pack(">8sHHIIIIIIH22x", APP_MAGIC, FORMAT, HEADER_BYTES, DEVICE_ID, L["APP_START"],
                       length, version, crc32, build_time, sml)
    return head + struct.pack(">I", header_crc(head))


def spl_header(length, version, min_ipl, protocol, crc32, build_time):
    head = struct.pack(">8sHHIIIHHIII20x", SPL_MAGIC, FORMAT, HEADER_BYTES, DEVICE_ID, version, min_ipl,
                       protocol, 0, build_time, length, crc32)
    return head + struct.pack(">I", header_crc(head))


def parse_file(data):
    """A file's header and image: a dict of its fields. Raises on any
    inconsistency."""
    if len(data) < HEADER_BYTES:
        raise ImageError("shorter than a header")
    head = data[:HEADER_BYTES]
    if struct.unpack(">I", head[60:])[0] != header_crc(head[:60]):
        raise ImageError("header CRC-32 wrong")
    magic, form, size = struct.unpack(">8sHH", head[:12])
    if form != FORMAT or size != HEADER_BYTES:
        raise ImageError("format %d, header %d bytes: not this tool's" % (form, size))
    if magic == APP_MAGIC:
        f = dict(zip(("kind", "devid", "start", "length", "version", "crc32", "build_time", "sml_version"),
                     ("app",) + struct.unpack(">IIIIIIH", head[12:38])))
    elif magic == SPL_MAGIC:
        f = dict(zip(("kind", "devid", "version", "min_ipl", "protocol", "build_time", "length", "crc32"),
                     ("spl",) + struct.unpack(">IIIH2xIII", head[12:40])))
        f["start"] = L["SPL_START"]
    else:
        raise ImageError("not an image file (magic %r)" % magic)
    image = data[HEADER_BYTES:]
    if len(image) != 3 * f["length"]:
        raise ImageError("image %d bytes, header says %d instructions" % (len(image), f["length"]))
    if zlib.crc32(image) & 0xFFFFFFFF != f["crc32"]:
        raise ImageError("image CRC-32 wrong")
    f["image"] = image
    return f


# ---- Building ---------------------------------------------------------------------

def app_words(path, warn):
    """The application's words from 0x002000, configuration words dropped."""
    words, config = split_config(read_hex(path))
    if config:
        warn("%s: configuration words dropped (%s): the IPL's" % (path, spans(config)))
    take_region(words, L["APP_START"], L["APP_END"], path)
    target = goto_target(words, L["APP_START"])
    if target is None:
        raise ImageError("%s: no GOTO at 0x%06X: not linked for the bootloader" % (path, L["APP_START"]))
    slots = sum(goto_target(words, L["APP_FORWARD_TABLE"] + 4 * i) is not None
                for i in range(L["APP_VECTORS"]))
    if slots != L["APP_VECTORS"]:
        raise ImageError("%s: %d of %d forwarding slots are GOTOs" % (path, slots, L["APP_VECTORS"]))
    if max(a for a, w in words.items() if w != ERASED) >= L["TEST_START"]:
        warn("%s: reaches the test page 0x%06X: SPL updates can no longer be tested" %
             (path, L["TEST_START"]))
    return words


def spl_words(path, warn):
    words, config = split_config(read_hex(path))
    if config:
        warn("%s: configuration words dropped (%s): the IPL's" % (path, spans(config)))
    take_region(words, L["SPL_START"], L["SPL_RECORD_ADDRESS"], path)
    if image_length(words, L["SPL_START"]) > L["SPL_CODE_MAX"]:
        raise ImageError("%s: longer than %d instructions" % (path, L["SPL_CODE_MAX"]))
    target = goto_target(words, L["SPL_START"])
    if target is None or not L["SPL_START"] + 4 <= target < L["SPL_RECORD_ADDRESS"]:
        raise ImageError("%s: no GOTO into the SPL's code at 0x%06X: not linked by spl.gld" %
                         (path, L["SPL_START"]))
    return words


def build_app(path, build_time, warn):
    words = app_words(path, warn)
    length = image_length(words, L["APP_START"])
    image = pack(words, L["APP_START"], length)
    crc = zlib.crc32(image) & 0xFFFFFFFF
    version, sml = app_version(), sml_version()
    return app_header(length, version, crc, build_time, sml) + image


def build_spl(path, version, min_ipl, build_time, warn):
    words = spl_words(path, warn)
    length = image_length(words, L["SPL_START"])
    image = pack(words, L["SPL_START"], length)
    crc = zlib.crc32(image) & 0xFFFFFFFF
    return spl_header(length, version, min_ipl, boot_protocol(), crc, build_time) + image


# ---- The IPL ----------------------------------------------------------------------

def ipl_info_crc(words):
    """CRC-16/IBM-3740 of page 0 below the info block, packed."""
    count = L["IPL_INFO_ADDRESS"] // 2
    return crc16(pack(words, L["IPL_START"], count))


def check_ipl(words, path):
    """Page 0 as the IPL must leave it: the reset GOTO and the services GOTO
    into its code, every interrupt at its forwarding slot, the traps into its
    code, the info block's magic. Returns the info block's words."""
    code = (L["IPL_SERVICES"] + 4, L["IPL_INFO_ADDRESS"])
    target = goto_target(words, L["IPL_START"])
    if target is None or not code[0] <= target < code[1]:
        raise ImageError("%s: the reset vector does not go into the IPL's code" % path)
    target = goto_target(words, L["IPL_SERVICES"])
    if target is None or not code[0] <= target < code[1]:
        raise ImageError("%s: no GOTO into the IPL's code at IPL_SERVICES 0x%06X" % (path, L["IPL_SERVICES"]))
    for index in range(L["APP_VECTORS"]):
        entry = words.get(4 + 2 * index)
        if index < 8:
            if entry is None or not code[0] <= entry < code[1]:
                raise ImageError("%s: trap vector %d not to a stub in the IPL" % (path, index))
        elif entry != L["APP_FORWARD_TABLE"] + 4 * index:
            raise ImageError("%s: IVT entry %d not to its forwarding slot" % (path, index))
    info = [words.get(L["IPL_INFO_ADDRESS"] + 2 * i, ERASED) & 0xFFFF for i in range(L["IPL_INFO_WORDS"])]
    if info[0] != L["IPL_INFO_MAGIC_VALUE"] or info[1] != L["RECORD_FORMAT_1"]:
        raise ImageError("%s: no info block at 0x%06X" % (path, L["IPL_INFO_ADDRESS"]))
    return info


def build_ipl(path, build_time, warn):
    """The release hex: checked, with the info block's build time and CRC."""
    words, config = split_config(read_hex(path))
    take_region(words, L["IPL_START"], L["IPL_END"], path)
    if not config:
        warn("%s: no configuration words: the IPL's hex should hold them" % path)
    info = check_ipl(words, path)

    def put(index, value):
        address = L["IPL_INFO_ADDRESS"] + 2 * index
        words[address] = (words.get(address, ERASED) & 0xFF0000) | value

    put(L["IPL_INFO_BUILD_TIME"], build_time >> 16 & 0xFFFF)
    put(L["IPL_INFO_BUILD_TIME"] + 1, build_time & 0xFFFF)
    put(L["IPL_INFO_CRC"], ipl_info_crc(words))
    version = info[L["IPL_INFO_VERSION"]] << 16 | info[L["IPL_INFO_VERSION"] + 1]
    used = [a for a in words if a < L["IPL_INFO_ADDRESS"]]
    summary = "IPL %s, %d instructions below the info block, CRC-16 0x%04X" % (
        version_text(version), len(used), words[L["IPL_INFO_ADDRESS"] + 2 * L["IPL_INFO_CRC"]] & 0xFFFF)
    words.update(config)
    return words, summary


def build_install(ipl_path, spl_path, app_path, spl_version, build_time, warn):
    ipl, config = split_config(read_hex(ipl_path))
    take_region(ipl, L["IPL_START"], L["IPL_END"], ipl_path)
    if not config:
        raise ImageError("%s: no configuration words: the IPL's hex must hold them" % ipl_path)
    info = check_ipl(ipl, ipl_path)
    if info[L["IPL_INFO_CRC"]] != ipl_info_crc(ipl):
        warn("%s: the info block's CRC-16 is not filled in: not a released IPL (make_image.py ipl)" % ipl_path)
    app = app_words(app_path, warn)
    out = dict(ipl)
    out.update(config)

    if spl_path is not None:
        spl = spl_words(spl_path, warn)
        length = image_length(spl, L["SPL_START"])
        crc = zlib.crc32(pack(spl, L["SPL_START"], length)) & 0xFFFFFFFF
        out.update(spl)
        out.update(record_instructions(L["SPL_RECORD_ADDRESS"],
                                       spl_record(spl_version, length, crc, build_time, boot_protocol(), True)))

    length = image_length(app, L["APP_START"])
    crc = zlib.crc32(pack(app, L["APP_START"], length)) & 0xFFFFFFFF
    out.update(app)
    out.update(record_instructions(L["APP_RECORD_ADDRESS"],
                                   app_record(app_version(), L["APP_START"], length, crc, build_time,
                                              sml_version())))
    return out


def describe(f):
    lines = ["%s image, DEVID 0x%04X, %s, %d instructions from 0x%06X, CRC-32 0x%08X" %
             ("Application" if f["kind"] == "app" else "SPL", f["devid"], version_text(f["version"]),
              f["length"], f["start"], f["crc32"]),
             "built %s" % time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(f["build_time"]))]
    if f["kind"] == "app":
        lines.append("SML %d.%d" % (f["sml_version"] >> 8, f["sml_version"] & 0xFF))
    else:
        lines.append("bootloader protocol %d, IPL %s or later" % (f["protocol"], version_text(f["min_ipl"])))
    return "\n".join(lines)


def default_build_time(path):
    if "SOURCE_DATE_EPOCH" in os.environ:
        return int(os.environ["SOURCE_DATE_EPOCH"])
    return int(os.path.getmtime(path))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="action", required=True)
    p = sub.add_parser("app")
    p.add_argument("hex")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("--build-time", type=int)
    p = sub.add_parser("spl")
    p.add_argument("hex")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("--version")
    p.add_argument("--min-ipl", default="0.0.0")
    p.add_argument("--build-time", type=int)
    p = sub.add_parser("install")
    p.add_argument("--ipl", required=True)
    p.add_argument("--spl")
    p.add_argument("--app", required=True)
    p.add_argument("--spl-version")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("--build-time", type=int)
    p = sub.add_parser("ipl")
    p.add_argument("hex")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("--build-time", type=int)
    p = sub.add_parser("info")
    p.add_argument("file")
    args = parser.parse_args(argv)

    def warn(text):
        print("warning: " + text, file=sys.stderr)

    try:
        if args.action == "info":
            with open(args.file, "rb") as handle:
                print(describe(parse_file(handle.read())))
            return 0
        if args.action == "ipl":
            words, summary = build_ipl(args.hex, args.build_time or default_build_time(args.hex), warn)
            write_hex(args.output, words)
            print("wrote %s: %s" % (args.output, summary))
            return 0
        if args.action == "app":
            data = build_app(args.hex, args.build_time or default_build_time(args.hex), warn)
        elif args.action == "spl":
            data = build_spl(args.hex, parse_version(args.version) if args.version else spl_version(),
                             parse_version(args.min_ipl),
                             args.build_time or default_build_time(args.hex), warn)
        else:
            if args.spl is None and args.spl_version is not None:
                raise ImageError("--spl-version without --spl")
            words = build_install(args.ipl, args.spl, args.app,
                                  (parse_version(args.spl_version) if args.spl_version else spl_version())
                                  if args.spl else None,
                                  args.build_time or default_build_time(args.app), warn)
            write_hex(args.output, words)
            print("wrote %s: IPL, %sapplication (committed), configuration words"
                  % (args.output, "SPL (committed, confirmed), " if args.spl else "no SPL, "))
            return 0
        with open(args.output, "wb") as handle:
            handle.write(data)
        print("wrote %s" % args.output)
        print(describe(parse_file(data)))
        return 0
    except (ImageError, OSError) as error:
        print("make_image: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
