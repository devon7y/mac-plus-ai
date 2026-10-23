#!/usr/bin/env python3
"""Make every Blockworld release file: the starting world, both boot floppies, the MacBinaries.

    make_release.py [--skip-build]

1. build/release/Blockworld Default.bin: the shipped starting world (tools/bwsave.py make: the seed,
   spot, view and time below; format 7 with the tight LZ parse; type BWwd, creator BWrd, locked).
2. build/blockworld.img: the 4 MB Plus 800K boot floppy (build.py; the world next to the game).
3. build/blockworld_se.img: the SE 1.44 MB System 6.0.7 boot floppy (make_se_floppy.py).
4. build/release/Blockworld.bin: the game (MacBinary, with its Finder icon and bundle).
The game and its world go in the same folder: it opens "Blockworld World" there, else "Blockworld
Default" (and writes "Blockworld World" when saving).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REL = os.path.join(HERE, "build", "release")

# The starting world (chosen 2026-10-20 from proto/pick_mouth.py's candidates, checked in MAME; see
# HANDOFF.md): seed 146, on the grass at (55, 34) facing west-south-west, the view a little down: the
# sea on the left, a tree, a wooded hill on the right with a cave mouth at its foot 7 blocks ahead,
# morning (a whole day ahead).
SEED, X, Z, YAW, AIM, TIME = 146, 55.5, 34.5, 730, -12, 1000


def run(*cmd):
    print("$", " ".join(os.path.basename(c) if i == 1 else c for i, c in enumerate(cmd)))
    r = subprocess.run(cmd)
    if r.returncode:
        sys.exit(f"failed: {' '.join(cmd)}")


def free_and_files(img):
    env = dict(os.environ, HOME=tempfile.mkdtemp(prefix="hfs_home_"))
    subprocess.run(["hmount", img], env=env, capture_output=True, check=True)
    try:
        ls = subprocess.run(["hls", "-l", ":System Folder"], env=env, capture_output=True, text=True).stdout
        vol = subprocess.run(["hvol"], env=env, capture_output=True, text=True).stdout
    finally:
        subprocess.run(["humount"], env=env, capture_output=True)
    free = [l.strip() for l in vol.splitlines() if "free" in l][0]
    return free, ls.rstrip()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--skip-build", action="store_true", help="only the world and the MacBinaries")
    a = ap.parse_args()
    os.makedirs(REL, exist_ok=True)
    world = os.path.join(REL, "Blockworld Default.bin")
    run(sys.executable, os.path.join(HERE, "tools", "bwsave.py"), "make", "--seed", str(SEED), "--x", str(X),
        "--z", str(Z), "--yaw", str(YAW), "--aim", str(AIM), "--time", str(TIME), "--out", world)
    if not a.skip_build:
        run(sys.executable, os.path.join(HERE, "build.py"), "--target", "Blockworld", "--world", world)
        run(sys.executable, os.path.join(HERE, "make_se_floppy.py"), "--world", world)
    shutil.copyfile(os.path.join(HERE, "build", "cmake", "Blockworld.bin"), os.path.join(REL, "Blockworld.bin"))
    print("\nrelease files:")
    for f in sorted(os.listdir(REL)):
        print(f"  {os.path.join(REL, f)}  {os.path.getsize(os.path.join(REL, f)):,} bytes")
    for img in ("blockworld.img", "blockworld_se.img"):
        p = os.path.join(HERE, "build", img)
        if os.path.exists(p):
            free, ls = free_and_files(p)
            print(f"\n{p}: {free}\n{ls}")


if __name__ == "__main__":
    main()
