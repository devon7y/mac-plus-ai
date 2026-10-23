#!/usr/bin/env python3
"""Where does the quantisation gap come from? Starting from a FLOAT student checkpoint (no training),
switch the Mac's quantisers on one at a time and measure how far the 6-step pictures move from the
float student's pictures (same noise, the Mac's integer sampler update for all variants).

  tern   ternary weights (per-row scale folded into the gain, as to_qat does), on chosen layers
  pow2   gains rounded to powers of two (2^-round(lam), lam clamped to the Mac's range)
  int    integer input, floor after the shift, integer biases (b0, the integer subject formula,
         rounded time bias), i.e. the activation/bias side of the Mac arithmetic

  python ablate.py <float.pt> [--qat <finetuned qat.pt>] --out <dir>
"""
import argparse
import copy
import json
import math
import os

import torch
import torch.nn.functional as F

from student import LAM_MAX, LAM_MIN, DreamStudent, scatter_d2s, tab_range, ternarize  # adds engine/
import arch  # noqa: E402
from common import grid_image, to_u8  # noqa: E402
from distill import Grid, eval_noise, student_sample  # noqa: E402
from student_tools import load_student  # noqa: E402
from student_x import StudentX  # noqa: E402


class AblStudent(DreamStudent):
    """DreamStudent with independently switchable quantisers (eval only).
    tern: layer indices with ternary weights; q4: layer indices with 4-bit weights (-7..7, row scale
    max|w|/7); gains: 'float' | 'pow2' | 'g2' (nearest two-term value, student_x.G2_CANDS); intact: integer
    input/biases and floor after each shift term. With 'in' in q4 the input layer reads x >> 3 (Q6,
    clamp +-255) and its gain is multiplied by 8, as in the lead's planned spec."""

    def setup(self, tern=(), q4=(), gains="float", intact=False, pow2=False):
        self.tern, self.q4 = set(tern), set(q4)
        self.gmode = "pow2" if pow2 else gains
        self.intact = intact
        if intact:
            self.freeze_subject_quant()

    def abl_bias(self, s, t):
        if not self.intact:
            return self.b0[None] + self.E[s] @ self.U + self.time_bias(t)
        old = self.mode
        self.mode = "int"
        sb = self.subject_bias(s).float()
        self.mode = old
        return torch.round(self.b0)[None] + sb + torch.round(self.time_bias(t))

    def gain_apply(self, acc, lam):
        if self.gmode == "float":
            v = acc * torch.pow(2.0, -lam)[None, :, None, None]
            return torch.floor(v) if self.intact else v
        if self.gmode == "pow2":
            v = acc * torch.pow(2.0, -torch.round(lam.clamp(LAM_MIN, LAM_MAX)))[None, :, None, None]
            return torch.floor(v) if self.intact else v
        k1, d, sg, f = StudentX.g2_choice(None, lam)
        g1 = torch.pow(2.0, -k1)[None, :, None, None]
        g2 = torch.pow(2.0, -(k1 + d))[None, :, None, None] * (d > 0).float()[None, :, None, None]
        sg = sg[None, :, None, None]
        if self.intact:
            return torch.floor(acc * g1) + sg * torch.floor(acc * g2)
        return acc * (g1 + sg * g2)

    @torch.no_grad()
    def forward(self, xt_q9, s, t):
        p = arch.CONFIGS[self.cfg]["patch"]
        x = F.pixel_unshuffle(xt_q9.clamp(-arch.IN_MAX, arch.IN_MAX), p)
        if self.intact:
            x = torch.round(x)
        tens = {"xt": x}
        bias = self.abl_bias(s, t)
        for i, l in enumerate(self.L):
            w, lam = self.W[i], self.lam[i]
            src = tens[l["src"]]
            if i in self.q4:
                sc = w.reshape(l["cout"], -1).abs().max(1).values.clamp(min=1e-8) / 7
                w = torch.round(w / sc.reshape(-1, *([1] * (w.dim() - 1)))).clamp(-7, 7)
                lam = lam - torch.log2(sc)
                if l["tag"] == "in":
                    src = torch.floor(src / 8).clamp(-255, 255) if self.intact else src / 8
                    lam = lam - 3
            elif i in self.tern and not (l["kind"] == "dw" and l["center"]):
                w, alpha = ternarize(w, self.theta)
                w = torch.round(w)
                lam = lam - torch.log2(alpha.clamp(min=1e-8))
            acc = self.conv(i, src, w)
            v = self.gain_apply(acc, lam)
            a, b = self.offs[i]
            v = v + bias[:, a:b, None, None]
            if l["kind"] == "pw" and l["scatter"] == "d2s":
                v = scatter_d2s(v, l["cout"] // 4)
            if l["mode"] == "addh":
                v = tens[l["dst"]] + v
            r = tab_range(l["tab"])
            tens[l["dst"]] = v if r is None else v.clamp(r[0], r[1])
        self.last = tens
        return F.pixel_shuffle(tens["out"], p)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("float_ckpt")
    ap.add_argument("--qat", help="the fine-tuned qat checkpoint, for reference")
    ap.add_argument("--out", required=True)
    ap.add_argument("--seeds", type=int, default=6)
    ap.add_argument("--subj", type=int, default=20)
    ap.add_argument("--sheet_subj", type=int, default=10)
    ap.add_argument("--only", help="comma list of variant names to put on the sheet")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    m0, ck = load_student(a.float_ckpt, "cpu")
    assert m0.mode == "float"
    G = Grid(len(ck["ts"]) - 1, ck["grid"]["kind"])
    names = ck["names"]
    pres = ck["present"][:a.subj]
    S = [s for s in pres for _ in range(a.seeds)]
    seeds = [1 + j for _ in pres for j in range(a.seeds)]
    z = eval_noise(seeds)
    s = torch.tensor(S)
    L = m0.L
    io = [i for i, l in enumerate(L) if l["tag"] in ("in", "out")]
    i_in = [i for i, l in enumerate(L) if l["tag"] == "in"]
    i_out = [i for i, l in enumerate(L) if l["tag"] == "out"]
    allL = list(range(len(L)))
    rest = [i for i in allL if i not in io]
    variants = [
        ("float", dict()),
        ("a_tern", dict(tern=allL)),
        ("b_pow2", dict(gains="pow2")),
        ("b2_g2", dict(gains="g2")),
        ("c_int", dict(intact=True)),
        ("d_tern_io", dict(tern=io)),
        ("d1_tern_in", dict(tern=i_in)),
        ("d2_tern_out", dict(tern=i_out)),
        ("e_tern_not_io", dict(tern=rest)),
        ("all_noft", dict(tern=allL, gains="pow2", intact=True)),
        ("q4io_pow2_noft", dict(tern=rest, q4=io, gains="pow2", intact=True)),
        ("tern_g2_noft", dict(tern=allL, gains="g2", intact=True)),
        ("q4io_g2_noft", dict(tern=rest, q4=io, gains="g2", intact=True)),
    ]
    res, out = {}, {}
    for name, kw in variants:
        m = AblStudent(m0.cfg, n_subjects=m0.n_subjects, emb_dim=m0.emb_dim)
        m.load_state_dict(m0.state_dict())
        m.theta = m0.theta
        m.setup(**kw)
        m.eval()
        xs, _ = student_sample(m, z, s, G)
        res[name] = xs[-1]
    # sanity: the plain float variant equals the student's own float forward; all_noft equals to_qat()
    xs, _ = student_sample(m0, z, s, G)
    out["check_float_equals_student"] = float((xs[-1] - res["float"]).abs().max())
    mq = copy.deepcopy(m0)
    mq.to_qat()
    xq, _ = student_sample(mq, z, s, G)
    out["check_allnoft_equals_to_qat"] = float((xq[-1] - res["all_noft"]).abs().max())
    if a.qat:
        mf, _ = load_student(a.qat, "cpu")
        mf.eval()
        res["qat_finetuned"], _ = student_sample(mf, z, s, G)[0][-1], None
    ref = res["float"].clamp(-1, 1)
    for k, x in res.items():
        out[f"mse_vs_float_{k}"] = round(float((x.clamp(-1, 1) - ref).pow(2).mean()), 5)
    json.dump(out, open(os.path.join(a.out, "ablation.json"), "w"), indent=1)
    print(json.dumps(out, indent=1))
    order = a.only.split(",") if a.only else list(res)
    U = {k: to_u8(res[k][:, 0], 16) for k in order}
    rows, labels = [], []
    for r, sj in enumerate(pres[:a.sheet_subj]):
        for j in range(2):
            k = r * a.seeds + j
            rows.append([U[o][k] for o in order])
            labels.append(f"{names[sj]} s{j + 1}")
    grid_image(rows, scale=2, labels=labels).save(os.path.join(a.out, "ablation_" + "_".join(order) + ".png"))
    print("columns:", order)


if __name__ == "__main__":
    main()
