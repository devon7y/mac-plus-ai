#!/usr/bin/env python3
"""Make a Doodle model file (model_file.py format) for the Mac.

  export.py random <out> [--H 256] [--wh tern|int4] [--wo tern|int4] [--seed 1]
      random integer model with the right shapes (for testing the app before training ends)
  export.py ckpt <checkpoint.pt> <out> [--names categories.txt]
      a trained paint/train checkpoint (quant 'tern', 'int4' or 'tern_o4') -> integers,
      following FORMAT.md ("Exporting a trained model")

Category names default to paint/app/data/categories.txt (the order prep_data.py uses).
"""
import argparse
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import model_file  # noqa: E402
from model_file import INT4, TERN  # noqa: E402

NAMES = os.path.join(HERE, "..", "app", "data", "categories.txt")
NOUT, NEMB = 123, 32


def names_from(path):
    return [l.strip() for l in open(path) if l.strip()]


def pick_A(codes):
    """Largest A <= 127 with sum_j |w_ij| * A <= 32767 for every row (exact 16-bit sums)."""
    return int(min(127, 32767 // max(1, int(np.abs(codes).sum(axis=1).max()))))


def largest_shift(vals_fn, hi=20):
    """Largest shift s <= hi with every |vals_fn(s)| <= 32767."""
    for s in range(hi, -1, -1):
        if np.abs(vals_fn(s)).max() <= 32767:
            return s
    raise ValueError("no shift fits")


def rnd(x):
    return np.round(np.asarray(x, np.float64)).astype(np.int64)


def check16(name, a):
    a = np.asarray(a)
    if a.min() < -32768 or a.max() > 32767:
        raise ValueError(f"{name} out of int16 range: {a.min()}..{a.max()}")
    return a


# ------------------------------------------------------------------ random model
def random_model(H, kind_wh, kind_wo, seed, names, r2=False, eq_a=False):
    r = np.random.default_rng(seed)
    R = 3 * H

    def wts(rows, kind):
        if kind == TERN:
            return r.choice([-1, 0, 1], size=(rows, H), p=[0.3, 0.4, 0.3])
        return np.clip(np.round(r.normal(0, 2.2, size=(rows, H))), -8, 7).astype(np.int64)

    U, WY = wts(R, kind_wh), wts(NOUT, kind_wo)
    A, AO = pick_A(U), pick_A(WY)
    m = {"H": H, "kind_wh": kind_wh, "kind_wo": kind_wo, "A": A, "AO": AO, "SHR": 12, "SHO": 12,
         "SHA": 3, "SHB": 12, "FQ": 1600, "names": names, "U": U, "WY": WY}
    m["M"] = r.integers(1500, 4000, 2 * H)
    m["MN"] = np.stack([r.integers(1500, 4000, H), r.integers(-800, 800, H)], axis=1)
    m["MY"] = r.integers(800, 3000, NOUT)
    m["BY"] = r.integers(-1500, 1500, NOUT)
    m["BY"][60:100] = r.integers(-2500, -1200, 40)      # log sigma around exp(-1.8): moves of a few px
    m["BY"][20:60] = r.integers(-600, 600, 40)          # mixture means within +-0.6 (+-15 px)
    m["BY"][120:123] = (4000, 1500, -9000)              # pen: mostly down, sometimes lifted, rarely ends
    if r2:                                              # residual term: same kind, smaller multipliers
        m["WY2"] = wts(NOUT, kind_wo)
        m["MY2"] = r.integers(-1500, 1500, NOUT)
        m["SHO2"] = 13
        m["AO"] = min(m["AO"], pick_A(m["WY2"]))
        if eq_a:                                        # like a trained model: one hq for everything
            m["A"] = m["AO"] = min(m["A"], m["AO"])
    m["B0"] = r.integers(-1000, 1000, 4 * H)
    m["MC"] = r.integers(500, 3000, 4 * H)
    m["WC"] = r.integers(-8, 8, (4 * H, NEMB))
    m["EMB"] = r.integers(-3000, 3000, (len(names), NEMB))
    m["P"] = r.integers(-1000, 1000, (3, R))
    m["WDX"] = r.integers(-2000, 2000, R)
    m["WDY"] = r.integers(-2000, 2000, R)
    return m


# ------------------------------------------------------------------ trained checkpoint
def codes(w, kind):
    if kind == TERN:
        s = np.maximum(np.abs(w).mean(axis=1), 1e-8)
        c = np.clip(np.round(w / s[:, None]), -1, 1)
    else:
        s = np.maximum(np.abs(w).max(axis=1), 1e-8) / 7
        c = np.clip(np.round(w / s[:, None]), -8, 7)
    return c.astype(np.int64), s


def fq_int(w, bits):
    """The training's fake quantization (symmetric absmax per row) as floats."""
    qmax = 2 ** (bits - 1) - 1
    s = np.maximum(np.abs(w).max(axis=1), 1e-8) / qmax
    return np.clip(np.round(w / s[:, None]), -qmax - 1, qmax) * s[:, None]


def ckpt_model(path, names, force_a=None):
    import torch
    ck = torch.load(path, map_location="cpu", weights_only=False)
    sd = {k: v.detach().double().numpy() for k, v in ck["model"].items()}
    args = ck["args"]
    return state_model(sd, int(args["H"]), args["quant"], float(ck["scale"]), names, force_a)


def train_codes(w, kind):
    """Codes and scales exactly as the training exports them (paint/train/sketch_model.export_codes,
    float32: half-way cases round to even there, which float64 can miss)."""
    sys.dont_write_bytecode = True
    sys.path.insert(0, os.path.join(HERE, "..", "train"))
    import torch
    from sketch_model import export_codes
    c, s_ = export_codes(torch.from_numpy(np.asarray(w)).float(), kind)
    return np.asarray(c).astype(np.int64), np.asarray(s_).astype(np.float64)


def state_model(sd, H, quant, S, names, force_a=None):
    """Float parameters (numpy, sketch_model.SketchDecoder names) -> integer model dict."""
    if quant not in ("tern", "int4", "tern_o4", "tern_r2o"):
        raise SystemExit(f"quant '{quant}' has no integer form (train with tern, int4, tern_o4 or tern_r2o)")
    kind_wh = TERN if quant in ("tern", "tern_o4", "tern_r2o") else INT4
    kind_wo = INT4 if quant in ("int4", "tern_o4") else TERN
    R = 3 * H
    kname = {TERN: "tern", INT4: "int4"}
    U, s_h = train_codes(sd["wh"], kname[kind_wh])
    WY2 = None
    if quant == "tern_r2o":
        # two-term output s1*T1 + s2*T2: codes exactly as paint/train/sketch_model.export_codes makes them
        sys.dont_write_bytecode = True
        sys.path.insert(0, os.path.join(HERE, "..", "train"))
        import torch
        from sketch_model import export_codes
        c, sc = export_codes(torch.from_numpy(sd["wo"]).float(), "tern_r2")
        WY, WY2 = c[0].astype(np.int64), c[1].astype(np.int64)
        s_o, s_o2 = sc[0].astype(np.float64), sc[1].astype(np.float64)
    else:
        WY, s_o = train_codes(sd["wo"], kname[kind_wo])
    A, AO = pick_A(U), pick_A(WY if WY2 is None else np.concatenate([WY, WY2]))
    if force_a:          # e.g. 4-bit at A = 127: exactness then relies on the measured sums, not the bound
        A = AO = force_a
    b_ih, b_hh, bo, b0 = sd["b_ih"], sd["b_hh"], sd["bo"], sd["b0"]
    # floor(h*A) is h*A - 0.5 on average: put the expected error back into each row's bias
    comp_h = 1024 * 0.5 * s_h * U.sum(axis=1) / A
    comp_o = 1024 * 0.5 * s_o * WY.sum(axis=1) / AO
    if WY2 is not None:
        comp_o += 1024 * 0.5 * s_o2 * WY2.sum(axis=1) / AO
    m = {"H": H, "kind_wh": kind_wh, "kind_wo": kind_wo, "A": A, "AO": AO, "names": names,
         "U": U, "WY": WY, "FQ": int(round(64 * S))}
    mh = lambda sh: rnd(s_h * 2.0 ** (10 + sh) / A)
    m["SHR"] = largest_shift(mh, 24)
    Mall = mh(m["SHR"])
    m["M"] = Mall[:2 * H]
    m["MN"] = np.stack([Mall[2 * H:], check16("bhn", rnd(1024 * b_hh[2 * H:] + comp_h[2 * H:]))], axis=1)
    mo = lambda sh: rnd(s_o * 2.0 ** (10 + sh) / AO)
    m["SHO"] = largest_shift(mo, 24)
    m["MY"] = mo(m["SHO"])
    if WY2 is not None:
        mo2 = lambda sh: rnd(s_o2 * 2.0 ** (10 + sh) / AO)
        m["SHO2"] = largest_shift(mo2, 24)
        m["MY2"], m["WY2"] = mo2(m["SHO2"]), WY2
    m["BY"] = check16("BY", rnd(1024 * bo + comp_o))
    # fold: Wc (3H x 32) and W0 (H x 32) as int4 codes, embedding int8-fake-quantized -> int16
    WC, s_c = train_codes(np.concatenate([sd["wc"], sd["w0"]]), "int4")
    ce, se = train_codes(sd["emb"], "int8")
    emb = ce * se[:, None]
    KE = 2.0 ** int(math.floor(math.log2(32767 / np.abs(emb).max())))
    EMB = rnd(emb * KE)
    acc = WC @ EMB.T
    m["SHA"] = next(s for s in range(0, 24) if np.abs(acc >> s).max() <= 32767)
    mc = lambda sh: rnd(s_c * 1024 * 2.0 ** (m["SHA"] + sh) / KE)
    m["SHB"] = largest_shift(mc, 24)
    m["MC"], m["WC"], m["EMB"] = mc(m["SHB"]), WC, EMB
    cx_, sx_ = train_codes(sd["wx"], "int8")
    wx = cx_ * sx_[:, None]
    # exactly one pen column is active per step: move each row's midpoint of its 3 pen weights
    # into the row's bias, halving the range the pen words need (exact)
    pen = wx[:, 2:5]
    mid = (pen.max(axis=1) + pen.min(axis=1)) / 2
    b0f = np.concatenate([b_ih[:2 * H] + b_hh[:2 * H] + mid[:2 * H], b_ih[2 * H:] + mid[2 * H:], b0])
    comp_f = np.concatenate([comp_h[:2 * H], np.zeros(2 * H)])
    m["B0"] = check16("B0", rnd(1024 * b0f + comp_f))
    # cx += (WDX*dx + WDY*dy) >> SHX, dx in pixels: WDX = Wx/S * 1024 * 2^SHX (largest SHX that fits)
    wxy = lambda sh: rnd(wx[:, 0:2] * 1024 * 2.0 ** sh / S)
    m["SHX"] = largest_shift(wxy, 12)
    m["WDX"], m["WDY"] = wxy(m["SHX"])[:, 0], wxy(m["SHX"])[:, 1]
    m["P"] = check16("P", rnd(1024 * (pen - mid[:, None]).T))
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=("random", "ckpt"))
    ap.add_argument("args", nargs="+")
    ap.add_argument("--H", type=int, default=256)
    ap.add_argument("--wh", default="tern")
    ap.add_argument("--wo", default="tern")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--r2", action="store_true", help="random: two-term output layer (tern_r2o)")
    ap.add_argument("--same-a", action="store_true", help="random: A = AO (no separate output quantization)")
    ap.add_argument("--A", type=int, default=0, help="ckpt: force A = AO (default: the exact-sum bound)")
    ap.add_argument("--names", default=NAMES)
    a = ap.parse_args()
    names = names_from(a.names)
    kinds = {"tern": TERN, "int4": INT4}
    if a.what == "random":
        out = a.args[0]
        m = random_model(a.H, kinds[a.wh], kinds[a.wo], a.seed, names, a.r2, a.same_a)
    else:
        out = a.args[1]
        m = ckpt_model(a.args[0], names, a.A or None)
    n = model_file.write(out, m)
    print(f"{out}: {n:,} bytes; H={m['H']} wh={m['kind_wh']} wo={m['kind_wo']} A={m['A']} AO={m['AO']} "
          f"SHR={m['SHR']} SHO={m['SHO']} SHA={m['SHA']} SHB={m['SHB']} SHX={m.get('SHX', 6)} FQ={m['FQ']}"
          + (f" two-term output, SHO2={m['SHO2']}" if m.get("WY2") is not None else ""))


if __name__ == "__main__":
    main()
