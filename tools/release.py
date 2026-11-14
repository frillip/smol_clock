#!/usr/bin/env python3
"""A release, start to finish: tests, builds, dist/, upload, check, GitHub.

  release.py [--config NAME] [--device MAC ...] [--tag vX.Y.Z]
             [--skip-tests] [--skip-build] [--no-esp] [--no-upload]
             [--no-verify] [--allow-dirty] [--dry-run]

In order, stopping at the first failure:

 1. The working tree must be clean (--allow-dirty for a test build), so the
    release is a commit, and the tools it needs must be there.
 2. Every host test (run_tests.py).
 3. The builds: the ESP with arduino-cli, the IPL, SPL and application with
    MPLAB X's own Makefiles, headless, as the IDE builds them (production).
 4. The live manifest downloaded into dist/manifest.json, so the new
    configuration is merged into what the clocks see now, not into whatever
    an earlier release left on this machine (releases made from two
    machines would otherwise undo each other's configurations).
 5. make_dist.py: the release files and the manifest, as before.
 6. The upload, with OpenSSH's sftp (in Windows 10 and 11, and every Linux
    and macOS): the files first, the manifest last, written beside the old
    one and renamed over it, so no clock reads a manifest naming a file that
    is not there yet, or half a manifest.
 7. Each file and the manifest downloaded back over HTTPS and compared.
 8. With --tag, a GitHub release of this commit with the files attached
    (the GitHub CLI, gh). The commit must be on GitHub already.

Settings for this machine, in tools/release.local.json (not in git):

  {
    "upload": "user@rubidium.darksky.io:/var/www/smol_clock/v2plus",
    "ssh_port": 22,
    "arduino_cli": "arduino-cli",
    "mplabx": "C:/Program Files/Microchip/MPLABX/v6.20",
    "make": "make"
  }

"upload" is the folder the base URL (make_dist.py --base-url) serves, as
user@host:path; the SSH key is the one ssh uses (ssh-agent, or
~/.ssh/config). The rest are optional: arduino_cli defaults to the one on
the PATH, mplabx to the newest MPLAB X installed in its usual place, make
to MPLAB X's own on Windows and to the PATH's elsewhere. --dry-run prints
what would be run and uploaded, and runs and uploads nothing.
"""
import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(TOOLS, ".."))
sys.path.insert(0, TOOLS)
import make_dist  # noqa: E402
import run_tests  # noqa: E402

SETTINGS = os.path.join(TOOLS, "release.local.json")
ESP_SKETCH = os.path.join(ROOT, "esp_src")
ESP_LIBRARIES = os.path.join(ESP_SKETCH, "libraries")
# ESP32 Dev Module, 16MB flash, the Custom partition scheme (partitions.csv).
ESP_FQBN = "esp32:esp32:esp32:FlashSize=16M,PartitionScheme=custom"
ESP_CORE = "esp32:esp32"
ESP_CORE_SERIES = "3.3."
PIC_PROJECTS = ["dspic_ipl", "dspic_spl", "dspic_src"]
WINDOWS = os.name == "nt"


class ReleaseError(Exception):
    pass


def say(text):
    print(text, flush=True)


def step(text):
    say("\n=== " + text)


def run(command, cwd=ROOT, dry=False):
    say("$ " + " ".join('"%s"' % part if " " in part else part for part in command))
    if dry:
        return
    try:
        code = subprocess.call(command, cwd=cwd)
    except OSError as error:
        raise ReleaseError("%s: %s" % (command[0], error))
    if code != 0:
        raise ReleaseError("%s failed (exit %d)" % (os.path.basename(command[0]), code))


def output(command, cwd=ROOT):
    try:
        return subprocess.check_output(command, cwd=cwd, stderr=subprocess.STDOUT).decode(
            "utf-8", "replace")
    except (OSError, subprocess.CalledProcessError) as error:
        raise ReleaseError("%s: %s" % (" ".join(command), error))


def load_settings():
    if not os.path.isfile(SETTINGS):
        return {}
    with open(SETTINGS, encoding="utf-8") as handle:
        try:
            return json.load(handle)
        except ValueError as error:
            raise ReleaseError("%s: %s" % (os.path.relpath(SETTINGS, ROOT), error))


