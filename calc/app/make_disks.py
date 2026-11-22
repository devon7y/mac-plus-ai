#!/usr/bin/env python3
"""Scribble Calc's release floppies.

    python3 -I calc/app/make_disks.py
      -> calc/build/release/ScribbleCalc.bin         the app (MacBinary, named "Scribble Calc", Finder icon)
         calc/build/release/scribble_calc_plus.img    800K: trimmed System 6.0 + Narrow Screen, starts
                                                      straight into the app (keeps the SE patch, so an SE
                                                      boots it too)
         calc/build/release/scribble_calc_se.img      1.44 MB: System 6.0.7 + Finder (the SE recipe of
                                                      dream/app/make_se_floppy.py), the app as startup app
Builds the app first (calc/build/app, configured if needed). hfsutils runs with a private HOME.
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CALC = os.path.dirname(HERE)
ROOT = os.path.dirname(CALC)
sys.path.insert(0, os.path.join(ROOT, "mactools"))
import rsrc   # noqa: E402  (mactools: MacBinary read/write)

BUILD = os.path.join(CALC, "build", "app")
OUT = os.path.join(CALC, "build", "release")
NAME = b"Scribble Calc"
TOOLS607 = os.path.join(ROOT, "emu", "disks", "system-6.0.7-tools.dc42")
SE_SIZE = 1474560
STARTUP_NAME = 90


def run(cmd, env=None, cwd=None):
    r = subprocess.run(cmd, env=env, cwd=cwd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"failed: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return r.stdout


def build_app():
    if not os.path.exists(os.path.join(BUILD, "build.ninja")):
        tc = os.path.expanduser("~/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake")
        run(["cmake", "-S", HERE, "-B", BUILD, "-G", "Ninja", f"-DCMAKE_TOOLCHAIN_FILE={tc}"])
    run(["cmake", "--build", BUILD])
    h, d, r = rsrc.read_macbinary(os.path.join(BUILD, "ScribbleCalc.bin"))
    h = bytearray(h)
    h[1] = len(NAME)
    h[2:65] = NAME + b"\0" * (63 - len(NAME))
    named = os.path.join(OUT, "work", "named.bin")
    rsrc.write_macbinary(named, bytes(h), d, r)
    app = os.path.join(OUT, "ScribbleCalc.bin")
    run([sys.executable, "-I", os.path.join(ROOT, "toolchain", "icons", "add_icon.py"), named,
         "--icon", os.path.join(ROOT, "toolchain", "icons", "art", "scribblecalc.txt"), "--out", app])
    return app


def plus_disk(app):
    out = os.path.join(OUT, "scribble_calc_plus.img")
    txt = run([sys.executable, "-I", os.path.join(ROOT, "toolchain", "template", "build_disk.py"), "--app", app,
               "--out", out, "--label", "Scribble Calc", "--keep-se"])
    print(txt.strip().splitlines()[-2])
    print(txt.strip().splitlines()[-1])
    return out


def se_disk(app):
    out = os.path.join(OUT, "scribble_calc_se.img")
    work = tempfile.mkdtemp(prefix="calc_se_")
    env = dict(os.environ, HOME=tempfile.mkdtemp(prefix="hfs_home_"))
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
    open(out, "wb").write(bytes(SE_SIZE))
    run(["hformat", "-l", "Scribble Calc SE", out], env)
    try:
        run(["hmkdir", ":System Folder"], env)
        run(["hcopy", "-m", os.path.join(work, "System.bin"), ":System Folder:System"], env)
        run(["hcopy", "-m", os.path.join(work, "Finder.bin"), ":System Folder:Finder"], env)
        run(["hcopy", "-m", app, ":System Folder:"], env)
        run(["hattrib", "-b", ":System Folder"], env)
        listing = run(["hls", "-l", ":System Folder"], env)
        vol = run(["hvol"], env)
    finally:
        subprocess.run(["humount"], env=env, capture_output=True)
    boot[STARTUP_NAME:STARTUP_NAME + 16] = bytes([len(NAME)]) + NAME + b" " * (15 - len(NAME))
    img = bytearray(open(out, "rb").read())
    img[:1024] = boot
    open(out, "wb").write(img)
    print(listing.rstrip())
    print([l for l in vol.splitlines() if "free" in l][0].strip(), "->", out)
    return out


def main():
    os.makedirs(os.path.join(OUT, "work"), exist_ok=True)
    app = build_app()
    print("app:", app, os.path.getsize(app), "bytes")
    plus_disk(app)
    se_disk(app)


if __name__ == "__main__":
    main()
