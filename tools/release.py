#!/usr/bin/env python3
"""A release, start to finish: tests, builds, dist/, upload, check, GitHub.

  release.py [--config NAME] [--device MAC ...] [--tag vX.Y.Z]
             [--skip-tests] [--skip-build] [--no-esp] [--no-upload]
             [--no-verify] [--allow-dirty] [--dry-run]
  release.py --check-tag TAG [--no-ci-check]
  release.py --from-tag TAG [--no-ci-check]

Tags (the CI on caesium, .github/workflows/release.yml, runs these):

  vX.Y.Z-test  a test release: the ESP's and the application's version
               headers must say X.Y.Z, HEAD must be the tag; released to the
               "dev" configuration only, no GitHub release.
  vX.Y.Z       a release: as above, and promoted from a test: the newest
               vA.B.C-test tag before it (an ancestor, A.B.C lower) differs
               from it in the version headers alone, its CI run succeeded
               (the release workflow, by its commit: the GitHub CLI, unless
               --no-ci-check), and the commit is on origin/master. Released
               to the default configuration, and a GitHub release of it.

--check-tag runs those checks alone (the workflow's first job, before the
manual approval); --from-tag runs them and then the release.

In order, stopping at the first failure:

 1. The working tree must be clean (--allow-dirty for a test build), so the
    release is a commit, and the tools it needs must be there.
 2. The live manifest downloaded into dist/manifest.json, so the new
    configuration is merged into what the clocks see now, not into whatever
    an earlier release left on this machine (releases made from two
    machines would otherwise undo each other's configurations). Each
    component's version header (esp_src/src/version.h,
    dspic_src/platform/version.h, dspic_spl/spl_version.h) is compared with
    what it offers this configuration: only a component with another
    version is built and released. The others keep their files and
    manifest entries as they are, so the clocks are not offered a rebuild of
    a version they have (another SHA-256, so another download and staging).
    No new version at all: nothing to do. --allow-dirty (a test build)
    builds and releases everything, whatever its version.
 3. Every host test (run_tests.py).
 4. The builds of what is new: the ESP with arduino-cli, the SPL and the
    application with MPLAB X's own Makefiles, headless, as the IDE builds
    them (production); a PIC project also when its build is missing, the IPL
    only when none is released (dspic_ipl/release/).
 5. make_dist.py: the release files and the manifest, as before.
 6. The upload, by upload_dist.py (also run on its own): WinSCP on
    Windows, OpenSSH's sftp elsewhere, SFTP only (a chrooted SFTP-only
    account will do), public-key authentication only. The files first, the
    manifest last, written beside the old one and renamed over it, so no
    clock reads a manifest naming a file that is not there yet, or half a
    manifest.
 7. Each file and the manifest downloaded back over HTTPS and compared.
 8. With --tag, a GitHub release of this commit with the files attached
    (the GitHub CLI, gh). The commit must be on GitHub already.

Settings for this machine, in tools/release.local.json (not in git;
tools/release.sample.json is a sample, each key explained):

  {
    "upload": "user@rubidium.darksky.io:/var/www/smol_clock/v2plus",
    "ssh_port": 22,
    "ssh_key": "C:/Users/me/.ssh/rubidium.ppk",
    "upload_method": "auto",
    "hostkey": "SHA256:...",
    "arduino_cli": "arduino-cli",
    "arduino_config": "/home/smolbuild/.arduino15/arduino-cli.yaml",
    "esp32_core": "3.3.13",
    "mplabx": "C:/Program Files/Microchip/MPLABX/v6.20",
    "make": "make",
    "pic_build": "xc16",
    "xc16": "/opt/microchip/xc16/v2.10",
    "dfp": "/opt/microchip/packs/dsPIC33E-GM-GP-MC-GU-MU_DFP/1.6.297"
  }

"upload" is the folder the base URL (make_dist.py --base-url) serves, as
user@host:path. The upload's own settings (ssh_key, upload_method, winscp,
hostkey) are upload_dist.py's: its help explains them. The rest are
optional: arduino_cli defaults to the one on
the PATH, arduino_config to arduino-cli's own, esp32_core to the version
build_esp.py names (tools/build_esp.py builds the ESP: given, the core
version must be exactly that one), mplabx to the newest MPLAB X installed in its usual place, make
to MPLAB X's own on Windows and to the PATH's elsewhere. pic_build "xc16"
(the default) builds the PIC projects with XC16 alone (tools/build_pic.py:
the same commands as MPLAB X's Makefiles, from configurations.xml), with xc16
and dfp its folders (build_pic.py finds them in the usual places if not
given); "mplabx" uses MPLAB X's own Makefiles, with mplabx and make. --dry-run prints
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
import urllib.error
import urllib.request

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(TOOLS, ".."))
sys.path.insert(0, TOOLS)
import make_dist  # noqa: E402
import make_manifest  # noqa: E402
import run_tests  # noqa: E402
import build_esp as esp_builder  # noqa: E402
import build_pic as pic_builder  # noqa: E402
import upload_dist  # noqa: E402

# On the CI machine the settings live outside the checkout.
SETTINGS = os.environ.get("SMOL_RELEASE_SETTINGS") or os.path.join(TOOLS, "release.local.json")
VERSION_HEADERS = ["esp_src/src/version.h", "dspic_src/platform/version.h", "dspic_spl/spl_version.h"]
RELEASE_WORKFLOW = "release.yml"
PIC_PROJECTS = ["dspic_ipl", "dspic_spl", "dspic_src"]
# Each component of a release: the version header it is built from, and the
# project that builds it.
COMPONENTS = {
    "esp": (os.path.join(ROOT, "esp_src", "src", "version.h"), "ESP", None),
    "pic": (os.path.join(ROOT, "dspic_src", "platform", "version.h"), "PIC", "dspic_src"),
    "pic_spl": (os.path.join(ROOT, "dspic_spl", "spl_version.h"), "SPL", "dspic_spl"),
}
COMPONENT_NAMES = {"esp": "ESP", "pic": "application", "pic_spl": "SPL"}
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

def build_esp(settings, dry):
    """tools/build_esp.py, with the release settings' tools and core."""
    try:
        cli = esp_builder.find_cli(settings.get("arduino_cli"))
        config = esp_builder.find_config(settings.get("arduino_config"))
        if dry:
            say("$ " + " ".join(esp_builder.compile_command(cli, config)))
            return
        esp_builder.build(cli, config, settings.get("esp32_core"), say=say)
    except esp_builder.BuildError as error:
        raise ReleaseError(str(error))


