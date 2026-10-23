#!/usr/bin/env python3
"""Build Doodle's decoder benchmark: generate + assemble, package as a classic Mac application
(CODE 0 jump table + CODE 1), and make an 800K disk (trimmed System 6) that boots into it.

Usage: build_bench.py   (outputs in paint/build/)
Then:  BENCH_OUT=<dir> emu/run_test.sh paint/build/bench_disk.img paint/bench/bench.lua 1600
       python3 paint/bench/verify.py paint/build/layout.json <dir>
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BUILD = os.path.join(ROOT, "paint", "build")
TOOLS = os.path.join(ROOT, "mactools")
SYSTOOLS = os.path.join(ROOT, "emu", "disks", "system-6.0", "6.0 System Tools.dc42")
sys.path.insert(0, TOOLS)
import rsrc  # noqa: E402

APP = b"DoodleBench"
# hfsutils keeps its "current volume" in $HOME/.hcwd: use a private HOME so parallel agents
# mounting other images can't interfere.
HFSENV = dict(os.environ, HOME=tempfile.mkdtemp(prefix="doodle-hfs-"))


def run(*cmd, **kw):
    subprocess.run(cmd, check=True, env=HFSENV, **kw)


def package(code, app_name, creator, out_bin, below_a5=1024):
    """CODE 0 (one jump-table entry -> offset 0 of CODE 1) + CODE 1, written as MacBinary."""
    jt = struct.pack(">HHHH", 0, 0x3F3C, 1, 0xA9F0)
    code0 = struct.pack(">IIII", 32 + len(jt), below_a5, len(jt), 32) + jt
    code1 = struct.pack(">HH", 0, 1) + code
    fork = rsrc.build([
        {"type": b"CODE", "id": 0, "name": None, "attrs": 0x20, "data": code0},
        {"type": b"CODE", "id": 1, "name": None, "attrs": 0x20, "data": code1},
    ])
    hdr = bytearray(128)
    hdr[1] = len(app_name)
    hdr[2:2 + len(app_name)] = app_name
    hdr[65:69], hdr[69:73] = b"APPL", creator
    rsrc.write_macbinary(out_bin, bytes(hdr), b"", fork)


def system_files():
    """Extract and trim the System file once (cached in paint/build)."""
    raw = os.path.join(BUILD, "systools.img")
    sys_full, sys_trim = os.path.join(BUILD, "System.bin"), os.path.join(BUILD, "System-trim.bin")
    if not os.path.exists(sys_trim):
        d = open(SYSTOOLS, "rb").read()
        open(raw, "wb").write(d[84:84 + struct.unpack(">I", d[0x40:0x44])[0]])
        run("hmount", raw, stdout=subprocess.DEVNULL)
        try:
            run("hcopy", "-m", ":System Folder:System", sys_full)
        finally:
            subprocess.run(["humount"], capture_output=True, env=HFSENV)
        run(sys.executable, "-I", os.path.join(TOOLS, "trim_system.py"), sys_full, sys_trim, "--level", "aggressive")
    return raw, sys_trim


def main():
    os.makedirs(BUILD, exist_ok=True)
    run(sys.executable, os.path.join(HERE, "gen_bench.py"), BUILD)
    obj, binf = os.path.join(BUILD, "bench.o"), os.path.join(BUILD, "bench.bin")
    run("m68k-elf-as", "-m68000", "-o", obj, os.path.join(BUILD, "bench.s"))
    relocs = subprocess.run(["m68k-elf-objdump", "-r", obj], capture_output=True, text=True).stdout
    if "R_68K" in relocs:
        sys.exit("benchmark code has relocations:\n" + relocs)
    run("m68k-elf-objcopy", "-O", "binary", "-j", ".text", obj, binf)
    code = open(binf, "rb").read()
    print(f"CODE 1: {len(code):,} bytes")
    app_bin = os.path.join(BUILD, "DoodleBench.bin")
    package(code, APP, b"DdlB", app_bin)
    raw, sys_trim = system_files()
    run(sys.executable, "-I", os.path.join(TOOLS, "make_game_disk.py"), "--system", sys_trim, "--app", app_bin,
        "--bootblocks", raw, "--out", os.path.join(BUILD, "bench_disk.img"), "--label", "Doodle Bench")


if __name__ == "__main__":
    main()
