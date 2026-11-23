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
    def run_main(self, argv, live, settings=None, check=None):
        calls = []
        with mock.patch.object(release, "source_version", sources), \
                mock.patch.object(release, "say", lambda text: None), \
                mock.patch.object(release, "step", lambda text: None), \
                mock.patch.object(release, "check_clean", lambda allow: None), \
                mock.patch.object(release, "head_commit", lambda: "0" * 40), \
                mock.patch.object(release, "need", lambda tool, why: tool), \
                mock.patch.object(release, "load_settings", lambda: settings or {}), \
                mock.patch.object(release.upload_dist, "check", check or (lambda s: "scp")), \
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

    def test_upload_checked_before_anything_is_built(self):
        # The real check: no target set, so it stops at step 1.
        code, calls = self.run_main(["--allow-dirty"], json.dumps(manifest()).encode(),
                                    check=release.upload_dist.check)
        self.assertEqual((code, calls), (1, []))
        code, calls = self.run_main(["--allow-dirty", "--no-upload"], json.dumps(manifest()).encode(),
                                    check=release.upload_dist.check)
        self.assertEqual(code, 0)

    def test_allow_dirty_builds_everything(self):
        code, calls = self.run_main(["--allow-dirty"], json.dumps(manifest()).encode())
        self.assertEqual(code, 0)
        self.assertIn("esp", calls)
        self.assertTrue(any(isinstance(c, tuple) and "dspic_src" in c and "dspic_spl" in c
                            for c in calls))


