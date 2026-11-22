#!/usr/bin/env python3
"""dist/ to the update server, the manifest last.

  upload_dist.py [--dist DIR] [--target USER@HOST:/FOLDER] [--port N]
                 [--key FILE] [--method auto|winscp|sftp] [--winscp PATH]
                 [--hostkey FINGERPRINT] [--all] [--force] [--dry-run] [FILE ...]

What goes: the FILEs given (inside dist/), else every file the manifest
names that is in dist/ (what the clocks download), else with --all every
file in dist/. The manifest goes last in every case: written beside the old
one as manifest.json.new and renamed over it, so no clock reads a manifest
that names a file not there yet, or half of one. A failure before that
leaves the old manifest in place.

Files already there are skipped: the live manifest is read first (an SFTP
get, no web server needed), and a file it offers at the same path with the
same SHA-256 as ours is not sent again. A file it does not offer (an install
hex given by name, say) always goes. When nothing has changed, the manifest
included, nothing is sent. --force sends everything regardless.

SFTP only: no shell, no scp, no command run on the server, so an account
chrooted to internal-sftp will do. The rename over the old manifest is one
step where the server has OpenSSH's posix-rename extension (OpenSSH's
sftp-server and internal-sftp do). Where the rename is refused because the
manifest is there (WinSCP does not use the extension: field, 2026-11-22),
the old one is renamed aside to a name of its own
(manifest.json.old-YYYYMMDDHHMMSS, never one already there), then the new
one into place, and the old one removed: a moment without a manifest (a
clock checking just then finds none and tries again later), never a wrong
one. If the second rename fails the old one is put back. Each step is a
session of its own whose commands must all work (WinSCP reports any error
in a session, even one let through), so a failure says which. For the
one-step replace on Windows, use OpenSSH's sftp (it has the extension):
"upload_method": "sftp" and an OpenSSH key.

How, by public-key authentication only (no password is ever asked for):

  winscp  WinSCP's console (WinSCP.com), on Windows. The key is a PuTTY
          .ppk, or Pageant's when no key is given. WinSCP checks the
          server's host key against --hostkey (it does not read OpenSSH's
          known_hosts): the SHA256:... that "ssh HOST" shows on a first
          connection ("ED25519 key fingerprint is SHA256:..."), or WinSCP's
          own form, as its
          Session > Generate Session URL/Code shows it ("ssh-ed25519 255
          SHA256:..."); WinSCP takes either.
  sftp    OpenSSH's sftp in batch mode (every Linux and macOS, and Windows
          10 and 11): the key is an OpenSSH one, or the agent's or
          ~/.ssh/config's when none is given. With a host key given
          ("hostkey", or "winscp_hostkey": the same fingerprint either way),
          sftp first connects once to learn the server's key (into a
          known_hosts of its own, and nothing else done), it is checked
          against the one given, and only then used, that key alone
          trusted (ssh-keyscan is not used: Windows' fails on
          OpenSSH 8.9's key exchange offer); without one, the
          host key is checked against known_hosts, so the server must be in
          it already (no prompt in batch mode). ("scp" is taken to mean
          this.) sftp's own messages are shown, so a refused key or host
          key says so.
  auto    winscp on Windows when WinSCP is installed, else sftp.

The folder is as the account sees it: under a chroot, the path inside it
(e.g. "smol@rubidium.darksky.io:/v2plus").

The settings default to tools/release.local.json (shared with release.py;
not in git):

  {
    "upload": "user@rubidium.darksky.io:/var/www/smol_clock/v2plus",
    "ssh_port": 22,
    "ssh_key": "C:/Users/me/.ssh/rubidium.ppk",
    "upload_method": "auto",
    "winscp": "C:/Program Files (x86)/WinSCP/WinSCP.com",
    "hostkey": "SHA256:..."
  }

Only "upload" is needed; the command line overrides each. --dry-run reads
the live manifest, prints what would go and the scripts, and changes
nothing.
"""
import argparse
import base64
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(TOOLS, ".."))
SETTINGS = os.path.join(TOOLS, "release.local.json")
DIST = os.path.join(ROOT, "dist")
WINDOWS = os.name == "nt"
WINSCP_PLACES = [
    os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "WinSCP", "WinSCP.com"),
    os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "WinSCP", "WinSCP.com"),
    os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs", "WinSCP", "WinSCP.com"),
]
# Remote names: what make_dist.py writes, nothing an SFTP batch would read
# otherwise.
SAFE = re.compile(r"^[A-Za-z0-9._/+-]+$")


class UploadError(Exception):
    pass


def say(text):
    print(text, flush=True)


