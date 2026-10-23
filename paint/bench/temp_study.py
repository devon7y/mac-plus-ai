#!/usr/bin/env python3
"""Temperature study with the Mac's exact arithmetic (ref.py): for each (mixture, pen) pair,
drawings from scratch (96: 48 categories x 2) and completions of real partial drawings
(fetch_partials.py output): share that loops to the 200-move cap, share of completions that
stop early (< 5 generated moves), median moves, moves per stroke.

Usage: temp_study.py <model.dmdl> <partials.json> mix,pen [mix,pen ...]
"""
import json
import sys

import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
import model_file  # noqa: E402
import ref  # noqa: E402

m = model_file.read(sys.argv[1])
parts = json.load(open(sys.argv[2]))["partials"]
names = m["names"]
prompts = []
for p in parts:
    pts = [(x, y, 1 if t == len(xs) - 1 else 0) for xs, ys in p["strokes"] for t, (x, y) in enumerate(zip(xs, ys))]
    r = ref.prompt_rows(pts)
    if r:
        prompts.append((names.index(p["category"]), r[1]))
cats = list(range(0, 345, 7))[:48]
print(f"{len(prompts)} completions, 96 drawings from scratch per setting")
for arg in sys.argv[3:]:
    tm, tp = (float(v) for v in arg.split(","))
    scratch = [ref.finish_it(m, [], c, 5000 + 31 * i + j, (tm, tp)) for i, c in enumerate(cats) for j in range(2)]
    comp = [ref.finish_it(m, rows, c, 900 + k, (tm, tp)) for k, (c, rows) in enumerate(prompts)]
    ls, lc = np.array([len(g) for g in scratch]), np.array([len(g) for g in comp])
    per = [len(g) / max(1, sum(1 for *_, p in g if p != 0)) for g in scratch]
    print(f"mixture {tm} pen {tp}: from scratch loops {np.mean(ls >= 200):.0%}, median {int(np.median(ls))} moves, "
          f"{np.median(per):.1f} per stroke | completions loops {np.mean(lc >= 200):.0%}, "
          f"early stops (<5) {np.mean(lc < 5):.0%}, median {int(np.median(lc))} generated")