def need(tool, why):
    found = shutil.which(tool)
    if found is None:
        raise ReleaseError("%s not found (%s)" % (tool, why))
    return found


# ---- 1. The tree and the tools ------------------------------------------------

def head_commit():
    return output(["git", "rev-parse", "HEAD"]).strip()


def check_clean(allow_dirty):
    changed = output(["git", "status", "--porcelain", "--untracked-files=normal"]).strip()
    if changed and not allow_dirty:
        raise ReleaseError("uncommitted changes (commit them, or --allow-dirty for a test build):\n"
                           + changed)
    if changed:
        say("warning: uncommitted changes: this build is not a commit")


def version_key(path):
    match = re.search(r"v(\d+)\.(\d+)", os.path.basename(os.path.normpath(path)))
    return tuple(int(part) for part in match.groups()) if match else (0, 0)


def find_mplabx(settings):
    if settings.get("mplabx"):
        return settings["mplabx"]
    patterns = (["C:/Program Files/Microchip/MPLABX/v*", "C:/Program Files (x86)/Microchip/MPLABX/v*"]
                if WINDOWS else
                ["/opt/microchip/mplabx/v*", "/Applications/microchip/mplabx/v*"])
    found = [path for pattern in patterns for path in glob.glob(pattern)]
    if not found:
        raise ReleaseError("MPLAB X not found: give its folder as \"mplabx\" in %s"
                           % os.path.relpath(SETTINGS, ROOT))
    return max(found, key=version_key)


def pic_tools(settings):
    mplabx = find_mplabx(settings)
    generator = os.path.join(mplabx, "mplab_platform", "bin",
                             "prjMakefilesGenerator" + (".bat" if WINDOWS else ".sh"))
    if not os.path.isfile(generator):
        raise ReleaseError("%s: no Makefile generator there (is it MPLAB X's folder?)" % generator)
    make = settings.get("make")
    if not make:
        bundled = os.path.join(mplabx, "gnuBins", "GnuWin32", "bin", "make.exe")
        make = bundled if WINDOWS and os.path.isfile(bundled) else "make"
    if not os.path.isfile(make):
        make = need(make, "make, for the PIC builds")
    return generator, make


# ---- 3. The builds -------------------------------------------------------------

def check_core(cli):
    """The esp32 core installed, warned about if not the 3.3 series."""
    try:
        listing = json.loads(output([cli, "core", "list", "--json"]))
    except ValueError:
        say("warning: could not read arduino-cli's core list")
        return
    platforms = listing.get("platforms", listing) if isinstance(listing, dict) else listing
    for platform in platforms or []:
        identity = platform.get("id") or platform.get("ID")
        version = platform.get("installed_version") or platform.get("installed") or ""
        if identity == ESP_CORE:
            say("%s %s" % (ESP_CORE, version))
            if not version.startswith(ESP_CORE_SERIES):
                say("warning: the firmware is built and run with %sx" % ESP_CORE_SERIES)
            return
    raise ReleaseError("arduino-cli has no %s core: arduino-cli core install %s" % (ESP_CORE, ESP_CORE))


def build_esp(settings, dry):
    cli = settings.get("arduino_cli") or "arduino-cli"
    if not os.path.isfile(cli):
        cli = need(cli, "arduino-cli, for the ESP build")
    if not dry:
        check_core(cli)
    if not os.path.isfile(os.path.join(ESP_LIBRARIES, "ArduinoJson", "library.properties")) and \
            not os.path.isdir(os.path.join(ESP_LIBRARIES, "ArduinoJson", "src")):
        raise ReleaseError("the ESP's libraries are not there: git submodule update --init")
    # --export-binaries: esp_src/build/esp32.esp32.esp32/, where make_dist.py
    # looks, as the IDE's "Export Compiled Binary" leaves it.
    run([cli, "compile", "--fqbn", ESP_FQBN, "--libraries", ESP_LIBRARIES, "--export-binaries",
         "--warnings", "default", ESP_SKETCH], dry=dry)


def build_pic(settings, dry):
    generator, make = pic_tools(settings)
    for project in PIC_PROJECTS:
        folder = os.path.join(ROOT, project)
        # The Makefiles from nbproject/configurations.xml, as the IDE makes them.
        run([generator, "-v", folder + "@default"], dry=dry)
        run([make, "CONF=default", "clean"], cwd=folder, dry=dry)
        run([make, "CONF=default", "build"], cwd=folder, dry=dry)