def load_settings(path=SETTINGS):
    if not os.path.isfile(path):
        return {}
    with open(path, encoding="utf-8") as handle:
        try:
            return json.load(handle)
        except ValueError as error:
            raise UploadError("%s: %s" % (path, error))


class Target(object):
    """Where and how: from the settings, the command line over them."""

    def __init__(self, settings, target=None, port=None, key=None, method=None, winscp=None,
                 hostkey=None):
        target = target or settings.get("upload")
        if not target or ":" not in target or "@" not in target.split(":", 1)[0]:
            raise UploadError("no target: \"upload\": \"user@host:/folder\" in %s, or --target"
                              % os.path.relpath(SETTINGS, ROOT))
        login, folder = target.split(":", 1)
        self.user, self.host = login.rsplit("@", 1)
        self.folder = folder.rstrip("/") or "."
        if not SAFE.match(self.folder) or not SAFE.match(self.user) or not SAFE.match(self.host):
            raise UploadError("%s: letters, digits and ._/+- only" % target)
        self.port = int(port or settings.get("ssh_port") or 22)
        self.key = key or settings.get("ssh_key")
        if self.key:
            self.key = os.path.expanduser(self.key)
            if not os.path.isfile(self.key):
                raise UploadError("%s: no such key" % self.key)
        self.method = method or settings.get("upload_method") or "auto"
        self.method = "sftp" if self.method == "scp" else self.method
        if self.method not in ("auto", "winscp", "sftp"):
            raise UploadError("upload method %r: auto, winscp or sftp" % self.method)
        self.winscp = winscp or settings.get("winscp")
        self.hostkey = hostkey or settings.get("hostkey") or settings.get("winscp_hostkey")
        self.known_hosts = None  # sftp: the pinned host key's own file

    def login(self):
        return "%s@%s" % (self.user, self.host)


def find_winscp(given):
    if given:
        if not os.path.isfile(given):
            raise UploadError("%s: no WinSCP there" % given)
        return given
    on_path = shutil.which("WinSCP.com") or shutil.which("winscp.com")
    if on_path:
        return on_path
    for place in WINSCP_PLACES:
        if os.path.isfile(place):
            return place
    return None


def choose(target):
    """The method and its program: ("winscp", WinSCP.com) or ("sftp", sftp)."""
    method = target.method
    winscp = None
    if method == "winscp" or (method == "auto" and WINDOWS):
        winscp = find_winscp(target.winscp)
    if method == "winscp" or (method == "auto" and winscp):
        if not winscp:
            raise UploadError("WinSCP not found: install it, or give \"winscp\": its WinSCP.com")
        if not target.hostkey:
            raise UploadError("WinSCP needs the server's host key: \"hostkey\": "
                              "\"SHA256:...\" (\"ssh %s\" shows it on a first connection)"
                              % target.host)
        if target.key and not target.key.lower().endswith(".ppk"):
            raise UploadError("%s: WinSCP takes a PuTTY .ppk key (PuTTYgen or WinSCP's own key "
                              "tool converts an OpenSSH one)" % target.key)
        return "winscp", winscp
    sftp = shutil.which("sftp")
    if not sftp:
        raise UploadError("OpenSSH's sftp not found" + (" (nor WinSCP)" if WINDOWS else ""))
    return "sftp", sftp


def check(settings, **overrides):
    """For release.py's first step: the target and the tools, before any build.
    The method chosen."""
    target = Target(settings, **overrides)
    return choose(target)[0]


def chosen_files(dist, manifest, given, every):
    """The files to upload (absolute), the manifest left out: given, else
    every one the manifest names that is in dist/, else with every all."""
    manifest = os.path.abspath(manifest)
    dist = os.path.abspath(dist)
    if given:
        found = [os.path.abspath(path) for path in given]
    elif every:
        found = []
        for folder, _, names in os.walk(dist):
            found += [os.path.join(folder, name) for name in names]
    else:
        with open(manifest, encoding="utf-8") as handle:
            data = json.load(handle)
        found = []
        for entry in data.get("configurations", []):
            for key in ("esp", "pic", "pic_spl"):
                url = (entry.get(key) or {}).get("url")
                if not url:
                    continue
                # The URL's path below the manifest's folder: the file's
                # place in dist/.
                path = None
                for parts in range(1, url.count("/")):
                    tail = url.rsplit("/", parts)[1:]
                    candidate = os.path.join(dist, *tail)
                    if os.path.isfile(candidate):
                        path = candidate
                if path:
                    found.append(path)
    files = []
    for path in found:
        if path == manifest or path.endswith(".new"):
            continue
        if os.path.relpath(path, dist).startswith(".."):
            raise UploadError("%s: not in %s" % (path, dist))
        if not os.path.isfile(path):
            raise UploadError("%s: no such file" % path)
        if path not in files:
            files.append(path)
    return files


