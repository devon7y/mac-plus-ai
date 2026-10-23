#!/usr/bin/env python3
"""Shared pieces of Dream's training code: noise schedule, data loading (real splits and a synthetic
shape set for debugging), EMA, a small AdamW with per-element step scales, and sample sheets.

Pixels: uint8 levels 0..15 (0 = black, 15 = white) <-> x in [-1, 1] with x = level / 7.5 - 1.
Schedule (continuous time t in [0, 1]): alpha = cos(pi t / 2), sigma = sin(pi t / 2),
x_t = alpha x0 + sigma eps, v = alpha eps - sigma x0.
"""
import json
import math
import os
import sys

import numpy as np
import torch
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "data_pipeline"))


# ----------------------------------------------------------------------------- schedule
def alpha_sigma(t):
    """Cosine schedule with exact end points (alpha(1) = 0, sigma(0) = 0)."""
    t = torch.as_tensor(t, dtype=torch.float32)
    a = torch.cos(0.5 * math.pi * t)
    s = torch.sin(0.5 * math.pi * t)
    a = torch.where(t >= 1, torch.zeros_like(a), a)
    s = torch.where(t <= 0, torch.zeros_like(s), s)
    return a, s


def time_grid(n, kind="angle"):
    """Student time grid t_0 = 1 > t_1 > ... > t_n = 0 (list of python floats).
    angle:  uniform in t (= uniform in the cosine angle, the DDIM/progressive-distillation default)
    logsnr: uniform in log-SNR between -/+ L at t_1 / t_{n-1}, L = log-SNR of the angle grid's t_1
    """
    if kind == "angle":
        return [1.0 - i / n for i in range(n + 1)]
    if kind.startswith("shift"):           # shift<k>: angle grid with t -> t warped towards high noise
        k = float(kind[5:])
        u = [1.0 - i / n for i in range(n + 1)]
        # warp keeps 0 and 1, moves interior points towards 1 (k > 1) using the SNR shift formula
        out = []
        for x in u:
            if x in (0.0, 1.0):
                out.append(x)
                continue
            a, s = math.cos(0.5 * math.pi * x), math.sin(0.5 * math.pi * x)
            lsnr = 2 * math.log(a / s) - 2 * math.log(k)
            out.append(2 / math.pi * math.atan(math.exp(-lsnr / 2)))
        return out
    raise ValueError(kind)


def levels_to_x(lv):
    return lv.float() / 7.5 - 1.0


def x_to_levels(x):
    return ((x.clamp(-1, 1) + 1) * 7.5).round().to(torch.uint8)


# ----------------------------------------------------------------------------- data
class Split:
    """images (N, R, R) uint8 levels, labels (N,) int64 subject ids (index into names, which for real data
    is the full subject list, so the ids are the Mac's), names[K]; present = the ids that occur."""

    def __init__(self, images, labels, names, orig=None):
        self.images = images
        self.labels = labels
        self.names = list(names)
        self.orig = list(orig) if orig is not None else list(range(len(names)))
        self.present = sorted(set(int(x) for x in labels))

    @property
    def K(self):
        return len(self.names)

    def __len__(self):
        return len(self.labels)


def read_subjects(path=None):
    """The data pipeline's subject list (one per line, first tab field), as prompts.read_list reads it."""
    path = path or os.path.join(HERE, "..", "data_pipeline", "subjects.txt")
    with open(path) as f:
        return [l.split("\t")[0].strip() for l in f if l.strip()]


def load_split(path, res=64, subjects=None):
    """A split written by data_pipeline/process.write_split (images{res}.npy, labels.npy, meta.jsonl) or
    an .npz with images{res}, labels (and optionally subjects). Labels index the full subject list
    (subjects.txt, 261 names) and are kept as they are: they are the subject ids the Mac uses."""
    if path.endswith(".npz"):                       # pilot64.npz or a pack_split.py pack
        z = np.load(path)
        imgs, lab = z[f"images{res}"], z["labels"].astype(np.int64)
        if subjects or "subjects" not in z:
            names = read_subjects(subjects)
        elif z["subjects"].dtype == np.uint8:        # pack: the bytes of subjects.txt
            names = [l.split("\t")[0].strip() for l in z["subjects"].tobytes().decode().splitlines() if l.strip()]
        else:
            names = [str(x) for x in z["subjects"]]
    else:
        imgs = np.load(os.path.join(path, f"images{res}.npy"))
        lab = np.load(os.path.join(path, "labels.npy")).astype(np.int64)
        own = os.path.join(path, "subjects.txt")
        names = read_subjects(subjects or (own if os.path.exists(own) else None))
        mp = os.path.join(path, "meta.jsonl")
        if os.path.exists(mp):                      # sanity check: meta subject == names[label]
            with open(mp) as f:
                bad = sum(json.loads(line).get("subject", names[int(l)]) != names[int(l)] for l, line in zip(lab, f))
            if bad:
                raise ValueError(f"{bad} labels disagree with meta.jsonl subjects; wrong subject list?")
    assert lab.max() < len(names), "label out of range of the subject list"
    return Split(imgs, lab, names)


