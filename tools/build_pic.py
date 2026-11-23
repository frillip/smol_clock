#!/usr/bin/env python3
"""
Builds a PIC project (dspic_src, dspic_spl, dspic_ipl) with XC16 alone, from
its nbproject/configurations.xml, the commands MPLAB X's generated Makefiles
run for the production image: no MPLAB X, no make.

  python3 tools/build_pic.py dspic_src [dspic_spl ...] [--xc16 DIR] [--dfp DIR]
                             [--jobs N] [--print] [--clean]

What it reads from configurations.xml: the device, the sources in the
project tree's order (the link order), the linker script, and the compiler,
assembler and linker options. Each option is turned into the flags MPLAB X
6.20's generator writes for it (OPTIONS below, taken from its
nbproject/Makefile-default.mk). An option set to anything this table does
not know stops the build, rather than build something MPLAB would not: add
it to the table from a Makefile MPLAB generated with it.

The outputs are where MPLAB puts them, so make_image.py and make_dist.py
find them: build/default/production/<path>.o (files outside the project in
_ext/<MPLAB's hash of their folder>/), and
dist/default/production/<project>.production.{elf,hex,map}.

XC16 and the device family pack: --xc16 and --dfp, else "xc16" and "dfp"
in the release settings (SMOL_RELEASE_SETTINGS, else tools/release.local.json),
else the usual places (/opt/microchip/xc16/v2.10, C:/Program Files/Microchip/
xc16/v2.10; ~/.mchp_packs/Microchip/<family>/<version>, the newest).
--print prints the commands and runs nothing.
"""
import argparse
import concurrent.futures
import glob
import json
import os
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ElementTree

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WINDOWS = os.name == "nt"
CONF = "default"
OBJECT_DIR = "build/default/production"
DIST_DIR = "dist/default/production"
XC16_VERSION = "v2.10"
# The device family pack each device needs.
PACKS = {"dsPIC33EP256GP504": "dsPIC33E-GM-GP-MC-GU-MU_DFP"}


class BuildError(Exception):
    pass


# ---- MPLAB X's options, as its generator writes them ---------------------------
#
# (section, key) -> {value: flags}, flags a list. Every value not listed is
# refused. Where the flags go in each command, and in what order, is in
# compile_command() and link_command(), as the generated Makefile has them.
# Verified against MPLAB X 6.20 / XC16 2.10 for all three projects (their
# Makefile-default.mk and build logs of 2026-11-23).

COMPILER = {
    "code-model": {"default": [], "large-code": ["-mlarge-code"],
                   "small-code": ["-msmall-code"]},
    "data-model": {"default": [], "small-data": ["-msmall-data"]},
    "const-model": {"default": []},
    "scalar-model": {"default": []},
    "optimization-level": {"0": ["-O0"], "1": ["-O1"]},
    "isolate-each-function": {"false": [], "true": ["-ffunction-sections"]},
    "oXC16gcc-data-sects": {"false": [], "true": ["-fdata-sections"]},
    "oXC16gcc-smar-io-lvl": {"0": ["-msmart-io=0"], "1": ["-msmart-io=1"],
                             "2": ["-msmart-io=2"]},
    "enable-all-warnings": {"true": ["-Wall"], "false": []},
    "oXC16gcc-sfr-warn": {"false": ["-msfr-warn=off"]},
    "enable-symbols": {"true": ["-g"]},
    "post-instruction-scheduling": {"default": []},
    "pre-instruction-scheduling": {"default": []},
}
# Must be as here (MPLAB's defaults), whatever they would add.
COMPILER_FIXED = {
    "cast-align": "false", "disable-instruction-scheduling": "false",
    "enable-ansi-std": "false", "enable-ansi-warnings": "false",
    "enable-fatal-warnings": "false", "enable-large-arrays": "false",
    "enable-omit-frame-pointer": "false", "enable-procedural-abstraction": "false",
    "enable-short-double": "false", "enable-unroll-loops": "false",
    "expand-pragma-config": "false", "extra-include-directories": "", "keep-inline": "false",
    "oXC16gcc-align-arr": "false", "oXC16gcc-cnsts-mauxflash": "false", "oXC16gcc-errata": "",
    "oXC16gcc-fillupper": "", "oXC16gcc-large-aggregate": "false",
    "oXC16gcc-mauxflash": "false", "oXC16gcc-mpa-lvl": "", "oXC16gcc-name-text-sec": "",
    "oXC16gcc-near-chars": "false", "oXC16gcc-no-isr-warn": "false",
    "oXC16gcc-smart-io-fmt": "", "use-cci": "false", "use-iar": "false",
}

