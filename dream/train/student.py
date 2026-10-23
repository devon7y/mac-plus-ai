#!/usr/bin/env python3
"""Dream's student network in PyTorch, the trainable twin of the Mac engine
(dream/engine/arch.py layer program + dream/engine/ref.py integer arithmetic).

Everything is expressed in the Mac's integer units: x_t enters in Q9 (value * 512, clamp +-2047),
activations are table-clamped integers (relu: 0..m, lin: -m..m, residual stream: +-3640), and
the output layer gives x0 in Q9. Each layer computes

    acc = sum_i w_i x_i          (1x1 conv, 2x2 space-to-depth conv, depthwise 3x3 or centre tap)
    v   = acc * 2^-lam_c + b_c   (per-channel gain, per-sample bias = b0 + subject + noise level)
    out = clamp(v) / clamp(old + v) (addh) / v (raw)

Modes:
  float  float weights and gains, no rounding (pre-training)
  qat    ternary weights (straight-through), gains 2^-round(lam), floor after the shift and integer
         biases (straight-through). Equal to the Mac whenever no 16-bit sum overflows.
  int    exact integer emulation (wrap16, asr, int16 clamp table), no gradients. Bit-exact with
         ref.py (tested by test_student.py).

Conditioning enters only through the biases, as on the Mac (folded once per step):
  b = round(b0) + subject_bias(s) + round(time_mlp(t)).
subject_bias in qat/int mode is the Mac's integer formula: E_int (n_subj, D) int8 with a global
scale, U_int (D, Ctot) int8 with per-channel scales, sb_c = ((sum_d E_int U_int) >> 6) * M_c >> 16.
"""
import math
import os
import sys

import torch
import torch.nn as nn
import torch.nn.functional as F

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "engine"))
import arch  # noqa: E402

# Exactness of the qat forward: integer-valued float32 convolutions are exact only without
# Winograd/FFT (CPU NNPACK switches on at batch >= 16) and without TF32 (GPU, 10-bit mantissa).
try:
    torch.backends.nnpack.set_flags(False)
except Exception:
    pass
torch.backends.cudnn.allow_tf32 = False
torch.backends.cuda.matmul.allow_tf32 = False

Q_IN = 512.0          # x_t and x0 scale (Q9)
BIAS_F64 = True       # qat/int: time MLP and subject-bias product in float64 (device-independent rounding)
LAM_MIN, LAM_MAX = -3, 15


def tab_range(name):
    if name == "raw":
        return None
    if name.startswith("relu"):
        return 0, int(name[4:])
    if name == "lin":
        return -arch.LIN_MAX, arch.LIN_MAX
    if name.startswith("lin"):
        m = int(name[3:])
        return -m, m
    if name == "res":
        return -arch.RES_MAX, arch.RES_MAX
    raise ValueError(name)


def ste_round(x):
    return x + (torch.round(x) - x).detach()


def ste_floor(x):
    return x + (torch.floor(x) - x).detach()


def ternarize(w, theta=0.7):
    """Per-output-channel ternary codes (forward) with a straight-through gradient. Returns (t, alpha):
    t in {-1, 0, 1}, alpha = mean |w| over the kept weights (the scale the gain absorbs).
    The threshold is computed in float64 so the codes don't depend on the device's summation order
    (QAT piles weights up at the threshold; found by the training helper, 2026-10-17)."""
    flat = w.reshape(w.shape[0], -1)
    flat64 = flat.abs().double()
    delta = theta * flat64.mean(1, keepdim=True)
    mask = (flat64 > delta).to(flat.dtype)
    alpha = (flat.abs() * mask).sum(1, keepdim=True) / mask.sum(1, keepdim=True).clamp(min=1)
    t = torch.sign(flat) * mask
    a = alpha.detach()
    t = t.detach() + (flat / a - (flat / a).detach())
    return t.reshape(w.shape), alpha.reshape(-1)


def quantize4(w):
    """Per-output-channel 4-bit codes -7..7 (straight-through). Returns (q, alpha), alpha = max|w| / 7."""
    flat = w.reshape(w.shape[0], -1)
    alpha = (flat.abs().double().max(1, keepdim=True).values / 7).clamp(min=1e-12).to(flat.dtype)
    a = alpha.detach()
    q = torch.round(flat / a).clamp(-7, 7)
    q = q.detach() + (flat / a - (flat / a).detach())
    return q.reshape(w.shape), alpha.reshape(-1)


