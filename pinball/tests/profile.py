#!/usr/bin/env python3
"""Per-function profile from tests/profile.lua samples.
    profile.py <NeuralPinball.code.bin.gdb> <pc_samples.txt>"""
import bisect
import collections
import os
import subprocess
import sys

NM = os.path.expanduser("~/Retro68-build/toolchain/bin/m68k-apple-macos-nm")
elf, path = sys.argv[1], sys.argv[2]
syms = []
anchor = None
for line in subprocess.run([NM, "-n", elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3:
        if p[2] == "prof_anchor":
            anchor = int(p[0], 16)
        if p[1] in "tTWw":
            syms.append((int(p[0], 16), p[2]))
syms.sort()
addrs = [a for a, _ in syms]
cnt = collections.Counter()
tot = 0
for line in open(path):
    pc, anc = (int(v) for v in line.split())
    tot += 1
    if pc >= 0x400000:
        cnt["ROM (traps, interrupts, QuickDraw)"] += 1
        continue
    e = pc - (anc - anchor)
    i = bisect.bisect_right(addrs, e) - 1
    if i < 0 or e > addrs[-1] + 0x4000:
        cnt["outside the app (System RAM)"] += 1
    else:
        cnt[syms[i][1]] += 1
for name, n in cnt.most_common(25):
    print(f"{100 * n / tot:5.1f}%  {n:5d}  {name}")
print(f"{tot} samples")
if "--lines" in sys.argv:
    # source lines of the samples in the app (needs a -g build)
    pcs = []
    for line in open(path):
        pc, anc = (int(v) for v in line.split())
        if pc < 0x400000:
            pcs.append(pc - (anc - anchor))
    out = subprocess.run([os.path.expanduser("~/Retro68-build/toolchain/bin/m68k-apple-macos-addr2line"), "-e", elf]
                         + ["%x" % p for p in pcs], capture_output=True, text=True).stdout.split()
    lc = collections.Counter(os.path.basename(l) for l in out)
    print("hottest lines:")
    for name, n in lc.most_common(30):
        print(f"{100 * n / tot:5.1f}%  {name}")
