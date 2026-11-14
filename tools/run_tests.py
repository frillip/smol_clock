#!/usr/bin/env python3
"""Every host test, in order, with a summary: the shared files' copies, then
the dsPIC's (IPL, SPL, application, the release tools), then the ESP's.

  run_tests.py [--list] [--only NAME ...] [--clean]

Each suite is built and run by its own Makefile, so a suite is also run by
hand with make in its folder. Needs make, gcc and g++ (on Windows, from
MSYS2 or WSL; the ESP's suites use the address and undefined-behaviour
sanitisers, which MinGW's gcc does not have). Exits non-zero if any suite
fails. Used by CI (.github/workflows/ci.yml) and by release.py.
"""
import argparse
import os
import subprocess
import sys
import time

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
PYTHON = sys.executable or "python3"

# (name, folder from the root, command). spl_sim (dspic-spl) is built before
# the release tools' tests, which run pic_boot.py against it.
SUITES = [
    ("shared-copies", ".", [PYTHON, "tools/sync_common.py", "--check"]),
    ("dspic-ipl", "dspic_ipl/tests/host", ["make"]),
    ("dspic-spl", "dspic_spl/tests/host", ["make"]),
    ("dspic-app", "dspic_src/tests/host", ["make"]),
    ("release-tools", "dspic_src",
     [PYTHON, "-m", "unittest", "discover", "-s", "tests/tools", "-t", "tests/tools"]),
    ("esp-commands", "esp_src/tests/commands", ["make", "test"]),
    ("esp-manifest", "esp_src/tests/manifest", ["make", "test"]),
    ("esp-net", "esp_src/tests/net", ["make", "test"]),
    ("esp-ntp", "esp_src/tests/ntp", ["make", "test"]),
    ("esp-pic-update", "esp_src/tests/pic_update", ["make", "test"]),
    ("esp-settings", "esp_src/tests/settings", ["make", "test"]),
]


def run(name, folder, command, clean):
    path = os.path.join(ROOT, folder)
    if clean and command[0] == "make":
        subprocess.call(["make", "clean"], cwd=path, stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL)
    print("=== %s (%s: %s)" % (name, folder, " ".join(command)), flush=True)
    start = time.time()
    try:
        code = subprocess.call(command, cwd=path)
    except OSError as error:
        print("%s: %s" % (name, error), file=sys.stderr)
        code = 127
    return code, time.time() - start


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--list", action="store_true", help="list the suites and stop")
    parser.add_argument("--only", action="append", help="run only this suite (repeatable)")
    parser.add_argument("--clean", action="store_true", help="make clean before each build")
    args = parser.parse_args(argv)
    if args.list:
        for name, folder, command in SUITES:
            print("%-16s %s: %s" % (name, folder, " ".join(command)))
        return 0
    names = [suite[0] for suite in SUITES]
    for name in args.only or []:
        if name not in names:
            parser.error("--only %s: one of %s" % (name, ", ".join(names)))
    results = []
    for name, folder, command in SUITES:
        if args.only and name not in args.only:
            continue
        code, seconds = run(name, folder, command, args.clean)
        results.append((name, code, seconds))
    print()
    for name, code, seconds in results:
        print("%-16s %-6s %5.0f s" % (name, "ok" if code == 0 else "FAILED", seconds))
    failed = [name for name, code, _ in results if code != 0]
    if failed:
        print("%d of %d suites failed: %s" % (len(failed), len(results), ", ".join(failed)))
        return 1
    print("all %d suites passed" % len(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
