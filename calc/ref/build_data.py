#!/usr/bin/env python3
"""Build Scribble Calc's training and test rasters (24 x 24, 1 bit, packed) into calc/build/data/.

    python3 -I build_data.py [--scale 1.0] [--procs 6]

Sources (all checked 2026-10-23):
  * UJI Pen Characters v2 (Prat, Castro, Llorens, Marzal, Vilar; UCI ML Repository; CC BY 4.0):
    digits, ( ) . - and lowercase x (used as extra multiplication signs). 40 "trn" writers train,
    the 20 "tst" writers are held out.
  * Pen-Based Recognition of Handwritten Digits, original data (Alpaydin & Alimoglu; UCI; CC BY 4.0):
    30 writers train, the 14 test writers are held out.
  * Synthetic strokes (synth.py, ours) for every class, the only source for + x / =.
Every sample goes through synth.degrade() (random slant, size, mouse-rate resampling, wobble)
and strokes.raster_fast() (the Mac's exact rasteriser).
"""
import argparse
import os
import sys
from multiprocessing import Pool

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from strokes import CLASSES, CALC, load_pendigits, load_uji2, raster_fast   # noqa: E402
from synth import Glyphs, degrade                                         # noqa: E402

OUT = os.path.join(CALC, "build", "data")
CI = {c: i for i, c in enumerate(CLASSES)}


def real_samples():
    uji = load_uji2()
    # per-writer reference height: the median digit height
    hs = {}
    for o in uji:
        if o["label"].isdigit():
            p = np.concatenate(o["strokes"])
            hs.setdefault(o["writer"], []).append(p[:, 1].max() - p[:, 1].min())
    href = {w: float(np.median(v)) for w, v in hs.items()}
    for o in uji:
        o["href"] = href[o["writer"]]
    pd = load_pendigits("tra") + load_pendigits("tes")
    for o in pd:
        p = np.concatenate(o["strokes"])
        o["href"] = float(max(p[:, 1].max() - p[:, 1].min(), 1))
    return uji, pd


def job(args):
    kind, items, seed, reps, clean_frac = args
    rng = np.random.default_rng(seed)
    X, Y = [], []
    if kind == "synth":
        g = Glyphs(rng)
        for lab, n in items:
            for _ in range(n):
                s, minD = degrade(g.make(lab), 1.0, rng, clean=rng.random() < clean_frac)
                X.append(np.packbits(raster_fast(s, minD)))
                Y.append(CI[lab])
    else:
        for o in items:
            for _ in range(reps):
                s, minD = degrade(o["strokes"], o["href"], rng, clean=rng.random() < clean_frac)
                X.append(np.packbits(raster_fast(s, minD)))
                Y.append(CI[o["label"]])
    return np.array(X, np.uint8).reshape(-1, 72), np.array(Y, np.uint8)


def run(pool, jobs):
    res = pool.map(job, jobs)
    return np.concatenate([r[0] for r in res]), np.concatenate([r[1] for r in res])


def chunks(lst, n):
    k = max(1, (len(lst) + n - 1) // n)
    return [lst[i:i + k] for i in range(0, len(lst), k)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--procs", type=int, default=6)
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    uji, pd = real_samples()
    uji_tr = [o for o in uji if o["split"] == "trn"]
    uji_te = [o for o in uji if o["split"] == "tst"]
    for o in uji_te:
        if o["label"] == "x":
            o["label"] = "x"
    pd_tr = [o for o in pd if o["split"] == "trn"]
    pd_te = [o for o in pd if o["split"] == "tst"]
    print(f"uji trn {len(uji_tr)} tst {len(uji_te)}; pendigits trn {len(pd_tr)} tst {len(pd_te)}")
    S = a.scale
    syn_n = {c: int((5000 if c.isdigit() else 12000) * S) for c in CLASSES}
    syn_n["."] = int(6000 * S)
    with Pool(a.procs) as pool:
        jobs = []
        seed = 1000
        for part in chunks(pd_tr, 24):
            jobs.append(("real", part, seed, max(1, int(6 * S)), 0.25)); seed += 1
        for part in chunks(uji_tr, 24):
            jobs.append(("real", part, seed, max(1, int(30 * S)), 0.25)); seed += 1
        per = []
        for c in CLASSES:
            n = syn_n[c]
            for k in range(0, n, 1000):
                per.append([(c, min(1000, n - k))])
        for p in per:
            jobs.append(("synth", p, seed, 0, 0.25)); seed += 1
        Xtr, Ytr = run(pool, jobs)
        np.savez_compressed(os.path.join(OUT, "train.npz"), X=Xtr, Y=Ytr)
        print("train", Xtr.shape, np.bincount(Ytr, minlength=len(CLASSES)))
        tests = {}
        for name, items, clean in (("pd_clean", pd_te, 1.0), ("pd_mouse", pd_te, 0.0),
                                   ("uji_clean", uji_te, 1.0), ("uji_mouse", uji_te, 0.0)):
            reps = 1 if name.startswith("pd") else 3
            tests[name] = run(pool, [("real", p, 50000 + i, reps, clean) for i, p in enumerate(chunks(items, 12))])
        tests["syn_mouse"] = run(pool, [("synth", [(c, 300)], 90000 + i, 0, 0.0) for i, c in enumerate(CLASSES)])
        for k, (X, Y) in tests.items():
            np.savez_compressed(os.path.join(OUT, f"test_{k}.npz"), X=X, Y=Y)
            print(k, X.shape, np.bincount(Y, minlength=len(CLASSES)))


if __name__ == "__main__":
    main()