ASSEMBLER_FIXED = {
    "assembler-symbols": "", "expand-macros": "false",
    "extra-include-directories-for-assembler": "",
    "extra-include-directories-for-preprocessor": "", "false-conditionals": "false",
    "keep-locals": "false", "list-assembly": "false", "list-section-info": "false",
    "list-source": "false", "list-symbols": "false", "oXC16asm-extra-opts": "",
    "oXC16asm-list-to-file": "false", "omit-debug-dirs": "false", "omit-forms": "false",
    "preprocessor-macros": "", "relax": "false", "warning-level": "emit-warnings",
}

# Linker options: inside -Wl, in this order (the generated Makefile's).
LINKER = [
    ("linker-stack", {"true": ["--local-stack"]}),
    # The empty fields are MPLAB's (its Makefile's ",," joins); ld skips them.
    (None, ["", "--defsym=__MPLAB_BUILD=1", ""]),
    ("@script", None),
    ("stack-size", {"16": ["--stack=16"]}),
    # MPLAB writes --check-sections with this false (its default).
    ("enable-check-sections", {"false": ["--check-sections"]}),
    ("enable-data-init", {"true": ["--data-init"], "false": ["--no-data-init"]}),
    ("enable-pack-data", {"true": ["--pack-data"]}),
    ("enable-handles", {"true": ["--handles"]}),
    ("enable-default-isr", {"true": ["--isr"], "false": ["--no-isr"]}),
    ("remove-unused-sections", {"false": ["--no-gc-sections"],
                                "true": ["--gc-sections"]}),
    ("oXC16ld-fill-upper", {"0": ["--fill-upper=0"]}),
    ("oXC16ld-stackguard", {"16": ["--stackguard=16"]}),
    ("oXC16ld-force-link", {"false": ["--no-force-link"]}),
    ("oXC16ld-no-smart-io", {"false": ["--smart-io"]}),
    ("@map", None),
    ("report-memory-usage", {"true": ["--report-mem", "--memorysummary",
                                      DIST_DIR + "/memoryfile.xml"]}),
]
LINKER_FIXED = {
    "additional-options-use-response-files": "false", "boot-eeprom": "no_eeprom",
    "boot-flash": "no_flash", "boot-ram": "no_ram", "boot-write-protect": "no_write_protect",
    "extra-lib-directories": "", "fill-flash-options-addr": "", "fill-flash-options-const": "",
    "fill-flash-options-how": "0", "fill-flash-options-inc-const": "1",
    "fill-flash-options-increment": "", "fill-flash-options-seq": "",
    "fill-flash-options-what": "0", "general-code-protect": "no_code_protect",
    "general-write-protect": "no_write_protect", "generate-cross-reference-file": "false",
    "heap-size": "", "input-libraries": "", "linker-symbols": "",
    "map-file": "${DISTDIR}/${PROJECTNAME}.${IMAGE_TYPE}.map", "no-ivt": "false",
    "oXC16ld-extra-opts": "", "oXC16ld-nostdlib": "false", "preprocessor-macros": "",
    "secure-eeprom": "no_eeprom", "secure-flash": "no_flash", "secure-ram": "no_ram",
    "secure-write-protect": "no_write_protect", "symbol-stripping": "", "trace-symbols": "",
    "warn-section-align": "false",
}
# legacy-libc is left over from XC16 1.x; 2.x ignores it (one libc).
GLOBAL_FIXED = {
    "combine-sourcefiles": "false", "common-include-directories": "",
    "dual-boot-partition": "0", "fast-math": "false", "generic-16-bit": "false",
    "mpreserve-all": "false", "oXC16glb-macros": "", "omit-pack-options": "1",
    "output-file-format": "elf", "preserve-all": "false", "preserve-file": "",
    "relaxed-math": "false", "save-temps": "false",
}


