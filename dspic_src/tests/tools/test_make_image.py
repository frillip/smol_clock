"""tools/make_image.py: the image files, the first-installation hex and the
records, against synthetic hex files and, when given, real ones.

Run from dspic_src: python3 -m unittest discover -s tests/tools -t tests/tools
With the phase 0 link test's builds (application at 0x002000, stand-in IPL):
  MAKE_IMAGE_APP_HEX=... MAKE_IMAGE_IPL_HEX=... python3 -m unittest ...
"""
import os
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
import make_dist  # noqa: E402
import make_image as mi  # noqa: E402

L = mi.L


def even(count):
    return count + (count & 1)


def goto(target):
    return [0x040000 | (target & 0xFFFE), target >> 16 & 0x7F]


def app_words(code=200, extra=None):
    """A minimal application: the entry GOTO, the table, some code."""
    words = {}
    first, second = goto(0x002400)
    words[0x2000], words[0x2002] = first, second
    for index in range(L["APP_VECTORS"]):
        first, second = goto(0x002500 if index in (11, 15) else 0x002600)
        words[L["APP_FORWARD_TABLE"] + 4 * index] = first
        words[L["APP_FORWARD_TABLE"] + 4 * index + 2] = second
    for index in range(code):
        words[0x2400 + 2 * index] = (index * 7919) & 0xFFFFFF
    words.update(extra or {})
    return words


def ipl_words(info=True):
    """A minimal IPL: reset and services GOTOs into its code, traps to stubs,
    every interrupt at its slot, the info block (build time and CRC erased)."""
    words = {}
    for address, target in ((0x0000, 0x0300), (0x0200, 0x0320)):
        words[address], words[address + 2] = goto(target)
    for index in range(L["APP_VECTORS"]):
        words[4 + 2 * index] = 0x0400 + 0x10 * index if index < 8 else L["APP_FORWARD_TABLE"] + 4 * index
    for index in range(100):
        words[0x0300 + 2 * index] = (index * 12345) & 0xFFFFFF
    if info:
        for index, value in enumerate([0x4950, 1, 0, 0x0100, 1, 0, 0xFFFF, 0xFFFF, 0xFFFF]):
            words[L["IPL_INFO_ADDRESS"] + 2 * index] = value
    words[0x2AFF0], words[0x2AFF4] = 0xFFCE, 0xFF7A
    return words


