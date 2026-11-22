#!/usr/bin/env python3
"""Bit-exactness on the Mac: replay the title screen's attract demo in the numpy twin with the
integer policies the app was built with, and compare with what MAME printed (tests/game.lua):
the state hash every 30 ticks of the first demo round, and one decision's logits (DBG PROBE).

    demo_ref.py MAME_LOG [--gen tanks/build/gen]
"""
import argparse
import json
import os
import re
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
TANKS = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(TANKS, "sim"))
sys.path.insert(0, os.path.join(TANKS, "train"))
import defs as D  # noqa: E402
from twin import Twin, ARENA_SPAWNS  # noqa: E402
from model import load_int, int_forward  # noqa: E402


def demo_round(pols, pm, r, hashes, difficulty=1):
    """one attract-demo round (arena r % 8, seed 12345 + r): append its hashes, return the probe"""
    sp = ARENA_SPAWNS[r % len(ARENA_SPAWNS)]
    tw = Twin(1, 3)
    tw.init(np.array([r % len(ARENA_SPAWNS)]), np.array([12345 + r]))
    tanks = [(D.T_PLAYER, 0, sp["P"], 0), (D.T_RUSHER, 1, sp["a"], 128), (D.T_SNIPER, 1, sp["b"], 128)]
    ctl = []
    for i, (typ, team, (tx, ty), a) in enumerate(tanks):
        tw.set_tank(i, typ, team, tx * 256 + 128, ty * 256 + 128, a)
        ctl.append(pm["polmap"][typ][difficulty])
    probe = {}
    while len(hashes) < 32:
        tick = int(tw.tick[0])
        for i in range(3):
            if tw.t["alive"][0, i] and tick % D.DECIDE == i % D.DECIDE:
                if ctl[i] >= 0:
                    o = tw.obs(i)
                    z, _ = int_forward(pols[ctl[i]], o)
                    act = tw.pick(z, pols[ctl[i]][-1]["SH"] - D.PICK_SHIFT)
                    tw.apply_action(i, act.astype(np.int32))
                    if i == 1:
                        probe[tick] = (o[0].tolist(), z[0].tolist(), ctl[i])
                else:
                    kind = D.BOT_SITTER if tanks[i][0] == D.T_SNIPER else (D.BOT_WANDER if tanks[i][0] == D.T_CAREFUL else D.BOT_HUNTER)
                    tw.bot_act(i, kind)
        tw.step()
        al = tw.t["alive"][0]
        team = tw.t["team"][0]
        if not (al[team == 0].any() and al[team == 1].any()) or tw.tick[0] >= 1800:
            break
        if tw.tick[0] % 30 == 0:
            hashes.append(int(tw.hash()[0]))
    return probe


def demo_hashes(gen):
    pm = json.load(open(os.path.join(gen, "polmap.json")))
    pols = [load_int(os.path.join(gen, f"policy_{n}.npz")) for n in pm["names"]]
    hashes = []
    probe = demo_round(pols, pm, 0, hashes)
    r = 1
    while len(hashes) < 32 and r < 64:
        demo_round(pols, pm, r, hashes)
        r += 1
    return hashes, probe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--gen", default=os.path.join(TANKS, "build", "gen"))
    a = ap.parse_args()
    text = open(a.log).read()
    mac = [int(v) for _, v in re.findall(r"DBG HASH (\d+) (\d+)", text)]
    m = re.search(r"DBG PROBE (-?\d+) (-?\d+) ([-\d ]+) \| ([-\d ]+)", text)
    ref, probe = demo_hashes(a.gen)
    n = min(len(mac), len(ref))
    same = sum(1 for i in range(n) if mac[i] == ref[i])
    print(f"demo hashes: Mac {len(mac)}, reference {len(ref)}, equal {same}/{n}")
    ok = n >= 8 and same == n          # (the Mac may record fewer if the title screen didn't run long)
    if m:
        tick, pol = int(m.group(1)), int(m.group(2))
        obs = [int(v) for v in m.group(3).split()]
        logits = [int(v) for v in m.group(4).split()]
        pm = json.load(open(os.path.join(a.gen, "polmap.json")))
        layers = load_int(os.path.join(a.gen, f"policy_{pm['names'][pol]}.npz"))
        z, _ = int_forward(layers, np.array([obs]))
        lok = z[0].tolist() == logits
        print(f"probe at tick {tick}: Mac logits {'==' if lok else '!='} int_forward(obs)")
        if tick in probe:
            ook = probe[tick][0] == obs
            print(f"probe obs {'==' if ook else '!='} the twin's obs at that tick")
            ok = ok and ook
        ok = ok and lok
    print("RESULT", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