class Project(object):
    def __init__(self, folder):
        self.folder = os.path.abspath(folder)
        self.name = os.path.basename(self.folder.rstrip("/\\"))
        path = os.path.join(self.folder, "nbproject", "configurations.xml")
        if not os.path.isfile(path):
            raise BuildError("%s: no nbproject/configurations.xml" % self.folder)
        root = ElementTree.parse(path).getroot()
        conf = None
        for candidate in root.iter("conf"):
            if candidate.get("name") == CONF:
                conf = candidate
        if conf is None:
            raise BuildError("%s: no %s configuration" % (path, CONF))
        self.device = conf.findtext("toolsSet/targetDevice")
        toolchain = conf.findtext("toolsSet/languageToolchain")
        if toolchain != "XC16":
            raise BuildError("%s: toolchain %s, not XC16" % (path, toolchain))
        self.sections = {}
        for section in ("C30", "C30-AS", "C30-LD", "C30Global"):
            element = conf.find(section)
            self.sections[section] = {} if element is None else {
                item.get("key"): item.get("value") or "" for item in element.iter("property")}
        folders = {folder.get("name"): folder for folder in root.find("logicalFolder")
                   if folder.tag == "logicalFolder"}
        self.sources = [path for path in items(folders.get("SourceFiles"))
                        if path.endswith((".c", ".s", ".S"))]
        scripts = [path for path in items(folders.get("LinkerScript")) if path.endswith(".gld")]
        if len(scripts) != 1:
            raise BuildError("%s: %d linker scripts, one wanted" % (path, len(scripts)))
        self.script = scripts[0]
        if not self.sources:
            raise BuildError("%s: no sources" % path)

    @property
    def cpu(self):
        """-mcpu: the device less its dsPIC or PIC prefix, as MPLAB passes it."""
        for prefix in ("dsPIC", "PIC"):
            if self.device.startswith(prefix):
                return self.device[len(prefix):]
        return self.device

    def option(self, section, key):
        return self.sections[section].get(key, "")


def items(folder):
    """The item paths under a logical folder, depth first, in order."""
    if folder is None:
        return []
    found = []
    for child in folder:
        if child.tag == "logicalFolder":
            found.extend(items(child))
        elif child.tag == "itemPath":
            found.append(child.text.strip())
    return found


def java_hash(text):
    value = 0
    for character in text:
        value = (31 * value + ord(character)) & 0xFFFFFFFF
    return value - (1 << 32) if value >= 1 << 31 else value


def object_path(source):
    """build/default/production/<source>.o; outside the project in _ext/<MPLAB's hash>."""
    stem = os.path.splitext(source)[0] + ".o"
    if source.startswith("../"):
        directory, name = source.rsplit("/", 1)
        stem = "_ext/%d/%s" % (abs(java_hash(directory)), os.path.splitext(name)[0] + ".o")
    return OBJECT_DIR + "/" + stem


def check_fixed(project, section, fixed):
    for key, value in fixed.items():
        actual = project.option(section, key)
        if actual != value:
            raise BuildError("%s: %s %s is %r; build_pic.py knows only %r (see OPTIONS)"
                             % (project.name, section, key, actual, value))


def mapped(project, section, key, table):
    value = project.option(section, key)
    if value not in table:
        raise BuildError("%s: %s %s is %r; build_pic.py knows %s (see OPTIONS)"
                         % (project.name, section, key, value, sorted(table)))
    return table[value]


