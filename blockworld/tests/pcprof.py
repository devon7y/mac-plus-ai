#!/usr/bin/env python3
"""Symbolize PC samples: pcprof.py PCS.txt [ELF.gdb] (default: build/cmake/BWBench.code.bin.gdb).
Prints the share of samples per function, and with --lines the hottest addresses."""
import bisect, collections, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
pcs = open(sys.argv[1]).read().split()
elf = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("-") else os.path.join(HERE, "..", "build", "cmake", "BWBench.code.bin.gdb")
main_rt = int(pcs[1]); samples = [int(x) for x in pcs[2:]]
nm = subprocess.run([os.path.expanduser("~/Retro68-build/toolchain/bin/m68k-apple-macos-nm"), "-n", elf],
                    capture_output=True, text=True).stdout.split("\n")
syms = sorted((int(p[0], 16), p[2]) for p in (ln.split() for ln in nm) if len(p) == 3 and p[1].lower() in "tw")
off = main_rt - [a for a, n in syms if n == "main"][0]
addrs = [a for a, _ in syms]
c = collections.Counter(); hot = collections.Counter()
for pc in samples:
    a = pc - off
    i = bisect.bisect_right(addrs, a) - 1
    name = syms[i][1] if 0 <= i and 0 <= a < addrs[-1] + 0x4000 else ("ROM" if pc >= 0x400000 else "other RAM")
    c[name] += 1
    hot[a] += 1
n = len(samples)
for name, k in c.most_common(20):
    print(f"{100 * k / n:5.1f}%  {name}")
print(n, "samples")
if "--lines" in sys.argv:
    for a, k in hot.most_common(25):
        print(f"{a:08x} {100 * k / n:5.1f}%")
