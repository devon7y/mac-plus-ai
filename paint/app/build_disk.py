#!/usr/bin/env python3
"""Make a bootable 800K floppy image that starts straight into a Retro68 application.

    build_disk.py [--app build/Doodle.bin] [--out build/doodle_disk.img]
                  [--no-narrow] [--level aggressive|safe] [--label NAME]
                  [--extra FILE.bin ...]

The disk holds a System Folder with a trimmed System 6.0 System file (from
emu/disks/system-6.0/6.0 System Tools.dc42, via mactools/trim_system.py), the
application (startup app and shell in the boot blocks, no Finder; via
mactools/make_game_disk.py) and, unless --no-narrow, narrowscreen/build/Narrow
Screen.bin (32/32 margins -> screenBits 448 px wide). --extra copies more MacBinary
files into the System Folder (for example data files the app opens).

Works from any folder inside the repo (it finds the repo root by looking for AGENTS.md),
so it can be copied as is. Needs hfsutils (brew install hfsutils).

hfsutils keeps its "current volume" in $HOME/.hcwd, shared by every process of the user.
Several agents building disks at the same time would write into each other's images, so
every hfsutils call here runs with a private HOME.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def repo_root():
    d = HERE
    while d != os.path.dirname(d):
        if os.path.isdir(os.path.join(d, "mactools")):
            return d
        d = os.path.dirname(d)
    sys.exit("repo root (with mactools) not found above " + HERE)


ROOT = repo_root()
TOOLS = os.path.join(ROOT, "mactools")
SYSTOOLS = os.path.join(ROOT, "emu", "disks", "system-6.0", "6.0 System Tools.dc42")
NARROW = os.path.join(ROOT, "narrowscreen", "build", "Narrow Screen.bin")


def run(cmd, env, quiet=False):
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"failed: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    if not quiet and r.stdout.strip():
        print(r.stdout.rstrip())
    return r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--app", default=os.path.join(HERE, "build", "Doodle.bin"))
    ap.add_argument("--out", default=os.path.join(HERE, "build", "doodle_disk.img"))
    ap.add_argument("--no-narrow", action="store_true", help="leave Narrow Screen out (512 px screen)")
    ap.add_argument("--level", choices=("aggressive", "safe"), default="aggressive",
                    help="System trim level (aggressive drops all fonts except the ROM's)")
    ap.add_argument("--label", default="Doodle", help="volume name")
    ap.add_argument("--extra", nargs="*", default=[], help="more MacBinary files for the System Folder")
    ap.add_argument("--se", action="store_true",
                    help="keep the SE's ROM patch (PTCH 630) so the disk also boots a Macintosh SE")
    a = ap.parse_args()

    out = os.path.abspath(a.out)
    work = os.path.join(os.path.dirname(out), "disk_work")
    os.makedirs(work, exist_ok=True)
    home = tempfile.mkdtemp(prefix="hfs_home_")          # private $HOME for hfsutils
    env = dict(os.environ, HOME=home)
    try:
        # 1. Raw System Tools image (for the System file and the boot blocks).
        raw = os.path.join(work, "systools.img")
        d = open(SYSTOOLS, "rb").read()
        open(raw, "wb").write(d[84:84 + struct.unpack(">I", d[0x40:0x44])[0]])   # DiskCopy 4.2 -> raw
        # 2. System file, trimmed.
        sys_full = os.path.join(work, "System.bin")
        sys_trim = os.path.join(work, f"System-{a.level}.bin")
        run(["hmount", raw], env, quiet=True)
        try:
            run(["hcopy", "-m", ":System Folder:System", sys_full], env)
        finally:
            subprocess.run(["humount"], env=env, capture_output=True)
        run([sys.executable, "-I", os.path.join(TOOLS, "trim_system.py"), sys_full, sys_trim, "--level", a.level], env)
        if a.se:                                            # (Doodle) put the SE's ROM patch back
            sys.path.insert(0, TOOLS)
            import rsrc
            full = rsrc.parse(rsrc.read_macbinary(sys_full)[2])
            th, td, tf = rsrc.read_macbinary(sys_trim)
            keep = rsrc.parse(tf) + [r for r in full if r["type"] == b"PTCH" and r["id"] == 630]
            rsrc.write_macbinary(sys_trim, th, td, rsrc.build(keep))
            print(f"kept PTCH 630 for the SE: System is {os.path.getsize(sys_trim) - 128:,} bytes (MacBinary payload)")
        # 3. Disk with System + app, boot blocks naming the app as startup and shell.
        made = run([sys.executable, "-I", os.path.join(TOOLS, "make_game_disk.py"), "--system", sys_trim,
                    "--app", a.app, "--bootblocks", raw, "--out", out, "--label", a.label], env, quiet=True)
        print(made.strip().splitlines()[-1])
        # 4. Extras: Narrow Screen and anything else, into the System Folder.
        extras = ([] if a.no_narrow else [NARROW]) + a.extra
        run(["hmount", out], env, quiet=True)
        try:
            for f in extras:
                run(["hcopy", "-m", f, ":System Folder:"], env)
            print(run(["hls", "-l", ":System Folder"], env, quiet=True).rstrip())
            vol = run(["hvol"], env, quiet=True)
            print([l for l in vol.splitlines() if "free" in l][0].strip())
        finally:
            subprocess.run(["humount"], env=env, capture_output=True)
        print(f"Narrow Screen: {'no (512 px)' if a.no_narrow else 'yes (32/32 -> 448 px)'}  ->  {out}")
    finally:
        shutil.rmtree(home, ignore_errors=True)


if __name__ == "__main__":
    main()
