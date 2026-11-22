#!/usr/bin/env python3
"""Bit-exactness test: the numpy twin (twin.py) against the C simulation (sim.c, built for the host).

    test_bitexact.py [--games 64] [--ticks 900] [--seed 1]

Plays random games (random arenas, 2-4 tanks of random types and teams at random places, each
driven by random raw inputs, a scripted bot or random policy actions) in both, and compares the
state hash after every tick and every tank's observation before every tick.
"""
import argparse
import ctypes
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import defs as D  # noqa: E402
from twin import Twin  # noqa: E402

BUILD = os.path.join(os.path.dirname(HERE), "build", "host")


def build_lib():
    os.makedirs(BUILD, exist_ok=True)
    subprocess.run([sys.executable, os.path.join(HERE, "gen_tables.py")], check=True)
    lib = os.path.join(BUILD, "libtanksim.dylib")
    srcs = [os.path.join(HERE, f) for f in ("sim.c", "sim_tables.c", "hostlib.c")]
    subprocess.run(["cc", "-O2", "-Wall", "-shared", "-fPIC", "-o", lib] + srcs, check=True)
    return ctypes.CDLL(lib)


def random_setup(rng, nt):
    ar = int(rng.integers(len(D.ARENAS)))
    _, grid, _ = D.arena(ar)
    free = [(x, y) for y in range(D.TH) for x in range(D.TW) if not grid[y][x]]
    pick = rng.choice(len(free), size=nt, replace=False)
    spec = []
    teams = rng.integers(0, 2, size=nt)
    teams[0], teams[1] = 0, 1
    for i in range(nt):
        tx, ty = free[pick[i]]
        spec.append([int(rng.integers(4)), int(teams[i]), tx * 256 + 128, ty * 256 + 128, int(rng.integers(256))])
    return ar, np.array(spec, dtype=np.int16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--games", type=int, default=64)
    ap.add_argument("--ticks", type=int, default=900)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    lib = build_lib()
    rng = np.random.default_rng(a.seed)
    bad = 0
    total_ticks = 0
    for nt in (2, 3, 4):
        G = a.games // 3 + 1
        setups = [random_setup(rng, nt) for _ in range(G)]
        ctrl = rng.integers(0, 5, size=(G, nt)).astype(np.int16)
        T = a.ticks
        # random inputs, held for a random few ticks so the tanks actually go places
        inp = np.zeros((G, T, nt, 4), dtype=np.int16)
        for g in range(G):
            for i in range(nt):
                k = 0
                while k < T:
                    hold = int(rng.integers(1, 30))
                    if ctrl[g, i] == 4:
                        v = [rng.integers(3), rng.integers(3), rng.integers(7), rng.integers(2)]
                    else:
                        v = [rng.integers(-1, 2), rng.integers(-1, 2), rng.integers(256), int(rng.random() < 0.3)]
                    inp[g, k:k + hold, i] = v
                    k += hold
        seeds = rng.integers(1, 2 ** 32, size=G)
        # C
        ch = np.zeros((G, T), dtype=np.uint32)
        co = np.zeros((G, T, nt, D.NOBS), dtype=np.int16)
        for g in range(G):
            ar, spec = setups[g]
            lib.run_game(ctypes.c_short(ar), ctypes.c_uint(int(seeds[g])), ctypes.c_short(nt),
                         spec.ctypes.data_as(ctypes.c_void_p), np.ascontiguousarray(ctrl[g]).ctypes.data_as(ctypes.c_void_p),
                         ctypes.c_long(T), np.ascontiguousarray(inp[g]).ctypes.data_as(ctypes.c_void_p),
                         ch[g].ctypes.data_as(ctypes.c_void_p), co[g].ctypes.data_as(ctypes.c_void_p))
        # twin
        tw = Twin(G, nt)
        tw.init(np.array([s[0] for s in setups]), seeds)
        for i in range(nt):
            sp = np.array([s[1][i] for s in setups], dtype=np.int32)
            tw.set_tank(i, sp[:, 0], sp[:, 1], sp[:, 2], sp[:, 3], sp[:, 4])
        first_bad = None
        for k in range(T):
            for i in range(nt):
                o = tw.obs(i)
                if not np.array_equal(o, co[:, k, i].astype(np.int32)):
                    g = int(np.argmax(np.any(o != co[:, k, i], axis=1)))
                    if first_bad is None:
                        diff = np.nonzero(o[g] != co[g, k, i])[0]
                        first_bad = f"obs nt={nt} game {g} tick {k} tank {i} features {diff.tolist()} twin {o[g, diff].tolist()} C {co[g, k, i, diff].tolist()}"
                c = ctrl[:, i]
                raw = c == 0
                ii = inp[:, k, i].astype(np.int32)
                tw.t["drive"][raw, i] = ii[raw, 0]
                tw.t["turn"][raw, i] = ii[raw, 1]
                tw.t["tgt"][raw, i] = ii[raw, 2] & 255
                tw.t["fire"][raw, i] = ii[raw, 3]
                if k % D.DECIDE == i % D.DECIDE:
                    tw.apply_action(i, ii, c == 4)
                    for kind in (1, 2, 3):
                        if (c == kind).any():
                            tw.bot_act(i, kind, c == kind)
            tw.step()
            h = tw.hash()
            if not np.array_equal(h, ch[:, k].astype(np.int64)):
                g = int(np.argmax(h != ch[:, k]))
                if first_bad is None:
                    first_bad = f"hash nt={nt} game {g} tick {k}"
                bad += 1
                break
        total_ticks += G * T
        alive = tw.t["alive"].sum()
        print(f"nt={nt}: {G} games x {T} ticks: {'OK' if first_bad is None else 'MISMATCH: ' + first_bad}"
              f"  (tanks alive at the end {alive}/{G * nt}, shells alive {int(tw.s['alive'].sum())})")
        if first_bad:
            bad += 1
    # sim_pick: random logits, the twin draws in the same order from the same generator
    n = 5000
    z = rng.integers(-2 ** 27, 2 ** 27, size=(n, 15)).astype(np.int32)
    z[::7] = z[::7] // 4096                 # some close calls, where the noise decides
    for shift in (11, 13):
        act = np.zeros((n, 4), dtype=np.int16)
        r = ctypes.c_uint(0)
        lib.pick_test(ctypes.c_uint(12345), ctypes.c_long(n), z.ctypes.data_as(ctypes.c_void_p), ctypes.c_short(shift),
                      act.ctypes.data_as(ctypes.c_void_p), ctypes.byref(r))
        tw = Twin(1, 1)
        tw.init(np.array([0]), np.array([12345]))
        ta = np.concatenate([tw.pick(z[k:k + 1], shift) for k in range(n)])
        ok = np.array_equal(ta, act.astype(np.int32)) and int(tw.rng[0]) == r.value
        print(f"sim_pick shift {shift}: {n} picks {'OK' if ok else 'MISMATCH'}")
        bad += 0 if ok else 1
    print("RESULT", "PASS" if not bad else "FAIL", f"({total_ticks} game-ticks compared)")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