class Files(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.warnings = []

    def tearDown(self):
        self.dir.cleanup()

    def path(self, name):
        return os.path.join(self.dir.name, name)

    def hexfile(self, name, words):
        path = self.path(name)
        mi.write_hex(path, words)
        return path

    def warn(self, text):
        self.warnings.append(text)


class TestHex(Files):
    def test_round_trip(self):
        words = {0: 0x040206, 2: 0, 0x200: 0xDA4000, 0x1FFFE: 0x123456, 0x20000: 0xABCDEF, 0x2AFF0: 0xFFCE}
        self.assertEqual(mi.read_hex(self.hexfile("a.hex", words)), words)

    def test_checksum(self):
        path = self.path("bad.hex")
        with open(path, "w") as handle:
            handle.write(":0400000000000000FF\n")
        with self.assertRaises(mi.ImageError):
            mi.read_hex(path)

    def test_pack(self):
        words = {0x2000: 0x333231, 0x2002: 0x363534, 0x2004: 0x393837}
        self.assertEqual(mi.pack(words, 0x2000, 3), b"123456789")
        self.assertEqual(zlib.crc32(mi.pack(words, 0x2000, 3)), 0xCBF43926)
        self.assertEqual(mi.pack({}, 0x2000, 2), b"\xff" * 6)
        self.assertEqual(mi.unpack(b"123456789", 0x2000), words)


class TestApp(Files):
    def test_image(self):
        words = app_words()
        words[0x2AFF0] = 0xFFCE  # configuration words: dropped
        data = mi.build_app(self.hexfile("app.hex", words), 1793000000, self.warn)
        f = mi.parse_file(data)
        self.assertEqual(f["kind"], "app")
        self.assertEqual(f["devid"], mi.DEVICE_ID)
        self.assertEqual(f["start"], 0x2000)
        last = 0x2400 + 2 * 199
        self.assertEqual(f["length"], even((last - 0x2000) // 2 + 1))
        self.assertEqual(f["length"] % 2, 0)
        self.assertEqual(f["version"], mi.app_version())
        self.assertEqual(f["sml_version"], mi.sml_version())
        self.assertEqual(f["sml_version"] >> 8, 2)
        self.assertEqual(f["build_time"], 1793000000)
        self.assertEqual(mi.unpack(f["image"], 0x2000)[0x2000], words[0x2000])
        self.assertEqual(f["crc32"], zlib.crc32(f["image"]))
        self.assertTrue(any("configuration" in w for w in self.warnings))
        self.assertEqual(len(data), 64 + 3 * f["length"])
        # Header layout, as the specification's table.
        self.assertEqual(data[:8], b"SMOLPIC\0")
        self.assertEqual(struct.unpack(">HHIII", data[8:24]), (1, 64, mi.DEVICE_ID, 0x2000, f["length"]))

    def test_trailing_erased_trimmed(self):
        words = app_words(code=10, extra={0x2400 + 2 * 50: mi.ERASED})
        f = mi.parse_file(mi.build_app(self.hexfile("app.hex", words), 0, self.warn))
        self.assertEqual(f["length"], even((0x2400 + 18 - 0x2000) // 2 + 1))

    def test_refusals(self):
        cases = {
            "below": app_words(extra={0x200: 0x000000}),
            "record page": app_words(extra={0x29000: 0x000000}),
            "store": app_words(extra={0x29800: 0x000000}),
        }
        for name, words in cases.items():
            with self.subTest(name), self.assertRaises(mi.ImageError):
                mi.build_app(self.hexfile(name + ".hex", words), 0, self.warn)
        words = app_words()
        words[0x2000] = 0x000000
        with self.assertRaises(mi.ImageError):
            mi.build_app(self.hexfile("nogoto.hex", words), 0, self.warn)
        words = app_words()
        del words[L["APP_FORWARD_TABLE"] + 4 * 100]
        with self.assertRaises(mi.ImageError):
            mi.build_app(self.hexfile("noslot.hex", words), 0, self.warn)

    def test_test_page_warning(self):
        words = app_words(extra={L["TEST_START"]: 0x000000})
        mi.build_app(self.hexfile("big.hex", words), 0, self.warn)
        self.assertTrue(any("test page" in w for w in self.warnings))

    def test_tampering(self):
        data = bytearray(mi.build_app(self.hexfile("app.hex", app_words()), 0, self.warn))
        for offset in (20, 100, len(data) - 1):
            bad = bytearray(data)
            bad[offset] ^= 1
            with self.subTest(offset), self.assertRaises(mi.ImageError):
                mi.parse_file(bytes(bad))
        with self.assertRaises(mi.ImageError):
            mi.parse_file(bytes(data[:-3]))


def spl_words(count=4):
    """An SPL: the entry GOTO, then code."""
    words = {0x800 + 2 * i: (i * 31) & 0xFFFFFF for i in range(count)}
    words[0x800], words[0x802] = goto(0x000900)
    return words


class TestSpl(Files):
    def spl(self, count):
        return spl_words(count)

    def test_image(self):
        data = mi.build_spl(self.hexfile("spl.hex", self.spl(1700)), 0x000102, 0x010000, 5, self.warn)
        f = mi.parse_file(data)
        self.assertEqual((f["kind"], f["version"], f["min_ipl"], f["protocol"]), ("spl", 0x000102, 0x010000, 1))
        self.assertEqual(f["length"], 1700)
        self.assertEqual(data[:8], b"SMOLSPL\0")
        self.assertEqual(struct.unpack(">IIIHHIII", data[12:40]),
                         (mi.DEVICE_ID, 0x000102, 0x010000, 1, 0, 5, 1700, f["crc32"]))

    def test_limits(self):
        mi.build_spl(self.hexfile("full.hex", self.spl(3008)), 1, 0, 0, self.warn)
        with self.assertRaises(mi.ImageError):  # into the record row
            mi.build_spl(self.hexfile("long.hex", self.spl(3009)), 1, 0, 0, self.warn)
        with self.assertRaises(mi.ImageError):
            mi.build_spl(self.hexfile("low.hex", {0x7FE: 0}), 1, 0, 0, self.warn)
        with self.assertRaises(mi.ImageError):  # no entry GOTO
            mi.build_spl(self.hexfile("noentry.hex", {0x800 + 2 * i: 0 for i in range(10)}), 1, 0, 0,
                         self.warn)
        with self.assertRaises(mi.ImageError):  # a GOTO out of the SPL
            words = self.spl(100)
            words[0x800], words[0x802] = goto(0x002000)
            mi.build_spl(self.hexfile("out.hex", words), 1, 0, 0, self.warn)

    def test_image_file_given_as_hex(self):
        data = mi.build_spl(self.hexfile("s.hex", self.spl(50)), 1, 0, 0, self.warn)
        with open(self.path("spl-0.1.0.hex"), "wb") as handle:
            handle.write(data)
        with self.assertRaisesRegex(mi.ImageError, "an image file"):
            mi.read_hex(self.path("spl-0.1.0.hex"))
        with open(self.path("binary.hex"), "wb") as handle:
            handle.write(b":\x81\x00")
        with self.assertRaisesRegex(mi.ImageError, "not text"):
            mi.read_hex(self.path("binary.hex"))

    def test_version_from_header(self):
        header = self.path("spl_version.h")
        with open(header, "w") as handle:
            handle.write("#define SPL_VERSION_MAJOR 1\n#define SPL_VERSION_MINOR 2\n"
                         "#define SPL_VERSION_PATCH 3\n")
        self.assertEqual(mi.spl_version(header), 0x010203)
        expected = mi.spl_version()
        self.assertEqual(mi.main(["spl", self.hexfile("s.hex", self.spl(50)), "-o", self.path("s.smolspl")]), 0)
        with open(self.path("s.smolspl"), "rb") as handle:
            self.assertEqual(mi.parse_file(handle.read())["version"], expected)


class TestRecords(unittest.TestCase):
    def test_record_crc_matches_c(self):
        # The same record as dspic_src/tests/host/test_boot_layout.c.
        words = mi.app_record(0x040000, 0x2000, 0xE0E0, 0x12345678, 0x6A081F00, 0x0205)
        self.assertEqual(words[:14], [0x534D, 1, 0x0004, 0, 0, 0x2000, 0, 0xE0E0, 0x1234, 0x5678,
                                      0x6A08, 0x1F00, 0x0205, 0])
        self.assertEqual(words[L["RECORD_CRC"]], 0x3748)
        self.assertEqual(words[16:18], [0xA55A, 0x5AA5])

    def test_spl_record(self):
        words = mi.spl_record(0x000102, 1700, 0xDEADBEEF, 5, 1, True)
        self.assertEqual(len(words), L["SPL_RECORD_REJECT"])  # reject left erased
        self.assertEqual(words[L["SPL_RECORD_CONFIRM"]:], [0xC33C, 0x3CC3])
        self.assertEqual(words[L["SPL_RECORD_LENGTH"]:L["SPL_RECORD_LENGTH"] + 2], [0, 1700])
        self.assertEqual(words[L["RECORD_CRC"]], mi.record_crc(words))
        self.assertEqual(len(mi.spl_record(1, 2, 3, 4, 1, False)), L["SPL_RECORD_CONFIRM"])

    def test_version(self):
        self.assertEqual(mi.parse_version("0.4.0"), 0x000400)
        self.assertEqual(mi.version_text(0x010203), "1.2.3")
        for bad in ("1.2", "1.2.3.4", "1.256.0", "x"):
            with self.subTest(bad), self.assertRaises(mi.ImageError):
                mi.parse_version(bad)


class TestInstall(Files):
    def test_install(self):
        ipl = ipl_words()
        spl = spl_words(100)
        app = app_words()
        app[0x2AFF0] = 0xFFFF  # the application's own: dropped for the IPL's
        words = mi.build_install(self.hexfile("ipl.hex", ipl), self.hexfile("spl.hex", spl),
                                 self.hexfile("app.hex", app), 0x000100, 1793000000, self.warn)
        out = mi.read_hex(self.hexfile("out.hex", words))
        self.assertEqual(out[0x2AFF0], 0xFFCE)
        self.assertEqual(out[0x2AFF4], 0xFF7A)
        self.assertEqual(out[0x300], 0)
        self.assertTrue(any("not a released IPL" in w for w in self.warnings))
        # The SPL record: committed and confirmed, reject erased.
        record = [out[L["SPL_RECORD_ADDRESS"] + 2 * i] for i in range(L["SPL_RECORD_REJECT"])]
        self.assertTrue(all(w >> 16 == 0xFF for w in record))
        record = [w & 0xFFFF for w in record]
        self.assertEqual(record[0], 0x5350)
        self.assertEqual(record[L["RECORD_CRC"]], mi.record_crc(record))
        self.assertEqual(record[16:20], [0xA55A, 0x5AA5, 0xC33C, 0x3CC3])
        self.assertNotIn(L["SPL_RECORD_ADDRESS"] + 2 * L["SPL_RECORD_REJECT"], out)
        length = record[L["SPL_RECORD_LENGTH"] + 1]
        self.assertEqual(length, 100)
        crc = record[L["SPL_RECORD_CRC32"]] << 16 | record[L["SPL_RECORD_CRC32"] + 1]
        self.assertEqual(crc, zlib.crc32(mi.pack(out, 0x800, length)))
        # The application record: committed, its CRC over the image in the hex.
        record = [out[L["APP_RECORD_ADDRESS"] + 2 * i] & 0xFFFF for i in range(L["APP_RECORD_WORDS"])]
        self.assertEqual(record[0], 0x534D)
        self.assertEqual(record[L["RECORD_CRC"]], mi.record_crc(record))
        self.assertEqual(record[16:18], [0xA55A, 0x5AA5])
        length = record[6] << 16 | record[7]
        crc = record[8] << 16 | record[9]
        self.assertEqual(crc, zlib.crc32(mi.pack(out, 0x2000, length)))
        self.assertEqual(record[2] << 16 | record[3], mi.app_version())

    def test_install_without_spl(self):
        words = mi.build_install(self.hexfile("ipl.hex", ipl_words()), None, self.hexfile("app.hex", app_words()),
                                 None, 0, self.warn)
        self.assertFalse(any(L["SPL_START"] <= a < L["SPL_END"] for a in words))
        self.assertEqual(words[L["APP_RECORD_ADDRESS"]] & 0xFFFF, 0x534D)
        self.assertEqual(mi.main(["install", "--ipl", self.hexfile("i.hex", ipl_words()), "--app",
                                  self.hexfile("a.hex", app_words()), "--spl-version", "1.0.0", "-o",
                                  self.path("x.hex")]), 1)

    def test_install_needs_config(self):
        ipl = {a: w for a, w in ipl_words().items() if a < 0x2A000}
        with self.assertRaises(mi.ImageError):
            mi.build_install(self.hexfile("ipl.hex", ipl), self.hexfile("spl.hex", spl_words()),
                             self.hexfile("app.hex", app_words()), 1, 0, self.warn)


class TestIpl(Files):
    def test_release(self):
        words, summary = mi.build_ipl(self.hexfile("ipl.hex", ipl_words()), 0x6A081F00, self.warn)
        info = [words[L["IPL_INFO_ADDRESS"] + 2 * i] & 0xFFFF for i in range(L["IPL_INFO_WORDS"])]
        self.assertEqual(info[6:8], [0x6A08, 0x1F00])
        # The CRC-16 over page 0 below the block, packed, erased words as 0xFF.
        packed = bytes(b for a in range(0, L["IPL_INFO_ADDRESS"], 2)
                       for b in (words.get(a, 0xFFFFFF).to_bytes(4, "little")[:3]))
        self.assertEqual(len(packed), 3 * 960)
        self.assertEqual(info[8], mi.crc16(packed))
        self.assertIn("IPL 0.1.0", summary)
        self.assertEqual(words[0x2AFF0], 0xFFCE)
        # Released: install takes it without a warning about it.
        self.warnings.clear()
        mi.build_install(self.hexfile("rel.hex", words), self.hexfile("spl.hex", spl_words()),
                         self.hexfile("app.hex", app_words()), 1, 0, self.warn)
        self.assertFalse(any("released" in w for w in self.warnings))

    def test_refusals(self):
        cases = {}
        words = ipl_words()
        words[0x0002] = 0x000001  # reset not a GOTO
        cases["reset"] = words
        words = ipl_words()
        words[0x0200], words[0x0202] = goto(0x2000)  # services out of the IPL
        cases["services"] = words
        words = ipl_words()
        words[4 + 2 * 20] = 0x002000  # an interrupt not at its slot
        cases["ivt"] = words
        words = ipl_words()
        words[4] = 0x002004  # a trap straight to the application
        cases["trap"] = words
        cases["no info"] = ipl_words(info=False)
        words = ipl_words()
        words[0x0800] = 0
        cases["outside"] = words
        for name, words in cases.items():
            with self.subTest(name), self.assertRaises(mi.ImageError):
                mi.build_ipl(self.hexfile(name + ".hex", words), 0, self.warn)


@unittest.skipUnless(os.environ.get("MAKE_IMAGE_APP_HEX"), "no real application hex given")
class TestRealHex(Files):
    def test_application(self):
        path = os.environ["MAKE_IMAGE_APP_HEX"]
        data = mi.build_app(path, 0, self.warn)
        f = mi.parse_file(data)
        print("\n  %s" % mi.describe(f).replace("\n", ", "))
        self.assertGreater(f["length"], 50000)

    @unittest.skipUnless(os.environ.get("MAKE_IMAGE_IPL_HEX"), "no IPL hex given")
    def test_stand_in_ipl_refused(self):
        # The phase 0 stand-in IPL: no configuration words, no services, no info block.
        with self.assertRaises(mi.ImageError):
            mi.build_ipl(os.environ["MAKE_IMAGE_IPL_HEX"], 0, self.warn)
        with self.assertRaises(mi.ImageError):
            mi.build_install(os.environ["MAKE_IMAGE_IPL_HEX"], self.hexfile("spl.hex", spl_words()),
                             os.environ["MAKE_IMAGE_APP_HEX"], 1, 0, self.warn)


class TestCommandLine(Files):
    def test_cli(self):
        app = self.hexfile("app.hex", app_words())
        out = self.path("app.smolpic")
        self.assertEqual(mi.main(["app", app, "-o", out, "--build-time", "7"]), 0)
        self.assertEqual(mi.main(["info", out]), 0)
        bad = self.path("bad.smolpic")
        with open(out, "rb") as source, open(bad, "wb") as sink:
            data = bytearray(source.read())
            data[-1] ^= 0xFF
            sink.write(data)
        self.assertEqual(mi.main(["info", bad]), 1)


class ReleasedIpl(Files):
    """make_dist.py's released IPL: the newest in dspic_ipl/release/, copied
    as it is, refused unless its info block was filled in."""

    def released(self, name, info_crc=True):
        words, _ = mi.build_ipl(self.hexfile("build.hex", ipl_words()), 0x6A081F00, self.warn)
        if not info_crc:
            words[L["IPL_INFO_ADDRESS"] + 2 * L["IPL_INFO_CRC"]] ^= 1
        os.makedirs(self.path("release"), exist_ok=True)
        path = os.path.join(self.path("release"), name)
        mi.write_hex(path, words)
        return path

    def test_newest_copied_as_is(self):
        saved = make_dist.IPL_RELEASE
        make_dist.IPL_RELEASE = self.path("release")
        try:
            self.assertIsNone(make_dist.released_ipl())
            self.released("ipl-0.9.0.hex")
            newest = self.released("ipl-1.10.0.hex")
            self.released("ipl-1.2.0.hex")
            self.assertEqual(make_dist.released_ipl(), newest)
            os.makedirs(self.path("out"))
            version, copied = make_dist.copy_released_ipl(newest, self.path("out"))
            with open(newest, "rb") as a, open(copied, "rb") as b:
                self.assertEqual(a.read(), b.read())
            self.assertEqual(os.path.basename(copied), "ipl-%s.hex" % version)
        finally:
            make_dist.IPL_RELEASE = saved

    def test_not_released_refused(self):
        path = self.released("ipl-1.0.0.hex", info_crc=False)
        os.makedirs(self.path("out"))
        with self.assertRaises(make_dist.DistError):
            make_dist.copy_released_ipl(path, self.path("out"))


if __name__ == "__main__":
    unittest.main()
