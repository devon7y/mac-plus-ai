"""Tune's network (PyTorch, for training): a GRU whose input is a sum of embedding rows (the previous
token's symbol and duration, its bar position, triplet state, bar and section counters, and the
tune's type, meter and mode), with optional quantisation-aware training (QAT) matching the 68000
integer version (tune/ref): ternary recurrent / output weights with per-row scales, hidden state
quantised to int8 (x127) before every matrix product.
"""
import math
import os
import sys

import torch
import torch.nn as nn
import torch.nn.functional as F

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "corpus"))
import vocab as V  # noqa: E402

FEATS = [("sym", V.NSYM), ("dur", V.NDUR), ("pos", V.MAXPOS), ("trip", 4), ("bar", V.MAXBAR),
         ("sec", V.MAXSEC), ("lasym", V.NSYM + 1), ("ladur", V.NDUR), ("lbsym", V.NSYM + 1),
         ("lbdur", V.NDUR)]
CONDS = [("typ", len(V.TYPES)), ("met", len(V.METERS)), ("mode", len(V.MODES))]


def ste_round(x):
    return x + (torch.round(x) - x).detach()


def ternary(w):
    """Per-row absmean ternary (BitNet b1.58) with a straight-through gradient."""
    a = w.abs().mean(dim=1, keepdim=True).clamp(min=1e-5)
    q = torch.clamp(torch.round(w / a), -1, 1) * a
    return w + (q - w).detach()


def quant_h(h):
    return ste_round(h * 127.0) / 127.0


class GRULayer(nn.Module):
    def __init__(self, H, nin=0):
        super().__init__()
        self.H = H
        self.whh = nn.Parameter(torch.empty(3 * H, H).uniform_(-1 / math.sqrt(H), 1 / math.sqrt(H)))
        self.bhn = nn.Parameter(torch.zeros(H))
        if nin:
            self.wih = nn.Parameter(torch.empty(3 * H, nin).uniform_(-1 / math.sqrt(nin), 1 / math.sqrt(nin)))
            self.bih = nn.Parameter(torch.zeros(3 * H))
        else:
            self.wih = None

    def run(self, x, qat, h=None):
        """x: (B, T, 3H) gate inputs (layer 1) or (B, T, nin) inputs (layer 2+). -> (B, T, H)."""
        B, T, _ = x.shape
        H = self.H
        whh = ternary(self.whh) if qat else self.whh
        if self.wih is not None:
            xin = quant_h(x) if qat else x
            wih = ternary(self.wih) if qat else self.wih
            x = xin @ wih.t() + self.bih
        if h is None:
            h = x.new_zeros(B, H)
        out = []
        for t in range(T):
            hq = quant_h(h) if qat else h
            gh = hq @ whh.t()
            xt = x[:, t]
            r = torch.sigmoid(xt[:, :H] + gh[:, :H])
            z = torch.sigmoid(xt[:, H:2 * H] + gh[:, H:2 * H])
            n = torch.tanh(xt[:, 2 * H:] + r * (gh[:, 2 * H:] + self.bhn))
            h = n + z * (h - n)
            out.append(h)
        return torch.stack(out, 1), h


class TuneNet(nn.Module):
    def __init__(self, H=256, layers=1, H2=None, drop=0.1):
        super().__init__()
        self.H = H
        self.H2 = H2 or H
        self.nlayers = layers
        G = 3 * H
        self.emb = nn.ModuleDict({k: nn.Embedding(n, G) for k, n in FEATS + CONDS})
        for e in self.emb.values():
            nn.init.normal_(e.weight, std=0.3)
        self.l1 = GRULayer(H)
        self.l2 = GRULayer(self.H2, nin=H) if layers > 1 else None
        Hl = self.H2 if layers > 1 else H
        self.wout = nn.Parameter(torch.empty(V.VOCAB, Hl).uniform_(-1 / math.sqrt(Hl), 1 / math.sqrt(Hl)))
        self.bout = nn.Parameter(torch.zeros(V.VOCAB))
        self.drop = nn.Dropout(drop)
        self.qat = False

    def gate_inputs(self, feats, cond):
        """feats (B, T, 6) long, cond (B, 3) long -> (B, T, 3H)."""
        x = 0
        for i, (k, _) in enumerate(FEATS):
            x = x + self.emb[k](feats[..., i])
        c = 0
        for i, (k, _) in enumerate(CONDS):
            c = c + self.emb[k](cond[:, i])
        return x + c[:, None, :]

    def forward(self, feats, cond, mask=None):
        x = self.gate_inputs(feats, cond)
        x = self.drop(x)
        h, _ = self.l1.run(x, self.qat)
        if self.l2 is not None:
            h, _ = self.l2.run(self.drop(h), self.qat)
        h = self.drop(h)
        if self.qat:
            logits = quant_h(h) @ ternary(self.wout).t() + self.bout
        else:
            logits = h @ self.wout.t() + self.bout
        if mask is not None:
            logits = logits.masked_fill(~mask, -1e4)
        return logits

    def macs(self):
        """Weights touched per token on the Mac (recurrent + layer-2 input + output)."""
        n = self.l1.whh.numel() + self.wout.numel()
        if self.l2 is not None:
            n += self.l2.whh.numel() + self.l2.wih.numel()
        return n
