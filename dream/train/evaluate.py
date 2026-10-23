#!/usr/bin/env python3
"""Dream evaluation sheets. All models start from the Mac's own noise for seeds 1..S (export.mac_noise),
so the columns line up:
  teacher   guided DDIM, --teacher_steps uniform steps, weight w (as distilled)
  float     float student, N steps (the Mac's integer sampler, see distill.student_sample)
  qat       qat student, N steps
  int       export.mac_sample: exact Mac arithmetic (int-mode network, Q14 update) = what the Mac shows
Writes per-model sheets (rows = subjects, columns = seeds; 2x nearest neighbour; plain and 16 levels),
a side-by-side comparison sheet, the nearest-neighbour check against the training set, and
summary.json (MSE between models, NN distances, qat vs int, overflow margins).

  python evaluate.py --teacher T/ema.pt --float S/float.pt --qat S/qat.pt --data <split|synth:500> --out S/eval
"""
import argparse
import copy
import json
import os

import numpy as np
import torch

from common import get_split, grid_image, levels_to_x, to_u8
from distill import Grid, load_teacher, quant_in, student_sample
from export import mac_noise, mac_sample
from student_tools import load_student, overflow_stats
from teacher import ddim


def sheet(x, ns, S, names, path, levels=None, scale=2, page=30):
    """Rows = subjects, columns = seeds; split into pages of `page` rows (path gets _p<k> when paged)."""
    a = to_u8(x[:, 0], levels)
    paths = []
    for p0 in range(0, ns, page):
        rows = [[a[r * S + j] for j in range(S)] for r in range(p0, min(ns, p0 + page))]
        out = path if ns <= page else path.replace(".png", f"_p{p0 // page}.png")
        grid_image(rows, scale=scale, labels=names[p0:p0 + page]).save(out)
        paths.append(out)
    return paths