class TagTest(unittest.TestCase):
    """The CI's tags against a repository of their own: a test tag, then a
    release promoted from it with the version headers alone changed."""

    def git(self, *arguments):
        import subprocess
        subprocess.check_call(["git", "-c", "user.name=t", "-c", "user.email=t@t", "-c",
                               "commit.gpgsign=false", "-c", "tag.gpgsign=false"] + list(arguments),
                              cwd=self.repo, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def headers(self, esp, pic, spl="1.0.0"):
        for path, prefix, version in (("esp_src/src/version.h", "ESP", esp),
                                      ("dspic_src/platform/version.h", "PIC", pic),
                                      ("dspic_spl/spl_version.h", "SPL", spl)):
            full = os.path.join(self.repo, *path.split("/"))
            os.makedirs(os.path.dirname(full), exist_ok=True)
            major, minor, patch = version.split(".")
            with open(full, "w") as handle:
                handle.write("#define %s_VERSION_MAJOR %s\n#define %s_VERSION_MINOR %s\n"
                             "#define %s_VERSION_PATCH %s\n" % (prefix, major, prefix, minor, prefix, patch))

    def commit(self, message, tag=None):
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)
        if tag:
            self.git("tag", tag)
        self.git("update-ref", "refs/remotes/origin/master", "HEAD")

    def code(self, text):
        with open(os.path.join(self.repo, "code.c"), "w") as handle:
            handle.write(text)

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.repo = self.folder.name
        self.git("init", "-q", "-b", "master")
        self.headers("0.3.71", "0.3.71")
        self.code("one")
        self.commit("start")
        self.code("two")
        self.headers("0.3.72", "0.3.72")
        self.commit("work", "v0.3.72-test")
        self.out = []
        self.say = mock.patch.object(release, "say", self.out.append)
        self.say.start()

    def tearDown(self):
        self.say.stop()
        self.folder.cleanup()

    def check(self, tag, ci=None):
        self.git("checkout", "-q", tag)
        if ci is None:
            return release.check_tag(tag, ci_check=False, cwd=self.repo)
        with mock.patch.object(release, "ci_passed", lambda commit, cwd: ci), \
                mock.patch.object(release, "output", self.no_fetch):
            return release.check_tag(tag, ci_check=True, cwd=self.repo)

    def no_fetch(self, command, cwd=release.ROOT):
        if command[:2] == ["git", "fetch"]:
            return ""
        return self.real_output(command, cwd)

    real_output = staticmethod(release.output)

    def test_test_tag(self):
        self.assertTrue(self.check("v0.3.72-test"))
        self.assertTrue(any("for the dev configuration" in line for line in self.out))

    def test_promotion(self):
        self.git("checkout", "-q", "master")
        self.headers("0.3.73", "0.3.73")
        self.commit("0.3.73", "v0.3.73")
        self.assertFalse(self.check("v0.3.73", ci=True))
        self.assertTrue(any("promoted from v0.3.72-test" in line for line in self.out))
        with self.assertRaises(release.ReleaseError) as caught:
            self.check("v0.3.73", ci=False)
        self.assertIn("no successful release.yml run for v0.3.72-test", str(caught.exception))

    def test_more_than_versions(self):
        self.git("checkout", "-q", "master")
        self.headers("0.3.73", "0.3.73")
        self.code("three")
        self.commit("0.3.73 and a change", "v0.3.73")
        with self.assertRaises(release.ReleaseError) as caught:
            self.check("v0.3.73")
        self.assertIn("more than the version headers changed since v0.3.72-test: code.c",
                      str(caught.exception))

    def test_versions_must_match(self):
        self.git("checkout", "-q", "master")
        self.headers("0.3.73", "0.3.72")
        self.commit("half", "v0.3.73")
        with self.assertRaises(release.ReleaseError) as caught:
            self.check("v0.3.73")
        self.assertIn("the application's version header says 0.3.72", str(caught.exception))

    def test_no_test_tag_or_not_on_master(self):
        self.git("checkout", "-q", "master")
        self.headers("0.3.72", "0.3.72")
        self.git("tag", "v0.3.72")  # the same version as the test: nothing lower to promote
        with self.assertRaises(release.ReleaseError) as caught:
            self.check("v0.3.72")
        self.assertIn("no vX.Y.Z-test tag before it", str(caught.exception))
        self.git("checkout", "-q", "-b", "side")
        self.headers("0.3.73", "0.3.73")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "side")
        self.git("tag", "v0.3.73")
        with self.assertRaises(release.ReleaseError) as caught:
            self.check("v0.3.73")
        self.assertIn("not on origin/master", str(caught.exception))

    def test_names_and_head(self):
        for bad in ("0.3.73", "v0.3", "v0.3.73-rc1", "v0.3.73-test2"):
            with self.assertRaises(release.ReleaseError):
                release.check_tag(bad, ci_check=False, cwd=self.repo)
        self.git("checkout", "-q", "master")
        self.code("later")
        self.commit("later")
        with self.assertRaises(release.ReleaseError) as caught:
            release.check_tag("v0.3.72-test", ci_check=False, cwd=self.repo)
        self.assertIn("is not HEAD", str(caught.exception))

    def test_repository(self):
        with mock.patch.dict(os.environ, {"GITHUB_REPOSITORY": "frillip/smol_clock"}):
            self.assertEqual(release.repository(self.repo), "frillip/smol_clock")
        with mock.patch.dict(os.environ, {}, clear=True):
            self.git("remote", "add", "origin", "git@github.com:frillip/smol_clock.git")
            self.assertEqual(release.repository(self.repo), "frillip/smol_clock")

    def test_from_tag_sets_the_release(self):
        recorded = []

        def parse(argv):
            recorded.append(argv)
            raise release.ReleaseError("stop here")

        for tag, config in (("v0.3.72-test", ["--config", "dev"]), ("v0.3.73", [])):
            with mock.patch.object(release, "check_tag", lambda t, ci_check=True: t.endswith("-test")), \
                    mock.patch.object(release, "load_settings", lambda: {}), \
                    mock.patch.object(release, "check_clean", lambda allow: None), \
                    mock.patch.object(release, "head_commit", lambda: "abc"), \
                    mock.patch.object(release.upload_dist, "check", lambda s: "sftp"), \
                    mock.patch.object(release, "need", lambda tool, why: tool), \
                    mock.patch.object(release.make_dist, "parse_args", parse), mock.patch("sys.stderr"):
                self.assertEqual(release.main(["--from-tag", tag]), 1)
            self.assertEqual(recorded[-1][2:], config)  # after --base-url URL
        # --from-tag decides --config and --tag itself.
        with self.assertRaises(SystemExit), mock.patch("sys.stderr"):
            release.main(["--from-tag", "v0.3.73", "--config", "dev"])


if __name__ == "__main__":
    unittest.main()


class PicBuildTest(unittest.TestCase):
    def test_xc16_alone(self):
        built = []
        with mock.patch.object(release.pic_builder, "find_xc16", lambda given: "/x/xc16"), \
                mock.patch.object(release.pic_builder, "find_dfp", lambda given, device: "/p/dfp"), \
                mock.patch.object(release.pic_builder, "build",
                                  lambda folder, xc16, dfp, show=False: built.append(
                                      (os.path.basename(folder), xc16, dfp, show))):
            release.build_pic({"pic_build": "xc16"}, True, ["dspic_spl", "dspic_src"])
        self.assertEqual(built, [("dspic_spl", "/x/xc16", "/p/dfp", True),
                                 ("dspic_src", "/x/xc16", "/p/dfp", True)])

    def test_unknown_builder(self):
        with self.assertRaises(release.ReleaseError):
            release.build_pic({"pic_build": "make"}, True, ["dspic_src"])
