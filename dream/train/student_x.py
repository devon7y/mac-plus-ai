#!/usr/bin/env python3
"""Prototype extensions of DreamStudent (student.py stays untouched; the lead ports what works):

  q4      4-bit weights (-7..7) on chosen layers (e.g. the 32->16 output layer). The per-row scale is
          frozen at conversion (q4s) and folded into the gain, as to_qat folds the ternary scale.
          int16 safety: out layer sums 32 inputs of |.| <= 127 times |w| <= 7 = 28,448 < 32,768.
  gain2   two-term gains, v = (acc >> k1) + sgn (acc >> k2), k2 in k1+1..k1+3: per-channel scales
          {1, 1 +- 1/8, 1 +- 1/4, 1 +- 1/2} x 2^-k1 instead of powers of two. Each term is floored
          separately, exactly as two shifts and an add on the Mac. In float mode nothing changes.

The int-mode forward (exact Mac arithmetic) is reimplemented here for both options, so qat == int can be
tested (test at the bottom: python3.11 student_x.py).
"""
import math

import torch
import torch.nn.functional as F

from student import (LAM_MAX, LAM_MIN, DreamStudent, gather_s2d, scatter_d2s, shift16, tab_range,  # adds engine/
                     ternarize, wrap16)
import arch  # noqa: E402

# candidate (factor relative to 2^-k1, k2 - k1, sign); factor 1 means a single shift
G2_CANDS = [(1.0, 0, 0)] + [(1 + sg * 2.0 ** -d, d, sg) for d in (1, 2, 3) for sg in (1, -1)]


def ste(x, y):
    return x + (y - x).detach()


