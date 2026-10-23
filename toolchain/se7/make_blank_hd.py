#!/usr/bin/env python3
"""Make a blank SCSI hard-disk image for MAME's Mac SE / Plus: Apple partition map, Apple SCSI
driver and one empty HFS partition ("Macintosh HD").

    make_blank_hd.py --out blank.hd [--label NAME]

The driver descriptor map, partition map and driver (blocks 0-95) are copied from the
System 7.0.1 compilation disk (emu/disks/system-7.0.1/dl-compilation/hd1.img, archive.org item
mac_MacOS_7.0.1_compilation); the rest is zeroed and the HFS partition (blocks 96..41787,
about 20 MB) is formatted with hfsutils (private $HOME, see AGENTS.md).
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SRC = os.path.join(ROOT, "emu", "disks", "system-7.0.1", "dl-compilation", "hd1.img")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--label", default="Macintosh HD")
    a = ap.parse_args()
    src = open(SRC, "rb").read()
    if src[:2] != b"ER" or src[512:514] != b"PM":
        sys.exit("source has no Apple partition map")
    img = bytearray(len(src))
    img[:96 * 512] = src[:96 * 512]                   # DDM + partition map + Apple_Driver43
    open(a.out, "wb").write(img)
    home = tempfile.mkdtemp(prefix="hfs_home_")
    try:
        env = dict(os.environ, HOME=home)
        r = subprocess.run(["hformat", "-l", a.label, a.out, "1"], env=env, capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stderr)
        subprocess.run(["humount"], env=env, capture_output=True)
    finally:
        shutil.rmtree(home, ignore_errors=True)
    blocks = struct.unpack(">I", src[4:8])[0]
    print(f"{a.out}: {len(img)} bytes ({blocks} blocks), HFS partition '{a.label}' formatted")


if __name__ == "__main__":
    main()