def build_pic(settings, dry, projects=PIC_PROJECTS):
    if settings.get("pic_build", "xc16") == "xc16":
        try:
            for project in projects:
                folder = os.path.join(ROOT, project)
                device = pic_builder.Project(folder).device
                xc16 = pic_builder.find_xc16(settings.get("xc16"))
                dfp = pic_builder.find_dfp(settings.get("dfp"), device)
                say("%s: XC16 alone (%s, %s)" % (project, xc16, os.path.basename(dfp)))
                pic_builder.build(folder, xc16, dfp, show=dry)
        except pic_builder.BuildError as error:
            raise ReleaseError(str(error))
        return
    if settings.get("pic_build") != "mplabx":
        raise ReleaseError("pic_build is \"mplabx\" or \"xc16\", not %r" % settings["pic_build"])
    generator, make = pic_tools(settings)
    for project in projects:
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
        return None
    if local is not None and local != live:
        say("dist/manifest.json differed from the live one: the live one is used")
    else:
        say("the live manifest, %d bytes" % len(live))
    if not dry:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(live)
    return live


# ---- Which components have a new version ------------------------------------------

def source_version(path, prefix):
    """X.Y.Z from a version header's <prefix>_VERSION_MAJOR/MINOR/PATCH."""
    with open(path, encoding="utf-8") as handle:
        text = handle.read()
    parts = []
    for part in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(r"^#define\s+%s_VERSION_%s\s+(\d+)" % (prefix, part), text, re.M)
        if not match:
            raise ReleaseError("%s: no %s_VERSION_%s" % (os.path.relpath(path, ROOT), prefix, part))
        parts.append(match.group(1))
    return ".".join(parts)