# ---- 4, 7. Over HTTPS ----------------------------------------------------------

def fetch(url):
    """The bytes at url, uncached; None if not found."""
    request = urllib.request.Request(url, headers={"Cache-Control": "no-cache",
                                                   "User-Agent": "smol-clock-release"})
    try:
        with urllib.request.urlopen(request, timeout=60) as reply:
            return reply.read()
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None
        raise ReleaseError("%s: HTTP %d" % (url, error.code))
    except (urllib.error.URLError, OSError) as error:
        raise ReleaseError("%s: %s" % (url, error))


def live_manifest(base_url, path, dry):
    """dist/manifest.json as the server has it, the local copy replaced."""
    live = fetch(base_url + "manifest.json")
    local = None
    if os.path.isfile(path):
        with open(path, "rb") as handle:
            local = handle.read()
    if live is None:
        say("no manifest at %s yet: a new one" % base_url)
        if local is not None and not dry:
            os.remove(path)
        return
    if local is not None and local != live:
        say("dist/manifest.json differed from the live one: the live one is used")
    else:
        say("the live manifest, %d bytes" % len(live))
    if not dry:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(live)


def verify(base_url, dist, files, manifest):
    for path in files + [manifest]:
        relative = os.path.relpath(path, dist).replace(os.sep, "/")
        with open(path, "rb") as handle:
            local = handle.read()
        served = fetch(base_url + relative)
        if served is None:
            raise ReleaseError("%s: not served (404)" % (base_url + relative))
        if hashlib.sha256(served).digest() != hashlib.sha256(local).digest():
            raise ReleaseError("%s: served, but not what was uploaded" % (base_url + relative))
        say("ok  %s" % relative)


# ---- 6. The upload -------------------------------------------------------------

def quote(path):
    if '"' in path:
        raise ReleaseError("%r: a path with a quote in it" % path)
    return '"%s"' % path


def upload(settings, dist, files, manifest, dry):
    target = settings.get("upload")
    if not target or ":" not in target:
        raise ReleaseError("no \"upload\": \"user@host:/folder\" in %s" % os.path.relpath(SETTINGS, ROOT))
    host, remote = target.split(":", 1)
    remote = remote.rstrip("/") or "."
    sftp = need("sftp", "OpenSSH's sftp, for the upload")
    lines = []
    folders = set()
    for path in files:
        relative = os.path.relpath(path, dist).replace(os.sep, "/")
        parts = relative.split("/")[:-1]
        for depth in range(len(parts) + 1):
            folders.add("/".join([remote] + parts[:depth]))
    # "-" ahead: carry on if the folder is there already.
    lines += ["-mkdir " + quote(folder) for folder in sorted(folders, key=len)]
    for path in files:
        relative = os.path.relpath(path, dist).replace(os.sep, "/")
        lines.append("put %s %s" % (quote(path.replace(os.sep, "/")), quote(remote + "/" + relative)))
    # The manifest last, beside the old one, then over it in one step:
    # OpenSSH's sftp renames with posix-rename, which replaces the target.
    lines.append("put %s %s" % (quote(manifest.replace(os.sep, "/")),
                                quote(remote + "/manifest.json.new")))
    lines.append("rename %s %s" % (quote(remote + "/manifest.json.new"),
                                   quote(remote + "/manifest.json")))
    say("\n".join("  " + line for line in lines))
    command = [sftp, "-b", "-"]
    if settings.get("ssh_port"):
        command += ["-P", str(settings["ssh_port"])]
    command.append(host)
    say("$ " + " ".join(command))
    if dry:
        return
    batch = tempfile.NamedTemporaryFile("w", suffix=".sftp", delete=False, encoding="utf-8")
    try:
        batch.write("\n".join(lines) + "\n")
        batch.close()
        with open(batch.name, "rb") as handle:
            code = subprocess.call(command, stdin=handle)
    finally:
        os.unlink(batch.name)
    if code != 0:
        raise ReleaseError("the upload failed (sftp exit %d): the manifest is only replaced last, "
                           "so the clocks still see the old one" % code)


