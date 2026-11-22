#!/usr/bin/env python3
"""A MAME test disk for Pixel's speech: a copy of the all-games demo hard disk (System 6.0, the Talking
Moose, MacinTalk 1.31 in the System Folder, like the website's disk) with Pixel as the startup app.

  make_talk_test.py --app build/app/mac/PixelTest.bin --model m.pxm --out DIR/talk.hd
                    [--test "seed T k maxnew variant" "line" ...] [--no-macintalk]

The app, both model parts (uncompressed, as on the SE) and the optional "Pixel Test" script go into the
System Folder, and the boot blocks name the app as the startup application and shell (no Finder), so
the Mac boots straight into it. --no-macintalk removes MacinTalk (the silent case). Run it with:

  RAM=4M HARDDISK=DIR/talk.hd MAME_ARGS="-wavwrite x.wav" emu/run_test.sh none llm/app/pixel_test.lua 400
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "ref"))
import export  # noqa: E402
import make_disks as MD  # noqa: E402

DEMO = os.path.join(MD.ROOT, "toolchain", "build", "demo", "demo.hd")
PART_START = 96 * 512                    # HFS partition at block 96 (toolchain/se7/build_se_disk.py)
SHELL_NAME, STARTUP_NAME = 26, 90        # Str15 fields in the boot blocks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--base", default=DEMO)
    ap.add_argument("--test", nargs="*")
    ap.add_argument("--no-macintalk", action="store_true")
    a = ap.parse_args()
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    work = tempfile.mkdtemp(prefix="talktest_")
    env = dict(os.environ, HOME=tempfile.mkdtemp(prefix="hfs_home_"))
    shutil.copyfile(a.base, a.out)
    os.chmod(a.out, 0o644)
    raw = export.to_bytes(export.read_model(a.model), huff=False)
    half = (len(raw) // 2) & ~511
    files = [a.app]
    for i, (lo, hi) in enumerate(((0, half), (half, len(raw)))):
        p = os.path.join(work, f"Pixel Model {i + 1}.bin")
        MD.macbin(p, f"Pixel Model {i + 1}", b"PXMD", MD.part(raw, i + 1, lo, hi))
        files.append(p)
    if a.test:
        t = os.path.join(work, "Pixel Test.bin")
        MD.macbin(t, "Pixel Test", b"TEXT", "\r".join(a.test).encode("mac_roman") + b"\r")
        files.append(t)
    MD.run(["hmount", a.out, "1"], env)
    try:
        for f in files:
            MD.run(["hcopy", "-m", f, ":System Folder:"], env)
        if a.no_macintalk:
            MD.run(["hdel", ":System Folder:MacinTalk"], env)
        ls = subprocess.run(["hls", ":System Folder"], env=env, capture_output=True).stdout
        print(ls.decode("mac_roman").replace("\n", "  "))
    finally:
        subprocess.run(["humount"], env=env, capture_output=True)
    hdr = open(a.app, "rb").read(128)
    name = hdr[2:2 + hdr[1]]
    img = bytearray(open(a.out, "rb").read())
    assert img[PART_START:PART_START + 2] == b"LK", "no boot blocks in the HFS partition"
    field = bytes([len(name)]) + name + b" " * (15 - len(name))
    img[PART_START + SHELL_NAME:PART_START + SHELL_NAME + 16] = field
    img[PART_START + STARTUP_NAME:PART_START + STARTUP_NAME + 16] = field
    open(a.out, "wb").write(img)
    print(f"{a.out}: {name.decode('mac_roman')} starts at boot")


if __name__ == "__main__":
    main()
