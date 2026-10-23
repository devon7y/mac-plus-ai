#!/usr/bin/env python3
"""Symbolize PC samples from pcprof.lua: pcprof.py RUN_DIR/pcs.txt Endurance.code.bin.gdb"""
import bisect, os, subprocess, sys, collections
pcs = open(sys.argv[1]).read().split()
main_rt = int(pcs[1]); samples = [int(x) for x in pcs[2:]]
nm = subprocess.run([os.path.expanduser("~/Retro68-build/toolchain/bin/m68k-apple-macos-nm"), "-n", sys.argv[2]],
                    capture_output=True, text=True).stdout.split("\n")
syms = []
for ln in nm:
    p = ln.split()
    if len(p) == 3 and p[1].lower() in "tw":
        syms.append((int(p[0], 16), p[2]))
syms.sort()
main_elf = [a for a, n in syms if n == "main"][0]
off = main_rt - main_elf
addrs = [a for a, _ in syms]
c = collections.Counter()
for pc in samples:
    a = pc - off
    i = bisect.bisect_right(addrs, a) - 1
    c[syms[i][1] if 0 <= i and 0 <= a < addrs[-1] + 0x20000 else ("ROM" if pc >= 0x400000 else "other RAM")] += 1
n = len(samples)
for name, k in c.most_common(25):
    print(f"{100 * k / n:5.1f}%  {name}")
print(n, "samples")