def local_of(path):
    if '"' in path:
        raise UploadError("%r: a path with a quote in it" % path)
    return path


def remote_of(target, dist, path):
    relative = os.path.relpath(path, dist).replace(os.sep, "/")
    if not SAFE.match(relative):
        raise UploadError("%s: letters, digits and ._/+- only in names" % relative)
    return target.folder + "/" + relative


def folders_of(target, remotes):
    found = set()
    for remote in remotes:
        parent = remote.rsplit("/", 1)[0]
        while parent and parent != target.folder and parent.startswith(target.folder):
            found.add(parent)
            parent = parent.rsplit("/", 1)[0]
    return sorted(found, key=lambda folder: (folder.count("/"), folder))


# ---- The sessions ------------------------------------------------------------
#
# A step is a list of (command, path, path2, may_fail): mkdir, put, rename,
# rm; written out in WinSCP's or OpenSSH sftp's batch language, one session
# each.

def fetch_step(target, local):
    """Reading the live manifest into local (allowed to be missing)."""
    return [("get", target.folder + "/manifest.json", local, True)]


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def already_there(target, dist, files, live):
    """The files the live manifest offers at the same path (its URL ending
    in the file's path below dist/) with the same SHA-256 as ours."""
    offered = {}
    for entry in (live or {}).get("configurations", []) if isinstance(live, dict) else []:
        for key in ("esp", "pic", "pic_spl"):
            part = entry.get(key) or {}
            if part.get("url") and part.get("sha256"):
                offered[part["url"]] = part["sha256"].lower()
    found = []
    for path in files:
        relative = "/" + os.path.relpath(path, dist).replace(os.sep, "/")
        hashes = [sha for url, sha in offered.items() if url.endswith(relative)]
        if hashes and sha256_of(path) in hashes:
            found.append(path)
    return found


def steps(target, dist, files, manifest, stamp=None):
    """The sessions: upload (folders, files, the manifest beside the old
    one), rename (it over the old one), and the fallback's aside (the old one
    to a name of its own, the new one in), restore (the old one back) and
    cleanup (the old one removed)."""
    new = target.folder + "/manifest.json.new"
    live = target.folder + "/manifest.json"
    old = target.folder + "/manifest.json.old-" + (stamp or time.strftime("%Y%m%d%H%M%S"))
    remotes = [remote_of(target, dist, path) for path in files]
    upload = [("mkdir", folder, None, True) for folder in folders_of(target, remotes)]
    upload += [("put", local_of(path), remote, False) for path, remote in zip(files, remotes)]
    upload.append(("put", local_of(os.path.abspath(manifest)), new, False))
    return {"upload": upload,
            "rename": [("rename", new, live, False)],
            "aside": [("rename", live, old, False), ("rename", new, live, False)],
            "restore": [("rename", old, live, False)],
            "cleanup": [("rm", old, None, False)],
            "old": old}


def winscp_script(target, step):
    """WinSCP's script for one step: open, the step (any failure not allowed
    stops it: batch abort), exit."""
    lines = ["option batch abort", "option confirm off"]
    opened = 'open sftp://%s@%s:%d/ -hostkey="%s"' % (target.user, target.host, target.port,
                                                      target.hostkey.replace('"', ""))
    if target.key:
        opened += ' -privatekey="%s"' % target.key
    lines.append(opened)
    for command, path, path2, may_fail in step:
        if may_fail:
            lines.append("option batch continue")
        if command == "put":
            lines.append('put -nopermissions -nopreservetime "%s" "%s"' % (path, path2))
        elif command == "get":
            lines.append('get "%s" "%s"' % (path, path2))
        elif command == "rename":
            lines.append('mv "%s" "%s"' % (path, path2))
        else:
            lines.append('%s "%s"' % (command, path))
        if may_fail:
            lines.append("option batch abort")
    lines.append("exit")
    return lines


def sftp_script(target, step):
    """OpenSSH sftp's batch for one step: a leading - lets a line fail; any
    other failure stops it. Local paths with forward slashes (sftp reads a
    backslash as an escape)."""
    lines = []
    for command, path, path2, may_fail in step:
        prefix = "-" if may_fail else ""
        if command == "put":
            lines.append('%sput "%s" "%s"' % (prefix, path.replace("\\", "/"), path2))
        elif command == "get":
            lines.append('%sget "%s" "%s"' % (prefix, path, path2.replace("\\", "/")))
        elif command == "rename":
            lines.append('%srename "%s" "%s"' % (prefix, path, path2))
        else:
            lines.append('%s%s "%s"' % (prefix, command, path))
    return lines


