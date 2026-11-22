"""make_dist.py: each build against what the manifest offered, a rebuild
under the same version warned of, and --check."""
import hashlib
import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import make_dist  # noqa: E402


def part(version, content):
    data = content.encode()
    return {"version": version, "url": "https://example.org/v2plus/prod/x", "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest()}


def entry(esp=("0.3.69", "a"), pic=("0.3.68", "b"), spl=("1.0.0", "c"), config=None, devices=None):
    found = {"hardware": "v2"}
    if config:
        found["config"] = config
    if devices:
        found["devices"] = devices
    for key, value in (("esp", esp), ("pic", pic), ("pic_spl", spl)):
        if value:
            found[key] = part(*value)
    return found


class CompareTest(unittest.TestCase):
    def test_states(self):
        old = entry()
        new = entry(esp=("0.3.69", "rebuilt"), pic=("0.3.69", "b2"), spl=("1.0.0", "c"))
        states = {key: state for key, state, text in make_dist.compare(old, new)}
        self.assertEqual(states, {"esp": "rebuild", "pic": "version", "pic_spl": "same"})
        text = [text for key, state, text in make_dist.compare(old, new) if key == "esp"][0]
        self.assertIn("ESP 0.3.69: another build under the same version", text)
        self.assertIn("update esp force", text)

    def test_new_and_gone(self):
        states = {key: state for key, state, text in make_dist.compare(None, entry(spl=None))}
        self.assertEqual(states, {"esp": "new", "pic": "new"})
        states = {key: state for key, state, text in make_dist.compare(entry(), entry(esp=None))}
        self.assertEqual(states["esp"], "gone")

    def test_offered_by_target(self):
        manifest = {"format": 2, "configurations": [
            entry(esp=("1", "dev"), devices=["EC:64:C9:06:D3:50"]),
            entry(esp=("2", "default")),
            entry(esp=("3", "beta"), config="beta")]}
        self.assertEqual(make_dist.offered(manifest, "", None)["esp"]["version"], "2")
        self.assertEqual(make_dist.offered(manifest, "beta", None)["esp"]["version"], "3")
        self.assertEqual(make_dist.offered(manifest, "", ["EC:64:C9:06:D3:50"])["esp"]["version"], "1")
        self.assertIsNone(make_dist.offered(manifest, "other", None))
        self.assertIsNone(make_dist.offered({}, "", None))


class CheckTest(unittest.TestCase):
    """--check, with the build itself played by the test: it writes the
    manifest make_dist.py would."""

    def run_check(self, live, built, extra=()):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "manifest.json")
            with open(path, "w") as handle:
                json.dump(live, handle)

            def fake_build(args):
                with open(args.manifest, "w") as handle:
                    json.dump({"format": 2, "configurations": [built]}, handle)
                return []

            out, err = io.StringIO(), io.StringIO()
            with mock.patch.object(make_dist, "build", fake_build), redirect_stdout(out), \
                    redirect_stderr(err):
                code = make_dist.main(["--check", "--manifest", path] + list(extra))
            return code, out.getvalue()

    def test_offers_them(self):
        code, out = self.run_check({"format": 2, "configurations": [entry()]}, entry())
        self.assertEqual(code, 0)
        self.assertIn("The manifest offers these builds.", out)
        self.assertIn("ok       ESP 0.3.69, the build offered", out)

    def test_built_not_in_the_manifest(self):
        # The case that started this: a new ESP build, the old manifest.
        code, out = self.run_check({"format": 2, "configurations": [entry()]},
                                   entry(esp=("0.3.69", "new build")))
        self.assertEqual(code, 1)
        self.assertIn("REBUILD  ESP 0.3.69: another build under the same version", out)
        self.assertIn("run make_dist.py, then upload dist/", out)

    def test_devices_matched_as_written(self):
        mac = "EC:64:C9:06:D3:50"
        live = {"format": 2, "configurations": [entry(devices=[mac]), entry(esp=("0.1.0", "x"))]}
        code, out = self.run_check(live, entry(devices=[mac]), ["--device", "ec64c906d350"])
        self.assertEqual(code, 0)
        self.assertIn("for " + mac, out)

    def test_no_manifest(self):
        err = io.StringIO()
        with redirect_stderr(err):
            code = make_dist.main(["--check", "--manifest", "/nonexistent/manifest.json"])
        self.assertEqual(code, 1)
        self.assertIn("no manifest there", err.getvalue())


class BuildWarnsTest(unittest.TestCase):
    def test_rebuild_warned(self):
        old = {"format": 2, "configurations": [entry()]}
        new = {"format": 2, "configurations": [entry(esp=("0.3.69", "new build"))]}
        manifests = [old, new]
        warned = []
        with mock.patch.object(make_dist, "load_manifest", lambda where: manifests.pop(0)), \
                mock.patch.object(make_dist, "warn", warned.append):
            before = make_dist.offered(make_dist.load_manifest("x"), "", None)
            for key, state, text in make_dist.compare(before, make_dist.offered(
                    make_dist.load_manifest("x"), "", None)):
                if state == "rebuild":
                    make_dist.warn(text)
        self.assertEqual(len(warned), 1)
        self.assertIn("clocks at 0.3.69 keep theirs", warned[0])


if __name__ == "__main__":
    unittest.main()