def numeric(version):
    """The first three numbers of a version string, or None."""
    found = re.findall(r"\d+", version or "")
    return tuple(int(part) for part in found[:3]) if len(found) >= 3 else None


def live_entry(live, config, devices):
    """This configuration's entry in the live manifest (its bytes), or None."""
    if live is None:
        return None
    try:
        manifest = json.loads(live.decode("utf-8"))
    except ValueError as error:
        raise ReleaseError("the live manifest: %s" % error)
    target = {"hardware": "v2", "config": config}
    if devices:
        target["devices"] = devices
    for entry in manifest.get("configurations", []):
        if make_manifest.same_target(entry, target):
            return entry
    return None


def changed_components(entry, keys):
    """For each component, whether its header's version differs from the one
    the live manifest offers this configuration (or it offers none)."""
    changed = {}
    for key in keys:
        header, prefix, _ = COMPONENTS[key]
        source = source_version(header, prefix)
        live = (entry or {}).get(key, {}).get("version")
        changed[key] = live is None or numeric(source) != numeric(live)
        if live is None:
            say("%-12s %s, none offered yet" % (COMPONENT_NAMES[key], source))
        elif changed[key]:
            older = numeric(source) < numeric(live)
            say("%-12s %s, offered %s%s" % (COMPONENT_NAMES[key], source, live,
                                            ": OLDER, clocks will not take it" if older else ""))
        else:
            say("%-12s %s, as offered: kept as it is" % (COMPONENT_NAMES[key], source))
    return changed


def keep_live(manifest_path, config, devices, live, keys, dist, folder):
    """The live files of unchanged components put back, so their clocks are
    not offered a rebuild of the same version (another SHA-256, another
    download): their manifest entries as they were, and the files in dist/
    as served. The paths of those files, to leave out of the upload."""
    with open(manifest_path, encoding="utf-8") as handle:
        manifest = json.load(handle)
    target = {"hardware": "v2", "config": config}
    if devices:
        target["devices"] = devices
    entry = next(e for e in manifest["configurations"] if make_manifest.same_target(e, target))
    kept = []
    for key in keys:
        entry[key] = live[key]
        name = live[key]["url"].rsplit("/", 1)[-1]
        data = fetch(live[key]["url"])
        if data is None or hashlib.sha256(data).hexdigest() != live[key]["sha256"]:
            raise ReleaseError("%s: not served as the manifest says: cannot keep it" % live[key]["url"])
        path = os.path.join(dist, folder, name)
        with open(path, "wb") as handle:
            handle.write(data)
        kept.append(path)
        say("%-12s %s kept: %s" % (COMPONENT_NAMES[key], live[key]["version"], name))
    with open(manifest_path, "w", encoding="utf-8") as handle:
        handle.write(json.dumps(manifest, indent=2) + "\n")
    return kept


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


# ---- 6. The upload: upload_dist.py -------------------------------------------

def upload(settings, dist, files, manifest, dry, force=False):
    try:
        upload_dist.upload(settings, dist, files, manifest, dry, force=force)
    except upload_dist.UploadError as error:
        raise ReleaseError(str(error))


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


# ---- Tags (CI) ------------------------------------------------------------------

TAG = re.compile(r"^v(\d+)\.(\d+)\.(\d+)(-test)?$")


def git_lines(arguments, cwd=ROOT):
    return [line for line in output(["git"] + arguments, cwd).splitlines() if line.strip()]


def tag_commit(tag, cwd=ROOT):
    return output(["git", "rev-parse", tag + "^{commit}"], cwd).strip()


def header_versions(cwd=ROOT):
    """The ESP's, the application's and the SPL's header versions."""
    return {key: source_version(os.path.join(cwd, os.path.relpath(header, ROOT)), prefix)
            for key, (header, prefix, _) in COMPONENTS.items()}


