#!/usr/bin/env python3
"""Tune's bootable 1.44 MB floppy for the Macintosh SE (SuperDrive), System 6.0.7.

    make_se_floppy.py [--app ../build/app/Tune.bin] [--out ../build/tune_se.img]

Adapted from dream/app/make_se_floppy.py (the shared recipe, verified with PvZ, Blockworld and
Dream): a 1,474,560-byte HFS image whose blessed System Folder holds the full System 6.0.7 System
and Finder (from emu/disks/system-6.0.7-tools.dc42) and the app (with its Finder icon); the boot
blocks are the 6.0.7 tools disk's first 1,024 bytes with the app's name (Str15) at offset 90, so
the SE starts straight into Tune (quitting goes to the Finder). The model is inside the app.
hfsutils runs with a private HOME (AGENTS.md).
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
TOOLS607 = os.path.join(ROOT, "emu", "disks", "system-6.0.7-tools.dc42")
SIZE = 1474560
STARTUP_NAME = 90


def run(cmd, env):
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"failed: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", default=os.path.join(ROOT, "tune", "build", "app", "Tune.bin"))
    ap.add_argument("--out", default=os.path.join(ROOT, "tune", "build", "tune_se.img"))
    ap.add_argument("--label", default="Tune SE")
    a = ap.parse_args()
    work = tempfile.mkdtemp(prefix="tune_se_")
    env = dict(os.environ, HOME=tempfile.mkdtemp(prefix="hfs_home_"))
    run([sys.executable, "-I", os.path.join(ROOT, "toolchain", "icons", "add_icon.py"), a.app, "--icon",
         os.path.join(ROOT, "toolchain", "icons", "art", "tune.txt"), "--out", os.path.join(work, "Tune_icon.bin")], env)
    a.app = os.path.join(work, "Tune_icon.bin")        # the app with its Finder icon (toolchain/icons)
    d = open(TOOLS607, "rb").read()
    raw = os.path.join(work, "tools607.img")
    open(raw, "wb").write(d[84:84 + struct.unpack(">I", d[0x40:0x44])[0]])     # DiskCopy 4.2 -> raw
    boot = bytearray(open(raw, "rb").read(1024))
    if boot[:2] != b"LK":
        sys.exit("the 6.0.7 tools disk has no boot blocks")
    run(["hmount", raw], env)
    try:
        for f in ("System", "Finder"):
            run(["hcopy", "-m", f":System Folder:{f}", os.path.join(work, f + ".bin")], env)
    finally:
        subprocess.run(["humount"], env=env, capture_output=True)
    h = open(a.app, "rb").read(128)
    name = h[2:2 + h[1]]
    if len(name) > 15:
        sys.exit("the app's name is too long for the boot blocks")
    open(a.out, "wb").write(bytes(SIZE))
    run(["hformat", "-l", a.label, a.out], env)
    try:
        run(["hmkdir", ":System Folder"], env)
        run(["hcopy", "-m", os.path.join(work, "System.bin"), ":System Folder:System"], env)
        run(["hcopy", "-m", os.path.join(work, "Finder.bin"), ":System Folder:Finder"], env)
        run(["hcopy", "-m", a.app, ":System Folder:"], env)
        run(["hattrib", "-b", ":System Folder"], env)
        listing = run(["hls", "-l", ":System Folder"], env)
        vol = run(["hvol"], env)
    finally:
        subprocess.run(["humount"], env=env, capture_output=True)
    boot[STARTUP_NAME:STARTUP_NAME + 16] = bytes([len(name)]) + name + b" " * (15 - len(name))
    img = bytearray(open(a.out, "rb").read())
    img[:1024] = boot
    open(a.out, "wb").write(img)
    print(listing.rstrip())
    print([l for l in vol.splitlines() if "free" in l][0].strip())
    print(f"startup app: {name.decode('mac_roman')}  ->  {a.out} ({SIZE:,} bytes)")


if __name__ == "__main__":
    main()
