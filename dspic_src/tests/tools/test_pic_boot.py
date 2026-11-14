"""tools/pic_boot.py against the SPL's core on a pipe (dspic_spl's host
simulator, spl_sim): the whole update as the ESP will do it, from a clock
running its application, and the other commands.

Run from dspic_src, after make in ../dspic_spl/tests/host:
  python3 -m unittest discover -s tests/tools -t tests/tools
Skipped when spl_sim has not been built.
"""
import contextlib
import io
import os
import random
import struct
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
import make_image as mi  # noqa: E402
import pic_boot  # noqa: E402

L = mi.L
SIM = os.path.normpath(os.path.join(HERE, "..", "..", "..", "dspic_spl", "tests", "host", "spl_sim"))
FLASH_WORDS = 0x02B000 // 2
ERASED = 0xFFFFFF


def image_words(length, seed):
    rng = random.Random(seed)
    words = [rng.randrange(0, 0x1000000) & 0xFFFFFE for _ in range(length)]
    for index in range(64 * 3, 64 * 4):  # a blank chunk, skipped
        words[index] = ERASED
    return words


def pack(words):
    return b"".join(struct.pack("<I", w)[:3] for w in words)


def app_file(words, version, build_time=1793000000):
    image = pack(words)
    return mi.app_header(len(words), version, zlib.crc32(image) & 0xFFFFFFFF, build_time, 0x0205) + image


def clock_flash(path, app, version, spl_confirmed=True):
    """The clock as programmed: the IPL's info block, the SPL and its record,
    an application and its record."""
    flash = [ERASED] * FLASH_WORDS

    def put(address, word):
        flash[address // 2] = word

    for index, word in enumerate([L["IPL_INFO_MAGIC_VALUE"], 1, 0, 0x0100]):
        put(L["IPL_INFO_ADDRESS"] + 2 * index, 0xFF0000 | word)
    spl = image_words(1700, 7)
    for index, word in enumerate(spl):
        put(L["SPL_START"] + 2 * index, word)
    record = mi.spl_record(0x000100, len(spl), zlib.crc32(pack(spl)) & 0xFFFFFFFF, 1793000000, 1, spl_confirmed)
    for address, word in mi.record_instructions(L["SPL_RECORD_ADDRESS"], record).items():
        put(address, word)
    if app is not None:
        for index, word in enumerate(app):
            put(L["APP_START"] + 2 * index, word)
        record = mi.app_record(version, L["APP_START"], len(app), zlib.crc32(pack(app)) & 0xFFFFFFFF,
                               1793000000, 0x0205)
        for address, word in mi.record_instructions(L["APP_RECORD_ADDRESS"], record).items():
            put(address, word)
    with open(path, "wb") as handle:
        handle.write(struct.pack("<%dI" % FLASH_WORDS, *flash))


def read_flash(path):
    with open(path, "rb") as handle:
        return struct.unpack("<%dI" % FLASH_WORDS, handle.read())


@unittest.skipUnless(os.path.exists(SIM), "spl_sim not built (make in dspic_spl/tests/host)")
class PicBootTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.flash = os.path.join(self.dir.name, "flash.bin")
        self.old = image_words(3000, 1)
        self.new = image_words(4100, 2)
        self.file = os.path.join(self.dir.name, "new.smolpic")
        with open(self.file, "wb") as handle:
            handle.write(app_file(self.new, 0x000400))

    def tearDown(self):
        self.dir.cleanup()

    def tool(self, *args, app=False):
        out = io.StringIO()
        err = io.StringIO()
        argv = ["--sim", SIM, "--flash", self.flash] + (["--sim-app"] if app else []) + list(args)
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = pic_boot.main(argv)
        return code, out.getvalue() + err.getvalue()

    def test_update_from_the_application(self):
        clock_flash(self.flash, self.old, 0x000301)
        code, text = self.tool("update", self.file, app=True)
        self.assertEqual(code, 0, text)
        self.assertIn("asking the application for the bootloader", text)
        self.assertIn("running 0.4.0", text)
        flash = read_flash(self.flash)
        base = L["APP_START"] // 2
        self.assertEqual(list(flash[base:base + len(self.new)]), self.new)
        record = L["APP_RECORD_ADDRESS"] // 2
        self.assertEqual(flash[record], 0xFF0000 | L["APP_RECORD_MAGIC_VALUE"])
        self.assertEqual(flash[record + 16:record + 18], (0xFFA55A, 0xFF5AA5))
        self.assertEqual(flash[record + 2:record + 4], (0xFF0000, 0xFF0400))

        code, text = self.tool("update", self.file, app=True)
        self.assertEqual(code, 0, text)
        self.assertIn("already installed: 0.4.0", text)
        code, text = self.tool("verify", self.file)
        self.assertEqual(code, 0, text)
        self.assertIn("matches", text)

    def test_hold_status_and_read(self):
        clock_flash(self.flash, self.old, 0x000301)
        code, text = self.tool("hold")
        self.assertEqual(code, 0, text)
        self.assertIn("mode HELD", text)
        self.assertIn("IMAGE_OK", text)
        self.assertIn("application 0.3.1, 3000 instructions", text)
        self.assertIn("SPL 0.1.0, RECORD COMMITTED INTACT CONFIRMED", text)
        code, text = self.tool("read", "0x2000", "1")
        self.assertEqual(code, 0, text)
        self.assertTrue(text.startswith("002000: %06X %06X" % tuple(self.old[:2])), text)
        code, text = self.tool("verify", self.file)
        self.assertEqual(code, 1)
        self.assertIn("chunks differ, first 0x002000", text)

    def test_test_page(self):
        clock_flash(self.flash, self.old, 0x000301)
        code, text = self.tool("test")
        self.assertEqual(code, 0, text)
        self.assertIn("all good", text)
        flash = read_flash(self.flash)
        self.assertTrue(all(w == ERASED for w in flash[L["TEST_START"] // 2:L["APP_RECORD_ADDRESS"] // 2]))
        base = L["APP_START"] // 2
        self.assertEqual(list(flash[base:base + len(self.old)]), self.old)

    def test_no_application(self):
        clock_flash(self.flash, None, 0)
        code, text = self.tool("status")
        self.assertIn("mode NO_APP", text)
        code, text = self.tool("update", self.file)
        self.assertEqual(code, 0, text)
        self.assertIn("running 0.4.0", text)

    def test_unconfirmed_spl(self):
        clock_flash(self.flash, self.old, 0x000301, spl_confirmed=False)
        code, text = self.tool("update", self.file, app=True)
        self.assertEqual(code, 1)
        self.assertIn("the SPL is not confirmed", text)
        code, text = self.tool("update", self.file, "--force")
        self.assertEqual(code, 1)
        self.assertIn("ERASE: NACK STATE", text)
        base = L["APP_START"] // 2
        self.assertEqual(list(read_flash(self.flash)[base:base + len(self.old)]), self.old)


if __name__ == "__main__":
    unittest.main()