def compiler_flags(project):
    check_fixed(project, "C30", COMPILER_FIXED)
    for key in project.sections["C30"]:
        if key not in COMPILER and key not in COMPILER_FIXED and key != "preprocessor-macros":
            raise BuildError("%s: C30 option %s not known to build_pic.py" % (project.name, key))
    flags = mapped(project, "C30", "enable-symbols", COMPILER["enable-symbols"])
    head = ["-mno-eds-warn"] + flags + ["-omf=elf", "-DXPRJ_default=" + CONF]
    tail = []
    for key in ("isolate-each-function", "oXC16gcc-data-sects", "code-model", "data-model",
                "const-model", "scalar-model", "optimization-level", "post-instruction-scheduling",
                "pre-instruction-scheduling"):
        tail += mapped(project, "C30", key, COMPILER[key])
    macros = project.option("C30", "preprocessor-macros")
    tail += ["-D" + macro for macro in macros.split(";") if macro]
    for key in ("oXC16gcc-smar-io-lvl", "enable-all-warnings", "oXC16gcc-sfr-warn"):
        tail += mapped(project, "C30", key, COMPILER[key])
    return head, tail


def compile_command(project, source, cc, dfp):
    target = object_path(source)
    device = ["-mcpu=" + project.cpu]
    pack = ["-mdfp=" + dfp + "/xc16"]
    if source.endswith((".s", ".S")):
        check_fixed(project, "C30-AS", ASSEMBLER_FIXED)
        return [cc, source, "-o", target, "-c"] + device + [
            "-omf=elf", "-DXPRJ_default=" + CONF,
            "-Wa,-MD,%s.d,--defsym=__MPLAB_BUILD=1,-g,--no-relax" % target] + pack
    head, tail = compiler_flags(project)
    return [cc, source, "-o", target, "-c"] + device + [
        "-MP", "-MMD", "-MF", target + ".d"] + head + tail + pack


def link_command(project, cc, dfp):
    check_fixed(project, "C30-LD", LINKER_FIXED)
    check_fixed(project, "C30Global", GLOBAL_FIXED)
    known = {key for key, _ in LINKER if key and not key.startswith("@")} | set(LINKER_FIXED)
    for key in project.sections["C30-LD"]:
        if key not in known:
            raise BuildError("%s: C30-LD option %s not known to build_pic.py" % (project.name, key))
    parts = []
    for key, table in LINKER:
        if key is None:
            parts += table
        elif key == "@script":
            parts.append("--script=" + project.script)
        elif key == "@map":
            parts.append("-Map=%s/%s.production.map" % (DIST_DIR, project.name))
        else:
            parts += mapped(project, "C30-LD", key, table)
    elf = "%s/%s.production.elf" % (DIST_DIR, project.name)
    return [cc, "-o", elf] + [object_path(source) for source in project.sources] + [
        "-mcpu=" + project.cpu, "-omf=elf", "-DXPRJ_default=" + CONF,
        "-Wl," + ",".join(parts), "-mdfp=" + dfp + "/xc16"]


def hex_command(project, bin2hex, dfp):
    elf = "%s/%s.production.elf" % (DIST_DIR, project.name)
    return [bin2hex, elf, "-a", "-omf=elf", "-mdfp=" + dfp + "/xc16"]


# ---- Tools ----------------------------------------------------------------------

def settings():
    path = os.environ.get("SMOL_RELEASE_SETTINGS") or os.path.join(ROOT, "tools",
                                                                    "release.local.json")
    if not os.path.isfile(path):
        return {}
    with open(path) as handle:
        return json.load(handle)


def find_xc16(given):
    candidates = [given] if given else (
        [settings().get("xc16")] if settings().get("xc16") else
        ["C:/Program Files/Microchip/xc16/" + XC16_VERSION,
         "/opt/microchip/xc16/" + XC16_VERSION,
         "/Applications/microchip/xc16/" + XC16_VERSION])
    for folder in candidates:
        if folder and os.path.isfile(os.path.join(folder, "bin", "xc16-gcc" +
                                                  (".exe" if WINDOWS else ""))):
            return folder
    raise BuildError("XC16 %s not found (tried %s): give its folder with --xc16, or \"xc16\" "
                     "in the release settings" % (XC16_VERSION, ", ".join(c for c in candidates if c)))


