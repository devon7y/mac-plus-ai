#!/usr/bin/env python3
"""Preprocess the Quick, Draw! sketch-rnn .npz files (CC BY 4.0) into flat training arrays.

Source: https://storage.googleapis.com/quickdraw_dataset/sketchrnn/<category>.npz
(stroke-3 int16 rows (dx, dy, lift) in raw screen pixels, already RDP-simplified at raw scale).

Per drawing, to match the Mac's 256x256 "AI frame" and the way the Mac will simplify the
user's mouse strokes:
  1. absolute points = cumulative sum (the first source row is an offset from an arbitrary
     origin and is discarded: drawings start at their first point);
  2. shift to min 0 and scale uniformly so the bounding box's larger side is 255
     (exactly what the dataset's "simplified" ndjson does), round to integer pixels;
  3. per stroke: drop consecutive duplicate points, RDP with epsilon 2.0 (rdp() below is
     the reference the Mac port must copy), drop strokes left with a single point
     (a single point draws nothing in stroke-3);
  4. stroke-3 rows relative to the first point: row k = (p_k - p_{k-1}, lift_k), where
     lift_k = 1 means the pen is lifted after point k. The first point itself is implicit.
Drawings with more than MAXLEN rows (or fewer than 1) are dropped.

Writes to OUT: {split}_xy.npy int16 [R,2], {split}_pen.npy uint8 [R], {split}_off.npy
int64 [N+1] (row offsets), {split}_cat.npy int16 [N], meta.json (scale factor, counts).
"""
import json
import os
import sys
from multiprocessing import Pool

import numpy as np

SRC = sys.argv[1]          # directory with <category>.npz and categories.txt one level up
OUT = sys.argv[2]
MAXLEN = 250
EPS = 2.0
SPLITS = ("train", "valid", "test")


def rdp(pts, eps=EPS):
    """Ramer-Douglas-Peucker on an (n,2) float array; keeps endpoints. Perpendicular
    distance to the chord (distance to the start point if the chord has zero length)."""
    n = len(pts)
    if n < 3:
        return pts
    keep = np.zeros(n, bool)
    keep[0] = keep[-1] = True
    stack = [(0, n - 1)]
    while stack:
        s, e = stack.pop()
        if e <= s + 1:
            continue
        a, b = pts[s], pts[e]
        seg = pts[s + 1:e]
        d = b - a
        L = np.hypot(d[0], d[1])
        if L == 0:
            dist = np.hypot(seg[:, 0] - a[0], seg[:, 1] - a[1])
        else:
            dist = np.abs(d[0] * (seg[:, 1] - a[1]) - d[1] * (seg[:, 0] - a[0])) / L
        i = int(np.argmax(dist))
        if dist[i] > eps:
            m = s + 1 + i
            keep[m] = True
            stack.append((s, m))
            stack.append((m, e))
    return pts[keep]


def convert(s3):
    """Source stroke-3 (int16, raw px) -> list of rows (dx, dy, lift) in the 255 box."""
    a = np.cumsum(s3[:, :2].astype(np.float64), axis=0)
    a -= a.min(axis=0)
    span = a.max()
    if span <= 0:
        return None
    a = np.round(a * (255.0 / span))
    ends = np.flatnonzero(s3[:, 2] == 1)
    if len(ends) == 0 or ends[-1] != len(s3) - 1:
        ends = np.append(ends, len(s3) - 1)
    strokes, start = [], 0
    for e in ends:
        p = a[start:e + 1]
        start = e + 1
        if len(p) > 1:
            dup = np.all(p[1:] == p[:-1], axis=1)
            p = np.concatenate([p[:1], p[1:][~dup]])
        if len(p) < 2:
            continue
        strokes.append(rdp(p))
    if not strokes:
        return None
    pts = np.concatenate(strokes)
    lift = np.concatenate([np.r_[np.zeros(len(p) - 1), 1.0] for p in strokes])
    d = np.diff(pts, axis=0)
    rows = np.column_stack([d, lift[1:]]).astype(np.int16)
    return rows


def work(args):
    ci, cat = args
    z = np.load(os.path.join(SRC, cat + ".npz"), allow_pickle=True, encoding="latin1")
    res = {}
    for sp in SPLITS:
        xs, ns, dropped = [], [], 0
        for s3 in z[sp]:
            r = convert(np.asarray(s3))
            if r is None or len(r) < 1 or len(r) > MAXLEN:
                dropped += 1
                continue
            xs.append(r)
            ns.append(len(r))
        res[sp] = (np.concatenate(xs), np.array(ns, np.int64), len(z[sp]), dropped)
    return ci, cat, res


def main():
    cats = [c.strip() for c in open(os.path.join(SRC, "..", "categories.txt")) if c.strip()]
    assert len(cats) == 345, len(cats)
    os.makedirs(OUT, exist_ok=True)
    nproc = int(os.environ.get("SLURM_CPUS_PER_TASK", "8"))
    per = {}
    with Pool(nproc) as pool:
        for ci, cat, res in pool.imap_unordered(work, list(enumerate(cats))):
            per[ci] = (cat, res)
            print(ci, cat, {sp: (res[sp][2], res[sp][3], round(float(res[sp][1].mean()), 1))
                            for sp in SPLITS}, flush=True)
    meta = {"categories": cats, "maxlen": MAXLEN, "rdp_eps": EPS, "box": 255,
            "counts": {}, "source_counts": {}, "dropped": {}}
    for sp in SPLITS:
        rows = [per[ci][1][sp][0] for ci in range(345)]
        lens = [per[ci][1][sp][1] for ci in range(345)]
        cat_ids = np.concatenate([np.full(len(l), ci, np.int16) for ci, l in enumerate(lens)])
        lens = np.concatenate(lens)
        rows = np.concatenate(rows)
        off = np.zeros(len(lens) + 1, np.int64)
        off[1:] = np.cumsum(lens)
        np.save(os.path.join(OUT, f"{sp}_xy.npy"), rows[:, :2].copy())
        np.save(os.path.join(OUT, f"{sp}_pen.npy"), rows[:, 2].astype(np.uint8))
        np.save(os.path.join(OUT, f"{sp}_off.npy"), off)
        np.save(os.path.join(OUT, f"{sp}_cat.npy"), cat_ids)
        meta["counts"][sp] = int(len(lens))
        meta["source_counts"][sp] = [int(per[ci][1][sp][2]) for ci in range(345)]
        meta["dropped"][sp] = [int(per[ci][1][sp][3]) for ci in range(345)]
        meta[f"{sp}_rows"] = int(len(rows))
        meta[f"{sp}_len_mean"] = float(lens.mean())
        meta[f"{sp}_len_p99"] = float(np.percentile(lens, 99))
        if sp == "train":
            meta["scale_factor"] = float(rows[:, :2].astype(np.float64).std())
    json.dump(meta, open(os.path.join(OUT, "meta.json"), "w"), indent=1)
    print({k: v for k, v in meta.items() if k not in ("categories", "source_counts", "dropped")})


if __name__ == "__main__":
    main()
