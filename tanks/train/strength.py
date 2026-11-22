#!/usr/bin/env python3
"""The strength table of the shipped integer policies (tanks/policies), as markdown.

    strength.py [--dir tanks/policies] [--games 400] [--out table.md]

Rows: each enemy type at each difficulty (its manifest entries). Columns: win / loss / draw (%)
against the scripted bots and the "player" net (all driving a player-type tank), and against the
same type's Hard net (a mirror duel: how the checkpoints compare). Duels as in evaluate.py: 400
games on the 8 arenas with random spawns, 60 s limit, deterministic argmax play.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from evaluate import duel, load_policy, TYPES, BOTS  # noqa: E402
import numpy as np  # noqa: E402
import defs as D  # noqa: E402

LEVELS = ["Easy", "Normal", "Hard"]


def style(p, typ, opp_kind=D.BOT_HUNTER, n=300, seed=2):
    """how a policy plays against a bot: mean distance (px) while both live, share of its kills made
    with a bounced shell, share of its decisions in the enemy's line of sight, mean game length (s)"""
    import numpy as np
    from evaluate import spawns
    from twin import Twin
    from evaluate import act_of
    rng = np.random.default_rng(seed)
    ar, pos = spawns(rng, n)
    tw = Twin(n, 2, ns=6)
    tw.init(ar, rng.integers(1, 2 ** 31, size=n))
    for i, ty in ((0, typ), (1, D.T_PLAYER)):
        tw.set_tank(i, ty, i, pos[:, i, 1] * 256 + 128, pos[:, i, 0] * 256 + 128, rng.integers(256, size=n))
    done = np.zeros(n, bool)
    end = np.full(n, 1800)
    dsum = dcnt = expo = dec = 0
    for k in range(1800):
        if k % 3 == 0:
            o = tw.obs(0)
            tw.apply_action(0, act_of(tw, p, o))
            live = ~done
            expo += int(((o[:, 21] == 127) & live).sum())
            dec += int(live.sum())
        if k % 3 == 1:
            tw.bot_act(1, opp_kind)
        tw.step()
        t = tw.t
        live = ~done
        d = np.maximum(np.abs(t["x"][:, 1] - t["x"][:, 0]), np.abs(t["y"][:, 1] - t["y"][:, 0])) >> 4
        dsum += int(d[live].sum())
        dcnt += int(live.sum())
        new = live & ((t["alive"][:, 0] == 0) | (t["alive"][:, 1] == 0))
        end[new] = k + 1
        done |= new
        if done.all():
            break
    kills = (tw.t["alive"][:, 1] == 0) & (tw.t["by"][:, 1] == 0)
    bank = float(tw.kill_bounce[:, 1][kills].mean()) if kills.any() else 0.0
    return dict(dist=dsum / max(dcnt, 1), bank=bank, exposed=expo / max(dec, 1), seconds=float(end.mean()) / 30)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(os.path.dirname(HERE), "policies"))
    ap.add_argument("--games", type=int, default=400)
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    man = json.load(open(os.path.join(a.dir, "manifest.json")))
    player = load_policy(os.path.join(a.dir, man["player"][-1])) if man.get("player") else None
    cols = list(BOTS) + (["player net"] if player else []) + ["vs own Hard"]
    lines = ["| Enemy | Level | " + " | ".join(cols) + " |", "| --- | --- | " + " | ".join("---" for _ in cols) + " |"]
    res = {}
    for tname in ("rusher", "sniper", "careful"):
        files = man.get(tname, [])
        if not files:
            continue
        typ = TYPES[tname]
        hard = load_policy(os.path.join(a.dir, files[-1]))
        for lv, f in zip(LEVELS, files):
            p = load_policy(os.path.join(a.dir, f))
            row = {}
            for bname, kind in BOTS.items():
                row[bname] = duel(p, typ, kind, D.T_PLAYER, a.games)
            if player:
                row["player net"] = duel(p, typ, player, D.T_PLAYER, a.games)
            row["vs own Hard"] = duel(p, typ, hard, typ, a.games, seed=1) if lv != "Hard" else None
            res[f"{tname}/{lv}"] = dict(row, style=style(p, typ))
            cells = ["-" if v is None else f"{v[0] * 100:.0f} / {v[1] * 100:.0f} / {v[2] * 100:.0f}" for v in row.values()]
            lines.append(f"| {tname} | {lv} | " + " | ".join(cells) + " |")
            print(lines[-1], flush=True)
    lines += ["", "| Enemy | Level | mean distance to the enemy (px) | kills by bank shot | time in line of sight | game length (s) |",
              "| --- | --- | --- | --- | --- | --- |"]
    for key, r in res.items():
        st = r["style"]
        t, lv = key.split("/")
        lines.append(f"| {t} | {lv} | {st['dist']:.0f} | {st['bank'] * 100:.0f}% | {st['exposed'] * 100:.0f}% | {st['seconds']:.1f} |")
    text = "\n".join(lines)
    print(text)
    if a.out:
        open(a.out, "w").write(text + "\n")
        json.dump(res, open(os.path.splitext(a.out)[0] + ".json", "w"), indent=1)


if __name__ == "__main__":
    main()