def gain2_codes(lam):
    """Nearest two-term gain 2^-k1 + s 2^-k2 (s in -1/0/+1, k2 > k1) to 2^-lam. Returns (k1, s, k2, g)
    as float tensors; g is the quantized gain value."""
    lam = lam.clamp(LAM_MIN, LAM_MAX)
    g = torch.pow(2.0, -lam)
    best = None
    for k1 in (torch.floor(lam), torch.ceil(lam)):
        k1 = k1.clamp(LAM_MIN, LAM_MAX)
        base = torch.pow(2.0, -k1)
        r = g - base
        k2 = torch.round(-torch.log2(r.abs().clamp(min=1e-30)))
        ok = (k2 > k1) & (k2 <= LAM_MAX) & (r.abs() > 0)
        k2 = torch.where(ok, k2, k1 + 1)
        sgn = torch.where(ok, torch.sign(r), torch.zeros_like(r))
        approx = base + sgn * torch.pow(2.0, -k2)
        err = (approx - g).abs()
        if best is None:
            best = [k1, sgn, k2, approx, err]
        else:
            take = err < best[4]
            best = [torch.where(take, a, b) for a, b in zip([k1, sgn, k2, approx, err], best)]
    return best[0], best[1], best[2], best[3]


# ---------------------------------------------------------------------------- integer helpers
def wrap16(x):
    return ((x + 32768) & 0xFFFF) - 32768


def shift16(acc, k):
    right = acc >> k.clamp(min=0)
    left = wrap16(acc << (-k).clamp(min=0))
    return torch.where(k >= 0, right, left)


