#!/usr/bin/env python3
"""Prove that Rail Runner's obstacle patterns always leave an escape route.

Because the player's z depends only on time (speed is a function of distance), the world at each
tick is the same for every possible player, so an exhaustive breadth-first search over player
states (lane, switch progress, height, vertical speed, roll timer) is exact.

Claim proved by induction:
  E = "settled" states: on the ground (y = 0), not switching lanes, any lane (a ground roll is
      allowed: jump and lane changes work during a roll, so it is never worse than running).
  For every pattern P, every speed it can appear at, and every entry state in E at P's start,
  some sequence of inputs survives P plus the gap after it (no crash, no stumble) and ends in E
  exactly where the next pattern starts.
  The run starts in E (START_CLEAR of empty track), so any chain of patterns is survivable.

Tested twice: normal jumps, and with super sneakers (higher, longer jumps). Inputs may only change
every DECISION ticks (0.1 s), so the routes don't need frame-perfect timing.

Also runs long chains from the real chainer (real speed ramp, seeds 1..N) end to end as a check
on the spawning code, and writes coin trails (coins follow a preferred safe route) to coins.json.

Usage: test_escape.py [--quick] [--chains N]
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sim import *  # noqa: E402,F403

DECISION = 6          # ticks between input decisions (0.1 s)
LEAD = 2 * U          # the BFS starts with the player this far before row 0
INPUTS = (NONE, LEFT, RIGHT, JUMP, ROLL)


def settled(k):
    lane, frm, sw, y, vy, gnd, roll = k
    return gnd and y == 0 and sw == 0      # rolling counts: a rolling player can still jump/switch


def bfs(objs, z, z_end, speed, start_keys, jump_v=JUMP_V, score=False):
    """Search from start_keys with the player at z until z >= z_end.
    speed(z) gives the speed; objs move as in the game. Returns (final states dict, ticks, fail_z,
    history). history[t] maps state key -> (parent key, input, score) when score=True."""
    states = {k: (None, NONE, 0) for k in start_keys}
    hist = []
    t = 0
    while z < z_end:
        z += speed(z)
        move_world(objs)
        env = env_at(objs, z)
        new = {}
        inputs = INPUTS if t % DECISION == 0 else (NONE,)
        for k, (_, _, sc) in states.items():
            for inp in inputs:
                p = Player.from_key(k)
                if player_step(p, inp, env, jump_v) != OK:
                    continue
                nk = p.key()
                if score:
                    s = sc + (2 if p.gnd and p.y >= TRAIN_H else 0) - (0 if p.gnd else 1) \
                        - (8 if inp in (LEFT, RIGHT) else 0) - (2 if inp in (JUMP, ROLL) else 0)
                    if nk not in new or new[nk][2] < s:
                        new[nk] = (k, inp, s)
                elif nk not in new:
                    new[nk] = (k, inp, 0)
        if score:
            hist.append(new)
        states = new
        t += 1
        if not states:
            return {}, t, z, hist
    return states, t, None, hist


def pattern_case(pat, v, lane, jump_v=JUMP_V, score=False):
    """Run pattern `pat` at constant speed v from a settled state in `lane`."""
    zs = DRAW_DIST + 4 * U
    z = zs - DRAW_DIST                                      # spawn moment
    objs = place(pat, zs, z, v)
    while z + v < zs - LEAD:                                # world runs until the BFS starts
        z += v
        move_world(objs)
    z_end = zs + (pat.rows + gap_rows(v)) * U - LEAD           # where the next pattern's BFS starts
    start = Player(lane=lane, frm=lane, cl=lane).key()
    states, ticks, fail_z, hist = bfs(objs, z, z_end, lambda _z: v, [start], jump_v, score)
    ok = [k for k in states if settled(k)]
    return ok, fail_z, (zs, z, hist)


def coin_trail(pat, v):
    """Coins along the best-scoring safe route from the middle lane (or any lane that works)."""
    for lane in (1, 0, 2):
        ok, fail_z, (zs, z0, hist) = pattern_case(pat, v, lane, score=True)
        if ok:
            break
    else:
        return []
    final = hist[-1]
    k = max(ok, key=lambda kk: final[kk][2])
    path = []
    for t in range(len(hist) - 1, -1, -1):
        path.append(k)
        k = hist[t][k][0]
    path.reverse()
    coins, next_row = [], 0
    for t, k in enumerate(path):
        pz = z0 + (t + 1) * v
        row_z = zs + next_row * U + U // 2
        if next_row >= pat.rows:
            break
        if pz >= row_z:
            lane, frm, sw, y, vy, gnd, roll = k
            cl = frm if sw > SWITCH_T // 2 else lane
            coins.append([next_row, cl, y])
            next_row += 1
    return coins


def speeds_for(pat):
    vs = list(range(TIER_SPEED[pat.tier], VMAX + 1, 6))
    if vs[-1] != VMAX:
        vs.append(VMAX)
    return vs


def test_patterns(pats, quick=False):
    fails = 0
    for pat in pats:
        t0 = time.time()
        vs = speeds_for(pat)
        if quick:
            vs = sorted({vs[0], vs[len(vs) // 2], vs[-1]})
        bad = []
        for jv, label in ((JUMP_V, "normal"), (JUMP_V_HI, "sneakers")):
            for v in vs:
                for lane in range(3):
                    ok, fail_z, _ = pattern_case(pat, v, lane, jv)
                    if not ok:
                        where = f"dead at row {(fail_z - DRAW_DIST - 4 * U) / U:.1f}" if fail_z else "no settled end"
                        bad.append(f"{label} v={v} lane={lane}: {where}")
        status = "ok" if not bad else "FAIL"
        print(f"{status:4} {pat.name:20} tier {pat.tier} speeds {vs[0]}..{vs[-1]} ({len(vs)})"
              f"  {time.time() - t0:.1f}s")
        for b in bad[:6]:
            print("       " + b)
        fails += bool(bad)
    return fails


def build_world(pats, seed, n_patterns, start_z=0):
    """The game's spawning logic: returns (spawn events, end z). Each event: (player z at spawn,
    pattern index, zs). Mirrors game.c's spawner."""
    rng = Rng(seed)
    ch = Chainer(pats, rng)
    events, nxt = [], start_z + START_CLEAR
    z = start_z
    while len(events) < n_patterns:
        while z + DRAW_DIST < nxt:
            z += speed_at(z)
        i = ch.pick(speed_at(z))
        events.append((z, i, nxt))
        nxt += (pats[i].rows + gap_rows(speed_at(z))) * U
    return events, nxt