# ---- 8. GitHub -----------------------------------------------------------------

def github_release(tag, commit, files, dry):
    gh = need("gh", "the GitHub CLI, for --tag")
    if not re.match(r"^v\d+\.\d+\.\d+([-.][A-Za-z0-9.]+)?$", tag):
        raise ReleaseError("--tag %s: vX.Y.Z" % tag)
    remote = output(["git", "branch", "-r", "--contains", commit]).strip()
    if not remote:
        raise ReleaseError("%s is not on GitHub yet: push it first" % commit[:12])
    run([gh, "release", "create", tag, "--target", commit, "--title", tag, "--generate-notes"]
        + files, dry=dry)


# ---- The whole -----------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--config", default="", help="the update config (make_dist.py --config)")
    parser.add_argument("--device", action="append", help="limit to an ESP (make_dist.py --device)")
    parser.add_argument("--base-url", default=make_dist.BASE_URL, help="(make_dist.py --base-url)")
    parser.add_argument("--tag", help="a GitHub release of this commit, vX.Y.Z")
    parser.add_argument("--skip-tests", action="store_true", help="no host tests")
    parser.add_argument("--skip-build", action="store_true", help="release the builds there now")
    parser.add_argument("--no-esp", action="store_true", help="the PIC's files only")
    parser.add_argument("--no-upload", action="store_true", help="stop after dist/")
    parser.add_argument("--no-verify", action="store_true", help="no download afterwards")
    parser.add_argument("--allow-dirty", action="store_true", help="uncommitted changes allowed")
    parser.add_argument("--dry-run", action="store_true", help="print, run nothing, upload nothing")
    args = parser.parse_args(argv)
    if args.tag and (args.allow_dirty or args.config or args.device):
        parser.error("--tag: a release for everyone, from a clean tree (no --allow-dirty, "
                     "--config or --device)")
    dry = args.dry_run
    try:
        settings = load_settings()
        step("1. The tree and the tools")
        check_clean(args.allow_dirty)
        commit = head_commit()
        say("commit %s" % commit)
        if not args.no_upload:
            need("sftp", "OpenSSH's sftp, for the upload")
        if args.tag:
            need("gh", "the GitHub CLI, for --tag")

        if not args.skip_tests:
            step("2. Host tests")
            if dry:
                say("(run_tests.py)")
            elif run_tests.main([]) != 0:
                raise ReleaseError("host tests failed")

        if not args.skip_build:
            step("3. Builds")
            if not args.no_esp:
                build_esp(settings, dry)
            build_pic(settings, dry)

        dist_argv = ["--base-url", args.base_url]
        if args.config:
            dist_argv += ["--config", args.config]
        for device in args.device or []:
            dist_argv += ["--device", device]
        if args.no_esp:
            dist_argv.append("--no-esp")
        dist_args = make_dist.parse_args(dist_argv)
        manifest = os.path.join(dist_args.dist, "manifest.json")

        step("4. The live manifest")
        live_manifest(dist_args.base_url, manifest, dry)

        step("5. dist/")
        if dry:
            say("(make_dist.py %s)" % " ".join(dist_argv))
            say("\ndry run: stops here, as the files to upload come from make_dist.py")
            return 0
        try:
            made = make_dist.build(dist_args)
        except (make_dist.DistError, make_dist.mi.ImageError, OSError) as error:
            raise ReleaseError("make_dist: %s" % error)
        for path, what in made:
            say("%-44s %s" % (os.path.relpath(path, ROOT), what))
        files = [path for path, _ in made if os.path.abspath(path) != os.path.abspath(manifest)]

        if args.no_upload:
            say("\nnot uploaded (--no-upload): dist/ is ready")
            return 0
        step("6. Upload")
        upload(settings, dist_args.dist, files, manifest, dry)

        if not args.no_verify:
            step("7. Served as uploaded")
            verify(dist_args.base_url, dist_args.dist, files, manifest)

        if args.tag:
            step("8. GitHub release %s" % args.tag)
            github_release(args.tag, commit, files, dry)
    except ReleaseError as error:
        print("\nrelease: %s" % error, file=sys.stderr)
        return 1
    say("\nreleased %s" % commit[:12])
    return 0


if __name__ == "__main__":
    sys.exit(main())
