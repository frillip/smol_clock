"""upload_dist.py: what is chosen to go, WinSCP's and OpenSSH sftp's
scripts (SFTP only: the manifest last, renamed over the old one, or the old
one aside first where that is refused), public-key only, and failures that
leave the old manifest in place. Nothing is run: the commands and scripts
are collected."""
import base64
import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import upload_dist as ud  # noqa: E402

BASE = "https://example.org/smol_clock/v2plus/"


class Dist(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.dist = os.path.join(self.folder.name, "dist")
        self.files = {}
        for relative in ("prod/esp-0.3.70.bin", "prod/pic-0.3.68.smolpic", "prod/spl-1.0.0.smolspl",
                         "dev/EC64C906D350/esp-0.3.71.bin", "prod/install-0.3.68.hex", "prod/esp-0.3.69.bin"):
            path = os.path.join(self.dist, *relative.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as handle:
                handle.write(relative)
            self.files[relative] = path
        self.manifest = os.path.join(self.dist, "manifest.json")
        with open(self.manifest, "w") as handle:
            json.dump({"format": 2, "configurations": [
                {"hardware": "v2", "devices": ["EC:64:C9:06:D3:50"], "config": "dev",
                 "esp": {"url": BASE + "dev/EC64C906D350/esp-0.3.71.bin"}},
                {"hardware": "v2", "esp": {"url": BASE + "prod/esp-0.3.70.bin"},
                 "pic": {"url": BASE + "prod/pic-0.3.68.smolpic"},
                 "pic_spl": {"url": BASE + "prod/spl-1.0.0.smolspl"}}]}, handle)
        self.key = os.path.join(self.folder.name, "rubidium.ppk")
        self.openssh_key = os.path.join(self.folder.name, "id_ed25519")
        for path in (self.key, self.openssh_key):
            with open(path, "w") as handle:
                handle.write("key")
        self.ran = []
        self.scripts = []
        self.reads = []
        self.live = None
        self.codes = []

    def tearDown(self):
        self.folder.cleanup()

    def runner(self, command):
        if "-b" in command:
            path = command[command.index("-b") + 1]
        else:
            path = [part for part in command if part.startswith("/script=")][0][8:]
        with open(path) as handle:
            lines = handle.read().splitlines()
        gets = [line for line in lines if line.lstrip("-").startswith("get ")]
        if gets:
            # Reading the live manifest: played by the test.
            self.reads.append(lines)
            if self.live is not None:
                local = gets[0].rsplit('"', 2)[1]
                with open(local, "wb") as handle:
                    handle.write(self.live)
            return 0
        self.scripts.append(lines)
        self.ran.append(command)
        return self.codes.pop(0) if self.codes else 0

    def settings(self, **more):
        found = {"upload": "clock@rubidium.example:/var/www/v2plus"}
        found.update(more)
        return found


class Chosen(Dist):
    def test_what_the_manifest_names(self):
        files = ud.chosen_files(self.dist, self.manifest, [], False)
        self.assertEqual(sorted(os.path.relpath(p, self.dist).replace(os.sep, "/") for p in files),
                         ["dev/EC64C906D350/esp-0.3.71.bin", "prod/esp-0.3.70.bin",
                          "prod/pic-0.3.68.smolpic", "prod/spl-1.0.0.smolspl"])

    def test_all_and_given(self):
        files = ud.chosen_files(self.dist, self.manifest, [], True)
        self.assertEqual(len(files), 6)  # the manifest left out
        files = ud.chosen_files(self.dist, self.manifest, [self.files["prod/install-0.3.68.hex"]], False)
        self.assertEqual(files, [self.files["prod/install-0.3.68.hex"]])
        with self.assertRaises(ud.UploadError):
            ud.chosen_files(self.dist, self.manifest, [self.key], False)  # outside dist/


class Sftp(Dist):
    def upload(self, settings, files=None):
        out = io.StringIO()
        with mock.patch.object(ud, "WINDOWS", False), \
                mock.patch.object(ud.shutil, "which", lambda name: "/usr/bin/" + name), \
                redirect_stdout(out):
            method = ud.upload(settings, self.dist, files or [self.files["prod/esp-0.3.70.bin"],
                                                              self.files["dev/EC64C906D350/esp-0.3.71.bin"]],
                               self.manifest, runner=self.runner)
        return method, out.getvalue()

    def test_sftp_only(self):
        method, out = self.upload(self.settings(ssh_port=2222, ssh_key=self.openssh_key))
        self.assertEqual(method, "sftp")
        # Two sessions: the upload, the rename. Nothing but sftp run.
        self.assertEqual([command[0] for command in self.ran], ["/usr/bin/sftp", "/usr/bin/sftp"])
        for command in self.ran:
            self.assertIn("BatchMode=yes", command)  # never a password prompt
            self.assertIn(self.openssh_key, command)
            self.assertEqual(command[-1], "clock@rubidium.example")
            self.assertEqual(command[command.index("-P") + 1], "2222")
        upload, rename = self.scripts
        # Parents first, allowed to fail (there already).
        self.assertEqual(upload[:3], ['-mkdir "/var/www/v2plus/dev"', '-mkdir "/var/www/v2plus/prod"',
                                      '-mkdir "/var/www/v2plus/dev/EC64C906D350"'])
        self.assertTrue(upload[3].startswith('put "') and upload[3].endswith('" "/var/www/v2plus/prod/esp-0.3.70.bin"'))
        self.assertTrue(upload[-1].endswith('" "/var/www/v2plus/manifest.json.new"'))
        self.assertNotIn("\\", "".join(upload))  # forward slashes for sftp
        self.assertEqual(rename, ['rename "/var/www/v2plus/manifest.json.new" "/var/www/v2plus/manifest.json"'])
        for line in upload + rename:
            self.assertNotRegex(line, r"^-?(mv|ln|chmod|!)")  # no shell

    def test_agent_key(self):
        self.upload(self.settings())
        self.assertNotIn("-i", self.ran[0])
        self.assertIn("BatchMode=yes", self.ran[0])

    def test_scp_means_sftp(self):
        method, out = self.upload(self.settings(upload_method="scp"))
        self.assertEqual(method, "sftp")

    def test_rename_refused_the_old_one_aside(self):
        # The field (WinSCP, 2026-11-22): the rename refused, the aside worked,
        # and an old file left behind. Now: a name of its own, then removed;
        # every command in every session must work (no "-").
        self.codes = [0, 1, 0, 0]
        with mock.patch.object(ud.time, "strftime", lambda form: "20261122071300"):
            method, out = self.upload(self.settings())
        self.assertIn("the old one aside", out)
        old = "/var/www/v2plus/manifest.json.old-20261122071300"
        self.assertEqual(self.scripts[2], ['rename "/var/www/v2plus/manifest.json" "%s"' % old,
                                           'rename "/var/www/v2plus/manifest.json.new" "/var/www/v2plus/manifest.json"'])
        self.assertEqual(self.scripts[3], ['rm "%s"' % old])
        self.assertIn("old one removed", out)

    def test_cleanup_failing_is_a_warning(self):
        self.codes = [0, 1, 0, 1]
        method, out = self.upload(self.settings())
        self.assertIn("warning: /var/www/v2plus/manifest.json.old-", out)
        self.assertIn("left behind", out)

    def test_failure_before_the_manifest(self):
        self.codes = [1]
        with self.assertRaises(ud.UploadError) as caught:
            self.upload(self.settings())
        self.assertIn("still see the old one", str(caught.exception))
        self.assertEqual(len(self.ran), 1)  # no rename tried

    def test_failure_in_place(self):
        # The aside failing: the old one put back if it had moved.
        self.codes = [0, 1, 1, 0]
        with self.assertRaises(ud.UploadError) as caught:
            self.upload(self.settings())
        self.assertIn("the old one is back", str(caught.exception))
        self.assertEqual(self.scripts[3][0].split()[0], "rename")
        self.assertTrue(self.scripts[3][0].endswith('"/var/www/v2plus/manifest.json"'))
        self.codes = [0, 1, 1, 1]
        self.scripts = []
        with self.assertRaises(ud.UploadError) as caught:
            self.upload(self.settings())
        self.assertIn("the old one was not moved", str(caught.exception))

    def test_chroot_folder_and_bad_names(self):
        self.upload(self.settings(upload="smol@rubidium.example:/v2plus"))
        self.assertEqual(self.scripts[1], ['rename "/v2plus/manifest.json.new" "/v2plus/manifest.json"'])
        with self.assertRaises(ud.UploadError):
            self.upload(self.settings(upload="clock@rubidium.example:/var/www/my folder"))
        with self.assertRaises(ud.UploadError):
            self.upload(self.settings(upload="rubidium.example:/var/www"))  # no user


class Skipping(Dist):
    upload = Sftp.upload

    def live_from_local(self, change=None):
        with open(self.manifest) as handle:
            data = json.load(handle)
        if change:
            change(data)
        self.live = json.dumps(data).encode()

    def hashed_manifest(self):
        # The manifest as make_dist.py writes it: each file's SHA-256.
        with open(self.manifest) as handle:
            data = json.load(handle)
        for entry in data["configurations"]:
            for key in ("esp", "pic", "pic_spl"):
                if key in entry:
                    tail = entry[key]["url"].split("/v2plus/", 1)[1]
                    entry[key]["sha256"] = ud.sha256_of(os.path.join(self.dist, *tail.split("/")))
        with open(self.manifest, "w") as handle:
            json.dump(data, handle)

    def all_named(self):
        return ud.chosen_files(self.dist, self.manifest, [], False)

    def test_nothing_changed_nothing_sent(self):
        self.hashed_manifest()
        with open(self.manifest, "rb") as handle:
            self.live = handle.read()
        method, out = self.upload(self.settings(), self.all_named())
        self.assertEqual(self.ran, [])
        self.assertEqual(len(self.reads), 1)
        self.assertEqual(self.reads[0], ['-get "/var/www/v2plus/manifest.json" "%s"'
                                         % self.reads[0][0].rsplit('"', 2)[1]])
        self.assertIn("nothing to upload", out)

    def test_only_what_changed(self):
        self.hashed_manifest()
        # The server offers the old ESP build: only that goes, and the manifest.
        def old_esp(data):
            data["configurations"][1]["esp"]["sha256"] = "0" * 64
        self.live_from_local(old_esp)
        method, out = self.upload(self.settings(), self.all_named())
        upload = self.scripts[0]
        puts = [line for line in upload if line.startswith("put ")]
        self.assertEqual(len(puts), 2)
        self.assertTrue(puts[0].endswith('"/var/www/v2plus/prod/esp-0.3.70.bin"'))
        self.assertTrue(puts[1].endswith('"/var/www/v2plus/manifest.json.new"'))
        self.assertIn("already there", out)
        self.assertIn("dev/EC64C906D350/esp-0.3.71.bin", out)

    def test_manifest_only(self):
        self.hashed_manifest()
        self.live_from_local(lambda data: data.update(note="older"))
        method, out = self.upload(self.settings(), self.all_named())
        puts = [line for line in self.scripts[0] if line.startswith("put ")]
        self.assertEqual(len(puts), 1)  # files all there; the manifest differs
        self.assertEqual(len(self.scripts), 2)

    def test_force_and_no_live_manifest(self):
        self.hashed_manifest()
        with open(self.manifest, "rb") as handle:
            self.live = handle.read()
        out = io.StringIO()
        with mock.patch.object(ud, "WINDOWS", False), \
                mock.patch.object(ud.shutil, "which", lambda name: "/usr/bin/" + name), redirect_stdout(out):
            ud.upload(self.settings(), self.dist, self.all_named(), self.manifest, runner=self.runner, force=True)
        self.assertEqual(self.reads, [])  # not even read
        self.assertEqual(len([line for line in self.scripts[0] if line.startswith("put ")]), 5)
        self.live = None
        self.scripts = []
        method, out = self.upload(self.settings(), self.all_named())
        self.assertIn("no live manifest read", out)
        self.assertEqual(len([line for line in self.scripts[0] if line.startswith("put ")]), 5)

    def test_given_files_not_offered_go(self):
        self.hashed_manifest()
        with open(self.manifest, "rb") as handle:
            self.live = handle.read()
        method, out = self.upload(self.settings(), [self.files["prod/install-0.3.68.hex"]])
        puts = [line for line in self.scripts[0] if line.startswith("put ")]
        self.assertTrue(puts[0].endswith('"/var/www/v2plus/prod/install-0.3.68.hex"'))


class HostKey(Dist):
    """sftp with a host key given: one connection learns the server's key into
    a known_hosts of its own, it is checked, and it alone trusted after."""

    def server_key(self):
        blob = b"\x00\x00\x00\x0bssh-ed25519\x00\x00\x00\x20" + bytes(range(32))
        return "rubidium.example ssh-ed25519 " + base64.b64encode(blob).decode(), ud.fingerprint(blob)

    def learning(self, line):
        """subprocess.run played: ssh writing the server's key (accept-new)."""
        self.probes = []

        def run(command, **kwargs):
            self.probes.append(command)
            path = [part for part in command if part.startswith("UserKnownHostsFile=")][0][19:]
            with open(path, "w") as handle:
                handle.write(line + "\n" if line else "")
        return run

    def test_pinned(self):
        line, fp = self.server_key()
        for given in ("SHA256:" + fp, "ssh-ed25519 255 " + fp, fp):
            target = ud.Target(self.settings(hostkey=given))
            path = ud.pin_host_key(target, "sftp", self.learning(line))
            try:
                probe = self.probes[0]
                self.assertIn("StrictHostKeyChecking=accept-new", probe)
                self.assertIn("BatchMode=yes", probe)
                with open(path) as handle:
                    self.assertEqual(handle.read(), line + "\n")
                target.known_hosts = path
                command = ud.sftp_command("sftp", target, "batch.txt")
                self.assertIn("StrictHostKeyChecking=yes", command)
                self.assertIn("UserKnownHostsFile=" + path.replace("\\", "/"), command)
                self.assertNotIn("-q", command)  # sftp's own messages shown
            finally:
                os.unlink(path)

    def test_mismatch_refused(self):
        line, fp = self.server_key()
        target = ud.Target(self.settings(hostkey="SHA256:" + "A" * 43))
        with self.assertRaises(ud.UploadError) as caught:
            ud.pin_host_key(target, "sftp", self.learning(line))
        self.assertIn("not the one given", str(caught.exception))
        self.assertIn("ssh-ed25519 SHA256:" + fp, str(caught.exception))
        with self.assertRaises(ud.UploadError) as caught:
            ud.pin_host_key(target, "sftp", self.learning(""))  # not reached
        self.assertIn("no host key learnt", str(caught.exception))

    def test_upload_uses_and_removes_it(self):
        line, fp = self.server_key()
        made = []
        real = ud.pin_host_key

        def pin(target, sftp):
            made.append(real(target, sftp, self.learning(line)))
            return made[-1]

        out = io.StringIO()
        with mock.patch.object(ud, "WINDOWS", True), mock.patch.object(ud, "find_winscp", lambda given: None), \
                mock.patch.object(ud.shutil, "which", lambda name: "C:/Windows/System32/OpenSSH/" + name), \
                mock.patch.object(ud, "pin_host_key", pin), redirect_stdout(out):
            ud.upload(self.settings(winscp_hostkey="SHA256:" + fp), self.dist, [], self.manifest,
                      runner=self.runner)
        self.assertTrue(all("StrictHostKeyChecking=yes" in command for command in self.ran))
        self.assertFalse(os.path.exists(made[0]))


class WinScp(Dist):
    def upload(self, settings, dry=False):
        out = io.StringIO()
        winscp = os.path.join(self.folder.name, "WinSCP.com")
        with open(winscp, "w") as handle:
            handle.write("")
        settings.setdefault("winscp", winscp)
        with mock.patch.object(ud, "WINDOWS", True), redirect_stdout(out):
            method = ud.upload(settings, self.dist, [self.files["prod/esp-0.3.70.bin"]], self.manifest,
                               dry=dry, runner=self.runner)
        return method, out.getvalue()

    def test_scripts(self):
        method, out = self.upload(self.settings(ssh_key=self.key,
                                                winscp_hostkey="ssh-ed25519 255 SHA256:abc"))
        self.assertEqual(method, "winscp")
        self.assertIn("/ini=nul", self.ran[0])
        upload, rename = self.scripts
        for script in (upload, rename):
            self.assertEqual(script[0], "option batch abort")
            self.assertEqual(script[2], 'open sftp://clock@rubidium.example:22/ -hostkey="ssh-ed25519 255 '
                                        'SHA256:abc" -privatekey="%s"' % self.key)
            self.assertEqual(script[-1], "exit")
            self.assertFalse(any(line.startswith("call") for line in script))  # no shell
        self.assertIn('mkdir "/var/www/v2plus/prod"', upload)
        puts = [line for line in upload if line.startswith("put ")]
        self.assertTrue(puts[0].endswith('"/var/www/v2plus/prod/esp-0.3.70.bin"'))
        self.assertTrue(puts[-1].endswith('"/var/www/v2plus/manifest.json.new"'))
        self.assertEqual(rename[3], 'mv "/var/www/v2plus/manifest.json.new" "/var/www/v2plus/manifest.json"')

    def test_reads_the_live_manifest(self):
        self.upload(self.settings(winscp_hostkey="SHA256:abc"))
        read = self.reads[0]
        self.assertEqual(read[3], "option batch continue")  # no manifest yet is fine
        self.assertTrue(read[4].startswith('get "/var/www/v2plus/manifest.json" "'))

    def test_rename_refused(self):
        self.codes = [0, 1, 0, 0]
        with mock.patch.object(ud.time, "strftime", lambda form: "20261122071300"):
            self.upload(self.settings(winscp_hostkey="SHA256:abc"))
        old = "/var/www/v2plus/manifest.json.old-20261122071300"
        aside, cleanup = self.scripts[2], self.scripts[3]
        self.assertEqual(aside[3:], ['mv "/var/www/v2plus/manifest.json" "%s"' % old,
                                     'mv "/var/www/v2plus/manifest.json.new" "/var/www/v2plus/manifest.json"',
                                     "exit"])
        self.assertEqual(cleanup[3:], ['rm "%s"' % old, "exit"])
        self.assertNotIn("option batch continue", aside + cleanup)

    def test_pageant_and_dry_run(self):
        method, out = self.upload(self.settings(winscp_hostkey="ssh-ed25519 255 SHA256:abc"), dry=True)
        self.assertEqual(self.ran, [])
        self.assertNotIn("-privatekey", out)
        self.assertIn('mv "/var/www/v2plus/manifest.json.new"', out)

    def test_needs_the_host_key_and_a_ppk(self):
        with self.assertRaises(ud.UploadError) as caught:
            self.upload(self.settings())
        self.assertIn("\"ssh rubidium.example\" shows it", str(caught.exception))
        with self.assertRaises(ud.UploadError) as caught:
            self.upload(self.settings(ssh_key=self.openssh_key, winscp_hostkey="x"))
        self.assertIn(".ppk", str(caught.exception))

    def test_failure(self):
        self.codes = [3]
        with self.assertRaises(ud.UploadError) as caught:
            self.upload(self.settings(winscp_hostkey="ssh-ed25519 255 SHA256:abc"))
        self.assertIn("exit 3, log", str(caught.exception))
        self.assertIn("still see the old one", str(caught.exception))

    def test_auto_falls_back_to_sftp(self):
        with mock.patch.object(ud, "WINDOWS", True), mock.patch.object(ud, "find_winscp", lambda given: None), \
                mock.patch.object(ud.shutil, "which", lambda name: "C:/Windows/System32/OpenSSH/" + name), \
                redirect_stdout(io.StringIO()):
            self.assertEqual(ud.upload(self.settings(), self.dist, [], self.manifest, runner=self.runner), "sftp")


class Main(Dist):
    def test_command_line(self):
        out = io.StringIO()
        with mock.patch.object(ud, "WINDOWS", False), \
                mock.patch.object(ud.shutil, "which", lambda name: "/usr/bin/" + name), \
                mock.patch.object(ud, "call", self.runner), redirect_stdout(out):
            code = ud.main(["--dist", self.dist, "--settings", os.path.join(self.folder.name, "none.json"),
                            "--target", "clock@rubidium.example:/srv/v2plus", "--dry-run"])
        self.assertEqual(code, 0)
        self.assertIn("sftp, 4 files and the manifest, to clock@rubidium.example:/srv/v2plus", out.getvalue())
        self.assertIn("dry run: nothing uploaded", out.getvalue())


if __name__ == "__main__":
    unittest.main()