SYNTH_CLASSES = ["circle", "square", "triangle", "star", "house", "tree", "mushroom", "fish", "car",
                 "cross", "ring", "arrow", "moon", "diamond", "hexagon", "flower", "sun", "cup",
                 "bicycle", "umbrella"]


def _synth_one(rng, cls):
    """Draw one 256x256 'clip art' shape (bold black outline, light fills) and post-process it like
    the real data (process.py): crop to ink, pad square, area-average to 64/32, 16 levels."""
    from PIL import ImageDraw
    S = 256
    im = Image.new("L", (S, S), 255)
    d = ImageDraw.Draw(im)
    lw = int(rng.integers(5, 13))
    fill = int(rng.choice([255, 255, 220, 180, 140, 90]))
    fill2 = int(rng.choice([255, 200, 150, 60, 0]))
    cx, cy = S / 2 + rng.normal(0, 6), S / 2 + rng.normal(0, 6)
    r = rng.uniform(60, 95)
    rot = rng.uniform(-0.25, 0.25)

    def poly(pts, f=fill, w=lw):
        c, s = math.cos(rot), math.sin(rot)
        q = [(cx + (x * c - y * s), cy + (x * s + y * c)) for x, y in pts]
        d.polygon(q, fill=f, outline=0, width=w)

    def ell(x0, y0, x1, y1, f=fill, w=lw):
        d.ellipse([cx + x0, cy + y0, cx + x1, cy + y1], fill=f, outline=0, width=w)

    if cls == "circle":
        ell(-r, -r, r, r)
    elif cls == "square":
        poly([(-r, -r), (r, -r), (r, r), (-r, r)])
    elif cls == "triangle":
        poly([(0, -r), (r, r * 0.8), (-r, r * 0.8)])
    elif cls == "star":
        pts = []
        for k in range(10):
            a = -math.pi / 2 + k * math.pi / 5
            rr = r if k % 2 == 0 else r * rng.uniform(0.38, 0.5)
            pts.append((rr * math.cos(a), rr * math.sin(a)))
        poly(pts)
    elif cls == "house":
        poly([(-r * 0.7, -r * 0.1), (r * 0.7, -r * 0.1), (r * 0.7, r), (-r * 0.7, r)])
        poly([(-r * 0.9, -r * 0.1), (0, -r), (r * 0.9, -r * 0.1)], f=fill2)
        poly([(-r * 0.15, r * 0.4), (r * 0.15, r * 0.4), (r * 0.15, r), (-r * 0.15, r)], f=255)
    elif cls == "tree":
        poly([(-r * 0.15, 0), (r * 0.15, 0), (r * 0.15, r), (-r * 0.15, r)], f=fill2)
        ell(-r * 0.75, -r, r * 0.75, r * 0.3)
    elif cls == "mushroom":
        poly([(-r * 0.25, 0), (r * 0.25, 0), (r * 0.3, r * 0.9), (-r * 0.3, r * 0.9)], f=255)
        d.chord([cx - r, cy - r * 0.9, cx + r, cy + r * 0.5], 180, 360, fill=fill2, outline=0, width=lw)
        for _ in range(3):
            x, y = rng.uniform(-r * 0.6, r * 0.6), rng.uniform(-r * 0.6, -r * 0.3)
            ell(x - 8, y - 8, x + 8, y + 8, f=255, w=3)
    elif cls == "fish":
        ell(-r, -r * 0.5, r * 0.5, r * 0.5)
        poly([(r * 0.45, 0), (r, -r * 0.5), (r, r * 0.5)], f=fill2)
        ell(-r * 0.7, -r * 0.2, -r * 0.5, 0, f=0, w=1)
    elif cls == "car":
        poly([(-r, -r * 0.1), (r, -r * 0.1), (r, r * 0.4), (-r, r * 0.4)])
        poly([(-r * 0.5, -r * 0.1), (-r * 0.3, -r * 0.5), (r * 0.4, -r * 0.5), (r * 0.6, -r * 0.1)], f=255)
        for x in (-r * 0.55, r * 0.55):
            ell(x - r * 0.25, r * 0.2, x + r * 0.25, r * 0.7, f=fill2)
    elif cls == "cross":
        a = r * rng.uniform(0.25, 0.4)
        poly([(-a, -r), (a, -r), (a, -a), (r, -a), (r, a), (a, a), (a, r), (-a, r), (-a, a), (-r, a),
              (-r, -a), (-a, -a)])
    elif cls == "ring":
        ell(-r, -r, r, r, f=fill)
        q = r * rng.uniform(0.4, 0.65)
        ell(-q, -q, q, q, f=255)
    elif cls == "arrow":
        a = r * 0.25
        poly([(-r, -a), (r * 0.2, -a), (r * 0.2, -r * 0.6), (r, 0), (r * 0.2, r * 0.6), (r * 0.2, a), (-r, a)])
    elif cls == "moon":
        ell(-r, -r, r, r, f=fill2 if fill2 < 200 else 120)
        o = r * rng.uniform(0.35, 0.6)
        d.ellipse([cx - r + o, cy - r - o * 0.3, cx + r + o, cy + r - o * 0.3], fill=255, outline=255)
    elif cls == "diamond":
        poly([(0, -r), (r * 0.7, 0), (0, r), (-r * 0.7, 0)])
    elif cls == "hexagon":
        poly([(r * math.cos(k * math.pi / 3), r * math.sin(k * math.pi / 3)) for k in range(6)])
    elif cls == "flower":
        q = r * 0.38
        for k in range(int(rng.integers(5, 8))):
            a = k * 2 * math.pi / 6
            x, y = r * 0.55 * math.cos(a), r * 0.55 * math.sin(a)
            ell(x - q, y - q, x + q, y + q, f=255)
        ell(-q * 0.8, -q * 0.8, q * 0.8, q * 0.8, f=fill2 if fill2 < 255 else 100)
    elif cls == "sun":
        for k in range(10):
            a = k * 2 * math.pi / 10
            d.line([cx + r * 0.6 * math.cos(a), cy + r * 0.6 * math.sin(a), cx + r * math.cos(a),
                    cy + r * math.sin(a)], fill=0, width=lw)
        ell(-r * 0.5, -r * 0.5, r * 0.5, r * 0.5)
    elif cls == "cup":
        poly([(-r * 0.6, -r * 0.6), (r * 0.5, -r * 0.6), (r * 0.4, r * 0.7), (-r * 0.5, r * 0.7)])
        d.arc([cx + r * 0.25, cy - r * 0.35, cx + r * 0.95, cy + r * 0.35], 270, 90, fill=0, width=lw)
    elif cls == "bicycle":
        for x in (-r * 0.55, r * 0.55):
            ell(x - r * 0.42, r * 0.0, x + r * 0.42, r * 0.84, f=255)
        for (x0, y0, x1, y1) in [(-r * 0.55, r * 0.42, 0, r * 0.42), (0, r * 0.42, r * 0.55, r * 0.42),
                                 (-r * 0.2, -r * 0.1, 0, r * 0.42), (-r * 0.2, -r * 0.1, r * 0.4, -r * 0.1),
                                 (r * 0.4, -r * 0.1, r * 0.55, r * 0.42), (r * 0.4, -r * 0.1, r * 0.35, -r * 0.35)]:
            d.line([cx + x0, cy + y0, cx + x1, cy + y1], fill=0, width=lw)
    elif cls == "umbrella":
        d.chord([cx - r, cy - r * 0.8, cx + r, cy + r * 0.6], 180, 360, fill=fill, outline=0, width=lw)
        d.line([cx, cy - r * 0.1, cx, cy + r * 0.8], fill=0, width=lw)
        d.arc([cx - r * 0.3, cy + r * 0.55, cx, cy + r * 0.95], 0, 180, fill=0, width=lw)
    import process
    g = np.asarray(im, np.float32) / 255.0
    if rng.random() < 0.5:
        g = g[:, ::-1]
    sq = process.crop_square(process.whiten(g))
    return process.quantize(process.area_resize(sq, 64)), process.quantize(process.area_resize(sq, 32))


