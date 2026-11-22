#!/usr/bin/env python3
"""QAT checkpoint (train/gnet.py) -> Guess My Doodle model file, plus the integer model's accuracy.

  export.py CKPT OUT.gmdl [--prep DIR --eval N]     (N validation drawings per class, 0 = all)

Folding (every ReLU layer): the ternary sum acc (weights t, per-channel scale alpha) of inputs that
are integers at step s_in becomes  y = gamma (alpha s_in acc - mu) / sigma + beta  and the next
integer activation is clamp(round(y / s_out), 0, A), i.e. clamp(floor(acc g + h + 1/2)) with
  g = gamma alpha s_in / (sigma s_out),  h = (beta - gamma mu / sigma) / s_out.
On the Mac: v = (acc M + B) >> S with M = round(g 2^S) (|M| < 2^15), B = round((h + 1/2) 2^S).
A negative g is made positive by negating the channel's weights (needed for max-pool fusion).
Output layer: logits in units of 1/LOGIT_SCALE.
The first layer becomes a table of the activations of all 512 binary 3x3 patterns.
"""
import argparse
import math
import os
import sys

import numpy as np
import torch
import torch.nn.functional as F

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "train"))
sys.path.insert(0, HERE)
import gnet  # noqa: E402
import layout  # noqa: E402
import model_file  # noqa: E402

LOGIT_SCALE = 32

# How the game says each category: "Draw a bicycle", "an apple", "the Eiffel Tower", "scissors".
DISPLAY = {"teddy-bear": "teddy bear", "rollerskates": "roller skates"}
NO_ARTICLE = {"asparagus", "binoculars", "bread", "broccoli", "drums", "eyeglasses", "flip flops", "grapes",
              "grass", "headphones", "ice cream", "lightning", "matches", "pants", "peas", "pliers", "rain",
              "rollerskates", "scissors", "shorts", "stairs", "stitches", "underwear", "yoga", "camouflage",
              "animal migration"}
AN = {"aircraft carrier", "airplane", "alarm clock", "ambulance", "angel", "ant", "anvil", "apple", "arm", "axe",
      "ear", "elbow", "elephant", "envelope", "eraser", "eye", "hourglass", "octagon", "octopus", "onion", "oven",
      "owl", "umbrella", "ocean"}


def say_name(cat):
    """'a|bicycle', 'an|apple', 'the|Eiffel Tower', '|scissors' (the model file's names)."""
    if cat.startswith("The "):
        return "the|" + cat[4:]
    art = "" if cat in NO_ARTICLE else "an" if cat in AN else "a"
    return art + "|" + DISPLAY.get(cat, cat)


def mbs(g, h):
    """Per-channel (M, S, B) for v = floor(acc g + h + 1/2)."""
    M, S, B = [], [], []
    for gi, hi in zip(g, h):
        gi = abs(float(gi))
        s = 30 if gi == 0 else int(math.floor(math.log2(32767.0 / gi)))
        s = min(s, 30, int(math.floor(math.log2((2 ** 30) / (abs(float(hi)) + 1)))))
        s = max(s, 1)
        m = int(round(gi * 2 ** s))
        while m > 32767:
            s -= 1
            m = int(round(gi * 2 ** s))
        M.append(m)
        S.append(s)
        B.append(int(round((float(hi) + 0.5) * 2 ** s)))
    return np.array(M), np.array(S), np.array(B)


def bn_fold(bn):
    sig = torch.sqrt(bn.running_var + bn.eps)
    return bn.weight / sig, bn.bias - bn.weight * bn.running_mean / sig


