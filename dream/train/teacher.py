#!/usr/bin/env python3
"""Dream's float teacher: a class-conditional pixel-space U-Net (ADM style) for 64x64 one-channel
pictures, v-prediction on the cosine schedule, classifier-free guidance (class id K = "no subject").

ResBlocks: GroupNorm -> SiLU -> conv3x3 -> GroupNorm -> scale/shift from (time + subject) embedding
-> SiLU -> dropout -> conv3x3 (zero init), plus skip. Self-attention at the 16x16 and 8x8 levels.
Also: the guided DDIM sampler used for sample sheets and as the distillation teacher.
"""
import math

import torch
import torch.nn as nn
import torch.nn.functional as F

from common import alpha_sigma


def gn(c):
    return nn.GroupNorm(32 if c % 32 == 0 else 16, c)


def timestep_embedding(t, dim):
    half = dim // 2
    f = torch.exp(-math.log(10000.0) * torch.arange(half, device=t.device, dtype=torch.float32) / half)
    a = (t.float() * 1000.0)[:, None] * f[None]
    return torch.cat([torch.cos(a), torch.sin(a)], 1)


class ResBlock(nn.Module):
    def __init__(self, cin, cout, edim, dropout):
        super().__init__()
        self.n1, self.c1 = gn(cin), nn.Conv2d(cin, cout, 3, padding=1)
        self.emb = nn.Linear(edim, 2 * cout)
        self.n2, self.c2 = gn(cout), nn.Conv2d(cout, cout, 3, padding=1)
        nn.init.zeros_(self.c2.weight)
        nn.init.zeros_(self.c2.bias)
        self.drop = nn.Dropout(dropout)
        self.skip = nn.Conv2d(cin, cout, 1) if cin != cout else nn.Identity()

    def forward(self, x, e):
        h = self.c1(F.silu(self.n1(x)))
        sc, sh = self.emb(e)[:, :, None, None].chunk(2, 1)
        h = F.silu(self.n2(h) * (1 + sc) + sh)
        h = self.c2(self.drop(h))
        return self.skip(x) + h