def test_chain(pats, seed, n, start_z):
    """End-to-end BFS through n chained patterns with the real speed ramp, from the middle lane.
    Objects are spawned exactly when the game would spawn them."""
    events, z_end = build_world(pats, seed, n, start_z)
    objs = []
    ev = 0
    z = start_z
    states = {Player().key(): (None, NONE, 0)}
    t = 0
    while z < z_end - LEAD:
        z += speed_at(z)
        while ev < len(events) and events[ev][0] <= z:
            zsp, i, zs = events[ev]
            objs.extend(place(pats[i], zs, z, speed_at(z)))
            ev += 1
        move_world(objs)
        objs = [o for o in objs if o.z1 > z - 4 * U]
        env = env_at(objs, z)
        new = {}
        inputs = INPUTS if t % DECISION == 0 else (NONE,)
        for k in states:
            for inp in inputs:
                p = Player.from_key(k)
                if player_step(p, inp, env) == OK:
                    new.setdefault(p.key(), None)
        states = new
        t += 1
        if not states:
            return False, z, [pats[e[1]].name for e in events]
    return True, z, [pats[e[1]].name for e in events]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--chains", type=int, default=6)
    ap.add_argument("--only")
    a = ap.parse_args()
    pats = load_patterns()
    if a.only:
        pats = [p for p in pats if p.name == a.only]
    # negative controls: the search must reject impossible patterns
    for name, objs in (("wall_row", [(WALL, l, 2, 3) for l in range(3)]),
                       ("fast_double_shift", [(TRAIN, 0, 1, 4), (TRAIN, 1, 1, 4), (TRAIN, 1, 5, 8),
                                              (TRAIN, 2, 5, 8)])):
        bad = Pattern(name, 0, 1, 10, objs)
        ok, _, _ = pattern_case(bad, VMAX, 1)
        print(f"negative control {name}: {'rejected (good)' if not ok else 'ACCEPTED (test is broken)'}")
        if ok:
            sys.exit(1)
    t0 = time.time()
    fails = test_patterns(pats, a.quick)
    print(f"patterns: {len(pats) - fails}/{len(pats)} pass ({time.time() - t0:.0f}s)")

    cfails = 0
    if not a.only:
        for seed in range(1, a.chains + 1):
            start = [0, 40 * SPEED_STEP_ZU, 90 * SPEED_STEP_ZU][seed % 3]
            ok, z, names = test_chain(pats, seed, 25, start)
            print(f"chain seed {seed} from v={speed_at(start)}: {'ok' if ok else 'FAIL at z=%d' % z}"
                  f"  ({len(names)} patterns: {' '.join(names[:8])} ...)")
            cfails += not ok

        coins = {}
        for p in pats:
            coins[p.name] = coin_trail(p, (TIER_SPEED[p.tier] + VMAX) // 2)
        out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "coins.json")
        json.dump(coins, open(out, "w"), indent=0)
        print(f"coin trails: {sum(len(c) for c in coins.values())} coins -> {out}")
    print("RESULT", "PASS" if fails == 0 and cfails == 0 else "FAIL")
    sys.exit(1 if fails or cfails else 0)


if __name__ == "__main__":
    main()
