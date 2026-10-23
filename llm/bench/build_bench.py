#!/usr/bin/env python3
"""Build the benchmark: generate + assemble, package as a classic Mac application
(CODE 0 jump table + CODE 1), and make an 800K disk that boots straight into it.

Usage: build_bench.py   (outputs in llm/build/)
"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BUILD = os.path.join(ROOT, "llm", "build")
TOOLS = os.path.join(ROOT, "mactools")
SYSTOOLS = os.path.join(ROOT, "emu", "disks", "system-6.0", "6.0 System Tools.dc42")
sys.path.insert(0, TOOLS)
import rsrc  # noqa: E402

# hfsutils keeps its "current volume" in $HOME/.hcwd, shared by every process. Give this
# script a private HOME so parallel disk builds can't write into each other's images.
import tempfile as _tempfile
os.environ["HOME"] = _tempfile.mkdtemp(prefix="hfs-home-")


APP = b"LLMBench"


def run(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def main():
    os.makedirs(BUILD, exist_ok=True)
    run(sys.executable, "-I", os.path.join(HERE, "gen_bench.py"), BUILD)
    obj, binf = os.path.join(BUILD, "bench.o"), os.path.join(BUILD, "bench.bin")
    run("m68k-elf-as", "-m68000", "-o", obj, os.path.join(BUILD, "bench.s"))
    relocs = subprocess.run(["m68k-elf-objdump", "-r", obj], capture_output=True, text=True).stdout
    if "R_68K" in relocs:
        sys.exit("benchmark code has relocations:\n" + relocs)
    run("m68k-elf-objcopy", "-O", "binary", "-j", ".text", obj, binf)
    code = open(binf, "rb").read()

    # CODE 0: above-A5 size, below-A5 (globals) size, jump table size, offset 32,
    # then one jump-table entry: offset 0 in segment 1, "move.w #1,-(sp); _LoadSeg".
    jt = struct.pack(">HHHH", 0, 0x3F3C, 1, 0xA9F0)
    code0 = struct.pack(">IIII", 32 + len(jt), 256, len(jt), 32) + jt
    code1 = struct.pack(">HH", 0, 1) + code           # first JT entry offset, entry count
    fork = rsrc.build([
        {"type": b"CODE", "id": 0, "name": None, "attrs": 0x20, "data": code0},
        {"type": b"CODE", "id": 1, "name": None, "attrs": 0x20, "data": code1},
    ])
    hdr = bytearray(128)
    hdr[1] = len(APP)
    hdr[2:2 + len(APP)] = APP
    hdr[65:69], hdr[69:73] = b"APPL", b"LLMb"
    app_bin = os.path.join(BUILD, "LLMBench.bin")
    rsrc.write_macbinary(app_bin, bytes(hdr), b"", fork)

    # Trimmed System (aggressive: no fonts needed, the app never draws) + boot blocks.
    raw = os.path.join(BUILD, "systools.img")
    d = open(SYSTOOLS, "rb").read()
    open(raw, "wb").write(d[84:84 + struct.unpack(">I", d[0x40:0x44])[0]])
    sys_full, sys_trim = os.path.join(BUILD, "System.bin"), os.path.join(BUILD, "System-trim.bin")
    run("hmount", raw, stdout=subprocess.DEVNULL)
    try:
        run("hcopy", "-m", ":System Folder:System", sys_full)
    finally:
        subprocess.run(["humount"], capture_output=True)
    run(sys.executable, "-I", os.path.join(TOOLS, "trim_system.py"), sys_full, sys_trim, "--level", "aggressive")
    run(sys.executable, "-I", os.path.join(TOOLS, "make_game_disk.py"), "--system", sys_trim, "--app", app_bin,
        "--bootblocks", raw, "--out", os.path.join(BUILD, "bench_disk.img"), "--label", "LLM Bench")


if __name__ == "__main__":
    main()