def gather_s2d(x):
    """(B, C, H, W) -> (B, 4C, H/2, W/2), channel = (dy*2+dx)*C + c (the Mac's order)."""
    B, C, H, W = x.shape
    return x.reshape(B, C, H // 2, 2, W // 2, 2).permute(0, 3, 5, 1, 2, 4).reshape(B, 4 * C, H // 2, W // 2)


def scatter_d2s(y, cf):
    """(B, 4Cf, h, w) -> (B, Cf, 2h, 2w), output channel o = (dy*2+dx)*Cf + c."""
    B, _, h, w = y.shape
    return y.reshape(B, 2, 2, cf, h, w).permute(0, 3, 4, 1, 5, 2).reshape(B, cf, 2 * h, 2 * w)


def sinusoidal(t, dim=64, dtype=torch.float32):
    half = dim // 2
    f = torch.exp(-math.log(10000.0) * torch.arange(half, device=t.device, dtype=dtype) / half)
    a = t.to(dtype)[:, None] * 1000.0 * f[None]
    return torch.cat([torch.sin(a), torch.cos(a)], 1)


class DreamStudent(nn.Module):
    def __init__(self, cfg="A64L2", n_subjects=250, emb_dim=32, time_dim=64, time_hidden=256):
        super().__init__()
        self.cfg = cfg
        self.T, self.L = arch.build(cfg)
        c = arch.CONFIGS[cfg] if isinstance(cfg, str) else cfg
        self.gain2 = bool(c.get("gain2"))
        self._g2cache = {}          # layer -> (lam version, device, (k1, sgn, k2, approx))
        self.in_shift, self.in_max = arch.in_shift(cfg)
        self.n_subjects, self.emb_dim = n_subjects, emb_dim
        self.W = nn.ParameterList()
        self.lam = nn.ParameterList()
        self.offs = []
        off = 0
        for l in self.L:
            if l["kind"] == "pw":
                w = torch.randn(l["cout"], l["cin"]) / math.sqrt(l["cin"])
            elif l["center"]:
                w = torch.ones(l["cout"], 1)
            else:
                w = torch.randn(l["cout"], 9) / 3.0
            self.W.append(nn.Parameter(w, requires_grad=not (l["kind"] == "dw" and l["center"])))
            self.lam.append(nn.Parameter(torch.zeros(l["cout"])))
            self.offs.append((off, off + l["cout"]))
            off += l["cout"]
        self.ctot = off
        self.b0 = nn.Parameter(torch.zeros(off))
        self.E = nn.Parameter(torch.randn(n_subjects, emb_dim) * 0.5)
        self.U = nn.Parameter(torch.zeros(emb_dim, off))
        self.tdim = time_dim
        self.tmlp = nn.Sequential(nn.Linear(time_dim, time_hidden), nn.SiLU(), nn.Linear(time_hidden, off))
        nn.init.zeros_(self.tmlp[2].weight)
        nn.init.zeros_(self.tmlp[2].bias)
        self.mode = "float"
        self.theta = 0.7
        # integer subject-bias quantisation (set by freeze_subject_quant / used in qat and int)
        self.register_buffer("sE", torch.tensor(1.0))
        self.register_buffer("sU", torch.ones(off))

    # ------------------------------------------------------------------ conditioning
    def subject_bias(self, s):
        if self.mode == "float":
            return self.E[s] @ self.U
        e_int, u_int, m = self.subject_quant()
        acc = e_int[s].double() @ u_int.double()               # exact integers (|acc| < 2^19)
        if self.mode == "int":
            acc = acc.round().long()
            return ((acc >> 6) * m.long()) >> 16
        a6 = ste_floor(acc / 64.0)                             # float64: a6 * M reaches 2.6e8 > 2^24
        return ste_floor(a6 * m.double() / 65536.0).float()

    def subject_quant(self):
        """E_int = round(E / sE) (|.| <= 127), U_int = round(U / sU_c), M_c = round(sE sU_c 2^22)."""
        e = self.E / self.sE
        u = self.U / self.sU[None]
        e_int = (e + (torch.round(e).clamp(-127, 127) - e).detach())
        u_int = (u + (torch.round(u).clamp(-127, 127) - u).detach())
        m = torch.round(self.sE * self.sU * 2 ** 22).clamp(-32767, 32767)
        return e_int, u_int, m

    @torch.no_grad()
    def freeze_subject_quant(self):
        self.sE.fill_(float(self.E.abs().max()) / 127 + 1e-12)
        self.sU.copy_(self.U.abs().max(0).values / 127 + 1e-12)
        # M_c must fit int16: shrink the per-channel scale if needed (re-quantises U finer)
        lim = 32767 / (self.sE * 2 ** 22)
        self.sU.clamp_(max=float(lim))

    def time_bias(self, t):
        if self.mode == "float":
            return self.tmlp(sinusoidal(t, self.tdim))
        # qat/int: float64 so round() decides the same way on every device (and in export)
        l1, l2 = self.tmlp[0], self.tmlp[2]
        x = sinusoidal(t, self.tdim, torch.float64)
        h = F.silu(x @ l1.weight.double().t() + l1.bias.double())
        return h @ l2.weight.double().t() + l2.bias.double()

    def bias(self, s, t):
        if self.mode == "float":
            return self.b0[None] + self.subject_bias(s) + self.time_bias(t)
        if self.mode == "int":
            return wrap16(torch.round(self.b0).long()[None] + self.subject_bias(s)
                          + torch.round(self.time_bias(t)).long())
        return ste_round(self.b0)[None] + self.subject_bias(s) + ste_round(self.time_bias(t)).float()

    # ------------------------------------------------------------------ layers
    def layer_weights(self, i):
        l, w = self.L[i], self.W[i]
        if self.mode == "float" or (l["kind"] == "dw" and l["center"]):
            return w
        if l.get("wq") == "q4":
            q, _ = quantize4(w)
            return q
        t, _ = ternarize(w, self.theta)
        return t

    def g2codes(self, i):
        """gain2_codes for layer i, cached until lam[i] changes (optimizer steps bump its version)."""
        lam = self.lam[i]
        key = (lam._version, lam.device)
        hit = self._g2cache.get(i)
        if hit is None or hit[0] != key:
            with torch.no_grad():
                hit = (key, gain2_codes(lam.detach()))
            self._g2cache[i] = hit
        return hit[1]

    def refresh_gain2(self):
        self._g2cache.clear()

    def gains(self, i):
        lam = self.lam[i]
        if self.mode == "float":
            return torch.pow(2.0, -lam)
        if self.gain2:
            g = torch.pow(2.0, -lam.clamp(LAM_MIN, LAM_MAX))
            return g + (self.g2codes(i)[3] - g).detach()
        k = lam.clamp(LAM_MIN, LAM_MAX)
        return torch.pow(2.0, -ste_round(k))

    def shift_codes(self, i):
        """Integer requantisation codes of layer i: k (cout,) or, with two-term gains, (cout, 2) =
        (k1, e2) with e2 = 0 (no second term) or s * (k2 + 4)."""
        lam = self.lam[i].detach()
        if not self.gain2:
            return lam.clamp(LAM_MIN, LAM_MAX).round().long()
        k1, sgn, k2, _ = self.g2codes(i)
        e2 = (sgn * (k2 + 4)).round().long()
        return torch.stack([k1.round().long(), e2], 1)

    def net_input(self, xt_q9):
        """The first layer's input from x_t in Q9: unchanged (clamp +-2047), or Q6 (>> 3, clamp +-255)
        for io_q4 configs. In qat/int modes the rounding is exact (floor of the integer x_t)."""
        x = xt_q9.clamp(-arch.IN_MAX, arch.IN_MAX)
        if self.mode != "float":
            x = ste_round(x)
        if self.in_shift:
            x = x / 2 ** self.in_shift
            if self.mode != "float":
                x = ste_floor(x)
            x = x.clamp(-self.in_max, self.in_max)
        return x

    def conv(self, i, x, w):
        l = self.L[i]
        if l["kind"] == "pw":
            if l["gather"] == "s2d":
                x = gather_s2d(x)
            return F.conv2d(x, w[:, :, None, None])
        if l["center"]:
            return x * w[:, 0][None, :, None, None]
        return F.conv2d(x, w.reshape(-1, 1, 3, 3), padding=1, groups=l["cout"])

    def forward(self, xt_q9, s, t):
        """xt_q9: (B, 1, H, W) noisy picture in Q9 units; s: (B,) subject ids; t: (B,) noise level in
        [0, 1]. Returns x0 in Q9 units, (B, 1, H, W)."""
        if self.mode == "int":
            return self.forward_int(xt_q9, s, t)
        p = arch.CONFIGS[self.cfg]["patch"] if isinstance(self.cfg, str) else self.cfg["patch"]
        x = F.pixel_unshuffle(self.net_input(xt_q9), p)
        tens = {"xt": x}
        bias = self.bias(s, t)
        for i, l in enumerate(self.L):
            src = tens[l["src"]]
            w = self.layer_weights(i)
            acc = self.conv(i, src, w)
            a, b = self.offs[i]
            if self.mode == "qat" and self.gain2:
                # exactly the Mac's (acc >> k1) + s (acc >> k2): each term floored separately
                k1, sgn, k2, _ = self.g2codes(i)
                g = self.gains(i)[None, :, None, None]
                t1 = acc * torch.pow(2.0, -k1)[None, :, None, None]
                t2 = acc * torch.pow(2.0, -k2)[None, :, None, None]
                vq = torch.floor(t1) + sgn[None, :, None, None] * torch.floor(t2)
                v = acc * g + (vq - acc * g).detach()
            else:
                g = self.gains(i)[None, :, None, None]
                v = acc * g
                if self.mode == "qat":
                    v = ste_floor(v)
            v = v + bias[:, a:b, None, None]
            if l["kind"] == "pw" and l["scatter"] == "d2s":
                v = scatter_d2s(v, l["cout"] // 4)
            if l["mode"] == "addh":
                v = tens[l["dst"]] + v
            r = tab_range(l["tab"])
            out = v if r is None else v.clamp(r[0], r[1])
            tens[l["dst"]] = out
        self.last = tens
        return F.pixel_shuffle(tens["out"], p)

    # ------------------------------------------------------------------ exact integer forward
    def int_params(self):
        """Integer weights (cout, n) int64, shifts k (cout,), and the bias function's integer parts."""
        out = []
        old = self.mode
        self.mode = "qat"
        with torch.no_grad():
            for i, l in enumerate(self.L):
                w = self.layer_weights(i).round().long()
                out.append((w.reshape(l["cout"], -1), self.shift_codes(i)))
        self.mode = old
        return out

    @torch.no_grad()
    def forward_int(self, xt_q9, s, t, bias=None, params=None):
        p = arch.CONFIGS[self.cfg]["patch"] if isinstance(self.cfg, str) else self.cfg["patch"]
        x = xt_q9.round().clamp(-arch.IN_MAX, arch.IN_MAX).long()
        if self.in_shift:
            x = (x >> self.in_shift).clamp(-self.in_max, self.in_max)
        x = F.pixel_unshuffle(x.float(), p).long()
        params = params or self.int_params()
        if bias is None:
            bias = self.bias(s, t)
        tens = {"xt": x}
        for i, l in enumerate(self.L):
            w, k = params[i]
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
            a, b = self.offs[i]
            acc = wrap16(acc)
            if k.dim() == 2:                       # two-term gain: (acc >> k1) + s (acc >> k2)
                k1, e2 = k[:, 0], k[:, 1]
                k2 = e2.abs() - 4
                t2 = torch.where(e2[None, :, None, None] != 0, shift16(acc, k2[None, :, None, None]), 0)
                t = wrap16(shift16(acc, k1[None, :, None, None]) + torch.sign(e2)[None, :, None, None] * t2)
            else:
                t = shift16(acc, k[None, :, None, None])
            v = wrap16(t + bias[:, a:b, None, None])
            if l["kind"] == "pw" and l["scatter"] == "d2s":
                v = scatter_d2s(v, l["cout"] // 4)
            if l["mode"] == "addh":
                v = wrap16(v + tens[l["dst"]])
            r = tab_range(l["tab"])
            if r is not None:                      # the Mac clamps the full int16 v (no wrap)
                v = v.clamp(r[0], r[1])
            tens[l["dst"]] = v
        self.last = tens
        return F.pixel_shuffle(tens["out"], p)

    # ------------------------------------------------------------------ init / conversion
    @torch.no_grad()
    def calibrate(self, xt_q9, s, t, target=None):
        """Data-dependent init of the float gains: pre-activation std about 1/3 of each table's
        range; residual branches (addh) start small so the stream begins near the input."""
        assert self.mode == "float"
        p = arch.CONFIGS[self.cfg]["patch"]
        tens = {"xt": F.pixel_unshuffle(self.net_input(xt_q9), p)}
        bias = self.bias(s, t)
        for i, l in enumerate(self.L):
            acc = self.conv(i, tens[l["src"]], self.W[i])
            std = acc.transpose(0, 1).reshape(l["cout"], -1).std(1) + 1e-6
            r = tab_range(l["tab"])
            if l["mode"] == "raw":
                tgt = 0.5 * Q_IN
            elif l["mode"] == "addh":
                tgt = 0.05 * r[1]
            elif l["mode"] == "seth":
                tgt = 0.15 * r[1]
            else:
                tgt = r[1] / 3
            if l["kind"] == "dw" and l["center"]:
                tgt = r[1] / 3
            self.lam[i].copy_(torch.log2(std / tgt))
            g = torch.pow(2.0, -self.lam[i])[None, :, None, None]
            a, b = self.offs[i]
            v = acc * g + bias[:, a:b, None, None]
            if l["kind"] == "pw" and l["scatter"] == "d2s":
                v = scatter_d2s(v, l["cout"] // 4)
            if l["mode"] == "addh":
                v = tens[l["dst"]] + v
            tens[l["dst"]] = v if r is None else v.clamp(r[0], r[1])

    @torch.no_grad()
    def to_qat(self):
        """Switch float -> qat: fold each row's ternary scale into its gain (lam -= log2 alpha)."""
        for i, l in enumerate(self.L):
            if l["kind"] == "dw" and l["center"]:
                continue
            if l.get("wq") == "q4":
                _, alpha = quantize4(self.W[i])
            else:
                _, alpha = ternarize(self.W[i], self.theta)
            self.lam[i].sub_(torch.log2(alpha.clamp(min=1e-8)))
        self.freeze_subject_quant()
        self.mode = "qat"

    @torch.no_grad()
    def adopt_q9_input_weights(self):
        """After loading a state_dict trained with Q9 input (e.g. an A64L2 float checkpoint) into an
        io_q4 config: the input layer now sees x_t / 8, so its gain must be 8x larger."""
        if self.in_shift:
            self.lam[0].sub_(float(self.in_shift))

    def n_params(self):
        return sum(arch.params(l) for l in self.L)