def promoted_from(tag, cwd=ROOT):
    """The test tag a release is promoted from: the highest vA.B.C-test that
    is an ancestor of the release's commit and lower than it."""
    version = tuple(int(part) for part in TAG.match(tag).groups()[:3])
    tests = []
    for name in git_lines(["tag", "--merged", tag, "--list", "v*-test"], cwd):
        match = TAG.match(name)
        if match and match.group(4):
            number = tuple(int(part) for part in match.groups()[:3])
            if number < version:
                tests.append((number, name))
    if not tests:
        raise ReleaseError("%s: no vX.Y.Z-test tag before it to promote" % tag)
    return max(tests)[1]


def repository(cwd=ROOT):
    """owner/name, from GITHUB_REPOSITORY (in CI) or the origin URL."""
    if os.environ.get("GITHUB_REPOSITORY"):
        return os.environ["GITHUB_REPOSITORY"]
    url = output(["git", "remote", "get-url", "origin"], cwd).strip()
    match = re.search(r"github\.com[:/]([^/]+/[^/]+?)(?:\.git)?$", url)
    if not match:
        raise ReleaseError("origin is not on GitHub: %s" % url)
    return match.group(1)


def ci_passed(commit, cwd=ROOT):
    """Whether the release workflow succeeded for that commit (gh)."""
    gh = need("gh", "the GitHub CLI, to see the test tag's CI run")
    runs = output([gh, "api", "repos/%s/actions/workflows/%s/runs?head_sha=%s&status=success"
                   % (repository(cwd), RELEASE_WORKFLOW, commit), "--jq", ".total_count"], cwd)
    try:
        return int(runs.strip() or "0") > 0
    except ValueError:
        raise ReleaseError("gh: %s" % runs.strip())


