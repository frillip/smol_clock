#!/usr/bin/env python3
"""The release files, from the latest builds, into dist/:

  ipl-X.Y.Z.hex                  the IPL to release (make_image.py ipl)
  spl-X.Y.Z.smolspl              the SPL file for the ESP (make_image.py spl)
  app-X.Y.Z.smolpic              the application file for the ESP (make_image.py app)
  merged-iX.Y.Z-sX.Y.Z-aX.Y.Z.hex  the first-installation hex (make_image.py install)
  esp-X.Y.Z.bin                  the ESP's image, as built
  manifest.json                  the update manifest for them (make_manifest.py)

  make_dist.py [--base-url URL] [--dist DIR] [--config NAME] [--device MAC ...]
               [--ipl HEX] [--spl HEX] [--app HEX] [--esp BIN] [--no-esp]

No version is typed in. The IPL's comes from its info block, the ESP's from
its image (its user agent string), and the SPL's and the application's from
the headers the builds were made from (dspic_spl/spl_version.h,
dspic_src/platform/version.h), as make_image.py reads them; a header changed
after its build is reported, as the build is then stale. Each file is named
after the version read back from the file written.

Paths default to the MPLAB X production builds and the Arduino build folder,
from the repository root, wherever this is run from.
"""
import argparse
import os
import shutil
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(TOOLS, ".."))
sys.path.insert(0, TOOLS)
import make_image as mi  # noqa: E402
import make_manifest  # noqa: E402

BASE_URL = "https://rubidium.darksky.io/smol_clock/v2plus/dev/"
IPL_HEX = os.path.join(ROOT, "dspic_ipl", "dist", "default", "production", "dspic_ipl.production.hex")
SPL_HEX = os.path.join(ROOT, "dspic_spl", "dist", "default", "production", "dspic_spl.production.hex")
APP_HEX = os.path.join(ROOT, "dspic_src", "dist", "default", "production", "dspic_src.production.hex")
ESP_BIN = os.path.join(ROOT, "esp_src", "build", "esp32.esp32.esp32", "esp_src.ino.bin")


class DistError(Exception):
    pass


def warn(text):
    print("warning: " + text, file=sys.stderr)


def check_fresh(built, header, what):
    """A version header edited after the build: the build does not carry it."""
    if os.path.getmtime(header) > os.path.getmtime(built):
        warn("%s: %s changed after %s was built: rebuild, or the version is wrong"
             % (what, os.path.relpath(header, ROOT), os.path.relpath(built, ROOT)))


def ipl_version(words):
    info = mi.check_ipl(words, "IPL")
    return info[mi.L["IPL_INFO_VERSION"]] << 16 | info[mi.L["IPL_INFO_VERSION"] + 1]


def write(path, data):
    with open(path, "wb") as handle:
        handle.write(data)


def build(args):
    for path, what in ((args.ipl, "IPL"), (args.spl, "SPL"), (args.app, "application")):
        if not os.path.isfile(path):
            raise DistError("%s: no %s build there" % (path, what))
    if not args.no_esp and not os.path.isfile(args.esp):
        raise DistError("%s: no ESP build there (or --no-esp)" % args.esp)
    os.makedirs(args.dist, exist_ok=True)
    made = []

    # The IPL: released, its version from its own info block.
    words, summary = mi.build_ipl(args.ipl, mi.default_build_time(args.ipl), warn)
    ipl = mi.version_text(ipl_version(words))
    ipl_path = os.path.join(args.dist, "ipl-%s.hex" % ipl)
    mi.write_hex(ipl_path, words)
    made.append((ipl_path, summary))

    # The SPL's and the application's files.
    check_fresh(args.spl, mi.SPL_VERSION_HEADER, "SPL")
    check_fresh(args.app, mi.VERSION_HEADER, "application")
    spl_data = mi.build_spl(args.spl, mi.spl_version(), 0, mi.default_build_time(args.spl), warn)
    app_data = mi.build_app(args.app, mi.default_build_time(args.app), warn)
    spl_file, app_file = mi.parse_file(spl_data), mi.parse_file(app_data)
    spl = mi.version_text(spl_file["version"])
    app = mi.version_text(app_file["version"])
    spl_path = os.path.join(args.dist, "spl-%s.smolspl" % spl)
    app_path = os.path.join(args.dist, "app-%s.smolpic" % app)
    write(spl_path, spl_data)
    write(app_path, app_data)
    made.append((spl_path, "SPL %s, %d instructions" % (spl, spl_file["length"])))
    made.append((app_path, "application %s, %d instructions, SML %d.%d" % (
        app, app_file["length"], app_file["sml_version"] >> 8, app_file["sml_version"] & 0xFF)))

    # The first installation, from the released IPL.
    merged = mi.build_install(ipl_path, args.spl, args.app, spl_file["version"],
                              mi.default_build_time(args.app), warn)
    merged_path = os.path.join(args.dist, "merged-i%s-s%s-a%s.hex" % (ipl, spl, app))
    mi.write_hex(merged_path, merged)
    made.append((merged_path, "first installation: IPL, SPL (confirmed), application"))

    # The ESP's image, named by its own version.
    esp_path = None
    if not args.no_esp:
        with open(args.esp, "rb") as handle:
            try:
                esp = make_manifest.esp_version(handle.read())
            except make_manifest.ManifestError as error:
                raise DistError("%s: %s" % (args.esp, error))
        esp_path = os.path.join(args.dist, "esp-%s.bin" % esp)
        shutil.copyfile(args.esp, esp_path)
        made.append((esp_path, "ESP %s" % esp))

    # The manifest for the files the ESP fetches.
    manifest_path = os.path.join(args.dist, "manifest.json")
    command = ["--base-url", args.base_url, "--pic", app_path, "--pic-spl", spl_path, "-o", manifest_path]
    if esp_path:
        command += ["--esp", esp_path]
    if args.config:
        command += ["--config", args.config]
    for device in args.device or []:
        command += ["--device", device]
    if make_manifest.main(command) != 0:
        raise DistError("the manifest was not written")
    made.append((manifest_path, "for %s" % args.base_url))
    return made


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--base-url", default=BASE_URL, help="where the files will be (default %(default)s)")
    parser.add_argument("--dist", default=os.path.join(ROOT, "dist"), help="output folder (default dist/)")
    parser.add_argument("--config", default="", help="the manifest entry's update config (none: the default)")
    parser.add_argument("--device", action="append", help="limit the manifest entry to a MAC address")
    parser.add_argument("--ipl", default=IPL_HEX, help="the IPL's production hex")
    parser.add_argument("--spl", default=SPL_HEX, help="the SPL's production hex")
    parser.add_argument("--app", default=APP_HEX, help="the application's production hex")
    parser.add_argument("--esp", default=ESP_BIN, help="the ESP's image (Arduino's build folder)")
    parser.add_argument("--no-esp", action="store_true", help="leave the ESP out")
    args = parser.parse_args(argv)
    try:
        made = build(args)
    except (DistError, mi.ImageError, OSError) as error:
        print("make_dist: %s" % error, file=sys.stderr)
        return 1
    for path, what in made:
        print("%-40s %s" % (os.path.relpath(path), what))
    return 0


if __name__ == "__main__":
    sys.exit(main())