def synth_split(n_per_class=500, seed=0, res=64, classes=None):
    classes = classes or SYNTH_CLASSES
    rng = np.random.default_rng(seed)
    imgs, labs = [], []
    for k, c in enumerate(classes):
        for _ in range(n_per_class):
            a64, a32 = _synth_one(rng, c)
            imgs.append(a64 if res == 64 else a32)
            labs.append(k)
    return Split(np.stack(imgs), np.array(labs, np.int64), classes)


def get_split(spec, res=64):
    """'synth[:n_per_class]' or a split directory."""
    if spec.startswith("synth"):
        n = int(spec.split(":")[1]) if ":" in spec else 500
        return synth_split(n, res=res)
    return load_split(spec, res)


class GPUData:
    """The whole split as a uint8 tensor on the GPU; random batches with horizontal flips."""

    def __init__(self, split, device):
        self.x = torch.tensor(split.images, dtype=torch.uint8, device=device)
        self.y = torch.tensor(split.labels, dtype=torch.long, device=device)
        self.device = device

    def batch(self, B, flip=True):
        i = torch.randint(0, len(self.y), (B,), device=self.device)
        x = levels_to_x(self.x[i])[:, None]
        if flip:
            f = torch.rand(B, device=self.device) < 0.5
            x = torch.where(f[:, None, None, None], x.flip(-1), x)
        return x, self.y[i]