def find_dfp(given, device):
    family = PACKS.get(device)
    if given or settings().get("dfp"):
        folder = given or settings()["dfp"]
    else:
        if family is None:
            raise BuildError("no device family pack known for %s" % device)
        found = glob.glob(os.path.join(os.path.expanduser("~"), ".mchp_packs", "Microchip",
                                       family, "*"))
        if not found:
            raise BuildError("the %s pack is not in ~/.mchp_packs: give its folder with --dfp, "
                             "or \"dfp\" in the release settings" % family)
        folder = max(found, key=lambda path: [int(part) for part in
                                              os.path.basename(path).split(".") if part.isdigit()])
    if not os.path.isdir(os.path.join(folder, "xc16")):
        raise BuildError("%s: not a device family pack (no xc16 folder)" % folder)
    return folder.replace("\\", "/")


def run(command, cwd):
    result = subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return result.returncode, result.stdout.decode("utf-8", "replace")


def build(folder, xc16, dfp, jobs=os.cpu_count() or 2, show=False, clean=True):
    project = Project(folder)
    exe = ".exe" if WINDOWS else ""
    cc = os.path.join(xc16, "bin", "xc16-gcc" + exe)
    bin2hex = os.path.join(xc16, "bin", "xc16-bin2hex" + exe)
    compiles = [compile_command(project, source, cc, dfp) for source in project.sources]
    link = link_command(project, cc, dfp)
    to_hex = hex_command(project, bin2hex, dfp)
    if show:
        for command in compiles + [link, to_hex]:
            print(" ".join(command))
        return project
    if clean:
        for directory in (OBJECT_DIR, DIST_DIR):
            shutil.rmtree(os.path.join(project.folder, directory), ignore_errors=True)
    for source in project.sources:
        os.makedirs(os.path.dirname(os.path.join(project.folder, object_path(source))),
                    exist_ok=True)
    os.makedirs(os.path.join(project.folder, DIST_DIR), exist_ok=True)
    print("%s: %d sources, %s, %s" % (project.name, len(project.sources), project.device,
                                      os.path.basename(dfp)))
    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        for source, (code, text) in zip(project.sources, pool.map(
                lambda command: run(command, project.folder), compiles)):
            if text.strip():
                sys.stdout.write(text)
            if code != 0:
                failed.append(source)
    if failed:
        raise BuildError("%s: %d failed to compile: %s" % (project.name, len(failed),
                                                           ", ".join(failed)))
    for command in (link, to_hex):
        code, text = run(command, project.folder)
        sys.stdout.write(text)
        if code != 0:
            raise BuildError("%s: %s failed" % (project.name, os.path.basename(command[0])))
    print("%s: %s/%s.production.hex" % (project.name, DIST_DIR, project.name))
    return project


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("projects", nargs="+", help="dspic_src, dspic_spl, dspic_ipl (folders)")
    parser.add_argument("--xc16", help="XC16's folder (holding bin/)")
    parser.add_argument("--dfp", help="the device family pack's folder (holding xc16/)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    parser.add_argument("--print", dest="show", action="store_true",
                        help="print the commands, run nothing")
    parser.add_argument("--no-clean", dest="clean", action="store_false",
                        help="keep the objects of an earlier build")
    args = parser.parse_args(argv)
    try:
        for name in args.projects:
            folder = name if os.path.isdir(name) else os.path.join(ROOT, name)
            project = Project(folder)
            xc16 = args.xc16 if args.show and args.xc16 else (
                "XC16" if args.show and not args.xc16 else find_xc16(args.xc16))
            dfp = args.dfp if args.show and args.dfp else (
                "DFP" if args.show and not args.dfp else find_dfp(args.dfp, project.device))
            build(folder, xc16, dfp, args.jobs, args.show, args.clean)
    except BuildError as error:
        sys.stderr.write("build_pic: %s\n" % error)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