def sftp_command(sftp, target, script):
    # BatchMode: never a password or a host key prompt; a key or nothing.
    command = [sftp, "-b", script, "-P", str(target.port), "-o", "BatchMode=yes"]
    if target.key:
        command += ["-i", target.key, "-o", "IdentitiesOnly=yes"]
    if target.known_hosts:
        command += ["-o", "UserKnownHostsFile=" + target.known_hosts.replace("\\", "/"),
                    "-o", "StrictHostKeyChecking=yes"]
    return command + [target.login()]


def fingerprint(blob):
    """OpenSSH's SHA256 fingerprint of a key blob: base64, no padding."""
    return base64.b64encode(hashlib.sha256(blob).digest()).decode().rstrip("=")


def check_known_hosts(target, path):
    """The key learnt (path, as ssh writes known_hosts) against the one
    given; UploadError if it is not that one."""
    wanted = target.hostkey.split()[-1]
    wanted = wanted[7:] if wanted.startswith("SHA256:") else wanted
    wanted = wanted.rstrip("=")
    offered = []
    with open(path) as handle:
        lines = [line for line in handle.read().splitlines() if line and not line.startswith("#")]
    for line in lines:
        parts = line.split()
        if len(parts) < 3:
            continue
        try:
            got = fingerprint(base64.b64decode(parts[2]))
        except ValueError:
            continue
        offered.append("%s SHA256:%s" % (parts[1], got))
        if got != wanted:
            raise UploadError("the server's host key is not the one given (\"hostkey\": SHA256:%s): "
                              "it offers %s. A changed key: check it before trusting it"
                              % (wanted, ", ".join(offered)))
    if not offered:
        raise UploadError("no host key learnt from %s:%d" % (target.host, target.port))


def pin_host_key(target, sftp, run=subprocess.run):
    """sftp with a host key given: one connection that only learns the
    server's key (accept-new into an empty known_hosts of its own, the
    session doing nothing: "bye"), then that key checked against the one
    given. The file's path, the pinned known_hosts (removed by the caller)."""
    handle, path = tempfile.mkstemp(prefix="upload_dist", suffix=".known_hosts")
    os.close(handle)
    batch = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8")
    try:
        batch.write("bye\n")
        batch.close()
        command = [sftp, "-b", batch.name, "-P", str(target.port), "-o", "BatchMode=yes",
                   "-o", "StrictHostKeyChecking=accept-new",
                   "-o", "UserKnownHostsFile=" + path.replace("\\", "/")]
        if target.key:
            command += ["-i", target.key, "-o", "IdentitiesOnly=yes"]
        try:
            run(command + [target.login()], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                timeout=60)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise UploadError("sftp %s: %s" % (target.host, error))
        check_known_hosts(target, path)
        return path
    except Exception:
        os.unlink(path)
        raise
    finally:
        os.unlink(batch.name)


LOG = os.path.join(tempfile.gettempdir(), "upload_dist.log")


def run_step(method, program, target, name, lines, dry, runner, quiet=False):
    """One session: its script shown, run (not if dry). Its exit code."""
    if not quiet:
        say("%s:" % name)
        say("\n".join("  " + line for line in lines))
    if dry:
        return 0
    script = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8")
    try:
        script.write("\n".join(lines) + "\n")
        script.close()
        if method == "winscp":
            command = [program, "/ini=nul", "/log=" + LOG, "/script=" + script.name]
        else:
            command = sftp_command(program, target, script.name)
        say("$ " + " ".join('"%s"' % part if " " in part else part for part in command))
        return runner(command)
    finally:
        os.unlink(script.name)


# ---- The whole ---------------------------------------------------------------

def call(command):
    try:
        return subprocess.call(command)
    except OSError as error:
        raise UploadError("%s: %s" % (command[0], error))


def read_live(method, program, target, runner):
    """The live manifest, read by SFTP; None if it is not there (or not
    read)."""
    script = winscp_script if method == "winscp" else sftp_script
    folder = tempfile.mkdtemp(prefix="upload_dist")
    local = os.path.join(folder, "manifest.json")
    try:
        run_step(method, program, target, "read", script(target, fetch_step(target, local)), False,
                 runner, quiet=True)
        if not os.path.isfile(local):
            return None, None
        with open(local, "rb") as handle:
            data = handle.read()
        try:
            return json.loads(data.decode()), data
        except ValueError:
            return None, data
    finally:
        if os.path.exists(local):
            os.unlink(local)
        os.rmdir(folder)


