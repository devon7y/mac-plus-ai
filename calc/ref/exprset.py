#!/usr/bin/env python3
"""Whole handwritten expressions from held-out writers, for testing segmentation + classifier +
grammar decoding together (the M1 numbers).

    python3 -I exprset.py ../build/models/S2.npz [--n 500] [--seed 7]

Digits come from the held-out writers of UJI Pen Characters v2 (20 writers) and pendigits (14
writers), ( ) . - and lowercase x (as a multiplication sign) from the UJI writers, + / = and some
multiplication signs from synth.py with a seed that training never used. Symbols are placed left
to right in href units with random gaps (sometimes touching), the whole line is slanted, scaled to
a random size and resampled like a mouse (synth.mouse_path).
"""
import argparse
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from decode import segment, viterbi, group_raster, raised_flags     # noqa: E402
from strokes import CLASSES, load_pendigits, load_uji2              # noqa: E402
from synth import Glyphs, mouse_path                                # noqa: E402
import ref                                                          # noqa: E402

CI = {c: i for i, c in enumerate(CLASSES)}


def pools():
    uji = load_uji2()
    hs = {}
    for o in uji:
        if o["label"].isdigit():
            p = np.concatenate(o["strokes"])
            hs.setdefault(o["writer"], []).append(p[:, 1].max() - p[:, 1].min())
    P = {c: [] for c in CLASSES}
    for o in uji:
        if o["split"] != "tst":
            continue
        href = float(np.median(hs[o["writer"]]))
        P[o["label"]].append(("uji", o["strokes"], href))
    for o in load_pendigits("tes"):
        p = np.concatenate(o["strokes"])
        P[o["label"]].append(("pd", o["strokes"], float(p[:, 1].max() - p[:, 1].min())))
    return P


def random_expr(rng, depth=0):
    """A random valid expression as a list of class labels (without '=')."""
    def number():
        k = rng.choice([1, 1, 2, 2, 3])
        s = [str(rng.integers(1 if k > 1 else 0, 10))] + [str(rng.integers(0, 10)) for _ in range(k - 1)]
        if rng.random() < 0.2:
            s += ["."] + [str(rng.integers(0, 10)) for _ in range(rng.choice([1, 2]))]
        return s

    def operand():
        if depth < 2 and rng.random() < 0.18:
            return ["("] + random_expr(rng, depth + 1) + [")"]
        return number()
    out = []
    if rng.random() < 0.1:
        out.append("-")
    out += operand()
    for _ in range(rng.choice([1, 1, 2, 2, 3])):
        out.append(rng.choice(["+", "-", "x", "/"]))
        out += operand()
    return out


def place(label, P, g, rng):
    """Strokes of one symbol in line units (y down, baseline 1.0, digit height about 1).
    A label '^c' is symbol c written as a superscript (smaller, raised)."""
    if label.startswith("^"):
        st = place(label[1:], P, g, rng)
        f = rng.uniform(0.45, 0.7)
        bottom = rng.uniform(0.25, 0.5)
        pts = np.concatenate(st)
        y1 = pts[:, 1].max()
        return [(s - [0, y1]) * f + [0, bottom] for s in st]
    use_real = label in "0123456789().-" or (label == "x" and rng.random() < 0.5)
    strokes = None
    if use_real and P[label]:
        src, strokes, href = P[label][rng.integers(len(P[label]))]
        strokes = [np.asarray(s, np.float64) / href for s in strokes]
        if label == "x":            # letter x written as two arcs ")(" is not a times sign
            a = [(s[:, 0].min(), s[:, 0].max()) for s in strokes]
            if len(a) != 2 or min(a[0][1], a[1][1]) - max(a[0][0], a[1][0]) < 0.5 * min(a[0][1] - a[0][0], a[1][1] - a[1][0]):
                strokes = None
    if strokes is None:
        strokes = g.make(label)
        while label == "x" and len(strokes) == 2 and min(strokes[0][:, 0].max(), strokes[1][:, 0].max()) - \
                max(strokes[0][:, 0].min(), strokes[1][:, 0].min()) < 0.3 * (strokes[0][:, 0].max() - strokes[0][:, 0].min()):
            strokes = g.make(label)
    pts = np.concatenate(strokes)
    x0, y0 = pts.min(0)
    x1, y1 = pts.max(0)
    if label.isdigit():
        f = rng.uniform(0.9, 1.1) / max(y1 - y0, 1e-3)
        ty = 1.0 + rng.normal(0, 0.03)
        strokes = [(s - [x0, y1]) * f + [0, ty] for s in strokes]
    elif label in "()":
        f = rng.uniform(1.05, 1.25) / max(y1 - y0, 1e-3)
        strokes = [(s - [x0, (y0 + y1) / 2]) * f + [0, 0.5] for s in strokes]
    elif label == ".":
        strokes = [(s - [x0, y1]) + [0, 1.0] for s in strokes]
    else:
        cy = 0.5 + rng.normal(0, 0.05)
        strokes = [(s - [x0, (y0 + y1) / 2]) + [0, cy] for s in strokes]
    return strokes