class Attn(nn.Module):
    def __init__(self, c, head_ch=64):
        super().__init__()
        self.h = max(1, c // head_ch)
        self.n = gn(c)
        self.qkv = nn.Conv2d(c, 3 * c, 1)
        self.proj = nn.Conv2d(c, c, 1)
        nn.init.zeros_(self.proj.weight)
        nn.init.zeros_(self.proj.bias)

    def forward(self, x):
        B, C, H, W = x.shape
        q, k, v = self.qkv(self.n(x)).reshape(B, 3, self.h, C // self.h, H * W).transpose(-1, -2).unbind(1)
        o = F.scaled_dot_product_attention(q, k, v)                # (B, h, HW, d)
        o = o.transpose(-1, -2).reshape(B, C, H, W)
        return x + self.proj(o)


class Down(nn.Module):
    def __init__(self, c):
        super().__init__()
        self.c = nn.Conv2d(c, c, 3, stride=2, padding=1)

    def forward(self, x):
        return self.c(x)


class Up(nn.Module):
    def __init__(self, c):
        super().__init__()
        self.c = nn.Conv2d(c, c, 3, padding=1)

    def forward(self, x):
        return self.c(F.interpolate(x, scale_factor=2, mode="nearest"))


class TeacherUNet(nn.Module):
    def __init__(self, n_classes, ch=96, mults=(1, 2, 3, 4), nres=2, attn_res=(16, 8), img=64,
                 dropout=0.1, head_ch=64):
        super().__init__()
        self.cfg = dict(n_classes=n_classes, ch=ch, mults=list(mults), nres=nres, attn_res=list(attn_res),
                        img=img, dropout=dropout, head_ch=head_ch)
        self.K = n_classes
        edim = 4 * ch
        self.ch = ch
        self.temb = nn.Sequential(nn.Linear(ch, edim), nn.SiLU(), nn.Linear(edim, edim))
        self.cemb = nn.Embedding(n_classes + 1, edim)
        self.cin = nn.Conv2d(1, ch, 3, padding=1)
        self.down = nn.ModuleList()
        chans = [ch]
        c = ch
        res = img
        for i, m in enumerate(mults):
            for _ in range(nres):
                blk = nn.ModuleList([ResBlock(c, ch * m, edim, dropout)])
                c = ch * m
                if res in attn_res:
                    blk.append(Attn(c, head_ch))
                self.down.append(blk)
                chans.append(c)
            if i + 1 < len(mults):
                self.down.append(nn.ModuleList([Down(c)]))
                chans.append(c)
                res //= 2
        self.mid = nn.ModuleList([ResBlock(c, c, edim, dropout), Attn(c, head_ch), ResBlock(c, c, edim, dropout)])
        self.up = nn.ModuleList()
        for i, m in reversed(list(enumerate(mults))):
            for j in range(nres + 1):
                blk = nn.ModuleList([ResBlock(c + chans.pop(), ch * m, edim, dropout)])
                c = ch * m
                if res in attn_res:
                    blk.append(Attn(c, head_ch))
                if i > 0 and j == nres:
                    blk.append(Up(c))
                    res *= 2
                self.up.append(blk)
        self.nout = gn(c)
        self.cout = nn.Conv2d(c, 1, 3, padding=1)
        nn.init.zeros_(self.cout.weight)
        nn.init.zeros_(self.cout.bias)

    def forward(self, x, t, y):
        """x: (B,1,H,W) x_t; t: (B,) in [0,1]; y: (B,) class ids, K = unconditional. Returns v."""
        e = self.temb(timestep_embedding(t, self.ch)) + self.cemb(y)
        e = F.silu(e)
        h = self.cin(x)
        hs = [h]
        for blk in self.down:
            for layer in blk:
                h = layer(h, e) if isinstance(layer, ResBlock) else layer(h)
            hs.append(h)
        for layer in self.mid:
            h = layer(h, e) if isinstance(layer, ResBlock) else layer(h)
        for blk in self.up:
            h = torch.cat([h, hs.pop()], 1)
            for layer in blk:
                h = layer(h, e) if isinstance(layer, ResBlock) else layer(h)
        return self.cout(F.silu(self.nout(h)))


# ----------------------------------------------------------------------------- sampling
@torch.no_grad()
def guided_v(model, x, t, y, w, amp=True):
    """v with classifier-free guidance: v_u + w (v_c - v_u) (w = 1: plain conditional)."""
    B = x.shape[0]
    tt = t.expand(B) if t.dim() == 0 or t.numel() == 1 else t
    with torch.autocast("cuda", dtype=torch.bfloat16, enabled=amp and x.is_cuda):
        if w == 1:
            return model(x, tt, y).float()
        v = model(torch.cat([x, x]), torch.cat([tt, tt]), torch.cat([y, torch.full_like(y, model.K)])).float()
    vc, vu = v.chunk(2)
    return vu + w * (vc - vu)


@torch.no_grad()
def ddim(model, x, y, t_from, t_to, steps, w, clip=True, amp=True):
    """Deterministic DDIM (eta = 0) with guidance from t_from to t_to in `steps` uniform substeps.
    t_from / t_to: floats or (B,) tensors. Returns (x at t_to, last x0 estimate)."""
    B = x.shape[0]
    dev = x.device
    tf = torch.as_tensor(t_from, dtype=torch.float32, device=dev).expand(B)
    tt = torch.as_tensor(t_to, dtype=torch.float32, device=dev).expand(B)
    x0 = None
    for k in range(steps):
        t0 = tf + (tt - tf) * (k / steps)
        t1 = tf + (tt - tf) * ((k + 1) / steps)
        a0, s0 = alpha_sigma(t0)
        a1, s1 = alpha_sigma(t1)
        a0, s0, a1, s1 = (z.to(dev)[:, None, None, None] for z in (a0, s0, a1, s1))
        v = guided_v(model, x, t0, y, w, amp)
        x0 = a0 * x - s0 * v
        if clip:
            x0 = x0.clamp(-1, 1)
            eps = torch.where(s0 > 0, (x - a0 * x0) / s0.clamp(min=1e-8), s0 * x + a0 * v)
        else:
            eps = s0 * x + a0 * v
        x = a1 * x0 + s1 * eps
    return x, x0


@torch.no_grad()
def sample(model, y, steps=64, w=3.0, gen=None, clip=True, noise=None):
    B = y.shape[0]
    x = noise if noise is not None else torch.randn(B, 1, model.cfg["img"], model.cfg["img"],
                                                    generator=gen, device=y.device)
    x, x0 = ddim(model, x, y, 1.0, 0.0, steps, w, clip)
    return x
