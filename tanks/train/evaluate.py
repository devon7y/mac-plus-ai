#!/usr/bin/env python3
"""Strength of Tank Duel's integer policies (exactly what the Mac runs: model.int_forward + sim_pick),
in many duels at once on the numpy twin, on the game's 8 arenas with random spawn points.

    evaluate.py --pols rusher=a.npz,sniper=b.npz,... [--opp player=p.npz] [--games 400] [--out table.json]
    evaluate.py --ckpts run_dir/type ...      (every ckpt_*.pt of a run: the checkpoint ladder)

A policy file is an exported .npz (model.save_int) or a training checkpoint .pt (exported here).
Policies pick their actions as the Mac does (integer logits + Gumbel noise from the game's random
generator, temperature 0.5: twin.pick / sim_pick).
Each row is a policy driving its own tank type; columns: the scripted bots (hunter, sitter, wander)
and policies driving a player-type tank. Win / loss / draw (60 s limit) percentages.
"""
import argparse
import glob
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "sim"))
import defs as D  # noqa: E402
from twin import Twin, ARENA_MAPS  # noqa: E402
from model import load_int, int_forward, export_int  # noqa: E402

TYPES = {"player": D.T_PLAYER, "rusher": D.T_RUSHER, "sniper": D.T_SNIPER, "careful": D.T_CAREFUL}
BOTS = {"hunter": D.BOT_HUNTER, "sitter": D.BOT_SITTER, "wander": D.BOT_WANDER}


def load_policy(path):
    if path.endswith(".npz"):
        return load_int(path)
    import torch
    from ppo import load_actor
    a = load_actor(path)
    a.quant = True
    return export_int(a)


SAMPLE = True        # as the Mac plays: sampled at temperature 0.5 (sim_pick); False = argmax


def act_of(tw, layers, obs):
    """the integer policy's action, exactly as the Mac picks it"""
    z, act = int_forward(layers, obs)
    if SAMPLE:
        act = tw.pick(z, layers[-1]["SH"] - D.PICK_SHIFT)
    return act.astype(np.int32)


def spawns(rng, n, min_d=6):
    """random arena (0..7) and two connected floor tiles at least min_d apart, for n games"""
    from env import connected
    ar = rng.integers(len(ARENA_MAPS), size=n)
    pos = np.zeros((n, 2, 2), dtype=np.int32)
    for e in range(n):
        g = ARENA_MAPS[ar[e]]
        free = np.argwhere(g == 0)
        while True:
            p = free[rng.choice(len(free), size=2, replace=False)]
            if max(abs(p[0][0] - p[1][0]), abs(p[0][1] - p[1][1])) >= min_d and connected(g, (p[0][1], p[0][0]), (p[1][1], p[1][0])):
                break
        pos[e] = p
    return ar, pos


def duel(a, a_type, b, b_type, n=400, seed=0, max_ticks=1800):
    """a, b: integer policy layers or a bot kind (int). Returns (win, loss, draw) fractions for a."""
    rng = np.random.default_rng(seed)
    ar, pos = spawns(rng, n)
    tw = Twin(n, 2, ns=6)
    tw.init(ar, rng.integers(1, 2 ** 31, size=n))
    for i, typ in ((0, a_type), (1, b_type)):
        tw.set_tank(i, typ, i, pos[:, i, 1] * 256 + 128, pos[:, i, 0] * 256 + 128, rng.integers(256, size=n))
    done = np.zeros(n, bool)
    res = np.zeros(n, np.int32)
    for k in range(max_ticks):
        for i, ctl in ((0, a), (1, b)):
            if k % D.DECIDE == i % D.DECIDE:
                if isinstance(ctl, int):
                    tw.bot_act(i, ctl)
                else:
                    tw.apply_action(i, act_of(tw, ctl, tw.obs(i)))
        tw.step()
        al = tw.t["alive"]
        new = ~done & ((al[:, 0] == 0) | (al[:, 1] == 0))
        res[new & (al[:, 0] == 1)] = 1
        res[new & (al[:, 1] == 1)] = -1
        done |= new
        if done.all():
            break
    return float((res == 1).mean()), float((res == -1).mean()), float((res == 0).mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pols", default="", help="name=path,... (name starts with the tank type)")
    ap.add_argument("--opp", default="", help="name=path,... policies driving a player tank (columns)")
    ap.add_argument("--ladder", default="", help="a run folder: its ckpt_*.pt against its final.pt")
    ap.add_argument("--games", type=int, default=400)
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    table = {}
    pols = [p.split("=", 1) for p in a.pols.split(",") if p]
    opps = [p.split("=", 1) for p in a.opp.split(",") if p]
    loaded = {name: load_policy(path) for name, path in pols + opps}
    for name, _ in pols:
        typ = TYPES[name.split("_")[0]]
        row = {}
        for bname, kind in BOTS.items():
            row[bname] = duel(loaded[name], typ, kind, D.T_PLAYER, a.games)
        for oname, _ in opps:
            row[oname] = duel(loaded[name], typ, loaded[oname], D.T_PLAYER, a.games)
        table[name] = row
        print(name, "  ".join(f"{c}: {w * 100:.0f}/{l * 100:.0f}/{d * 100:.0f}" for c, (w, l, d) in row.items()), flush=True)
    if a.ladder:
        cks = sorted(glob.glob(os.path.join(a.ladder, "ckpt_*.pt")))
        final = load_policy(os.path.join(a.ladder, "final.pt") if os.path.exists(os.path.join(a.ladder, "final.pt"))
                            else cks[-1])
        typ = TYPES[os.path.basename(a.ladder.rstrip("/")).split("_")[0]]
        for c in cks:
            p = load_policy(c)
            w = duel(p, typ, final, typ, a.games)
            h = duel(p, typ, D.BOT_HUNTER, D.T_PLAYER, a.games)
            table[os.path.basename(c)] = {"vs_final": w, "hunter": h}
            print(os.path.basename(c), f"vs final {w[0] * 100:.0f}/{w[1] * 100:.0f}/{w[2] * 100:.0f}",
                  f"vs hunter {h[0] * 100:.0f}/{h[1] * 100:.0f}/{h[2] * 100:.0f}", flush=True)
    if a.out:
        json.dump(table, open(a.out, "w"), indent=1)


if __name__ == "__main__":
    main()