# ----------------------------------------------------------------------------- EMA / optimiser
class EMA:
    def __init__(self, model, decay):
        self.decay = decay
        self.shadow = {k: v.detach().clone().float() for k, v in model.state_dict().items()}

    @torch.no_grad()
    def update(self, model, step):
        d = min(self.decay, (1 + step) / (10 + step))
        for k, v in model.state_dict().items():
            if v.dtype.is_floating_point:
                self.shadow[k].mul_(d).add_(v.detach().float(), alpha=1 - d)
            else:
                self.shadow[k].copy_(v)

    def state_dict(self):
        return self.shadow


class ScaledAdamW(torch.optim.Optimizer):
    """AdamW whose step is multiplied element-wise by a fixed 'scale' tensor per parameter (default 1).
    Adam's steps are about lr in size whatever the gradient scale, so integer-unit parameters (biases
    worth tens or thousands of activation units) need a per-element step size."""

    def __init__(self, params, lr=1e-3, betas=(0.9, 0.99), eps=1e-8, weight_decay=0.0):
        super().__init__(params, dict(lr=lr, betas=betas, eps=eps, weight_decay=weight_decay, scale=None))

    @torch.no_grad()
    def step(self):
        for g in self.param_groups:
            b1, b2 = g["betas"]
            for p in g["params"]:
                if p.grad is None:
                    continue
                st = self.state[p]
                if not st:
                    st["t"] = 0
                    st["m"] = torch.zeros_like(p)
                    st["v"] = torch.zeros_like(p)
                st["t"] += 1
                m, v = st["m"], st["v"]
                m.mul_(b1).add_(p.grad, alpha=1 - b1)
                v.mul_(b2).addcmul_(p.grad, p.grad, value=1 - b2)
                mh = m / (1 - b1 ** st["t"])
                vh = v / (1 - b2 ** st["t"])
                upd = mh / (vh.sqrt() + g["eps"])
                if g["weight_decay"]:
                    upd = upd + g["weight_decay"] * p
                sc = g["scale"]
                if sc is not None:
                    upd = upd * sc
                p.sub_(upd * g["lr"])


# ----------------------------------------------------------------------------- sheets
def to_u8(x, levels=None):
    """x in [-1, 1] (any shape) -> uint8 0..255 (white = 255). levels=16 quantises like the Mac."""
    x = x.detach().float().clamp(-1, 1)
    if levels:
        q = ((x + 1) * (levels - 1) / 2).round() / (levels - 1)
        return (q * 255).round().to(torch.uint8).cpu().numpy()
    return ((x + 1) * 127.5).round().to(torch.uint8).cpu().numpy()


def grid_image(rows, scale=2, pad=2, labels=None, label_w=110):
    """rows: list of lists of (H, W) uint8 arrays -> PIL image (nearest-neighbour upscaling), optional
    row labels on the left."""
    from PIL import ImageDraw
    H, W = rows[0][0].shape
    ncol = max(len(r) for r in rows)
    lw = label_w if labels else 0
    cw, ch = W * scale + pad, H * scale + pad
    out = Image.new("L", (lw + ncol * cw + pad, len(rows) * ch + pad), 128)
    d = ImageDraw.Draw(out)
    for r, row in enumerate(rows):
        for c, a in enumerate(row):
            im = Image.fromarray(a).resize((W * scale, H * scale), Image.NEAREST)
            out.paste(im, (lw + pad + c * cw, pad + r * ch))
        if labels:
            d.text((4, pad + r * ch + ch // 2 - 6), str(labels[r])[:16], fill=255)
    return out