def export(ckpt):
    sd = torch.load(ckpt, map_location="cpu")
    spec, cats = sd["arch"], sd["cats"]
    net = gnet.Net(spec, len(cats), sd.get("afull", False))
    net.load_state_dict(sd["model"])
    net.set_quant(True)
    net.eval()
    tensors, layers = layout.layout(spec, len(cats))
    li = 0
    s_in = 1.0
    stats = []
    with torch.no_grad():
        for L, mod in zip(net.kinds, net.layers):
            k = L[0]
            if k == "f":
                continue
            l = layers[li]
            li += 1
            if k == "t":
                pats = ((torch.arange(512)[:, None] >> (8 - torch.arange(9))[None]) & 1).float().view(512, 1, 3, 3)
                y = mod["bn"](F.conv2d(pats, mod["conv"].weight))
                s_out = float(mod["act"].s.abs())
                A = mod["act"].A
                l["A"] = A
                l["table"] = torch.clamp(torch.round(F.relu(y) / s_out), 0, A).view(512, -1).long().numpy()
                l["stride"] = L[2]
                s_in = s_out
            elif k in ("d", "c", "l", "o"):
                lin = mod["fc"] if k in ("l", "o") else mod["conv"]
                t, alpha = lin.qweight()
                t = t.clone()
                alpha = alpha.view(-1)
                if k == "o":
                    g = alpha * s_in * mod["oscale"].g * LOGIT_SCALE
                    h = mod["oscale"].b * LOGIT_SCALE
                    A = -1
                else:
                    gs, hb = bn_fold(mod["bn"])
                    s_out = float(mod["act"].s.abs())
                    g = gs * alpha * s_in / s_out
                    h = hb / s_out
                    A = mod["act"].A
                    s_in = s_out
                neg = g < 0
                t[neg] = -t[neg]
                g = g.abs()
                M, S, B = mbs(g.numpy(), h.numpy())
                if k in ("l", "o"):
                    w = t.view(t.shape[0], -1)          # gnet flattens channels-last, as the Mac stores
                elif k == "c":
                    w = t.permute(0, 2, 3, 1)                                   # [cout][ky][kx][cin]
                else:
                    w = t.view(t.shape[0], 9)
                l["w"] = w.reshape(-1).long().numpy().astype(np.int8)
                l["M"], l["S"], l["B"] = M, S, B
                l["A"] = A
                stats.append((L, float(g.min()), float(g.max()), float(h.abs().max()), int(S.min()), int(S.max()),
                              float((t == 0).float().mean())))
    m = dict(G=32, tensors=tensors, layers=layers, names=[say_name(c) for c in cats])
    return m, stats


# ----------------------------------------------------------------------------- batched integer model
def wrap16(a):
    return ((a + 32768) & 0xFFFF) - 32768


def wrap32(a):
    return ((a + (1 << 31)) & 0xFFFFFFFF) - (1 << 31)