def upload(settings, dist, files, manifest, dry=False, runner=None, force=False, **overrides):
    """files (absolute paths inside dist), then the manifest over the old one;
    unless force, those the live manifest already offers left out, and
    nothing at all if nothing changed. The method used."""
    runner = runner or call
    target = Target(settings, **overrides)
    method, program = choose(target)
    if method == "sftp" and target.hostkey:
        target.known_hosts = pin_host_key(target, program)
    try:
        return upload_to(target, method, program, dist, files, manifest, dry, runner, force)
    finally:
        if target.known_hosts:
            os.unlink(target.known_hosts)


def upload_to(target, method, program, dist, files, manifest, dry, runner, force):
    script = winscp_script if method == "winscp" else sftp_script
    where = ", log %s" % LOG if method == "winscp" else ""
    if not force:
        live, live_bytes = read_live(method, program, target, runner)
        skipped = already_there(target, dist, files, live)
        with open(manifest, "rb") as handle:
            same_manifest = live_bytes == handle.read()
        if skipped:
            say("already there (the live manifest offers them, the same SHA-256): %s" % ", ".join(
                os.path.relpath(path, dist).replace(os.sep, "/") for path in skipped))
        files = [path for path in files if path not in skipped]
        if live is None:
            say("no live manifest read: everything goes")
        if not files and same_manifest:
            say("nothing to upload: the server has these files and this manifest")
            return method
    say("%s, %d files and the manifest, to %s:%s (port %d)%s" % (
        "WinSCP" if method == "winscp" else "sftp", len(files), target.login(), target.folder,
        target.port, ", key " + target.key if target.key else ""))
    plan = steps(target, dist, files, manifest)

    def session(name):
        return run_step(method, program, target, name, script(target, plan[name]), dry, runner)

    code = session("upload")
    if code != 0:
        raise UploadError("the upload failed (exit %d%s): the manifest is only replaced last, so "
                          "the clocks still see the old one" % (code, where))
    if session("rename") == 0:
        say("uploaded: the manifest replaced in one step")
        return method
    say("the rename over the old manifest was refused: the old one aside, the new one in")
    code = session("aside")
    if code != 0:
        # Either nothing moved (the old one still live), or the old one moved
        # aside and the new one not in: put it back (refused in the first case).
        put_back = session("restore") == 0
        raise UploadError("the new manifest could not be put in place (exit %d%s): %s; the new one "
                          "is manifest.json.new" % (code, where,
                                                    "the old one is back" if put_back else
                                                    "the old one was not moved"))
    if session("cleanup") != 0:
        say("warning: %s left behind (the old manifest): remove it by hand" % plan["old"])
    say("uploaded: the manifest replaced (old one aside, new one in, old one removed)")
    return method


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("files", nargs="*", help="files in dist/ (default: what the manifest names)")
    parser.add_argument("--dist", default=DIST, help="the folder (default dist/)")
    parser.add_argument("--manifest", help="default: DIST/manifest.json")
    parser.add_argument("--settings", default=SETTINGS, help="default tools/release.local.json")
    parser.add_argument("--target", help="user@host:/folder")
    parser.add_argument("--port", type=int)
    parser.add_argument("--key", help="private key: .ppk for WinSCP, OpenSSH's for sftp")
    parser.add_argument("--method", choices=["auto", "winscp", "sftp", "scp"])
    parser.add_argument("--winscp", help="WinSCP.com")
    parser.add_argument("--hostkey", help="the server's host key fingerprint (SHA256:...)")
    parser.add_argument("--all", action="store_true", help="every file in dist/")
    parser.add_argument("--force", action="store_true",
                        help="send everything, even what the server has already")
    parser.add_argument("--dry-run", action="store_true", help="print, upload nothing")
    args = parser.parse_args(argv)
    manifest = args.manifest or os.path.join(args.dist, "manifest.json")
    try:
        if not os.path.isfile(manifest):
            raise UploadError("%s: no manifest (make_dist.py writes it)" % manifest)
        files = chosen_files(args.dist, manifest, args.files, args.all)
        upload(load_settings(args.settings), os.path.abspath(args.dist), files, manifest,
               args.dry_run, force=args.force, target=args.target, port=args.port, key=args.key,
               method=args.method, winscp=args.winscp, hostkey=args.hostkey)
    except (UploadError, OSError, ValueError) as error:
        print("upload_dist: %s" % error, file=sys.stderr)
        return 1
    if args.dry_run:
        say("dry run: nothing uploaded")
    return 0


if __name__ == "__main__":
    sys.exit(main())
