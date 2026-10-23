#!/usr/bin/env python3
"""Build Endurance: generate tables/kernels, assemble, package the application, and make an
800K disk that boots straight into it (trimmed System + Narrow Screen + the game).

    build.py [--main m1] [--w 384 --h 240 --hy 80] [--out NAME] [--no-narrow]

Outputs in racer/build/: NAME.bin (MacBinary app), NAME.img (raw 800K disk), gen/ (generated).
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

# hfsutils keeps its current volume in $HOME/.hcwd (shared by all processes): every command that
# runs hfsutils gets a private HOME (HFS_ENV).
HFS_ENV = dict(os.environ, HOME=tempfile.mkdtemp(prefix="racer_hfs_"))

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(HERE, "build")
TOOLS = os.path.join(ROOT, "mactools")
SYSTOOLS = os.path.join(ROOT, "emu", "disks", "system-6.0", "6.0 System Tools.dc42")
NARROW = os.path.join(ROOT, "narrowscreen", "build", "Narrow Screen.bin")
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.join(HERE, "engine"))
sys.path.insert(0, HERE)
import rsrc  # noqa: E402

APP_NAME = b"Endurance"
CREATOR = b"ENDR"


def run(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def assemble(main, gen, out_bin):
    obj = out_bin[:-4] + ".o"
    lst = out_bin[:-4] + ".lst"
    with open(lst, "w") as f:
        run("m68k-elf-as", "-m68000", "-I", gen, "-I", os.path.join(HERE, "src"), f"-alns={lst}",
            "-o", obj, os.path.join(HERE, "src", main + ".s"))
    relocs = subprocess.run(["m68k-elf-objdump", "-r", obj], capture_output=True, text=True).stdout
    if "R_68K" in relocs:
        sys.exit("code has relocations (use PC-relative addressing):\n" + relocs[:2000])
    dis = subprocess.run(["m68k-elf-objdump", "-d", obj], capture_output=True, text=True).stdout
    if "%pc@(ffff" in dis:
        sys.exit("a PC-relative reference is out of 16-bit range (GNU as wrapped it): use LEAF")
    run("m68k-elf-objcopy", "-O", "binary", "-j", ".text", obj, out_bin)
    return open(out_bin, "rb").read()


def package(code, out, globals_size=4096, extra_res=()):
    jt = struct.pack(">HHHH", 0, 0x3F3C, 1, 0xA9F0)
    code0 = struct.pack(">IIII", 32 + len(jt), globals_size, len(jt), 32) + jt
    code1 = struct.pack(">HH", 0, 1) + code
    res = [{"type": b"CODE", "id": 0, "name": None, "attrs": 0x20, "data": code0},
           {"type": b"CODE", "id": 1, "name": None, "attrs": 0x20, "data": code1}]
    res += list(extra_res)
    fork = rsrc.build(res)
    hdr = bytearray(128)
    hdr[1] = len(APP_NAME)
    hdr[2:2 + len(APP_NAME)] = APP_NAME
    hdr[65:69], hdr[69:73] = b"APPL", CREATOR
    rsrc.write_macbinary(out, bytes(hdr), b"", fork)


def system_files(keep_se=False):
    """Trimmed System (aggressive; --keep-se keeps the SE ROM patch so the floppy also boots an SE)
    and the System Tools image for boot blocks, cached."""
    raw = os.path.join(BUILD, "systools.img")
    sys_trim = os.path.join(BUILD, "System-trim-se.bin" if keep_se else "System-trim.bin")
    if os.path.exists(raw) and os.path.exists(sys_trim):
        return raw, sys_trim
    d = open(SYSTOOLS, "rb").read()
    open(raw, "wb").write(d[84:84 + struct.unpack(">I", d[0x40:0x44])[0]])
    sys_full = os.path.join(BUILD, "System.bin")
    if not os.path.exists(sys_full):
        run("hmount", raw, stdout=subprocess.DEVNULL, env=HFS_ENV)
        try:
            run("hcopy", "-m", ":System Folder:System", sys_full, env=HFS_ENV)
        finally:
            subprocess.run(["humount"], capture_output=True, env=HFS_ENV)
    run(sys.executable, "-I", os.path.join(TOOLS, "trim_system.py"), sys_full, sys_trim, "--level", "aggressive",
        *(["--keep-se"] if keep_se else []))
    return raw, sys_trim


def make_disk(app_bin, img, narrow=True, label="Endurance", keep_se=False):
    raw, sys_trim = system_files(keep_se)
    run(sys.executable, "-I", os.path.join(TOOLS, "make_game_disk.py"), "--system", sys_trim, "--app", app_bin,
        "--bootblocks", raw, "--out", img, "--label", label, stdout=subprocess.DEVNULL, env=HFS_ENV)
    if narrow:
        run("hmount", img, stdout=subprocess.DEVNULL, env=HFS_ENV)
        try:
            run("hcopy", "-m", NARROW, ":System Folder:", env=HFS_ENV)
            free = subprocess.run(["hvol"], capture_output=True, text=True, env=HFS_ENV).stdout
        finally:
            subprocess.run(["humount"], capture_output=True, env=HFS_ENV)
        print([ln for ln in free.splitlines() if "free" in ln][0].strip())


RETRO = os.path.join(os.environ["HOME"], "Retro68-build", "toolchain")
TOOLCHAIN_FILE = os.path.join(RETRO, "m68k-apple-macos", "cmake", "retro68.toolchain.cmake")


def build_game(a, gen):
    """Retro68 C build of racer/game with the generated engine and data."""
    run(sys.executable, os.path.join(HERE, "gen_assets.py"), gen, "--w", str(a.w), "--h", str(a.h),
        "--hy", str(a.hy))
    bdir = os.path.join(BUILD, "cmake_game")
    env = dict(os.environ, PATH=os.path.join(RETRO, "bin") + ":" + os.environ["PATH"])
    if not os.path.exists(os.path.join(bdir, "build.ninja")):
        run("cmake", "-S", os.path.join(HERE, "game"), "-B", bdir, "-G", "Ninja",
            f"-DCMAKE_TOOLCHAIN_FILE={TOOLCHAIN_FILE}", f"-DGEN_DIR={gen}", stdout=subprocess.DEVNULL, env=env)
    run("cmake", "--build", bdir, env=env)
    dis = subprocess.run([os.path.join(RETRO, "bin", "m68k-apple-macos-objdump"), "-d",
                          os.path.join(bdir, "Endurance.code.bin.gdb")], capture_output=True, text=True).stdout
    if "%pc@(ffff" in dis:
        sys.exit("a PC-relative reference is out of 16-bit range (GNU as wrapped it)")
    app = os.path.join(bdir, "Endurance.bin")
    run(sys.executable, "-I", os.path.join(ROOT, "toolchain", "icons", "add_icon.py"), app, "--preset", "endurance")
    img = os.path.join(BUILD, "endurance.img")
    make_disk(app, img, label="Endurance", keep_se=True)     # boots a Plus and (with the SE patch) an SE
    print(f"app {os.path.getsize(app)} bytes -> {app}\ndisk {img}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--main", default="game")
    ap.add_argument("--w", type=int, default=384)
    ap.add_argument("--h", type=int, default=224)
    ap.add_argument("--hy", type=int, default=75)
    ap.add_argument("--out", default=None)
    ap.add_argument("--no-narrow", action="store_true")
    a = ap.parse_args()
    out = a.out or a.main
    gen = os.path.join(BUILD, "gen_" + out)
    os.makedirs(gen, exist_ok=True)
    run(sys.executable, os.path.join(HERE, "engine", "gen_road.py"), gen, "--w", str(a.w), "--h", str(a.h),
        "--hy", str(a.hy))
    if a.main == "game":
        return build_game(a, gen)
    run(sys.executable, os.path.join(HERE, "src", f"gen_{a.main}_data.py"), gen, "--w", str(a.w), "--h", str(a.h),
        "--hy", str(a.hy))
    code = assemble(a.main, gen, os.path.join(BUILD, out + ".code"))
    app = os.path.join(BUILD, out + ".bin")
    package(code, app)
    img = os.path.join(BUILD, out + ".img")
    make_disk(app, img, narrow=not a.no_narrow)
    print(f"code {len(code)} bytes -> {app}, disk {img}")


if __name__ == "__main__":
    main()