class IntNet:
    """The integer model on the GPU (int64 / exact float64 sums): identical to ref.py."""

    def __init__(self, m, dev):
        self.m, self.dev = m, dev
        self.p = []
        for l in m["layers"]:
            d = {}
            if l["kind"] == "T":
                d["table"] = torch.tensor(np.asarray(l["table"]), dtype=torch.long, device=dev)
            if "w" in l:
                w = torch.tensor(np.asarray(l["w"], np.float64), device=dev)
                if l["kind"] == "C":
                    w = w.view(l["cout"], l["k"], l["k"], l["cin"]).permute(0, 3, 1, 2).contiguous()
                elif l["kind"] == "D":
                    w = w.view(l["cout"], 1, 3, 3)
                else:
                    w = w.view(l["cout"], l["cin"])
                d["w"] = w
                for f in ("M", "S", "B"):
                    d[f] = torch.tensor(np.asarray(l[f], np.int64), device=dev)
            self.p.append(d)
        self.pow = (2 ** (8 - torch.arange(9, device=dev))).double().view(1, 1, 3, 3)

    def rq(self, acc, l, d):
        """acc: (..., C) int64 with channels last."""
        if hasattr(self, "accmax"):
            k = id(l)
            self.accmax[k] = max(self.accmax.get(k, 0), int(acc.abs().max()))
        acc = wrap16(acc)
        p = wrap32(acc * d["M"] + d["B"])
        v = wrap16(p >> d["S"])
        return v if l["A"] < 0 else v.clamp(0, l["A"])

    def __call__(self, bits):
        """bits: (B, 32, 32) uint8 -> logits (B, ncls) int64."""
        x = bits
        for l, d in zip(self.m["layers"], self.p):
            k = l["kind"]
            if k == "T":
                st = l.get("stride", 1) or 1
                idx = F.conv2d(x.double().unsqueeze(1), self.pow, padding=1, stride=st).squeeze(1).long()
                x = d["table"][idx]                                      # (B, H, W, C)
            elif k == "P":
                B_, H, W, C = x.shape
                x = x.view(B_, H // 2, 2, W // 2, 2, C).amax(dim=(2, 4))
            elif k == "G":
                x = x.amax(dim=(1, 2), keepdim=True)
            elif k == "L":
                acc = torch.round(x.reshape(x.shape[0], -1).double() @ d["w"].T).long()
                x = self.rq(acc, l, d).view(x.shape[0], 1, 1, -1)
            else:
                xc = x.permute(0, 3, 1, 2).double()
                if k == "D":
                    acc = F.conv2d(xc, d["w"], padding=1, groups=l["cout"])
                else:
                    acc = F.conv2d(xc, d["w"], padding=l["k"] // 2, stride=l["stride"])
                x = self.rq(torch.round(acc).long().permute(0, 2, 3, 1), l, d)
        return x.reshape(x.shape[0], -1)


def cat_of(name):
    """The Quick, Draw! category of a model file name ('the|Eiffel Tower' -> 'The Eiffel Tower')."""
    art, nm = name.split("|", 1)
    if art == "the":
        return "The " + nm
    return {v: k for k, v in DISPLAY.items()}.get(nm, nm)


def evaluate(m, prep, per, dev, split="valid"):
    import qd
    va = qd.Split(prep, split, [cat_of(n) for n in m["names"]], dev, per or None)
    net = IntNet(m, dev)
    res = {}
    for fr in (1.0, 0.75, 0.5, 0.25):
        n = c1 = c3 = 0
        for img, lab, _ in qd.eval_batches(va, 2048, fr):
            lo = net(img)
            top = lo.topk(3, 1).indices
            c1 += int((top[:, 0] == lab).sum())
            c3 += int((top == lab[:, None]).any(1).sum())
            n += len(lab)
        res[fr] = (c1 / n, c3 / n)
    return res, va


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ckpt")
    ap.add_argument("out")
    ap.add_argument("--prep", default="")
    ap.add_argument("--eval", type=int, default=-1, help="validation drawings per class (0 = all; -1 = no eval)")
    ap.add_argument("--check", type=int, default=8, help="drawings checked against ref.py")
    ap.add_argument("--split", default="valid", help="valid or test")
    ap.add_argument("--no-write", action="store_true", help="evaluate an existing model file (OUT)")
    ap.add_argument("--accstats", action="store_true", help="print the largest |sum| per layer (validation)")
    a = ap.parse_args()
    if not a.no_write:
        m, stats = export(a.ckpt)
        n = model_file.write(a.out, m)
        print(f"{a.out}: {n} bytes, {len(m['names'])} classes")
        for s in stats:
            print("  %-14s g %.4g..%.4g |h| <= %.1f  S %d..%d  zeros %.2f" % s)
    if a.eval >= 0:
        dev = "cuda" if torch.cuda.is_available() else "cpu"
        m2 = model_file.read(a.out)
        res, va = evaluate(m2, a.prep, a.eval, dev, a.split)
        for fr, (t1, t3) in res.items():
            print(f"INT {a.split} {fr:.2f} of the drawing: top-1 {t1:.4f} top-3 {t3:.4f} ({va.n} drawings)")
        if a.accstats:
            import qd
            net = IntNet(m2, dev)
            net.accmax = {}
            for fr in (1.0, 0.5):
                for img, lab, _ in qd.eval_batches(va, 2048, fr):
                    net(img)
            for l in m2["layers"]:
                if id(l) in net.accmax:
                    print(f"  {l['kind']} {l['cin']}->{l['cout']} A={l['A']}: max |sum| {net.accmax[id(l)]}")
        if a.check:
            import qd
            import ref
            net = IntNet(m2, dev)
            idx = va.sel[torch.randperm(va.n, generator=torch.Generator().manual_seed(0))[:a.check].to(dev)]
            p, lift, npts = va.gather(idx)
            t = torch.ceil(npts.float() * 0.6).long()
            img = qd.rasterize(p, lift, t)
            lo = net(img).cpu().numpy()
            bad = sum(int((ref.run(m2, img[i].cpu().numpy()) != lo[i]).any()) for i in range(len(idx)))
            print(f"batched integer model vs ref.py on {len(idx)} drawings: {'BIT-EXACT' if not bad else f'{bad} MISMATCHES'}")


if __name__ == "__main__":
    main()