def make_line(labels, P, g, rng, owner=None):
    x = 0.0
    out = []
    for lab in labels:
        st = place(lab, P, g, rng)
        w = max(s[:, 0].max() for s in st)
        gap = rng.uniform(-0.03, 0.45) if lab not in ".)" else rng.uniform(0.05, 0.3)
        if lab.startswith("^"):
            gap = rng.uniform(0.0, 0.2)
        if out:
            x += gap
        out.append([s + [x, 0] for s in st])
        x += w
    # line: slant, scale, mouse
    k = rng.uniform(-0.25, 0.15)
    th = math.radians(np.clip(rng.normal(0, 3), -8, 8))
    c, s_ = math.cos(th), math.sin(th)
    M = np.array([[c, -s_], [s_, c]]) @ np.array([[1, k], [0, 1]])
    hpx = math.exp(rng.uniform(math.log(24), math.log(80)))
    strokes = []
    for si, sym in enumerate(out):
        for st in sym:
            p = (st @ M.T) * hpx + [20, 20]
            strokes.append(mouse_path(p, hpx, rng))
            if owner is not None:
                owner.append(si)
    return strokes


def evaluate(model_path, n=500, seed=7, verbose=0):
    M = ref.load(model_path)
    P = pools()
    rng = np.random.default_rng(seed)
    g = Glyphs(np.random.default_rng(seed + 1000))
    stats = dict(lines=0, seg_ok=0, sym=0, sym_ok_raw=0, sym_ok_dec=0, line_ok_raw=0, line_ok_dec=0)
    for it in range(n):
        labels = random_expr(rng) + ["="]
        strokes = make_line(labels, P, g, rng)
        groups, href = segment(strokes)
        imgs = np.stack([group_raster(strokes, gr, href) for gr in groups])
        S = ref.forward_batch(M, imgs)
        raw = [int(v) for v in S.argmax(1)]
        dec, _, _ = viterbi(S, None, raised_flags(strokes, groups, href))
        truth = [CI[c] for c in labels]
        stats["lines"] += 1
        if len(groups) == len(truth):
            stats["seg_ok"] += 1
            stats["sym"] += len(truth)
            stats["sym_ok_raw"] += sum(a == b for a, b in zip(raw, truth))
            stats["sym_ok_dec"] += sum(a == b for a, b in zip(dec, truth))
        stats["line_ok_raw"] += raw == truth
        stats["line_ok_dec"] += dec == truth
        if verbose and dec != truth:
            show = lambda v: "".join(CLASSES[c] for c in v)
            print(f"  truth {''.join(labels)}  raw {show(raw)}  dec {show(dec)}  groups {len(groups)}/{len(truth)}")
    L = stats["lines"]
    print(f"expressions {L} ({sum(1 for _ in range(1))} seed {seed}): segmentation correct {stats['seg_ok'] / L * 100:.1f}%; "
          f"symbols (correctly segmented lines) raw {stats['sym_ok_raw'] / max(1, stats['sym']) * 100:.2f}% "
          f"decoded {stats['sym_ok_dec'] / max(1, stats['sym']) * 100:.2f}%; whole line raw "
          f"{stats['line_ok_raw'] / L * 100:.1f}% decoded {stats['line_ok_dec'] / L * 100:.1f}%")
    return stats


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("--n", type=int, default=500)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("-v", type=int, default=0)
    a = ap.parse_args()
    evaluate(a.model, a.n, a.seed, a.v)
