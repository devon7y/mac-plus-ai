#!/usr/bin/env python3
"""Build an 800K bootable game disk: a System file plus one application that starts
instead of the Finder (no Finder on the disk).

Usage: make_game_disk.py --system System.bin --app App.bin --bootblocks src.img --out disk.img
       [--label NAME]

System.bin and App.bin are MacBinary files. --bootblocks is a raw disk image whose
first 1024 bytes (boot blocks) are copied, with the startup and shell names changed
to the application's name. Requires hfsutils.
"""
import argparse
import os
import struct
import subprocess
import sys

# hfsutils keeps its "current volume" in $HOME/.hcwd, shared by every process. Give this
# script a private HOME so parallel disk builds can't write into each other's images.
import tempfile as _tempfile
os.environ["HOME"] = _tempfile.mkdtemp(prefix="hfs-home-")


SHELL_NAME, STARTUP_NAME = 26, 90           # Str15 fields in the System 6 boot blocks


def run(*cmd):
    subprocess.run(cmd, check=True, capture_output=True)


def macbinary_name(path):
    h = open(path, "rb").read(128)
    return h[2:2 + h[1]]


def str15(name):
    if len(name) > 15:
        sys.exit(f"name too long for boot blocks (max 15): {name!r}")
    return bytes([len(name)]) + name + b" " * (15 - len(name))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--system", required=True)
    ap.add_argument("--app", required=True)
    ap.add_argument("--bootblocks", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--label", default="Game")
    a = ap.parse_args()

    app_name = macbinary_name(a.app)
    open(a.out, "wb").write(bytes(819200))
    run("hformat", "-l", a.label, a.out)
    try:
        run("hmkdir", ":System Folder")
        run("hcopy", "-m", a.system, ":System Folder:System")
        run("hcopy", "-m", a.app, ":System Folder:")
        run("hattrib", "-b", ":System Folder")
        listing = subprocess.run(["hls", "-l", ":System Folder"], capture_output=True, text=True).stdout
        free = subprocess.run(["hvol"], capture_output=True, text=True).stdout
    finally:
        subprocess.run(["humount"], capture_output=True)

    boot = bytearray(open(a.bootblocks, "rb").read(1024))
    if boot[:2] != b"LK":
        sys.exit("source has no boot blocks")
    boot[SHELL_NAME:SHELL_NAME + 16] = str15(app_name)
    boot[STARTUP_NAME:STARTUP_NAME + 16] = str15(app_name)
    img = bytearray(open(a.out, "rb").read())
    img[:1024] = boot
    open(a.out, "wb").write(img)

    print(listing.rstrip())
    print([l for l in free.splitlines() if "free" in l][0].strip())
    print(f"startup app: {app_name.decode('mac_roman')}  ->  {a.out}")


if __name__ == "__main__":
    main()
