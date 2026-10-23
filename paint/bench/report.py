#!/usr/bin/env python3
"""Summarise a benchmark run: seconds per part / per pen move for every configuration,
memory fit, and a full "Finish it" estimate (prime N user points + generate M new ones).

Usage: report.py <layout.json> <log with ENTRY lines> [prime=50] [gen=100]
"""
import json
import sys

TICK = 1 / 60.15
lay = json.load(open(sys.argv[1]))
NP = int(sys.argv[3]) if len(sys.argv) > 3 else 50
NG = int(sys.argv[4]) if len(sys.argv) > 4 else 100
ent = [l.split()[1:] for l in open(sys.argv[2]) if l.startswith("ENTRY")]
t, fit, mem, code = {}, {}, None, None
for e in ent:
    tag, a, b, c = e[0], int(e[1]), int(e[2]), int(e[3])
    if tag == "FIT":
        fit[c] = (a, b)
    elif tag == "MEM":
        mem = (a, b, c)
    elif tag == "CODE":
        code = (a, b)
    else:
        t[(c, tag)] = a * TICK / b
cf = lay["configs"]
print(f"Memory after _MaxApplZone: FreeMem {mem[0]:,}  MaxMem {mem[1]:,}  BufPtr-ApplZone {mem[2]:,} "
      f"({mem[2] / 1024:.0f}K); benchmark code {code[0]:,} bytes, its buffer {code[1]:,}")
print()
print("cfg  cell H    kind  fold   lut    rec    inp    gate   out    samp | prime  step(pen move) | sum-of-parts  "
      f"Finish-it({NP}+{NG})  footprint  fits")
for c in cf:
    i = c["id"]
    g = lambda p: t.get((i, p))
    row = f"{i:3d}  {c['cell']:4s} {c['H']:3d}  {c['kind']}  "
    if c["run"]:
        ref = cf[[x["id"] for x in cf if x["cell"] == "gru" and x["H"] == c["H"] and x["kind"] == c["kind"]][0]]["id"]
        get = lambda p: g(p) if g(p) is not None else t.get((ref, p))
        parts = [get(p) for p in ("fold", "lut", "rec", "inp", "gate", "out", "samp")]
        row += " ".join(f"{v:6.3f}" for v in parts)
        prim, step = g("prim"), g("step")
        tot = sum(parts[1:])
        fin = get("fold") + NP * prim + NG * step
        row += f" | {prim:6.3f} {step:6.3f}         | {tot:6.3f}        {fin:7.1f} s       "
    else:
        row += " " * 48 + "(not run)" + " " * 44
    ok, size = fit[i]
    row += f"{size / 1024:6.0f}K   {'yes' if ok else 'NO'}"
    print(row)
