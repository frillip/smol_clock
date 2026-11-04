#!/usr/bin/env python3
"""The copies of shared files, refreshed from their originals.

  sync_common.py           copy every original over its copy, where they differ
  sync_common.py --check   change nothing; exit 1, listing them, if any differ

The Arduino build compiles only what is inside the sketch, so the ESP keeps
copies of the SML codec and the bootloader's layout from common/, and the
helper sketches in etc/ a copy of the ESP's partition table. They are plain
files, not symbolic links, so a checkout works anywhere (Windows included)
without special settings. Edit the originals, then run this; the ESP's host
tests run --check, so a copy left behind fails them.
"""
import argparse
import filecmp
import os
import shutil
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

# (original, copy), from the repository root.
COPIES = [
    ("common/sml.c", "esp_src/src/pic/sml/sml.c"),
    ("common/sml.h", "esp_src/src/pic/sml/sml.h"),
    ("common/sml_proto.h", "esp_src/src/pic/sml/sml_proto.h"),
    ("common/boot_layout.h", "esp_src/src/pic/sml/boot_layout.h"),
    ("common/boot_crc.h", "esp_src/src/pic/sml/boot_crc.h"),
    ("common/boot_crc.c", "esp_src/src/pic/sml/boot_crc.c"),
    ("esp_src/partitions.csv", "etc/pic_uart_usb/partitions.csv"),
    ("esp_src/partitions.csv", "etc/i2cscanner/i2cscanner/partitions.csv"),
]


def differs(original, copy):
    return not os.path.isfile(copy) or os.path.islink(copy) or not filecmp.cmp(original, copy, shallow=False)


def stored_as_links(copies):
    """The copies git holds as symbolic links (mode 120000): what a checkout
    with core.symlinks false commits when a link is replaced by a file, and
    which other checkouts then cannot write. Empty without git."""
    try:
        listing = subprocess.run(["git", "ls-files", "-s", "--"] + copies, cwd=ROOT,
                                 capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return []
    return [line.split("\t", 1)[1] for line in listing.splitlines()
            if line.startswith("120000 ")]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--check", action="store_true", help="report copies that differ, change nothing")
    args = parser.parse_args(argv)
    stale = []
    for original, copy in COPIES:
        source = os.path.join(ROOT, original)
        target = os.path.join(ROOT, copy)
        if not differs(source, target):
            continue
        stale.append(copy)
        if not args.check:
            if os.path.islink(target):
                os.remove(target)
            shutil.copyfile(source, target)
            print("%s <- %s" % (copy, original))
    links = stored_as_links([copy for _, copy in COPIES])
    for copy in links:
        print("sync_common: %s is in git as a symbolic link, not a file: "
              "git rm --cached %s && git add %s" % (copy, copy, copy), file=sys.stderr)
    if args.check and stale:
        for copy in stale:
            print("sync_common: %s differs from its original: run tools/sync_common.py" % copy,
                  file=sys.stderr)
        return 1
    if links:
        return 1
    if not stale:
        print("sync_common: every copy is up to date")
    return 0


if __name__ == "__main__":
    sys.exit(main())
