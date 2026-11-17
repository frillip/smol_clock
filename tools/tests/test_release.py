"""release.py: only components with a new version are built and released."""
import hashlib
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import release  # noqa: E402


def manifest(esp="0.3.64", pic="0.3.40", spl="1.0.0", config=None):
    entry = {"hardware": "v2"}
    if config:
        entry["config"] = config
    base = "https://example.org/v2plus/%s/" % (config or "prod")
    for key, version, name in (("esp", esp, "esp-%s.bin"), ("pic", pic, "app-%s.smolpic"),
                               ("pic_spl", spl, "spl-%s.smolspl")):
        data = ("live " + key).encode()
        entry[key] = {"version": version, "url": base + name % version, "size": len(data),
                      "sha256": hashlib.sha256(data).hexdigest()}
    return {"format": 2, "configurations": [entry]}


SOURCES = {"ESP": "0.3.64", "PIC": "0.3.40", "SPL": "1.0.0"}


def sources(path, prefix):
    return SOURCES[prefix]


class VersionTest(unittest.TestCase):
    def test_headers_read(self):
        self.assertRegex(release.source_version(release.COMPONENTS["esp"][0], "ESP"), r"^\d+\.\d+\.\d+$")
        self.assertRegex(release.source_version(release.COMPONENTS["pic"][0], "PIC"), r"^\d+\.\d+\.\d+$")
        self.assertRegex(release.source_version(release.COMPONENTS["pic_spl"][0], "SPL"), r"^\d+\.\d+\.\d+$")

    def test_numeric(self):
        self.assertEqual(release.numeric("0.3.63_dev"), (0, 3, 63))
        self.assertIsNone(release.numeric("1.2"))

    @mock.patch.object(release, "source_version", sources)
    @mock.patch.object(release, "say", lambda text: None)
    def test_changed(self):
        live = json.dumps(manifest(esp="0.3.63")).encode()
        entry = release.live_entry(live, "", [])
        changed = release.changed_components(entry, ["pic", "pic_spl", "esp"])
        self.assertEqual(changed, {"pic": False, "pic_spl": False, "esp": True})
        # Another configuration: nothing offered there, all new.
        self.assertIsNone(release.live_entry(live, "dev", []))
        self.assertEqual(release.changed_components(None, ["pic"]), {"pic": True})


class KeepTest(unittest.TestCase):
    @mock.patch.object(release, "say", lambda text: None)
    def test_unchanged_files_and_entries_put_back(self):
        live = manifest()
        with tempfile.TemporaryDirectory() as dist:
            os.makedirs(os.path.join(dist, "prod"))
            # What make_dist.py wrote: a rebuild of the same versions.
            rebuilt = manifest()
            for key in ("pic", "pic_spl"):
                rebuilt["configurations"][0][key]["sha256"] = "0" * 64
                name = live["configurations"][0][key]["url"].rsplit("/", 1)[-1]
                with open(os.path.join(dist, "prod", name), "wb") as handle:
                    handle.write(b"rebuilt")
            path = os.path.join(dist, "manifest.json")
            with open(path, "w") as handle:
                json.dump(rebuilt, handle)
            served = {c["url"]: ("live " + k).encode()
                      for k, c in live["configurations"][0].items() if isinstance(c, dict)}
            with mock.patch.object(release, "fetch", lambda url: served.get(url)):
                kept = release.keep_live(path, "", [], live["configurations"][0], ["pic", "pic_spl"],
                                         dist, "prod")
            with open(path) as handle:
                after = json.load(handle)
            self.assertEqual(after["configurations"][0]["pic"], live["configurations"][0]["pic"])
            self.assertEqual(after["configurations"][0]["pic_spl"], live["configurations"][0]["pic_spl"])
            self.assertEqual(len(kept), 2)
            with open(kept[0], "rb") as handle:
                self.assertTrue(handle.read().startswith(b"live "))
            # Not served as the manifest says: refused.
            with mock.patch.object(release, "fetch", lambda url: b"other"):
                with self.assertRaises(release.ReleaseError):
                    release.keep_live(path, "", [], live["configurations"][0], ["pic"], dist, "prod")


class MainTest(unittest.TestCase):
    def run_main(self, argv, live):
        calls = []
        with mock.patch.object(release, "source_version", sources), \
                mock.patch.object(release, "say", lambda text: None), \
                mock.patch.object(release, "step", lambda text: None), \
                mock.patch.object(release, "check_clean", lambda allow: None), \
                mock.patch.object(release, "head_commit", lambda: "0" * 40), \
                mock.patch.object(release, "need", lambda tool, why: tool), \
                mock.patch.object(release, "load_settings", lambda: {}), \
                mock.patch.object(release, "live_manifest", lambda url, path, dry: live), \
                mock.patch.object(release.run_tests, "main", lambda a: calls.append("tests") or 0), \
                mock.patch.object(release, "build_esp", lambda s, d: calls.append("esp")), \
                mock.patch.object(release, "build_pic", lambda s, d, p=None: calls.append(tuple(p))):
            code = release.main(argv + ["--dry-run"])
        return code, calls

    def test_nothing_new_nothing_built(self):
        code, calls = self.run_main([], json.dumps(manifest()).encode())
        self.assertEqual((code, calls), (0, []))

    def test_only_the_new_one_built(self):
        code, calls = self.run_main([], json.dumps(manifest(pic="0.3.39")).encode())
        self.assertEqual(code, 0)
        self.assertNotIn("esp", calls)
        self.assertTrue(any(isinstance(c, tuple) and "dspic_src" in c for c in calls))
        self.assertFalse(any(isinstance(c, tuple) and "dspic_spl" in c and
                             os.path.isfile(release.make_dist.SPL_HEX) for c in calls))

    def test_allow_dirty_builds_everything(self):
        code, calls = self.run_main(["--allow-dirty"], json.dumps(manifest()).encode())
        self.assertEqual(code, 0)
        self.assertIn("esp", calls)
        self.assertTrue(any(isinstance(c, tuple) and "dspic_src" in c and "dspic_spl" in c
                            for c in calls))


if __name__ == "__main__":
    unittest.main()
