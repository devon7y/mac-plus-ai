#!/usr/bin/env python3
"""Scribble Calc's symbol classifier: architecture specs, float/QAT PyTorch model, 68000 cost model.

A spec is a list of layers:
  ("lut", C)            3x3 conv on the 1-bit 24x24 input (+BN, ReLU), then 2x2 max pool. On the Mac a
                        512-entry table per channel (the 9 input bits index it), so its weights stay float.
  ("conv", C, stride, pool, amax)
                        3x3 conv, ternary weights, padding 1, (+BN), stride 1 or 2, optional 2x2 max pool,
                        ReLU and quantisation to integers 0..amax.
  ("fc", N)             fully connected, ternary weights, to N outputs (the last one: 18 class scores).
Integer pipeline on the Mac (ref.py is the bit-exact reference):
  acc = sum(t * a) (int16), max-pooled raw, then q = clamp(((acc + b) * m + 32768) >> 16, 0, amax).
"""
import math

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

SPECS = {
    # name: layers
    "S": [("lut", 8), ("conv", 16, 1, True, 127), ("conv", 32, 1, True, 63), ("fc", 18)],
    "S2": [("lut", 8), ("conv", 16, 2, False, 127), ("conv", 32, 2, False, 63), ("fc", 18)],
    "T": [("lut", 6), ("conv", 12, 1, True, 127), ("conv", 24, 1, True, 63), ("fc", 18)],
    "M": [("lut", 8), ("conv", 16, 1, True, 127), ("conv", 32, 1, True, 63), ("fc", 32), ("fc", 18)],
    "L": [("lut", 12), ("conv", 24, 1, True, 127), ("conv", 32, 1, True, 63), ("fc", 18)],
    "S2b": [("lut", 8), ("conv", 24, 2, False, 127), ("conv", 48, 2, False, 63), ("fc", 18)],
    "U": [("lut", 8), ("conv", 16, 1, True, 127), ("conv", 32, 2, False, 63), ("fc", 18)],
    "S2bM": [("lut", 8), ("conv", 24, 2, False, 127), ("conv", 48, 2, False, 63), ("fc", 32), ("fc", 18)],
    "S2M": [("lut", 8), ("conv", 16, 2, False, 127), ("conv", 32, 2, False, 63), ("fc", 32), ("fc", 18)],
}


