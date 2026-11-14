#!/usr/bin/env python3
"""make_manifest.py: versions read from the files, sizes and SHA-256s, the
checks, and --merge."""
import contextlib
import hashlib
import io
import json
import os
import struct
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
import make_image as mi  # noqa: E402
import make_manifest  # noqa: E402

BASE = "https://example.org/smol/"


def app_file(version, instructions=4):
    image = bytes(range(3 * instructions))
    return mi.app_header(instructions, version, zlib.crc32(image), 1700000000, 0x0205) + image


def spl_file(version, instructions=4):
    image = bytes(3 * instructions)
    return mi.spl_header(instructions, version, 0x000100, 1, zlib.crc32(image), 1700000000) + image


def esp_file(version):
    data = bytearray(0x200)
    data[0] = 0xE9
    data[0x20:0x24] = struct.pack("<I", 0xABCD5432)
    agent = b"SMOL CLOCK V2/" + version.encode() + b"\0"
    data[0x100:0x100 + len(agent)] = agent
    return bytes(data)


class MakeManifest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.dir.cleanup()

    def write(self, name, data):
        path = os.path.join(self.dir.name, name)
        with open(path, "wb") as handle:
            handle.write(data)
        return path

    def run_tool(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = make_manifest.main(list(args))
        return code, out.getvalue(), err.getvalue()

    def test_versions_from_the_files(self):
        esp = self.write("esp_src.ino.bin", esp_file("0.4.0"))
        pic = self.write("app-9.9.9.smolpic", app_file(0x000401))
        spl = self.write("spl.smolspl", spl_file(0x000102))
        code, out, err = self.run_tool("--base-url", BASE.rstrip("/"), "--esp", esp, "--pic", pic,
                                       "--pic-spl", spl)
        self.assertEqual(code, 0, err)
        manifest = json.loads(out)
        self.assertEqual(manifest["format"], 2)
        entry = manifest["configurations"][0]
        self.assertEqual(entry["hardware"], "v2")
        self.assertNotIn("config", entry)
        self.assertEqual(entry["esp"]["version"], "0.4.0")
        self.assertEqual(entry["pic"]["version"], "0.4.1")
        self.assertEqual(entry["pic_spl"]["version"], "0.1.2")
        self.assertEqual(entry["pic"]["url"], BASE + "app-9.9.9.smolpic")
        with open(pic, "rb") as handle:
            data = handle.read()
        self.assertEqual(entry["pic"]["size"], len(data))
        self.assertEqual(entry["pic"]["sha256"], hashlib.sha256(data).hexdigest())
        self.assertIn("named 9.9.9, but holds 0.4.1", err)

    def test_refusals(self):
        bad = bytearray(app_file(0x000401))
        bad[70] ^= 1
        pic = self.write("app.smolpic", bytes(bad))
        self.assertEqual(self.run_tool("--base-url", BASE, "--pic", pic)[0], 1)
        spl = self.write("spl.smolspl", spl_file(0x000100))
        code, _, err = self.run_tool("--base-url", BASE, "--pic", spl)
        self.assertEqual(code, 1)
        self.assertIn("not .smolapp", err)
        esp = self.write("esp.bin", b"\0" * 0x200)
        self.assertEqual(self.run_tool("--base-url", BASE, "--esp", esp)[0], 1)
        two = bytearray(esp_file("0.4.0"))
        two[0x180:0x194] = b"SMOL CLOCK V2/0.5.0\0"
        esp = self.write("esp.bin", bytes(two))
        code, _, err = self.run_tool("--base-url", BASE, "--esp", esp)
        self.assertIn("2 user agent versions", err)
        code, out, _ = self.run_tool("--base-url", BASE, "--esp", esp, "--esp-version", "0.5.0")
        self.assertEqual(json.loads(out)["configurations"][0]["esp"]["version"], "0.5.0")
        self.assertEqual(self.run_tool("--base-url", BASE)[0], 1)

    def test_merge(self):
        pic = self.write("app.smolpic", app_file(0x000401))
        manifest = self.write("manifest.json", b"")
        code, _, err = self.run_tool("--base-url", BASE, "--pic", pic, "-o", manifest)
        self.assertEqual(code, 0, err)
        self.run_tool("--base-url", BASE, "--pic", pic, "--config", "development",
                      "--merge", manifest, "-o", manifest)
        self.run_tool("--base-url", BASE, "--pic", pic, "--device", "ec:64:c9:00:00:01",
                      "--merge", manifest, "-o", manifest)
        newer = self.write("app2.smolpic", app_file(0x000402))
        code, _, err = self.run_tool("--base-url", BASE, "--pic", newer, "--merge", manifest,
                                     "-o", manifest)
        self.assertIn("replaced configuration 2", err)
        with open(manifest) as handle:
            entries = json.load(handle)["configurations"]
        self.assertEqual(entries[0]["devices"], ["EC:64:C9:00:00:01"])
        self.assertNotIn("config", entries[1])
        self.assertEqual(entries[1]["pic"]["version"], "0.4.2")
        self.assertEqual(entries[2]["config"], "development")


if __name__ == "__main__":
    unittest.main()