def check_tag(tag, ci_check=True, cwd=ROOT):
    """A tag's checks (above). Whether it is a test tag."""
    match = TAG.match(tag or "")
    if not match:
        raise ReleaseError("%s: tags are vX.Y.Z (a release) or vX.Y.Z-test (a test)" % tag)
    version = ".".join(match.groups()[:3])
    test = bool(match.group(4))
    head = output(["git", "rev-parse", "HEAD"], cwd).strip()
    if tag_commit(tag, cwd) != head:
        raise ReleaseError("%s is not HEAD: check out the tag" % tag)
    versions = header_versions(cwd)
    for key in ("esp", "pic"):
        if versions[key] != version:
            raise ReleaseError("%s: the %s's version header says %s" % (tag, COMPONENT_NAMES[key], versions[key]))
    say("%s: the ESP and the application at %s, the SPL at %s" % (tag, version, versions["pic_spl"]))
    if test:
        say("%s: a test release, for the dev configuration" % tag)
        return True
    source = promoted_from(tag, cwd)
    changed = git_lines(["diff", "--name-only", source, tag], cwd)
    other = [path for path in changed if path not in VERSION_HEADERS]
    if other:
        raise ReleaseError("%s: more than the version headers changed since %s: %s"
                           % (tag, source, ", ".join(other)))
    say("%s: promoted from %s, only %s changed" % (tag, source, ", ".join(changed) or "nothing"))
    if ci_check:
        output(["git", "fetch", "--quiet", "origin", "master"], cwd)
    if subprocess.call(["git", "merge-base", "--is-ancestor", head, "origin/master"], cwd=cwd) != 0:
        raise ReleaseError("%s is not on origin/master" % tag)
    if ci_check:
        if not ci_passed(tag_commit(source, cwd), cwd):
            raise ReleaseError("%s: no successful %s run for %s" % (tag, RELEASE_WORKFLOW, source))
        say("%s: %s's CI run succeeded" % (tag, source))
    return False


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
    parser.add_argument("--force-upload", action="store_true",
                        help="upload every file, even those the server has already")
    parser.add_argument("--no-verify", action="store_true", help="no download afterwards")
    parser.add_argument("--allow-dirty", action="store_true", help="uncommitted changes allowed, and every component built and released whatever its version")
    parser.add_argument("--dry-run", action="store_true", help="print, run nothing, upload nothing")
    parser.add_argument("--check-tag", metavar="TAG", help="the tag's checks alone (above)")
    parser.add_argument("--from-tag", metavar="TAG",
                        help="the tag's checks, then its release (dev for a -test tag)")
    parser.add_argument("--no-ci-check", action="store_true",
                        help="with a release tag: skip the test tag's CI run and the fetch")
    args = parser.parse_args(argv)
    if args.check_tag:
        try:
            check_tag(args.check_tag, not args.no_ci_check)
        except ReleaseError as error:
            print("\nrelease: %s" % error, file=sys.stderr)
            return 1
        return 0
    if args.from_tag:
        if args.tag or args.config or args.device or args.allow_dirty:
            parser.error("--from-tag decides --config and --tag itself")
        try:
            test = check_tag(args.from_tag, not args.no_ci_check)
        except ReleaseError as error:
            print("\nrelease: %s" % error, file=sys.stderr)
            return 1
        if test:
            args.config = "dev"
        else:
            args.tag = args.from_tag
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
            try:
                say("upload by %s" % upload_dist.check(settings))
            except upload_dist.UploadError as error:
                raise ReleaseError(str(error))
        if args.tag:
            need("gh", "the GitHub CLI, for --tag")

        dist_argv = ["--base-url", args.base_url]
        if args.config:
            dist_argv += ["--config", args.config]
        for device in args.device or []:
            dist_argv += ["--device", device]
        dist_args = make_dist.parse_args(dist_argv)
        manifest = os.path.join(dist_args.dist, "manifest.json")
        devices = sorted(set(make_dist.normal_mac(d) for d in args.device or []))

        # Before the builds: what is new decides what is built.
        step("2. The live manifest, and what is new")
        entry = live_entry(live_manifest(dist_args.base_url, manifest, dry), args.config, devices)
        keys = ["pic", "pic_spl"] + ([] if args.no_esp else ["esp"])
        changed = changed_components(entry, keys)
        if args.allow_dirty:
            # A test build: everything rebuilt and offered, whatever its version.
            changed = {key: True for key in keys}
            say("--allow-dirty: every component built and released, whatever its version")
        elif not any(changed.values()):
            say("\nnothing to release: every version is the one offered. Raise one in its "
                "version header, or --allow-dirty to rebuild and release anyway.")
            return 0

        if not args.skip_tests:
            step("3. Host tests")
            if dry:
                say("(run_tests.py)")
            elif run_tests.main([]) != 0:
                raise ReleaseError("host tests failed")

        if not args.skip_build:
            step("4. Builds")
            if "esp" in keys and changed["esp"]:
                build_esp(settings, dry)
            # A PIC project is built when its version is new, or when its
            # build is not there for make_dist.py to read; the IPL only when
            # none is released.
            projects = []
            if make_dist.released_ipl() is None:
                projects.append("dspic_ipl")
            for key, built in (("pic_spl", make_dist.SPL_HEX), ("pic", make_dist.APP_HEX)):
                if changed[key] or not os.path.isfile(built):
                    projects.append(COMPONENTS[key][2])
            build_pic(settings, dry, [p for p in PIC_PROJECTS if p in projects])

        if args.no_esp or not changed.get("esp", False):
            dist_args.no_esp = True  # kept from the live manifest (keep_live), or not released

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
        unchanged = [key for key in keys if not changed[key]]
        kept = []
        if unchanged:
            folder = os.path.relpath(os.path.dirname(made[0][0]), dist_args.dist)
            kept = keep_live(manifest, args.config, devices, entry, unchanged, dist_args.dist, folder)
        kept_paths = set(os.path.abspath(path) for path in kept)
        files = [path for path, _ in made if os.path.abspath(path) != os.path.abspath(manifest) and
                 os.path.abspath(path) not in kept_paths]

        if args.no_upload:
            say("\nnot uploaded (--no-upload): dist/ is ready")
            return 0
        step("6. Upload")
        upload(settings, dist_args.dist, files, manifest, dry, args.force_upload)

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
