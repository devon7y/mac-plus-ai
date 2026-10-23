#!/usr/bin/env python3
"""Build Dream's boot floppy: trimmed System 6.0 (+ Narrow Screen), the Dream application, the
"Dream Model" file and the FLUX licence as a text file, all in the System Folder.

    make_disk.py --model <file.drm> [--app build/app/Dream.bin] [--out build/disks/dream.img] [--se]

Uses toolchain/template/build_disk.py (read-only shared tool) with --extra.
"""
import argparse
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "mactools"))
import rsrc  # noqa: E402


def macbinary(data, out, name, ftype, creator):
    hdr = bytearray(128)
    hdr[1] = len(name)
    hdr[2:2 + len(name)] = name
    hdr[65:69], hdr[69:73] = ftype, creator
    rsrc.write_macbinary(out, bytes(hdr), data, b"")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--app", default=os.path.join(ROOT, "dream", "build", "app", "Dream.bin"))
    ap.add_argument("--out", default=os.path.join(ROOT, "dream", "build", "disks", "dream.img"))
    ap.add_argument("--se", action="store_true", help="keep the SE's ROM patch (boots a Macintosh SE too)")
    ap.add_argument("--no-license", action="store_true")
    ap.add_argument("--app-name", default="Dream", help="name of the application on the disk")
    a = ap.parse_args()
    work = os.path.join(os.path.dirname(os.path.abspath(a.out)), "dream_disk_work")
    os.makedirs(work, exist_ok=True)
    h, d, r = rsrc.read_macbinary(a.app)
    name = a.app_name.encode("mac_roman")
    if h[2:2 + h[1]] != name:                       # e.g. DreamSm -> Dream
        h = bytearray(h)
        h[1] = len(name)
        h[2:65] = name + b"\0" * (63 - len(name))
        a.app = os.path.join(work, "Dream.bin")
        rsrc.write_macbinary(a.app, bytes(h), d, r)
    subprocess.run([sys.executable, "-I", os.path.join(ROOT, "toolchain", "icons", "add_icon.py"), a.app, "--preset",
                    "dream", "--out", os.path.join(work, "Dream_icon.bin")], check=True)
    a.app = os.path.join(work, "Dream_icon.bin")       # the app with its Finder icon (toolchain/icons)
    extras = []
    mb = os.path.join(work, "DreamModel.bin")
    macbinary(open(a.model, "rb").read(), mb, b"Dream Model", b"DrmM", b"Drem")
    extras.append(mb)
    if not a.no_license:
        text = open(os.path.join(ROOT, "dream", "LICENSE-FLUX.md"), encoding="utf-8").read()
        text = text.replace("’", "'").replace("“", '"').replace("”", '"').replace("×", "x")
        text = text.encode("mac_roman", errors="replace").replace(b"\n", b"\r")
        lb = os.path.join(work, "FLUXLicense.bin")
        macbinary(text, lb, b"Dream FLUX License", b"TEXT", b"ttxt")
        extras.append(lb)
    cmd = [sys.executable, "-I", os.path.join(ROOT, "toolchain", "template", "build_disk.py"), "--app", a.app,
           "--out", a.out, "--label", "Dream", "--extra"] + extras
    if a.se:
        cmd.insert(-len(extras) - 1, "--keep-se")
    subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
