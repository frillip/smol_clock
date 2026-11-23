"""build_esp.py: the command, the core check, and a build with a stand-in arduino-cli."""
import json
import os
import stat
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import build_esp  # noqa: E402

# Writes its arguments to CALLS (one JSON line a call) and lists CORE's
# version as installed (none if empty); compile exits with COMPILE_EXIT.
STAND_IN = """#!%s
import json, os, sys
with open(os.environ["CALLS"], "a") as handle:
    handle.write(json.dumps(sys.argv[1:]) + "\\n")
if "list" in sys.argv:
    core = os.environ.get("CORE")
    print(json.dumps({"platforms": [{"id": "esp32:esp32", "installed_version": core}]
                      if core else []}))
sys.exit(int(os.environ.get("COMPILE_EXIT", "0")) if "compile" in sys.argv else 0)
"""


class Command(unittest.TestCase):
    def test_compile(self):
        self.assertEqual(build_esp.compile_command("cli"), [
            "cli", "compile", "--fqbn", "esp32:esp32:esp32:FlashSize=16M,PartitionScheme=custom",
            "--libraries", build_esp.LIBRARIES, "--export-binaries", "--warnings", "default",
            build_esp.SKETCH])

    def test_with_a_configuration(self):
        command = build_esp.compile_command("cli", "conf.yaml")
        self.assertEqual(command[:4], ["cli", "--config-file", "conf.yaml", "compile"])


class Core(unittest.TestCase):
    def test_the_one_named(self):
        self.assertIsNone(build_esp.check_core(build_esp.CORE_VERSION))

    def test_another_in_the_series_warns(self):
        self.assertIn("3.3.9", build_esp.check_core("3.3.9"))

    def test_another_series_stops_it(self):
        for version in ("3.2.1", "2.0.17", "3.4.0"):
            with self.assertRaises(build_esp.BuildError):
                build_esp.check_core(version)

    def test_none_stops_it(self):
        with self.assertRaises(build_esp.BuildError) as caught:
            build_esp.check_core(None)
        self.assertIn("core install esp32:esp32@", str(caught.exception))

    def test_a_wanted_version_exactly(self):
        self.assertIsNone(build_esp.check_core("3.3.9", "3.3.9"))
        with self.assertRaises(build_esp.BuildError):
            build_esp.check_core(build_esp.CORE_VERSION, "3.3.9")


class WithStandIn(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.mkdtemp()
        self.cli = os.path.join(self.folder, "arduino-cli")
        with open(self.cli, "w") as handle:
            handle.write(STAND_IN % sys.executable)
        os.chmod(self.cli, os.stat(self.cli).st_mode | stat.S_IEXEC)
        self.calls = os.path.join(self.folder, "calls")
        self.config = os.path.join(self.folder, "arduino-cli.yaml")
        open(self.config, "w").close()
        self.environment = mock.patch.dict(os.environ, {
            "CALLS": self.calls, "CORE": build_esp.CORE_VERSION,
            "SMOL_RELEASE_SETTINGS": os.path.join(self.folder, "none.json")})
        self.environment.start()
        self.libraries = mock.patch.object(build_esp, "check_libraries", lambda: None)
        self.libraries.start()

    def tearDown(self):
        self.libraries.stop()
        self.environment.stop()

    def called(self):
        with open(self.calls) as handle:
            return [json.loads(line) for line in handle]

    @unittest.skipIf(os.name == "nt", "a stand-in script needs a shebang")
    def test_build(self):
        said = []
        build_esp.build(self.cli, self.config, say=said.append)
        calls = self.called()
        self.assertEqual(calls[0], ["--config-file", self.config, "core", "list", "--json"])
        self.assertEqual(calls[1], build_esp.compile_command(self.cli, self.config)[1:])
        self.assertIn("esp32:esp32 %s" % build_esp.CORE_VERSION, said)

    @unittest.skipIf(os.name == "nt", "a stand-in script needs a shebang")
    def test_the_wrong_core_compiles_nothing(self):
        os.environ["CORE"] = "3.2.0"
        with self.assertRaises(build_esp.BuildError):
            build_esp.build(self.cli, None, say=lambda text: None)
        self.assertEqual(len(self.called()), 1)

    @unittest.skipIf(os.name == "nt", "a stand-in script needs a shebang")
    def test_the_settings_name_the_core(self):
        with open(os.environ["SMOL_RELEASE_SETTINGS"], "w") as handle:
            json.dump({"esp32_core": "3.3.9"}, handle)
        with self.assertRaises(build_esp.BuildError) as caught:
            build_esp.build(self.cli, None, say=lambda text: None)
        self.assertIn("3.3.9 wanted", str(caught.exception))

    @unittest.skipIf(os.name == "nt", "a stand-in script needs a shebang")
    def test_a_failed_compile(self):
        os.environ["COMPILE_EXIT"] = "1"
        try:
            with self.assertRaises(build_esp.BuildError):
                build_esp.build(self.cli, None, say=lambda text: None)
        finally:
            del os.environ["COMPILE_EXIT"]

    def test_a_missing_configuration(self):
        with self.assertRaises(build_esp.BuildError):
            build_esp.find_config(os.path.join(self.folder, "missing.yaml"))

    def test_print_runs_nothing(self):
        said = []
        build_esp.build("cli", show=True, say=said.append)
        self.assertFalse(os.path.exists(self.calls))
        self.assertTrue(said[0].startswith("cli compile --fqbn"))


if __name__ == "__main__":
    unittest.main()