class StudentX(DreamStudent):
    def __init__(self, *args, q4_tags=(), gain2=False, **kw):
        super().__init__(*args, **kw)
        self.q4_tags = set(q4_tags)
        self.gain2 = gain2
        self.register_buffer("q4s", torch.ones(self.ctot))

    def is_q4(self, i):
        return self.L[i]["tag"] in self.q4_tags

    # ---------------------------------------------------------------- weights
    def layer_weights(self, i):
        l, w = self.L[i], self.W[i]
        if self.mode == "float" or (l["kind"] == "dw" and l["center"]) or not self.is_q4(i):
            return super().layer_weights(i)
        a, b = self.offs[i]
        s = self.q4s[a:b].reshape(-1, *([1] * (w.dim() - 1)))
        u = w / s
        return ste(u, torch.round(u).clamp(-7, 7))

    # ---------------------------------------------------------------- gains
    def g2_choice(self, lam):
        """lam -> (k1, k2, sgn, factor) per channel: the candidate gain nearest to 2^-lam (log space)."""
        lamc = lam.clamp(LAM_MIN, LAM_MAX)
        best = None
        for f, d, sg in G2_CANDS:
            k1 = torch.round(lamc + math.log2(f)).clamp(LAM_MIN, LAM_MAX - d)
            err = (lamc - (k1 - math.log2(f))).abs()
            cand = (k1, torch.full_like(k1, d), torch.full_like(k1, sg), torch.full_like(k1, f), err)
            if best is None:
                best = cand
            else:
                take = cand[4] < best[4]
                best = tuple(torch.where(take, c, bb) for c, bb in zip(cand, best))
        return best[:4]

    def apply_gain(self, i, acc):
        """qat: floor(acc 2^-k1) + sgn floor(acc 2^-k2), with a straight-through gradient of 2^-lam."""
        lam = self.lam[i]
        if not self.gain2:
            return None
        k1, d, sg, f = self.g2_choice(lam.detach())
        g1 = torch.pow(2.0, -k1)[None, :, None, None]
        g2 = torch.pow(2.0, -(k1 + d))[None, :, None, None]
        exact = torch.floor(acc * g1) + sg[None, :, None, None] * torch.where(
            d[None, :, None, None] > 0, torch.floor(acc * g2), torch.zeros_like(acc))
        smooth = acc * torch.pow(2.0, -lam.clamp(LAM_MIN, LAM_MAX))[None, :, None, None]
        return ste(smooth, exact)

    # ---------------------------------------------------------------- qat forward (copy of student.py's
    # forward with the gain step made pluggable)
    def forward(self, xt_q9, s, t):
        if self.mode == "int":
            return self.forward_int(xt_q9, s, t)
        if self.mode == "float" or not self.gain2:
            return super().forward(xt_q9, s, t)
        p = arch.CONFIGS[self.cfg]["patch"]
        x = ste(F.pixel_unshuffle(xt_q9.clamp(-arch.IN_MAX, arch.IN_MAX), p),
                torch.round(F.pixel_unshuffle(xt_q9.clamp(-arch.IN_MAX, arch.IN_MAX), p)))
        tens = {"xt": x}
        bias = self.bias(s, t)
        for i, l in enumerate(self.L):
            acc = self.conv(i, tens[l["src"]], self.layer_weights(i))
            v = self.apply_gain(i, acc)
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

    @torch.no_grad()
    def forward_int(self, xt_q9, s, t, bias=None, params=None):
        if not self.gain2:
            return super().forward_int(xt_q9, s, t, bias=bias, params=params)
        p = arch.CONFIGS[self.cfg]["patch"]
        x = F.pixel_unshuffle(xt_q9.round().clamp(-arch.IN_MAX, arch.IN_MAX), p).long()
        params = params or self.int_params()
        if bias is None:
            bias = self.bias(s, t)
        tens = {"xt": x}
        for i, l in enumerate(self.L):
            w, _ = params[i]
            k1, d, sg, _ = self.g2_choice(self.lam[i].detach())
            k1, k2, sg = k1.long(), (k1 + d).long(), sg.long()
            src = tens[l["src"]]
            if l["kind"] == "pw":
                xs = gather_s2d(src) if l["gather"] == "s2d" else src
                acc = torch.einsum("bihw,oi->bohw", xs, w)
            elif l["center"]:
                acc = src * w[:, 0][None, :, None, None]
            else:
                padded = F.pad(src, (1, 1, 1, 1))
                H, W_ = src.shape[2], src.shape[3]
                acc = torch.zeros_like(src)
                for tap in range(9):
                    dy, dx = tap // 3 - 1, tap % 3 - 1
                    acc = acc + padded[:, :, 1 + dy:1 + dy + H, 1 + dx:1 + dx + W_] * w[:, tap][None, :, None, None]
            acc = wrap16(acc)
            v1 = shift16(acc, k1[None, :, None, None])
            v2 = torch.where(d[None, :, None, None] > 0, shift16(acc, k2[None, :, None, None]), torch.zeros_like(acc))
            a, b = self.offs[i]
            v = wrap16(wrap16(v1 + sg[None, :, None, None] * v2) + bias[:, a:b, None, None])
            if l["kind"] == "pw" and l["scatter"] == "d2s":
                v = scatter_d2s(v, l["cout"] // 4)
            if l["mode"] == "addh":
                v = wrap16(v + tens[l["dst"]])
            r = tab_range(l["tab"])
            if r is not None:
                e = ((v + 16384) & 32767) - 16384
                v = e.clamp(r[0], r[1])
            tens[l["dst"]] = v
        self.last = tens
        return F.pixel_shuffle(tens["out"], p)

    # ---------------------------------------------------------------- conversion
    @torch.no_grad()
    def to_qat(self):
        """As student.to_qat, plus: q4 layers get a frozen per-row scale (max |w| / 7) folded into the
        gain instead of the ternary scale."""
        for i, l in enumerate(self.L):
            if l["kind"] == "dw" and l["center"]:
                continue
            if self.is_q4(i):
                a, b = self.offs[i]
                sc = self.W[i].reshape(l["cout"], -1).abs().max(1).values.clamp(min=1e-8) / 7
                self.q4s[a:b] = sc
                self.lam[i].sub_(torch.log2(sc))
            else:
                _, alpha = ternarize(self.W[i], self.theta)
                self.lam[i].sub_(torch.log2(alpha.clamp(min=1e-8)))
        self.freeze_subject_quant()
        self.mode = "qat"


def from_float(path, device="cpu", **kw):
    """A StudentX initialised from a float DreamStudent checkpoint."""
    d = torch.load(path, map_location=device)
    m = StudentX(d["cfg"], n_subjects=d["n_subjects"], emb_dim=d["emb_dim"], **kw).to(device)
    sd = dict(d["state_dict"])
    sd.setdefault("q4s", m.q4s)
    m.load_state_dict(sd)
    m.mode = d["mode"]
    return m, d


if __name__ == "__main__":
    # qat == int for both options on a calibrated random model; gradients flow
    torch.manual_seed(0)
    for kw in (dict(q4_tags={"out"}), dict(gain2=True), dict(q4_tags={"out", "in"}, gain2=True)):
        m = StudentX("A64L2", n_subjects=8, **kw)
        B = 24
        img = torch.randn(B, 1, 64, 64) * 512
        s = torch.arange(B) % 8
        t = torch.rand(B)
        with torch.no_grad():
            m.U.normal_(0, 3)
            m.tmlp[2].weight.normal_(0, 0.5)
        m.calibrate(img, s, t)
        m.to_qat()
        out_q = m(img, s, t)
        m.mode = "int"
        out_i = m(img, s, t)
        m.mode = "qat"
        loss = (m(img, s, t) / 512).pow(2).mean()
        loss.backward()
        gn = sum(p.grad.abs().sum().item() for p in m.parameters() if p.grad is not None)
        w = [m.layer_weights(i).detach() for i in range(len(m.L)) if m.is_q4(i)]
        print(kw, "qat vs int maxdiff", float((out_q - out_i.float()).abs().max()),
              "| q4 code range", [(int(x.min()), int(x.max())) for x in w], "| grad L1", round(gn, 2))
