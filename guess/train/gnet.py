"""Guess My Doodle classifier: architecture spec, float / QAT modules, and a 68000 cost model.

Spec: comma-separated tokens, applied to the 32x32 binary drawing:
  t<C>        3x3 conv on the binary input (the Mac uses a 512-entry pattern table), BN, ReLU
  p           2x2 max pool
  d           depthwise 3x3 (ternary), BN, ReLU
  c<C>[k<K>][s2]  KxK conv (default 1x1) to C channels (ternary), stride 2 with s2, BN, ReLU
  f           flatten (channels-last, as the Mac stores tensors)
  g           global max pool
  l<N>        fully connected (ternary), BN, ReLU
  o           output layer (ternary, per-class scale and bias): the logits
Quantised mode (QAT): every ReLU output is an integer 0..A (LSQ step size), A chosen so that the
next layer's 16-bit sums cannot overflow; weights are ternary with a per-channel threshold
(0.7 mean |w|, TWN) and straight-through gradients.
"""
import math

import torch
import torch.nn as nn
import torch.nn.functional as F

AMAX = 127


def parse(spec):
    out = []
    for tok in spec.replace("_", ",").split(","):
        tok = tok.strip()
        if tok[0] == "t":
            s2 = tok.endswith("s")
            out.append(("t", int(tok[1:-1] if s2 else tok[1:]), 2 if s2 else 1))
        elif tok == "p":
            out.append(("p",))
        elif tok == "d":
            out.append(("d",))
        elif tok == "f":
            out.append(("f",))
        elif tok == "g":
            out.append(("g",))
        elif tok == "o":
            out.append(("o",))
        elif tok[0] == "l":
            out.append(("l", int(tok[1:])))
        elif tok[0] == "c":
            s2 = tok.endswith("s2")
            t = tok[1:-2] if s2 else tok[1:]
            k = 1
            if "k" in t:
                t, kk = t.split("k")
                k = int(kk)
            out.append(("c", int(t), k, 2 if s2 else 1))
        else:
            raise ValueError(tok)
    return out


