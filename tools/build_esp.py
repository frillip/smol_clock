#!/usr/bin/env python3
"""
Builds the ESP firmware (esp_src) with arduino-cli.

  python3 tools/build_esp.py [--cli PATH] [--config-file FILE] [--core VERSION]
                             [--print]

The board is the ESP32 Dev Module with 16 MB flash and the custom partition
scheme (esp_src/partitions.csv), the libraries are esp_src/libraries (the
ArduinoJson submodule among them). --export-binaries leaves the images in
esp_src/build/esp32.esp32.esp32/, where make_dist.py looks, as the IDE's
"Export Compiled Binary" does.

The tools: --cli, else "arduino_cli" in the release settings
(SMOL_RELEASE_SETTINGS, else tools/release.local.json), else arduino-cli on
the PATH. --config-file (else "arduino_config") is passed to arduino-cli as
its --config-file: where its cores, toolchains and downloads live, so a
machine can keep one set for this firmware apart from its own.

The esp32 core: --core (else "esp32_core") is the version wanted, and
another one installed stops the build. Without either, CORE_VERSION is
wanted: another 3.3.x is a warning, any other series stops the build.
--print checks nothing and prints the command.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKETCH = os.path.join(ROOT, "esp_src")
LIBRARIES = os.path.join(SKETCH, "libraries")
# ESP32 Dev Module, 16MB flash, the Custom partition scheme (partitions.csv).
FQBN = "esp32:esp32:esp32:FlashSize=16M,PartitionScheme=custom"
CORE = "esp32:esp32"
# The core the firmware is built and run with (as .github/workflows/ci.yml
# and tools/ci/setup_caesium.sh install).
CORE_VERSION = "3.3.12"
CORE_INDEX = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"


class BuildError(Exception):
    pass


def settings():
    path = os.environ.get("SMOL_RELEASE_SETTINGS") or os.path.join(ROOT, "tools",
                                                                    "release.local.json")
    if not os.path.isfile(path):
        return {}
    with open(path) as handle:
        return json.load(handle)


def series(version):
    return ".".join(version.split(".")[:2])


def find_cli(given=None):
    cli = given or settings().get("arduino_cli") or "arduino-cli"
    if os.path.isfile(cli):
        return cli
    found = shutil.which(cli)
    if found is None:
        raise BuildError("%s not found: give it with --cli, or \"arduino_cli\" in the release "
                         "settings" % cli)
    return found


def find_config(given=None):
    config = given or settings().get("arduino_config")
    if config and not os.path.isfile(config):
        raise BuildError("%s: no such arduino-cli configuration" % config)
    return config


def base(cli, config):
    """arduino-cli, with its configuration if one is given."""
    return [cli] + (["--config-file", config] if config else [])


def installed_core(cli, config):
    """The esp32 core's installed version, or None."""
    try:
        text = subprocess.check_output(base(cli, config) + ["core", "list", "--json"],
                                       stderr=subprocess.STDOUT).decode("utf-8", "replace")
        listing = json.loads(text)
    except (OSError, subprocess.CalledProcessError) as error:
        raise BuildError("arduino-cli core list: %s" % error)
    except ValueError:
        raise BuildError("arduino-cli core list: not JSON")
    # 0.x: a list; 1.x: {"platforms": [...]}, each with id and installed_version.
    platforms = listing.get("platforms", []) if isinstance(listing, dict) else listing
    for platform in platforms or []:
        if (platform.get("id") or platform.get("ID")) == CORE:
            return platform.get("installed_version") or platform.get("installed") or None
    return None


def check_core(installed, wanted=None):
    """Stops the build on the wrong core; a warning (returned) or None."""
    install = "arduino-cli core install %s@%s" % (CORE, wanted or CORE_VERSION)
    if installed is None:
        raise BuildError("no %s core: %s (index %s)" % (CORE, install, CORE_INDEX))
    if wanted:
        if installed != wanted:
            raise BuildError("%s %s installed, %s wanted: %s" % (CORE, installed, wanted, install))
        return None
    if installed == CORE_VERSION:
        return None
    if series(installed) == series(CORE_VERSION):
        return "%s %s installed, built and run with %s" % (CORE, installed, CORE_VERSION)
    raise BuildError("%s %s installed, the firmware needs %s.x (%s): %s"
                     % (CORE, installed, series(CORE_VERSION), CORE_VERSION, install))


def compile_command(cli, config=None):
    return base(cli, config) + ["compile", "--fqbn", FQBN, "--libraries", LIBRARIES,
                                "--export-binaries", "--warnings", "default", SKETCH]


def check_libraries():
    folder = os.path.join(LIBRARIES, "ArduinoJson")
    if not (os.path.isfile(os.path.join(folder, "library.properties")) or
            os.path.isdir(os.path.join(folder, "src"))):
        raise BuildError("the ESP's libraries are not there: git submodule update --init")


def build(cli=None, config=None, core=None, show=False, say=print):
    """Checks the tools and the core, then compiles. show: the command only."""
    if show:
        command = compile_command(cli or "arduino-cli", config)
        say(" ".join(command))
        return command
    cli = find_cli(cli)
    config = find_config(config)
    wanted = core or settings().get("esp32_core")
    installed = installed_core(cli, config)
    warning = check_core(installed, wanted)
    say("%s %s" % (CORE, installed))
    if warning:
        say("warning: " + warning)
    check_libraries()
    command = compile_command(cli, config)
    say("$ " + " ".join('"%s"' % part if " " in part else part for part in command))
    try:
        code = subprocess.call(command, cwd=ROOT)
    except OSError as error:
        raise BuildError("%s: %s" % (cli, error))
    if code != 0:
        raise BuildError("arduino-cli compile failed (exit %d)" % code)
    return command


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("--cli", help="arduino-cli")
    parser.add_argument("--config-file", help="arduino-cli's configuration")
    parser.add_argument("--core", help="the esp32 core version wanted (exactly)")
    parser.add_argument("--print", dest="show", action="store_true",
                        help="print the command, check and run nothing")
    args = parser.parse_args(argv)
    try:
        build(args.cli, args.config_file, args.core, args.show)
    except BuildError as error:
        sys.stderr.write("build_esp: %s\n" % error)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
