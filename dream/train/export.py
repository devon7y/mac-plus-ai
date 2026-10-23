#!/usr/bin/env python3
"""Export a trained DreamStudent (qat) to a Mac model file, and sample exactly like the Mac.

    export.py <student.pt> <out.drm> --subjects subjects.txt [--steps-t 1,0.83,...,0] [--check N]

The checkpoint is a dict with "state_dict" (DreamStudent), "cfg", "n_subjects", "emb_dim" and
"ts" (the N+1 time grid t_0 = 1 > ... > t_N = 0 used in distillation), or pass --steps-t.

mac_sample(student, subjects, seeds, ts): the Mac's integer picture loop (FORMAT.md) with the
student in "int" mode, starting from the Mac's own noise (picture.noise). Use it for the
"integer model" sample sheets; it is bit-exact with the exported file run by picture.py / the Mac
(--check N verifies that on N random subject/seed pairs).
"""
import argparse
import os
import sys

import numpy as np
import torch
import torch.nn.functional as F

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "engine"))
import arch  # noqa: E402
import model_file  # noqa: E402
import picture  # noqa: E402
from student import DreamStudent  # noqa: E402


def mac_noise(seed, cfg):
    c = arch.CONFIGS[cfg]
    G, P = c["img"] // c["patch"], c["patch"]
    n = picture.noise(G * G * P * P, seed).reshape(G, G, P * P)
    x = torch.tensor(n).permute(2, 0, 1)[None].float()            # (1, P*P, G, G) storage -> NCHW
    return F.pixel_shuffle(x, P)                                   # (1, 1, img, img) Q9


def mac_noise_state(seed, cfg):
    c = arch.CONFIGS[cfg]
    G, P = c["img"] // c["patch"], c["patch"]
    return picture.noise(G * G * P * P, seed, return_state=True)[1]


def mac_noise_from_state(state, cfg):
    """(Q9 picture (1,1,img,img), next state) from a noise-stream state, in the Mac's order."""
    c = arch.CONFIGS[cfg]
    G, P = c["img"] // c["patch"], c["patch"]
    n, st = picture.noise(G * G * P * P, state, return_state=True)
    x = torch.tensor(n.reshape(G, G, P * P)).permute(2, 0, 1)[None].float()
    return F.pixel_shuffle(x, P), st


@torch.no_grad()
def mac_sample(m, subjects, seeds, ts, return_steps=False, c=1.0):
    """Integer sampling as on the Mac. Returns x0 (B, 1, img, img) in Q9 (and the per-step x0s).
    c < 1: the re-noising sampler (FORMAT.md), fresh noise continuing each seed's stream."""
    old = m.mode
    m.mode = "int"
    consts = model_file.sampler_consts(ts, c)
    x = torch.cat([mac_noise(s, m.cfg) for s in seeds]).long()
    states = [mac_noise_state(s, m.cfg) for s in seeds]
    s = torch.as_tensor(subjects, dtype=torch.long)
    steps = []
    params = m.int_params()
    for i, sp in enumerate(consts):
        c1, c2 = sp[0], sp[1]
        c3 = sp[3] if len(sp) > 3 else 0
        t = torch.full((len(seeds),), float(ts[i]))
        bias = m.bias(s, t)
        x0 = m.forward_int(x.float(), s, t, bias=bias, params=params).long().clamp(-512, 512)
        if c3:
            zs = []
            for j in range(len(seeds)):
                z, states[j] = mac_noise_from_state(states[j], m.cfg)
                zs.append(z)
            z = torch.cat(zs).long()
            x = ((c1 * x + c2 * x0 + c3 * z + 8192) >> 14).clamp(-2047, 2047)
        else:
            x = ((c1 * x + c2 * x0 + 8192) >> 14).clamp(-2047, 2047)
        steps.append(x0)
    m.mode = old
    return (steps[-1], steps) if return_steps else steps[-1]


