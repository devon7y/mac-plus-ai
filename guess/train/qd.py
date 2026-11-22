"""Quick, Draw! data on the GPU and the batch rasterizer (follows guess/engine/raster.py exactly).

Data: the prep arrays made for Doodle (paint/train/prep_data.py) from the sketch-rnn .npz files
(Google, CC BY 4.0): {split}_xy int16 [R,2] (dx, dy in the 255 box, RDP 2.0), {split}_pen uint8 [R]
(pen lifted after this point), {split}_off int64 [N+1], {split}_cat int16 [N], meta.json.
The first point of a drawing is implicit at (0, 0).
"""
import json
import math
import os

import numpy as np
import torch

G, MARGIN, MINSIDE = 32, 1, 48
E = G - 1 - 2 * MARGIN


class Split:
    def __init__(self, prep, split, cats, device, max_per_cat=None):
        meta = json.load(open(os.path.join(prep, "meta.json")))
        allcats = meta["categories"]
        self.cats = cats or allcats
        ids = {c: i for i, c in enumerate(allcats)}
        remap = np.full(len(allcats), -1, np.int64)
        for j, c in enumerate(self.cats):
            remap[ids[c]] = j
        cat = np.load(os.path.join(prep, f"{split}_cat.npy"))
        lab = remap[cat.astype(np.int64)]
        sel = np.flatnonzero(lab >= 0)
        if max_per_cat:
            keep = []
            for j in range(len(self.cats)):
                s = sel[lab[sel] == j]
                keep.append(s[:max_per_cat])
            sel = np.sort(np.concatenate(keep))
        self.sel = torch.from_numpy(sel).to(device)
        self.lab = torch.from_numpy(lab).to(device)
        self.xy = torch.from_numpy(np.load(os.path.join(prep, f"{split}_xy.npy"))).to(device)
        self.pen = torch.from_numpy(np.load(os.path.join(prep, f"{split}_pen.npy"))).to(device)
        self.off = torch.from_numpy(np.load(os.path.join(prep, f"{split}_off.npy"))).to(device)
        self.device = device
        self.n = len(sel)

    def gather(self, idx):
        """Drawings idx (indices into the split) -> points p (B, Lb+1, 2) int64, lift (B, Lb+1),
        npts (B,)."""
        start = self.off[idx]
        L = self.off[idx + 1] - start
        Lb = int(L.max())
        ar = torch.arange(Lb, device=self.device)
        m = ar[None] < L[:, None]
        rows = (start[:, None] + ar[None]).clamp(max=self.xy.shape[0] - 1)
        d = self.xy[rows].long() * m[..., None]
        z = torch.zeros(len(idx), 1, 2, dtype=torch.long, device=self.device)
        p = torch.cat([z, d.cumsum(1)], 1)
        lift = torch.cat([z[..., 0], self.pen[rows].long() * m], 1)
        return p, lift, L + 1


def rdiv(a, b):
    return torch.div(2 * a + b, 2 * b, rounding_mode="floor")


def augment(p, gen=None):
    """Mild random aspect change and rotation (canvas units), rounded back to integers."""
    B = p.shape[0]
    dev = p.device
    th = (torch.rand(B, device=dev, generator=gen) - 0.5) * (2 * math.radians(8))
    sx = torch.exp((torch.rand(B, device=dev, generator=gen) - 0.5) * 0.3)
    sy = 1.0 / sx
    c, s = torch.cos(th), torch.sin(th)
    x, y = p[..., 0].float(), p[..., 1].float()
    x2 = (c[:, None] * x - s[:, None] * y) * sx[:, None]
    y2 = (s[:, None] * x + c[:, None] * y) * sy[:, None]
    return torch.stack([x2, y2], -1).round().long()


def rasterize(p, lift, t):
    """p (B, P, 2) points, lift (B, P) pen lifted after point k, t (B,) points used (>= 1).
    Returns (B, G, G) uint8 images, exactly raster.py's."""
    B, P, _ = p.shape
    dev = p.device
    ar = torch.arange(P, device=dev)
    valid = ar[None] < t[:, None]
    big = 1 << 30
    x, y = p[..., 0], p[..., 1]
    minx = torch.where(valid, x, big).min(1).values
    maxx = torch.where(valid, x, -big).max(1).values
    miny = torch.where(valid, y, big).min(1).values
    maxy = torch.where(valid, y, -big).max(1).values
    w, h = maxx - minx, maxy - miny
    side = torch.maximum(torch.maximum(w, h), torch.full_like(w, MINSIDE))
    ox = MARGIN + torch.div(E - rdiv(w * E, side), 2, rounding_mode="floor")
    oy = MARGIN + torch.div(E - rdiv(h * E, side), 2, rounding_mode="floor")
    xr = (x - minx[:, None]).clamp(min=0)
    yr = (y - miny[:, None]).clamp(min=0)
    gx = ox[:, None] + rdiv(xr * E, side[:, None])
    gy = oy[:, None] + rdiv(yr * E, side[:, None])
    gx = torch.where(valid, gx, 0).clamp(0, G - 1)
    gy = torch.where(valid, gy, 0).clamp(0, G - 1)
    img = torch.zeros(B * G * G, dtype=torch.uint8, device=dev)
    base = (torch.arange(B, device=dev) * G * G)[:, None]
    img[(base + gy * G + gx)[valid]] = 1
    if P > 1:
        seg = (ar[None, :-1] + 1 < t[:, None]) & (lift[:, :-1] == 0)
        ax, ay, bx, by = gx[:, :-1], gy[:, :-1], gx[:, 1:], gy[:, 1:]
        dx, dy = bx - ax, by - ay
        adx, ady = dx.abs(), dy.abs()
        n = torch.maximum(adx, ady)
        n1 = n.clamp(min=1)[..., None]
        i = torch.arange(E + 1, device=dev)
        sx = torch.where(dx >= 0, 1, -1)[..., None]
        sy = torch.where(dy >= 0, 1, -1)[..., None]
        px = ax[..., None] + sx * torch.div(2 * i * adx[..., None] + n[..., None], 2 * n1, rounding_mode="floor")
        py = ay[..., None] + sy * torch.div(2 * i * ady[..., None] + n[..., None], 2 * n1, rounding_mode="floor")
        m = seg[..., None] & (i <= n[..., None])
        img[(base[..., None] + py * G + px)[m]] = 1
    return img.view(B, G, G)


def sample_t(npts, frac_full, gen=None):
    """Random prefix lengths: frac_full of the batch whole drawings, the rest uniform."""
    B = npts.shape[0]
    dev = npts.device
    u = torch.rand(B, device=dev, generator=gen)
    t = 1 + (u * npts.float()).long().clamp(max=npts - 1)
    full = torch.rand(B, device=dev, generator=gen) < frac_full
    return torch.where(full, npts, t)


def batch(split, B, frac_full=0.5, aug=True, gen=None):
    j = torch.randint(0, split.n, (B,), device=split.device, generator=gen)
    idx = split.sel[j]
    p, lift, npts = split.gather(idx)
    if aug:
        p = augment(p, gen)
    t = sample_t(npts, frac_full, gen)
    img = rasterize(p, lift, t)
    return img, split.lab[idx]


def eval_batches(split, B, frac):
    """Deterministic pass over the split: frac=1.0 whole drawings, else ceil(frac * npts) points."""
    for s in range(0, split.n, B):
        idx = split.sel[s:s + B]
        p, lift, npts = split.gather(idx)
        t = npts if frac >= 1 else torch.ceil(npts.float() * frac).long().clamp(min=1)
        yield rasterize(p, lift, t), split.lab[idx], idx
