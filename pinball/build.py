#!/usr/bin/env python3
"""Build Neural Pinball: generate the table data, compile with Retro68, name the app, add its
Finder icon, and make the boot floppies.

    python3 pinball/build.py [--defs HASHTEST,BENCH] [--tag NAME] [--no-disk] [--se]

Outputs in pinball/build/<tag>/ (tag defaults to "game"):
  gen/                       generated table_data.c/.h (tools/gen.py)
  cmake/                     the build tree (NeuralPinball.bin)
  Neural Pinball.bin         the application (MacBinary), named and with its icon
  pinball.img                800K boot floppy: trimmed System 6.0 + Narrow Screen + the game,
                             which starts straight away (also boots an SE: --keep-se)
  pinball_se.img             with --se: 1.44 MB System 6.0.7 floppy for the SE (tools/make_se_floppy.py)
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TC = os.path.expanduser("~/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake")
APP_NAME = b"Neural Pinball"


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f"FAILED: {' '.join(cmd)}\n{r.stdout[-4000:]}\n{r.stderr[-4000:]}")
    return r.stdout


def crc16(b):
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def rename_macbinary_bytes(d, name):
    had_crc = crc16(bytes(d[:124])) == (d[124] << 8 | d[125])
    d[1] = len(name)
    d[2:65] = name + bytes(63 - len(name))
    if had_crc:
        c = crc16(bytes(d[:124]))
        d[124] = c >> 8
        d[125] = c & 0xFF


def rename_macbinary(src, dst, name):
    """copy a MacBinary II file, setting the Finder name in its header (and the header CRC)"""
    d = bytearray(open(src, "rb").read())
    assert d[0] == 0 and 0 < d[1] <= 63, "not MacBinary"
    had_crc = crc16(bytes(d[:124])) == (d[124] << 8 | d[125])
    d[1] = len(name)
    d[2:65] = name + bytes(63 - len(name))
    if had_crc:
        c = crc16(bytes(d[:124]))
        d[124] = c >> 8
        d[125] = c & 0xFF
    open(dst, "wb").write(d)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--defs", default="")
    ap.add_argument("--tag", default=None)
    ap.add_argument("--no-disk", action="store_true")
    ap.add_argument("--se", action="store_true", help="also make the 1.44 MB SE floppy")
    ap.add_argument("--cflags", default="", help="extra C flags (e.g. -g for line-level profiling)")
    a = ap.parse_args()
    tag = a.tag or "game"
    out = os.path.join(HERE, "build", tag)
    gen = os.path.join(out, "gen")
    os.makedirs(gen, exist_ok=True)
    print(run([sys.executable, "-I", os.path.join(HERE, "tools", "gen.py"), gen]).strip())
    # the AI's aim table, measured on the host with the same physics (tests/aimtab.c)
    tool = os.path.join(out, "aimtab")
    run(["cc", "-O2", "-I" + os.path.join(HERE, "core"), "-I" + gen, os.path.join(HERE, "tests", "aimtab.c"),
         os.path.join(HERE, "core", "phys.c"), os.path.join(gen, "table_data.c"), "-o", tool])
    r = subprocess.run([tool, os.path.join(gen, "aimtab.h")], capture_output=True, text=True)
    print(r.stderr.strip())
    cm = os.path.join(out, "cmake")
    defs = ";".join(d for d in a.defs.split(",") if d)
    run(["cmake", "-S", os.path.join(HERE, "app"), "-B", cm, "-G", "Ninja",
         f"-DCMAKE_TOOLCHAIN_FILE={TC}", f"-DGEN_DIR={gen}", f"-DDEFS={defs}", f"-DCMAKE_C_FLAGS={a.cflags}"])
    print(run(["cmake", "--build", cm]).strip().splitlines()[-1])
    built = os.path.join(cm, "NeuralPinball.bin")
    app = os.path.join(out, "Neural Pinball.bin")
    rename_macbinary(built, app, APP_NAME)
    print(run([sys.executable, "-I", os.path.join(ROOT, "toolchain", "icons", "add_icon.py"), app,
               "--icon", "art/pinball.txt"]).strip())
    print(run([os.path.expanduser("~/Retro68-build/toolchain/bin/m68k-apple-macos-size"),
               os.path.join(cm, "NeuralPinball.code.bin.gdb")]).strip())
    print(f"app {os.path.getsize(app)} bytes -> {app}")
    if not a.no_disk:
        img = os.path.join(out, "pinball.img")
        # MacinTalk (Apple's, never committed) for the callouts, when it's on this Mac: 28 KB
        mt = os.path.join(ROOT, "toolchain", "build", "demo", "extras", "macintalk.bin")
        extra = ["--extra", os.path.join(out, "MacinTalk.bin")] if os.path.exists(mt) else []
        if extra:
            with open(mt, "rb") as f:
                d = bytearray(f.read())
            rename_macbinary_bytes(d, b"MacinTalk")
            with open(extra[1], "wb") as f:
                f.write(d)
        print(run([sys.executable, "-I", os.path.join(ROOT, "toolchain", "template", "build_disk.py"),
                   "--app", app, "--out", img, "--label", "Neural Pinball", "--keep-se"] + extra).strip())
        print("disk ->", img)
    if a.se:
        se = os.path.join(out, "pinball_se.img")
        print(run([sys.executable, "-I", os.path.join(HERE, "tools", "make_se_floppy.py"), "--app", app,
                   "--out", se]).strip())


if __name__ == "__main__":
    main()