@torch.no_grad()
def export(m, names, ts, path, drop=(), c=1.0):
    """Write the student (already converted with to_qat) to a model file. drop: subject names left
    out of the app (their rows of E and their names are removed; the rest keep their order)."""
    assert m.mode in ("qat", "int")
    T, L = arch.build(m.cfg)
    old = m.mode
    m.mode = "int"
    params = m.int_params()
    e_int, u_int, mc = m.subject_quant()
    N = len(ts) - 1
    BT = torch.round(m.time_bias(torch.tensor(ts[:N], dtype=torch.float32))).long().numpy()
    B0 = torch.round(m.b0).long().numpy()
    m.mode = old
    ws, ks = [], []
    for (w, k), l in zip(params, L):
        ws.append(w.numpy().astype(np.int8))
        ks.append(k.numpy())
        assert w.abs().max() <= (7 if l.get("wq") == "q4" else 1), "weights out of range"
        k1 = k[:, 0] if k.dim() == 2 else k
        assert k1.min() >= -8 and k1.max() <= 15
    keep = [i for i, n in enumerate(names) if n not in set(drop)]
    e_int = e_int[keep]
    names = [names[i] for i in keep]
    c_ = arch.CONFIGS[m.cfg]
    M = dict(T={n: T[n] for n in ["xt", "out"] + [n for n in T if n not in ("xt", "out")]}, L=L, w=ws, k=ks,
             B0=B0, E=e_int.round().long().numpy(), U=u_int.round().long().numpy(), Mc=mc.long().numpy(),
             BT=BT, samp=model_file.sampler_consts(ts, c), names=list(names), img=c_["img"], patch=c_["patch"],
             in_shift=m.in_shift)
    for name, a, lim in (("B0", B0, 16384), ("BT", BT, 16384), ("Mc", M["Mc"], 32768)):
        assert np.abs(a).max() < lim, f"{name} out of range: {np.abs(a).max()}"     # Mc: muls.w operand
    assert np.abs(M["E"]).max() <= 127 and np.abs(M["U"]).max() <= 127
    return model_file.write(path, M)


def check(m, path, ts, n=4, seed=0, names=None, drop=(), c=1.0):
    """picture.py on the exported file == mac_sample with the student, for n random pairs."""
    R = model_file.read(path)
    rs = np.random.RandomState(seed)
    full = list(names) if names is not None else None
    bad = 0
    for _ in range(n):
        s, sd = int(rs.randint(len(R["names"]))), int(rs.randint(1, 1 << 31))
        _, final, _ = picture.generate(R, s, sd)
        ms = full.index(R["names"][s]) if full is not None else s       # the student's own index
        x0 = mac_sample(m, [ms], [sd], ts, c=c)
        P = R["patch"]
        mine = F.pixel_unshuffle(x0.float(), P)[0].permute(1, 2, 0).long().numpy()
        ok = np.array_equal(mine, final)
        bad += not ok
        print(f"  subject {s} seed {sd}: {'BIT-EXACT' if ok else 'DIFFERS'}")
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ckpt")
    ap.add_argument("out")
    ap.add_argument("--subjects", required=True)
    ap.add_argument("--steps-t", default=None)
    ap.add_argument("--check", type=int, default=4)
    ap.add_argument("--drop", default="steak", help="comma-separated subjects left out of the app")
    ap.add_argument("--renoise", type=float, default=1.0,
                    help="sampler c: 1 = deterministic DDIM (default), 0 = full re-noising each step")
    a = ap.parse_args()
    ck = torch.load(a.ckpt, map_location="cpu")
    m = DreamStudent(ck.get("cfg", "A64L2"), n_subjects=ck["n_subjects"], emb_dim=ck.get("emb_dim", 32))
    m.load_state_dict(ck["state_dict"])
    m.mode = "qat"
    ts = [float(t) for t in a.steps_t.split(",")] if a.steps_t else list(ck["ts"])
    names = [l.strip() for l in open(a.subjects) if l.strip()][:m.n_subjects]
    drop = [d.strip() for d in a.drop.split(",") if d.strip()]
    size = export(m, names, ts, a.out, drop, a.renoise)
    print(f"wrote {a.out}: {size:,} bytes, {len(ts) - 1} steps, {len(names) - len(set(drop) & set(names))} subjects"
          f" (dropped: {', '.join(sorted(set(drop) & set(names))) or 'none'})")
    if a.check:
        bad = check(m, a.out, ts, n=a.check, names=names, drop=drop, c=a.renoise)
        print("EXPORT CHECK: " + ("ALL BIT-EXACT" if not bad else f"{bad} FAILURES"))
        sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
