#!/usr/bin/env python3
"""The update manifest, format 2, for a set of firmware files.

  make_manifest.py --base-url https://host/path/ [--esp esp_src.ino.bin]
                   [--pic app.smolpic] [--pic-spl spl.smolspl]
                   [--config NAME] [--device MAC ...] [--hardware v2]
                   [--merge manifest.json] [-o manifest.json]

Each file's version is read from the file itself, never from its name: the
PIC's files from their headers (checked whole, as make_image.py does), the
ESP's image from its user agent string ("SMOL CLOCK V2/X.Y.Z"). The ESP
refuses a PIC file whose header disagrees with the manifest, so the two must
match. A version in a file's name that differs is reported.

The URL of each file is the base URL and the file's name. The files are
uploaded as they are.

--merge reads an existing manifest and replaces its configuration with the
same hardware, config and devices, or adds one: a configuration for listed
devices goes first, any other last, as the ESP takes the first that matches.
Without -o the manifest is printed.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_image  # noqa: E402

FORMAT = 2
ESP_AGENT = re.compile(rb"SMOL CLOCK V2/(\d+\.\d+\.\d+)\x00")
ESP_IMAGE_MAGIC = 0xE9
ESP_APP_DESC_MAGIC = 0xABCD5432
NAME_VERSION = re.compile(r"(\d+\.\d+\.\d+)")


class ManifestError(Exception):
    pass


def esp_version(data):
    """The ESP image's version, from its user agent string."""
    if len(data) < 0x30 or data[0] != ESP_IMAGE_MAGIC:
        raise ManifestError("not an ESP32 application image")
    if struct.unpack("<I", data[0x20:0x24])[0] != ESP_APP_DESC_MAGIC:
        raise ManifestError("no application description: not an ESP32 application image")
    found = set(m.group(1).decode() for m in ESP_AGENT.finditer(data))
    if len(found) != 1:
        raise ManifestError("%s user agent versions found (%s): give --esp-version" %
                            (len(found) or "no", ", ".join(sorted(found))))
    return found.pop()


def pic_file(data, kind):
    try:
        f = make_image.parse_file(data)
    except make_image.ImageError as error:
        raise ManifestError(str(error))
    if f["kind"] != kind:
        raise ManifestError("a .smol%s file, not .smol%s" % (f["kind"], kind))
    if f["devid"] != make_image.DEVICE_ID:
        raise ManifestError("DEVID 0x%04X, not 0x%04X" % (f["devid"], make_image.DEVICE_ID))
    return f


def component(path, base_url, version, notes):
    with open(path, "rb") as handle:
        data = handle.read()
    name = os.path.basename(path)
    named = NAME_VERSION.search(name)
    if named and named.group(1) != version:
        notes.append("%s: named %s, but holds %s" % (name, named.group(1), version))
    return data, {
        "version": version,
        "url": base_url + name,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def build_entry(args, notes):
    entry = {"hardware": args.hardware}
    if args.config:
        entry["config"] = args.config
    if args.device:
        entry["devices"] = [mac.upper() for mac in args.device]
    if args.esp:
        with open(args.esp, "rb") as handle:
            version = args.esp_version or esp_version(handle.read())
        entry["esp"] = component(args.esp, args.base_url, version, notes)[1]
    for key, path, kind in (("pic", args.pic, "app"), ("pic_spl", args.pic_spl, "spl")):
        if path:
            with open(path, "rb") as handle:
                try:
                    f = pic_file(handle.read(), kind)
                except ManifestError as error:
                    raise ManifestError("%s: %s" % (path, error))
            version = make_image.version_text(f["version"])
            entry[key] = component(path, args.base_url, version, notes)[1]
            if kind == "app":
                notes.append("pic: %s, %d instructions, SML %d.%d" %
                             (version, f["length"], f["sml_version"] >> 8, f["sml_version"] & 0xFF))
            else:
                notes.append("pic_spl: %s, %d instructions, needs IPL %s" %
                             (version, f["length"], make_image.version_text(f["min_ipl"])))
    if "esp" in entry:
        notes.append("esp: %s, %d bytes" % (entry["esp"]["version"], entry["esp"]["size"]))
    if not any(key in entry for key in ("esp", "pic", "pic_spl")):
        raise ManifestError("no files given")
    return entry


def same_target(a, b):
    def key(entry):
        devices = entry.get("devices")
        return (entry.get("hardware"), entry.get("config", ""),
                tuple(sorted(d.upper() for d in devices)) if devices else None)
    return key(a) == key(b)


def merge(manifest, entry):
    if manifest.get("format") != FORMAT or not isinstance(manifest.get("configurations"), list):
        raise ManifestError("--merge: not a format %d manifest" % FORMAT)
    configurations = manifest["configurations"]
    for index, existing in enumerate(configurations):
        if same_target(existing, entry):
            configurations[index] = entry
            return "replaced configuration %d" % (index + 1)
    if "devices" in entry:
        configurations.insert(0, entry)
        return "added as configuration 1"
    configurations.append(entry)
    return "added as configuration %d" % len(configurations)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--base-url", required=True, help="where the files are, ending in /")
    parser.add_argument("--esp", help="the ESP's application image (.bin)")
    parser.add_argument("--esp-version", help="the ESP's version, if it cannot be read from the image")
    parser.add_argument("--pic", help="the PIC's application (.smolpic)")
    parser.add_argument("--pic-spl", help="the PIC's SPL (.smolspl)")
    parser.add_argument("--hardware", default="v2")
    parser.add_argument("--config", default="", help="the update config it is for (none: the default)")
    parser.add_argument("--device", action="append", help="a MAC address it is limited to (repeatable)")
    parser.add_argument("--merge", help="an existing manifest to add it to")
    parser.add_argument("-o", "--output", help="where to write it (default: print)")
    args = parser.parse_args(argv)
    if not re.match(r"https?://", args.base_url):
        parser.error("--base-url: http:// or https://")
    if not args.base_url.endswith("/"):
        args.base_url += "/"

    notes = []
    try:
        entry = build_entry(args, notes)
        if args.merge:
            with open(args.merge) as handle:
                manifest = json.load(handle)
            notes.append(merge(manifest, entry))
        else:
            manifest = {"format": FORMAT, "configurations": [entry]}
    except (ManifestError, OSError, ValueError) as error:
        print("make_manifest: %s" % error, file=sys.stderr)
        return 1

    text = json.dumps(manifest, indent=2) + "\n"
    if args.output:
        with open(args.output, "w") as handle:
            handle.write(text)
    else:
        sys.stdout.write(text)
    for note in notes:
        print(note, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
