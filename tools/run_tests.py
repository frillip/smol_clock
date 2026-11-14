#!/usr/bin/env python3
"""Every host test, in order, with a summary: the shared files' copies, then
the dsPIC's (IPL, SPL, application, the release tools), then the ESP's.

  run_tests.py [--list] [--only NAME ...] [--clean] [--no-wsl]

Each suite is built and run by its own Makefile, so a suite is also run by
hand with make in its folder. Needs make, gcc and g++. On Windows, where
they are not on the PATH, it runs itself again in WSL (wsl.exe, in this
folder there), which needs python3, make, gcc and g++ installed in the
distribution (apt install python3 make gcc g++); MSYS2's MinGW gcc will not
do, as the ESP's suites use the address and undefined-behaviour sanitisers.
Suites whose tools are missing are reported as SKIPPED, with what is
missing, and count as not passed. Exits non-zero unless every suite passed.
Used by CI (.github/workflows/ci.yml) and by release.py.
"""
import argparse
import os
import shutil
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


TOOLS = ("make", "gcc", "g++")


def missing_tools():
    return [tool for tool in TOOLS if shutil.which(tool) is None]


def in_wsl(argv):
    """On Windows without the tools: this script again, in WSL. None if
    there is no WSL to run it in."""
    wsl = shutil.which("wsl")
    if os.name != "nt" or wsl is None:
        return None
    print("make, gcc or g++ not on the PATH: running in WSL", flush=True)
    # wsl.exe starts in the Windows folder it is run from, as /mnt/c/...
    return subprocess.call([wsl, "--cd", ROOT, "python3", "tools/run_tests.py", "--no-wsl"] + argv)


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
    parser.add_argument("--no-wsl", action="store_true", help="on Windows, never run in WSL")
    raw = list(sys.argv[1:] if argv is None else argv)
    args = parser.parse_args(raw)
    if args.list:
        for name, folder, command in SUITES:
            print("%-16s %s: %s" % (name, folder, " ".join(command)))
        return 0
    names = [suite[0] for suite in SUITES]
    for name in args.only or []:
        if name not in names:
            parser.error("--only %s: one of %s" % (name, ", ".join(names)))
    missing = missing_tools()
    if missing and not args.no_wsl:
        code = in_wsl([part for part in raw if part != "--no-wsl"])
        if code is not None:
            return code
    results = []
    for name, folder, command in SUITES:
        if args.only and name not in args.only:
            continue
        if command[0] == "make" and missing:
            print("=== %s: SKIPPED, %s not found" % (name, ", ".join(missing)), flush=True)
            results.append((name, None, 0.0))
            continue
        code, seconds = run(name, folder, command, args.clean)
        results.append((name, code, seconds))
    print()
    for name, code, seconds in results:
        state = "SKIPPED" if code is None else "ok" if code == 0 else "FAILED"
        print("%-16s %-7s %5.0f s" % (name, state, seconds))
    failed = [name for name, code, _ in results if code is not None and code != 0]
    skipped = [name for name, code, _ in results if code is None]
    if failed:
        print("%d of %d suites failed: %s" % (len(failed), len(results), ", ".join(failed)))
    if skipped:
        print("%d of %d suites skipped, without %s: %s" % (len(skipped), len(results), ", ".join(missing),
                                                           ", ".join(skipped)))
        if os.name == "nt":
            print("on Windows: install WSL (wsl --install), then in it: "
                  "sudo apt install python3 make gcc g++")
    if failed or skipped:
        return 1
    print("all %d suites passed" % len(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
