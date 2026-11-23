"""build_pic.py's commands against MPLAB X's for the same project."""
import os
import shlex
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import build_pic  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))


def mplab_commands(name):
    """The fixture's command lines as argument lists (quotes as a shell takes them)."""
    commands = []
    with open(os.path.join(HERE, "fixtures", "mplab_%s_production.txt" % name)) as handle:
        for line in handle:
            if line.strip() and not line.startswith("#"):
                commands.append(shlex.split(line))
    return commands


class AgainstMplab:
    """Each project's commands against its MPLAB build log (NAME)."""
    NAME = None

    def setUp(self):
        self.project = build_pic.Project(os.path.join(ROOT, self.NAME))
        self.cc = "XC16/bin/xc16-gcc"
        self.ours = [build_pic.compile_command(self.project, source, self.cc, "DFP")
                     for source in self.project.sources]
        self.link = build_pic.link_command(self.project, self.cc, "DFP")
        self.to_hex = build_pic.hex_command(self.project, "XC16/bin/xc16-bin2hex", "DFP")
        self.theirs = mplab_commands(self.NAME)

    def test_compiles(self):
        theirs = {command[1]: command for command in self.theirs if "-c" in command}
        self.assertEqual(sorted(theirs), sorted(self.project.sources))
        for command in self.ours:
            self.assertEqual(command, theirs[command[1]], command[1])

    def test_link_in_mplab_order(self):
        theirs = [command for command in self.theirs
                  if command[0].endswith("xc16-gcc") and "-c" not in command]
        self.assertEqual(len(theirs), 1)
        self.assertEqual(self.link, theirs[0])

    def test_hex(self):
        theirs = [command for command in self.theirs if command[0].endswith("bin2hex")]
        self.assertEqual([self.to_hex], theirs)



class Application(AgainstMplab, unittest.TestCase):
    NAME = "dspic_src"


class SecondaryLoader(AgainstMplab, unittest.TestCase):
    NAME = "dspic_spl"


class InitialLoader(AgainstMplab, unittest.TestCase):
    NAME = "dspic_ipl"


class Paths(unittest.TestCase):
    def test_outside_the_project(self):
        self.assertEqual(build_pic.object_path("../common/sml.c"),
                         "build/default/production/_ext/1270477542/sml.o")


class Refusals(unittest.TestCase):
    def project_with(self, section, key, value):
        source = os.path.join(ROOT, "dspic_src", "nbproject", "configurations.xml")
        with open(source) as handle:
            text = handle.read()
        old = '<property key="%s" value="' % key
        start = text.index(old, text.index("<%s>" % section))
        end = text.index('"', start + len(old))
        text = text[:start + len(old)] + value + text[end:]
        folder = tempfile.mkdtemp()
        os.makedirs(os.path.join(folder, "nbproject"))
        with open(os.path.join(folder, "nbproject", "configurations.xml"), "w") as handle:
            handle.write(text)
        return build_pic.Project(folder)

    def test_an_unknown_value_stops_it(self):
        project = self.project_with("C30", "optimization-level", "s")
        with self.assertRaises(build_pic.BuildError):
            build_pic.compile_command(project, "main.c", "cc", "DFP")

    def test_an_option_off_its_default_stops_it(self):
        project = self.project_with("C30", "enable-unroll-loops", "true")
        with self.assertRaises(build_pic.BuildError):
            build_pic.compile_command(project, "main.c", "cc", "DFP")
        project = self.project_with("C30-LD", "heap-size", "512")
        with self.assertRaises(build_pic.BuildError):
            build_pic.link_command(project, "cc", "DFP")


if __name__ == "__main__":
    unittest.main()