def ship_sheet(a, teacher, m, present, names, dev, summ):
    """The shipping-bar sheet: `a.ship` random present subjects (fixed draw), int mode (exactly what the
    Mac shows) for each re-noise setting c in --ship_c (1 = deterministic DDIM, 0 = full re-noising), two
    seeds, the teacher on the same noise; pages of 20 subjects. Columns, in --ship_c order: int c seed A |
    int c seed B | ... | teacher seed A | teacher seed B."""
    import random
    subs = sorted(random.Random(2026).sample(present, min(a.ship, len(present))))
    sa, sb = [int(x) for x in a.ship_seeds.split(",")]
    cs = [float(x) for x in a.ship_c.split(",")]
    S = [s for s in subs for _ in (0, 1)]
    seeds = [sd for _ in subs for sd in (sa, sb)]
    ts = summ["ts"]
    mi = copy.deepcopy(m).cpu()
    cols = []
    for c in cs:
        outs = [mac_sample(mi, S[b:b + a.int_batch], seeds[b:b + a.int_batch], ts, c=c).float()
                for b in range(0, len(S), a.int_batch)]
        cols.append(to_u8((torch.cat(outs) / 512.0)[:, 0], 16))
    z = torch.cat([mac_noise(sd, m.cfg) for sd in seeds]) / 512.0
    with torch.no_grad():
        xt, _ = ddim(teacher, quant_in(z.to(dev)) / 512.0, torch.tensor(S, device=dev), 1.0, 0.0,
                     a.teacher_steps, summ["w"])
    cols.append(to_u8(xt[:, 0], 16))
    rows = [[c[2 * r + k] for c in cols for k in (0, 1)] for r in range(len(subs))]
    labels = [names[s] for s in subs]
    for p0 in range(0, len(subs), 20):
        grid_image(rows[p0:p0 + 20], scale=3, labels=labels[p0:p0 + 20], label_w=130).save(
            os.path.join(a.out, f"ship_int_p{p0 // 20}.png"))
    summ["ship_subjects"] = labels
    summ["ship_seeds"] = [sa, sb]
    summ["ship_columns"] = [f"int c={c:g} seed {sd}" for c in cs for sd in (sa, sb)] + \
        [f"teacher seed {sd}" for sd in (sa, sb)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--teacher", required=True)
    ap.add_argument("--float")
    ap.add_argument("--qat")
    ap.add_argument("--data", help="training split for the nearest-neighbour check")
    ap.add_argument("--out", required=True)
    ap.add_argument("--subj", type=int, default=24)
    ap.add_argument("--subj_from", type=int, default=0)
    ap.add_argument("--seeds", type=int, default=8)
    ap.add_argument("--teacher_steps", type=int, default=64)
    ap.add_argument("--w", type=float, default=None)
    ap.add_argument("--int_batch", type=int, default=48)
    ap.add_argument("--drop", default="steak", help="subjects the app drops (not evaluated)")
    ap.add_argument("--ship", type=int, default=0, help="shipping-bar sheet: this many random present subjects")
    ap.add_argument("--ship_seeds", default="101,202")
    ap.add_argument("--ship_c", default="1,0", help="re-noise settings to show (export.mac_sample c)")
    a = ap.parse_args()
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    torch.backends.cuda.matmul.allow_tf32 = False
    torch.backends.cudnn.allow_tf32 = False
    os.makedirs(a.out, exist_ok=True)
    teacher, tck = load_teacher(a.teacher, dev)
    names = tck["names"]
    K = len(names)
    drop = set(x.strip() for x in a.drop.split(",") if x.strip())
    present = [p for p in tck.get("present", list(range(K))) if names[p] not in drop]
    subs = present[a.subj_from:a.subj_from + a.subj]
    ns, S = len(subs), a.seeds
    rnames = [names[s] for s in subs]
    s_ = torch.tensor([s for s in subs for _ in range(S)])
    seeds = [1 + j for _ in subs for j in range(S)]
    stu = {}
    for tag, path in (("float", a.float), ("qat", a.qat)):
        if path:
            stu[tag] = load_student(path, dev)
    cfg0 = next(iter(stu.values()))[0].cfg if stu else "A64L2"
    z = torch.cat([mac_noise(sd, cfg0) for sd in seeds]) / 512.0
    res, summ = {}, dict(subjects=rnames, seeds=list(range(1, S + 1)))

    ck0 = next(iter(stu.values()))[1] if stu else None
    w = a.w if a.w is not None else (ck0["w"] if ck0 else 3.0)
    summ["w"] = w
    with torch.no_grad():
        res["teacher"], _ = ddim(teacher, quant_in(z.to(dev)) / 512.0, s_.to(dev), 1.0, 0.0, a.teacher_steps, w)
    for tag, (m, ck) in stu.items():
        G = Grid(len(ck["ts"]) - 1, ck["grid"]["kind"])
        assert G.t == list(ck["ts"])
        m.eval()
        xs, _ = student_sample(m, z.to(dev), s_.to(dev), G)
        res[tag] = xs[-1]
        summ["ts"] = G.t
        if tag == "qat":
            mi = copy.deepcopy(m).cpu()
            outs = []
            for b in range(0, len(seeds), a.int_batch):
                outs.append(mac_sample(mi, s_[b:b + a.int_batch].tolist(), seeds[b:b + a.int_batch], G.t).float())
            res["int"] = torch.cat(outs).to(dev) / 512.0
            # qat (float32 STE arithmetic) vs exact integers on the same Mac noise
            summ["qat_vs_int_maxdiff_q9"] = float((res["int"] - res["qat"]).abs().max()) * 512
            summ["qat_vs_int_pixels_differ"] = int(((res["int"] - res["qat"]).abs() > 1e-6).sum())
            # overflow margins along the qat trajectories
            worst = []
            for i in range(G.n):
                t = torch.full((len(seeds),), G.t[i], device=dev)
                rows, wst = overflow_stats(m, quant_in(xs[i]), s_.to(dev), t)
                worst.append(dict(step=i, **wst))
                if i == 0 or i == G.n - 1:
                    json.dump(rows, open(os.path.join(a.out, f"overflow_layers_step{i}.json"), "w"), indent=0)
            summ["overflow"] = worst
    for tag, x in res.items():
        sheet(x, ns, S, rnames, os.path.join(a.out, f"{tag}.png"))
        sheet(x, ns, S, rnames, os.path.join(a.out, f"{tag}_q16.png"), levels=16)
    order = [k for k in ("teacher", "float", "qat", "int") if k in res]
    # dot grid / grey cast: where the teacher's picture is pure white (level 15), each model's mean
    # deficit from white in grey levels, per position inside the 4x4 patch (the out layer's sub-pixels)
    lv = {k: ((res[k].clamp(-1, 1) + 1) * 7.5).round()[:, 0] for k in order}
    white = lv["teacher"] == 15
    dots = {}
    for k in order:
        if k == "teacher":
            continue
        d = (15 - lv[k]) * white
        per = torch.stack([d[:, py::4, px::4].sum() / white[:, py::4, px::4].sum().clamp(min=1)
                           for py in range(4) for px in range(4)]).reshape(4, 4)
        dots[k] = dict(mean_deficit=round(float(per.mean()), 3), spread=round(float(per.max() - per.min()), 3),
                       per_pos=[[round(float(v), 2) for v in row] for row in per])
    summ["white_bg_deficit_levels"] = dots
    for i, k1 in enumerate(order):
        for k2 in order[i + 1:]:
            summ[f"mse_{k1}_{k2}"] = float((res[k1].clamp(-1, 1) - res[k2].clamp(-1, 1)).pow(2).mean())
    # side by side: 4 seeds per subject, models next to each other
    rows = []
    U = {k: to_u8(res[k][:, 0], 16) for k in order}
    for r in range(ns):
        row = []
        for j in range(min(S, 4)):
            row += [U[k][r * S + j] for k in order]
        rows.append(row)
    for p0 in range(0, ns, 30):
        nm = "compare_" + "_".join(order) + ("" if ns <= 30 else f"_p{p0 // 30}") + ".png"
        grid_image(rows[p0:p0 + 30], scale=2, labels=rnames[p0:p0 + 30]).save(os.path.join(a.out, nm))

    # nearest neighbours in the training set (L2 over the whole picture, flips included)
    if a.data:
        R = teacher.cfg["img"]
        sp = get_split(a.data, res=R)
        tr = levels_to_x(torch.tensor(sp.images)).to(dev).reshape(len(sp), -1)
        tr = torch.cat([tr, levels_to_x(torch.tensor(sp.images[:, :, ::-1].copy())).to(dev).reshape(len(sp), -1)])
        gen_key = "int" if "int" in res else order[-1]

        def nn(x):
            d = torch.cdist(x.clamp(-1, 1).reshape(x.shape[0], -1), tr)
            v, i = d.min(1)
            return v / R, i                                # RMS distance per pixel, index (>= N: flipped)

        def q(v):
            return dict(p5=round(float(v.quantile(0.05)), 4), median=round(float(v.median()), 4),
                        min=round(float(v.min()), 4))
        dg, ig = nn(res[gen_key])
        summ["nn"] = {f"{k}_to_train_rms": q(nn(res[k])[0]) for k in order}
        pick = torch.randperm(len(sp), device=dev)[:min(len(sp), 1024)]
        # leave-one-out train->train: drop exact self (and its flip) distances
        d_tt = torch.cdist(tr[pick], tr)
        ar = torch.arange(len(pick), device=dev)
        d_tt[ar, pick] = float("inf")
        d_tt[ar, pick + len(sp)] = float("inf")
        summ["nn"]["train_to_train_rms"] = q(d_tt.min(1).values / R)
        rows = []
        A = to_u8(res[gen_key][:, 0], 16)
        ig = ig.cpu().numpy()
        B = [(sp.images[i % len(sp)][:, ::-1] if i >= len(sp) else sp.images[i]).astype(np.uint16) * 17
             for i in ig]
        B = [b.astype(np.uint8) for b in B]
        for r in range(ns):
            row = []
            for j in range(min(S, 4)):
                k = r * S + j
                row += [A[k], B[k]]
            rows.append(row)
        for p0 in range(0, ns, 30):
            nm = f"nn_{gen_key}" + ("" if ns <= 30 else f"_p{p0 // 30}") + ".png"
            grid_image(rows[p0:p0 + 30], scale=2, labels=rnames[p0:p0 + 30]).save(os.path.join(a.out, nm))
    if a.ship and "qat" in stu:
        ship_sheet(a, teacher, stu["qat"][0], present, names, dev, summ)
    json.dump(summ, open(os.path.join(a.out, "summary.json"), "w"), indent=1)
    print(json.dumps({k: v for k, v in summ.items() if k not in ("overflow",)}, indent=1))
    if "overflow" in summ:
        print("overflow margins per step:", json.dumps(summ["overflow"]))


if __name__ == "__main__":
    main()