def shapes(spec, G=24):
    """Per layer: (H_in, W_in, C_in, H_out, W_out, C_out, conv positions)."""
    out = []
    H, C = G, 1
    for l in spec:
        if l[0] == "lut":
            out.append((H, H, C, H // 2, H // 2, l[1], H * H))
            H, C = H // 2, l[1]
        elif l[0] == "conv":
            _, co, st, pool, _ = l
            Hc = (H + st - 1) // st
            Ho = Hc // 2 if pool else Hc
            out.append((H, H, C, Ho, Ho, co, Hc * Hc))
            H, C = Ho, co
        else:
            n = H * H * C
            out.append((1, 1, n, 1, 1, l[1], 1))
            H, C = 1, l[1]
    return out


def cost(spec, density=0.6):
    """Estimated 68000 cycles per symbol (MAME; real Plus about 1.5x) and code bytes."""
    cyc, code, rows = 0, 0, []
    for l, (Hi, Wi, Ci, Ho, Wo, Co, pos) in zip(spec, shapes(spec)):
        if l[0] == "lut":
            c = Ho * Wo * (4 * 40 + Co * 4 * 14)       # 4 indices, then max of 4 table words per channel
            k = 0
        elif l[0] == "conv":
            taps = 9 * Ci
            chunks = math.ceil(Co / 7)
            nnz = Co * taps * density
            per = chunks * taps * 12 + nnz * 4 + Co * 8 + 60
            c = pos * per + Ho * Wo * Co * (70 if l[3] else 40) + Ho * Wo * Co * 80
            k = chunks * taps * 4 + nnz * 2 + Co * 2 + 2
        else:
            taps = Ci
            chunks = math.ceil(Co / 7)
            nnz = Co * taps * density
            c = chunks * taps * 8 + nnz * 4 + Co * 100
            k = chunks * taps * 2 + nnz * 2
        rows.append((l[0], int(c), int(k)))
        cyc += c
        code += k
    return int(cyc), int(code), rows


def params(spec):
    n = 0
    for l, (Hi, Wi, Ci, Ho, Wo, Co, pos) in zip(spec, shapes(spec)):
        n += (9 * Ci * Co) if l[0] in ("lut", "conv") else Ci * Co
    return n


class Ternary(torch.autograd.Function):
    @staticmethod
    def forward(ctx, w):
        dims = tuple(range(1, w.dim()))
        delta = 0.7 * w.abs().mean(dim=dims, keepdim=True)
        t = (w > delta).float() - (w < -delta).float()
        alpha = (w.abs() * t.abs()).sum(dim=dims, keepdim=True) / t.abs().sum(dim=dims, keepdim=True).clamp(min=1)
        return t * alpha

    @staticmethod
    def backward(ctx, g):
        return g


def ternary_t(w):
    dims = tuple(range(1, w.dim()))
    delta = 0.7 * w.abs().mean(dim=dims, keepdim=True)
    return (w > delta).float() - (w < -delta).float()


class FakeQ(torch.autograd.Function):
    @staticmethod
    def forward(ctx, x, s, amax):
        ctx.save_for_backward(x, s)
        ctx.amax = amax
        return torch.clamp(torch.round(x / s), 0, amax) * s

    @staticmethod
    def backward(ctx, g):
        x, s = ctx.saved_tensors
        m = (x >= 0) & (x <= ctx.amax * s)
        return g * m, None, None


class Net(nn.Module):
    def __init__(self, spec):
        super().__init__()
        self.spec = spec
        self.layers = nn.ModuleList()
        self.bns = nn.ModuleList()
        C = 1
        sh = shapes(spec)
        for l, s in zip(spec, sh):
            if l[0] in ("lut", "conv"):
                st = 1 if l[0] == "lut" else l[2]
                self.layers.append(nn.Conv2d(C, l[1], 3, stride=st, padding=1, bias=False))
                self.bns.append(nn.BatchNorm2d(l[1]))
                C = l[1]
            else:
                self.layers.append(nn.Linear(s[2], l[1], bias=True))
                self.bns.append(nn.Identity())
        n = len(spec)
        self.register_buffer("ascale", torch.ones(n))     # activation scale after each layer
        self.qat = False

    def forward(self, x, collect=None):
        for i, (l, conv, bn) in enumerate(zip(self.spec, self.layers, self.bns)):
            last = i == len(self.spec) - 1
            if l[0] in ("lut", "conv"):
                w = conv.weight
                if self.qat and l[0] == "conv":
                    w = Ternary.apply(w)
                st = conv.stride
                x = F.conv2d(x, w, None, st, 1)
                x = bn(x)
                pool = True if l[0] == "lut" else l[3]
                if pool:
                    x = F.max_pool2d(x, 2)
                if collect is not None:
                    collect.append(x.detach())
                x = F.relu(x)
                if self.qat:
                    amax = 127 if l[0] == "lut" else l[4]
                    x = FakeQ.apply(x, self.ascale[i], amax)
            else:
                x = x.flatten(1)
                w = conv.weight
                if self.qat:
                    w = Ternary.apply(w)
                x = F.linear(x, w, conv.bias)
                if not last:
                    if collect is not None:
                        collect.append(x.detach())
                    x = F.relu(x)
                    if self.qat:
                        x = FakeQ.apply(x, self.ascale[i], 63)
        return x

    def calibrate(self, X, pct=99.9):
        coll = []
        with torch.no_grad():
            was = self.qat
            self.qat = False
            self.eval()
            self(X, coll)
            self.qat = was
        ai = 0
        for i, l in enumerate(self.spec):
            if l[0] == "fc" and i == len(self.spec) - 1:
                continue
            v = coll[ai].flatten()
            ai += 1
            v = v[v > 0]
            amax = 127 if l[0] == "lut" else (l[4] if l[0] == "conv" else 63)
            q = torch.quantile(v.float().cpu()[:1_000_000], pct / 100) if len(v) else torch.tensor(1.0)
            self.ascale[i] = max(float(q) / amax, 1e-4)
