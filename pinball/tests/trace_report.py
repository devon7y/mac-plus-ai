#!/usr/bin/env python3
"""Summarize a MAME instruction trace (tests/trace.lua) per function, with 68000 cycle
estimates per instruction class. trace_report.py <elf> <trace.log> <runtime anchor hex>"""
import bisect
import collections
import os
import re
import subprocess
import sys

elf, path, anc = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
NM = os.path.expanduser("~/Retro68-build/toolchain/bin/m68k-apple-macos-nm")
syms, anchor = [], None
for l in subprocess.run([NM, "-n", elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3:
        if p[2] == "prof_anchor":
            anchor = int(p[0], 16)
        if p[1] in "tTWw":
            syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]
off = anc - anchor
cnt, per = collections.Counter(), collections.Counter()
entries = collections.Counter()
starts = {a: n for a, n in syms}
tot = 0
for l in open(path):
    m = re.match(r"^([0-9A-F]{6}): (\S+)\s*(.*)", l)
    if not m:
        continue
    pc = int(m.group(1), 16)
    tot += 1
    if pc >= 0x400000:
        cnt["ROM"] += 1
        continue
    e = pc - off
    if e in starts:
        entries[starts[e]] += 1
    i = bisect.bisect_right(addrs, e) - 1
    name = syms[i][1] if i >= 0 else "?"
    cnt[name] += 1
    if len(sys.argv) > 4 and name == sys.argv[4]:
        per[(e - syms[i][0]) // 16 * 16] += 1
print(tot, "instructions traced")
for k, v in cnt.most_common(15):
    print(f"{v:7d} {100 * v / tot:5.1f}%  {k:24s} entered {entries.get(k, 0)} times, {v / max(entries.get(k, 1), 1):.0f} per call")
if per:
    print("hot offsets in", sys.argv[4])
    for o, v in sorted(per.items(), key=lambda x: -x[1])[:20]:
        print(f"  +{o:#06x}: {v}")
