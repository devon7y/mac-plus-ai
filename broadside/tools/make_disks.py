#!/usr/bin/env python3
"""Broadside's release disks.

    make_disks.py [--app build/app/Broadside.bin] [--out build/release]

  broadside.img      800K Mac Plus boot floppy: trimmed System 6.0 + Narrow Screen, Broadside as
                     the startup application (toolchain/template/build_disk.py); runs on a 1 MB Plus
  broadside_se.img   1.44 MB SE boot floppy: System 6.0.7 System and Finder, Broadside as the
                     startup application (the recipe of dream/app/make_se_floppy.py)
  Broadside.bin      the application with its Finder icon (toolchain/icons/art/broadside.txt)

hfsutils always runs with a private HOME (AGENTS.md).
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BS = os.path.join(ROOT, "broadside")
TOOLS607 = os.path.join(ROOT, "emu", "disks", "system-6.0.7-tools.dc42")
SE_SIZE = 1474560
STARTUP_NAME = 90          # offset of the startup application's name in the boot blocks


def run(cmd, env=None):
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"failed: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return r.stdout


def se_floppy(app, out, label, env, work):
    d = open(TOOLS607, "rb").read()
    raw = os.path.join(work, "tools607.img")
    open(raw, "wb").write(d[84:84 + struct.unpack(">I", d[0x40:0x44])[0]])      # DiskCopy 4.2 -> raw
    boot = bytearray(open(raw, "rb").read(1024))
    if boot[:2] != b"LK":
        sys.exit("the 6.0.7 tools disk has no boot blocks")
    run(["hmount", raw], env)
    try:
        for f in ("System", "Finder"):
            run(["hcopy", "-m", f":System Folder:{f}", os.path.join(work, f + ".bin")], env)
    finally:
        subprocess.run(["humount"], env=env, capture_output=True)
    h = open(app, "rb").read(128)
    name = h[2:2 + h[1]]
    if len(name) > 15:
        sys.exit("the app's name is too long for the boot blocks")
    open(out, "wb").write(bytes(SE_SIZE))
    run(["hformat", "-l", label, out], env)
    try:
        run(["hmkdir", ":System Folder"], env)
        run(["hcopy", "-m", os.path.join(work, "System.bin"), ":System Folder:System"], env)
        run(["hcopy", "-m", os.path.join(work, "Finder.bin"), ":System Folder:Finder"], env)
        run(["hcopy", "-m", app, ":System Folder:"], env)     # (the boot blocks name it; it is looked up here)
        run(["hattrib", "-b", ":System Folder"], env)
        listing = run(["hls", "-l", ":System Folder"], env)
        vol = run(["hvol"], env)
    finally:
        subprocess.run(["humount"], env=env, capture_output=True)
    boot[STARTUP_NAME:STARTUP_NAME + 16] = bytes([len(name)]) + name + b" " * (15 - len(name))
    img = bytearray(open(out, "rb").read())
    img[:1024] = boot
    open(out, "wb").write(img)
    print(listing.rstrip())
    print([l for l in vol.splitlines() if "free" in l][0].strip())
    print(f"SE floppy: startup app {name.decode('mac_roman')}  ->  {out} ({SE_SIZE:,} bytes)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", default=os.path.join(BS, "build", "app", "Broadside.bin"))
    ap.add_argument("--out", default=os.path.join(BS, "build", "release"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="broadside_disks_")
    env = dict(os.environ, HOME=tempfile.mkdtemp(prefix="hfs_home_"))
    app = os.path.join(a.out, "Broadside.bin")
    run([sys.executable, "-I", os.path.join(ROOT, "toolchain", "icons", "add_icon.py"), a.app,
         "--icon", "art/broadside.txt", "--out", app])
    print(run([sys.executable, "-I", os.path.join(ROOT, "toolchain", "template", "build_disk.py"), "--app", app,
               "--out", os.path.join(a.out, "broadside.img"), "--label", "Broadside"], env).rstrip())
    se_floppy(app, os.path.join(a.out, "broadside_se.img"), "Broadside SE", env, work)


if __name__ == "__main__":
    main()