def shapes(spec, ncls, G=32):
    """Per layer: (kind, in (H, W, C), out (H, W, C), fan_in, params, macs)."""
    H = W = G
    C = 1
    res = []
    for L in parse(spec):
        k = L[0]
        if k == "t":
            o = (H // L[2], W // L[2], L[1])
            res.append((L, (H, W, C), o, 9, 9 * L[1], o[0] * o[1] * 9 * L[1]))
        elif k == "p":
            o = (H // 2, W // 2, C)
            res.append((L, (H, W, C), o, 0, 0, 0))
        elif k == "g":
            o = (1, 1, C)
            res.append((L, (H, W, C), o, 0, 0, 0))
        elif k == "f":
            o = (1, 1, H * W * C)
            res.append((L, (H, W, C), o, 0, 0, 0))
        elif k == "d":
            o = (H, W, C)
            res.append((L, (H, W, C), o, 9, 9 * C, H * W * 9 * C))
        elif k == "c":
            _, co, kk, s = L
            Ho, Wo = H // s, W // s
            o = (Ho, Wo, co)
            res.append((L, (H, W, C), o, kk * kk * C, kk * kk * C * co, Ho * Wo * kk * kk * C * co))
        elif k in ("l", "o"):
            n = L[1] if k == "l" else ncls
            fi = H * W * C
            o = (1, 1, n)
            res.append((L, (H, W, C), o, fi, fi * n, fi * n))
        H, W, C = o
    return res


# ----------------------------------------------------------------------------- 68000 cost model
# MAME cycles (7.83 MHz, no bus contention); calibrated against the M0 benchmark (bench/).
RHO = 0.55          # fraction of non-zero ternary weights
RQ = 100            # requantise one output: muls.w #M, add.l #B, swap, clamp to 0..A, store
POS = 70            # per grid position: driver loop, jsr/rts


def cycles(spec, ncls):
    rows = []
    for L, i, o, fan, par, mac in shapes(spec, ncls):
        k = L[0]
        H, W, C = o
        if k == "t":
            cyc = H * W * (60 + 12 * C)            # pattern index + copy of a table row
        elif k == "p":
            cyc = H * W * C * 34                   # 4 loads, 3 compares
        elif k == "g":
            cyc = i[0] * i[1] * C * 14
        elif k == "f":
            cyc = 0
        elif k == "d":
            cyc = H * W * (POS + C * (RHO * 9 * 12 + 4 + RQ))
        else:                                       # c, l, o: chunks of 7 outputs
            kk = L[2] if k == "c" else 1
            nch = math.ceil(C / 7)
            per = POS + nch * (28 + kk * kk * 8 + fan * 8 + 8) + RHO * fan * C * 4 + C * RQ
            cyc = H * W * per
        rows.append((L, mac, par, cyc))
    return rows


def summary(spec, ncls):
    rows = cycles(spec, ncls)
    macs = sum(r[1] for r in rows)
    par = sum(r[2] for r in rows)
    cyc = sum(r[3] for r in rows)
    # code bytes: about 2 bytes per non-zero weight + 2 per (input, chunk) + RQ code per output channel
    code = 0
    for L, i, o, fan, p, m in shapes(spec, ncls):
        if L[0] in ("c", "l", "o", "d"):
            C = o[2]
            code += RHO * p * 2 + fan * math.ceil(C / 7) * 2 + C * 24
        elif L[0] == "t":
            code += 512 * o[2] * 2
    return dict(macs=macs, params=par, mame_s=cyc / 7.79e6, plus_s=cyc * 1.5 / 7.79e6, code_kb=code / 1024)


# ----------------------------------------------------------------------------- modules
class RoundSTE(torch.autograd.Function):
    @staticmethod
    def forward(ctx, x):
        return torch.round(x)

    @staticmethod
    def backward(ctx, g):
        return g


def grad_scale(x, s):
    return (x - x * s).detach() + x * s


class ActQ(nn.Module):
    """ReLU then (in QAT) LSQ quantisation to 0..A in steps of s."""

    def __init__(self, A):
        super().__init__()
        self.A = A
        self.s = nn.Parameter(torch.tensor(1.0))
        self.register_buffer("inited", torch.tensor(0))
        self.quant = False

    def forward(self, x):
        x = F.relu(x)
        if not self.quant:
            return x
        if not self.inited:
            with torch.no_grad():
                self.s.copy_(2 * x.mean() / math.sqrt(self.A) + 1e-6)
                self.inited.fill_(1)
        g = 1.0 / math.sqrt(x[0].numel() * self.A)
        s = grad_scale(self.s, g).abs() + 1e-8
        v = RoundSTE.apply(torch.clamp(x / s, 0, self.A))
        return v * s


def ternarize(w, dims):
    a = w.abs()
    delta = 0.7 * a.mean(dim=dims, keepdim=True)
    m = (a > delta).to(w.dtype)
    alpha = (a * m).sum(dim=dims, keepdim=True) / m.sum(dim=dims, keepdim=True).clamp(min=1)
    return torch.sign(w) * m, alpha


class TConv(nn.Module):
    def __init__(self, cin, cout, k, stride=1, groups=1):
        super().__init__()
        self.w = nn.Parameter(torch.empty(cout, cin // groups, k, k))
        nn.init.kaiming_normal_(self.w, nonlinearity="relu")
        self.stride, self.groups, self.pad = stride, groups, k // 2
        self.quant = False

    def qweight(self):
        t, alpha = ternarize(self.w, (1, 2, 3))
        return t, alpha

    def forward(self, x):
        w = self.w
        if self.quant:
            t, alpha = self.qweight()
            w = w + (t * alpha - w).detach()
        return F.conv2d(x, w, None, self.stride, self.pad, 1, self.groups)


class TLinear(nn.Module):
    def __init__(self, fin, fout, bias=False):
        super().__init__()
        self.w = nn.Parameter(torch.empty(fout, fin))
        nn.init.kaiming_normal_(self.w, nonlinearity="relu")
        self.b = nn.Parameter(torch.zeros(fout)) if bias else None
        self.quant = False

    def qweight(self):
        return ternarize(self.w, (1,))

    def forward(self, x):
        w = self.w
        if self.quant:
            t, alpha = self.qweight()
            w = w + (t * alpha - w).detach()
        return F.linear(x, w, self.b)


class Net(nn.Module):
    def __init__(self, spec, ncls, afull=False):
        """afull: every activation ceiling is AMAX (127) instead of the overflow-proof 32767 // fan-in
        (the sums seen in practice stay ~10x below 32767; the Mac and ref.py wrap identically anyway)."""
        super().__init__()
        self.spec, self.ncls, self.afull = spec, ncls, afull
        sh = shapes(spec, ncls)
        # A for each quantiser: from the fan-in of the next weighted layer
        fans = [s[3] for s in sh]
        kinds = [s[0][0] for s in sh]
        layers = []
        for j, (L, i, o, fan, par, mac) in enumerate(sh):
            k = L[0]
            nxt = next((fans[q] for q in range(j + 1, len(sh)) if kinds[q] in "dclo"), 1)
            A = AMAX if afull else min(AMAX, 32767 // max(nxt, 1))
            if k == "t":
                m = nn.ModuleDict(dict(conv=nn.Conv2d(1, L[1], 3, stride=L[2], padding=1, bias=False),
                                       bn=nn.BatchNorm2d(L[1]), act=ActQ(A)))
            elif k == "d":
                C = i[2]
                m = nn.ModuleDict(dict(conv=TConv(C, C, 3, 1, C), bn=nn.BatchNorm2d(C), act=ActQ(A)))
            elif k == "c":
                m = nn.ModuleDict(dict(conv=TConv(i[2], L[1], L[2], L[3]), bn=nn.BatchNorm2d(L[1]),
                                       act=ActQ(A)))
            elif k == "l":
                m = nn.ModuleDict(dict(fc=TLinear(fan, L[1]), bn=nn.BatchNorm1d(L[1]), act=ActQ(A)))
            elif k == "o":
                m = nn.ModuleDict(dict(fc=TLinear(fan, ncls), oscale=_OutScale(ncls)))
            else:
                m = nn.Identity()
            layers.append(m)
        self.layers = nn.ModuleList(layers)
        self.kinds = [s[0] for s in sh]

    def set_quant(self, q):
        for m in self.modules():
            if isinstance(m, (ActQ, TConv, TLinear)):
                m.quant = q

    def forward(self, x):
        """x: (B, 32, 32) uint8 or float -> logits."""
        x = x.float().unsqueeze(1)
        for L, m in zip(self.kinds, self.layers):
            k = L[0]
            if k in ("t", "d", "c"):
                x = m["act"](m["bn"](m["conv"](x)))
            elif k == "p":
                x = F.max_pool2d(x, 2)
            elif k == "g":
                x = x.amax(dim=(2, 3), keepdim=True)
            elif k == "f":
                x = x.permute(0, 2, 3, 1).reshape(x.shape[0], -1)      # channels-last flatten
            elif k == "l":
                if x.dim() == 4:
                    x = x.permute(0, 2, 3, 1).reshape(x.shape[0], -1)
                x = m["act"](m["bn"](m["fc"](x)))
            elif k == "o":
                if x.dim() == 4:
                    x = x.permute(0, 2, 3, 1).reshape(x.shape[0], -1)
                x = m["oscale"](m["fc"](x))
        return x


class _OutScale(nn.Module):
    def __init__(self, n):
        super().__init__()
        self.g = nn.Parameter(torch.ones(n))
        self.b = nn.Parameter(torch.zeros(n))

    def forward(self, x):
        return x * self.g + self.b


if __name__ == "__main__":
    import sys
    for spec in sys.argv[1:]:
        s = summary(spec, 150)
        print(f"{spec:60s} MACs {s['macs'] / 1e3:7.0f}K params {s['params'] / 1e3:6.0f}K "
              f"MAME {s['mame_s']:.2f}s Plus {s['plus_s']:.2f}s code {s['code_kb']:.0f}KB")
