#!/usr/bin/env python3
"""Turn pc_samples.txt (from profile.lua) into a per-function profile using the ELF symbols.
    profile.py <RailRunner.code.bin.gdb> <pc_samples.txt>"""
import bisect, collections, subprocess, sys, os
elf, path = sys.argv[1], sys.argv[2]
nm = subprocess.run([os.path.expanduser("~/Retro68-build/toolchain/bin/m68k-apple-macos-nm"), "-n", elf],
                    capture_output=True, text=True).stdout
syms = []
for line in nm.splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in "tTWw":
        syms.append((int(p[0], 16), p[2]))
syms.sort()
addrs = [a for a, _ in syms]
nm_all = subprocess.run([os.path.expanduser("~/Retro68-build/toolchain/bin/m68k-apple-macos-nm"), elf],
                        capture_output=True, text=True).stdout
ro = [int(l.split()[0], 16) for l in nm_all.splitlines() if l.endswith(" prof_anchor")][0]
mn = ro
cnt = collections.Counter()
tot = 0
for line in open(path):
    pc, anchor, amain = (int(v) for v in line.split())
    off = anchor - ro
    if amain - anchor != mn - ro:
        print("anchor mismatch: function pointers are not code addresses"); sys.exit(1)
    e = pc - off
    i = bisect.bisect_right(addrs, e) - 1
    if pc >= 0x400000:
        name = "ROM (traps/interrupts)"
    elif i < 0 or e > addrs[-1] + 0x2000 or e < 0x13b4:
        name = "other (CODE 1 / libgcc / system)"
    else:
        name = syms[i][1]
    cnt[name] += 1
    tot += 1
for name, c in cnt.most_common(25):
    print(f"{100 * c / tot:5.1f}%  {c:5d}  {name}")
print("total", tot)
