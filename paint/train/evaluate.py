#!/usr/bin/env python3
"""Evaluate a trained decoder: validation loss, quantized-activation simulation, sample grids,
completions of held-out partial drawings (PNG), and an integer-code export for the Mac.

  python evaluate.py --prep data/prep --ckpt runs/h256_tern/final.pt --out eval/h256_tern

Outputs in --out: metrics.json, grid_a.png / grid_b.png (10 categories x 6 samples each,
temperature --temp), complete_a.png / complete_b.png (per row: original drawing in grey,
the prompt = first 40% of its strokes, then 4 completions: prompt black, model red),
export.npz (integer codes + per-row scales, see sketch_model.export_codes).
Every cell shows the 256x256 AI frame (grey square) at half scale.
"""
import argparse
import json
import math
import os

import numpy as np
import torch
from PIL import Image, ImageDraw, ImageFont

from sketch_model import NCAT, GPUData, SketchDecoder, export_codes, seq_loss, split_params

GRID_CATS = ["cat", "bicycle", "The Eiffel Tower", "house", "tree", "face", "car", "fish",
             "flower", "airplane", "apple", "butterfly", "clock", "cup", "guitar", "pig",
             "rabbit", "sailboat", "snowman", "umbrella"]
COMP_CATS = ["cat", "bicycle", "house", "tree", "face", "car", "fish", "flower", "airplane",
             "butterfly", "clock", "pig", "rabbit", "snowman", "umbrella", "dog", "bird",
             "teapot", "truck", "lighthouse"]
CELL, FR, MARGIN = 160, 128, 16          # cell px, frame px (256 -> 128), margin


# ------------------------------------------------------------------ losses
def val_loss(model, data, B=2048, seed=1234):
    gen = torch.Generator(device=data.device)
    gen.manual_seed(seed)
    tot = np.zeros(3)
    with torch.no_grad():
        for b in data.batches(B, None, shuffle=False):
            inp, tgt, n, cat = data.make(b, dequant=True, gen=gen)
            a, c, k = seq_loss(model(inp, cat), tgt, n)
            tot += [a.item(), c.item(), k.item()]
    return {"loss": (tot[0] + tot[1]) / tot[2], "off": tot[0] / tot[2], "pen": tot[1] / tot[2]}


def step_loss(model, data, hq=None, B=4096, seed=1234):
    """Same loss computed with the step-by-step cell (optionally quantizing h each step)."""
    gen = torch.Generator(device=data.device)
    gen.manual_seed(seed)
    tot = np.zeros(3)
    with torch.no_grad():
        for b in data.batches(B, None, shuffle=False):
            inp, tgt, n, cat = data.make(b, dequant=True, gen=gen)
            W, h, gc = model.step_consts(cat)
            if hq is not None:
                h = hq(h)
            outs = []
            for t in range(inp.shape[1]):
                h, o = model.step(W, gc, h, inp[:, t], hq)
                outs.append(o)
            a, c, k = seq_loss(torch.stack(outs, 1), tgt, n)
            tot += [a.item(), c.item(), k.item()]
    return {"loss": (tot[0] + tot[1]) / tot[2], "off": tot[0] / tot[2], "pen": tot[1] / tot[2]}


def hq_int8(h):
    return torch.floor(h * 127) / 127


def auto_A(model):
    """FORMAT.md: A = min(127, floor(32767 / max row L1)) over the codes of Wh and Wo
    (overflow-free 16-bit accumulation)."""
    L = 0
    for nm, kind in (("wh", model.kind_wh), ("wo", model.kind_wo)):
        c, _ = export_codes(getattr(model, nm), kind)
        c = np.abs(c.astype(np.int64))
        if kind == "tern_r2":                    # two separate sums per row
            c = c.reshape(-1, c.shape[-1]).sum(1)
        elif kind.startswith("tern_g"):          # each group is its own 16-bit sum
            g = int(kind[6:])
            c = c.reshape(c.shape[0], -1, g).sum(2)
        else:
            c = c.sum(1)
        L = max(L, int(c.max()))
    return min(127, 32767 // L), L


def wrap16(s):
    return torch.remainder(s + 32768, 65536) - 32768


def mac_loss(model, data, A=127, wrap=True, B=4096, seed=1234):
    """Loss with the Mac pipeline of paint/bench/ref.py approximated in float:
    hq = floor(h*A) (int), recurrent and output matvecs on the integer weight codes,
    exact integer sums (counted when |S| > 32767, wrapped to 16 bits if wrap), per-row
    scales, sigmoid inputs clamped to [-8,8) and binned at 1/64, tanh at [-4,4) and 1/128
    (table midpoints), outputs clamped to Q10 range. Input/category/bias terms stay float
    (16-bit on the Mac). Returns loss dict plus overflow rates."""
    dev = data.device
    cw = {}
    for nm, kind in (("wh", model.kind_wh), ("wo", model.kind_wo)):
        c, s = export_codes(getattr(model, nm), kind)
        if kind == "tern_r2":                # two terms -> two "groups" spanning all columns
            c = np.concatenate([c[0][:, None, :], c[1][:, None, :]], axis=1)  # [rows, 2, H]
            s = s.T                                                          # [rows, 2]
            ct = torch.tensor(c, dtype=torch.float32, device=dev)
            st = torch.tensor(s, dtype=torch.float32, device=dev) / A
            cw[nm] = (ct, st, True)
            continue
        g = int(kind[6:]) if kind.startswith("tern_g") else c.shape[1]
        c = torch.tensor(c, dtype=torch.float32, device=dev).view(c.shape[0], -1, g)
        s = torch.tensor(s, dtype=torch.float32, device=dev).view(c.shape[0], -1) / A
        cw[nm] = (c, s, False)               # codes [rows, groups, g], scales [rows, groups]

    def mv(hq, nm):
        """Integer partial sums [B, rows, groups or terms] and their scales."""
        c, s, terms = cw[nm]
        if terms:
            S = torch.einsum("bh,rkh->brk", hq, c)
        else:
            S = torch.einsum("bkg,rkg->brk", hq.view(hq.shape[0], c.shape[1], c.shape[2]), c)
        return S, s
    H = model.H
    gen = torch.Generator(device=dev)
    gen.manual_seed(seed)
    tot = np.zeros(3)
    ovf = np.zeros(4)            # rec overflows, rec sums, out overflows, out sums
    smax = np.zeros(2)           # largest |S| seen (recurrent, output)
    with torch.no_grad():
        for b in data.batches(B, None, shuffle=False):
            inp, tgt, n, cat = data.make(b, dequant=True, gen=gen)
            W, h, gc = model.step_consts(cat)
            T = inp.shape[1]
            live = (torch.arange(T, device=dev)[None] < n[:, None])
            outs = []
            for t in range(T):
                hq = torch.floor(h * A)
                S, sc = mv(hq, "wh")
                m = live[:, t:t + 1, None]
                ovf[0] += ((S.abs() > 32767) & m).sum().item()
                smax[0] = max(smax[0], (S.abs() * m).max().item())
                ovf[1] += (m.sum() * S.shape[1] * S.shape[2]).item()
                if wrap:
                    S = wrap16(S)
                gh = (S * sc[None]).sum(2) + model.b_hh
                gi = inp[:, t] @ W["wx"].t() + gc
                p = (gi[:, :2 * H] + gh[:, :2 * H]).clamp(-8, 8 - 1 / 1024)
                g = torch.sigmoid(torch.floor(p * 64) / 64 + 1 / 128)
                r, z = g[:, :H], g[:, H:]
                p = (gi[:, 2 * H:] + r * gh[:, 2 * H:]).clamp(-4, 4 - 1 / 1024)
                nn_ = torch.tanh(torch.floor(p * 128) / 128 + 1 / 256)
                h = nn_ + z * (h - nn_)
                hq = torch.floor(h * A)
                SY, sc = mv(hq, "wo")
                ovf[2] += ((SY.abs() > 32767) & m).sum().item()
                smax[1] = max(smax[1], (SY.abs() * m).max().item())
                ovf[3] += (m.sum() * SY.shape[1] * SY.shape[2]).item()
                if wrap:
                    SY = wrap16(SY)
                outs.append(((SY * sc[None]).sum(2) + model.bo).clamp(-32, 32 - 1 / 1024))
            a, c, k = seq_loss(torch.stack(outs, 1), tgt, n)
            tot += [a.item(), c.item(), k.item()]
    return {"loss": (tot[0] + tot[1]) / tot[2], "off": tot[0] / tot[2], "pen": tot[1] / tot[2],
            "A": A, "wrap": wrap, "rec_overflow_rate": ovf[0] / ovf[1],
            "out_overflow_rate": ovf[2] / ovf[3], "max_abs_S_rec": smax[0],
            "max_abs_S_out": smax[1]}


# ------------------------------------------------------------------ sampling
@torch.no_grad()
def sample(model, cats, scale, temp=0.5, prompts=None, max_steps=250, gen=None, pen_temp=None):
    """cats: list of category ids; prompts: optional list of int stroke-3 arrays (moves).
    Returns list of int arrays [m,3] of sampled moves (dx, dy, lift) per item (rounded
    to whole pixels and fed back rounded, as on the Mac)."""
    dev = next(model.parameters()).device
    B = len(cats)
    W, h, gc = model.step_consts(torch.tensor(cats, device=dev))
    P = [0 if prompts is None else len(p) for p in prompts or [None] * B]
    maxP = max(P)
    o = None
    for t in range(maxP + 1):
        x = torch.zeros(B, 5, device=dev)
        for i in range(B):
            if t == 0 or t > P[i]:
                x[i, 2] = 1                       # S0 (or filler after the prompt)
            else:
                dx, dy, lift = prompts[i][t - 1]
                x[i, 0], x[i, 1] = dx / scale, dy / scale
                x[i, 2 + int(lift)] = 1
        hn, on = model.step(W, gc, h, x)
        upd = torch.tensor([t <= p for p in P], device=dev)[:, None]
        h = torch.where(upd, hn, h)
        o = on if o is None else torch.where(upd, on, o)
    out = [[] for _ in range(B)]
    done = np.zeros(B, bool)
    st = math.sqrt(temp)
    for _ in range(max_steps):
        pi, mx, my, lsx, lsy, rho, pen = split_params(o)
        k = torch.multinomial(torch.softmax(pi / temp, -1), 1, generator=gen)
        g = lambda a: a.gather(1, k).squeeze(1)
        mux, muy, sx, sy, r = g(mx), g(my), g(lsx).exp() * st, g(lsy).exp() * st, g(rho)
        z1 = torch.randn(B, device=dev, generator=gen)
        z2 = torch.randn(B, device=dev, generator=gen)
        dx = torch.round((mux + sx * z1) * scale)
        dy = torch.round((muy + sy * (r * z1 + torch.sqrt(1 - r * r) * z2)) * scale)
        ps = torch.multinomial(torch.softmax(pen / (pen_temp or temp), -1), 1,
                               generator=gen).squeeze(1)
        dxn, dyn, psn = dx.cpu().numpy(), dy.cpu().numpy(), ps.cpu().numpy()
        for i in range(B):
            if done[i]:
                continue
            if psn[i] == 2:
                done[i] = True
            else:
                out[i].append((int(dxn[i]), int(dyn[i]), int(psn[i])))
        if done.all():
            break
        x = torch.zeros(B, 5, device=dev)
        x[:, 0], x[:, 1] = dx / scale, dy / scale
        x[torch.arange(B), 2 + ps] = 1
        h, o = model.step(W, gc, h, x)
    return [np.array(m, np.int64).reshape(-1, 3) for m in out]


# ------------------------------------------------------------------ rendering
def polylines(moves, start=(0, 0), first_down=True):
    """Moves (dx, dy, lift) from a start point -> list of absolute polylines.
    A move is drawn if the pen was down after the previous point."""
    x, y = start
    lines, cur, down = [], [(x, y)], first_down
    for dx, dy, lift in moves:
        x, y = x + dx, y + dy
        if down:
            cur.append((x, y))
        else:
            if len(cur) > 1:
                lines.append(cur)
            cur = [(x, y)]
        down = lift == 0
    if len(cur) > 1:
        lines.append(cur)
    return lines


def extent(lines):
    pts = np.array([p for l in lines for p in l] or [(0, 0)], float)
    return pts.min(0), pts.max(0)


def draw_cell(img, ox, oy, layers, origin):
    """layers: list of (lines, colour). origin: frame coordinate of (0,0) of the drawing."""
    d = ImageDraw.Draw(img)
    fx, fy = ox + MARGIN, oy + MARGIN
    d.rectangle([fx - 1, fy - 1, fx + FR, fy + FR], outline=(205, 205, 205))
    for lines, col in layers:
        for l in lines:
            pts = [(fx + (px + origin[0]) * FR / 256, fy + (py + origin[1]) * FR / 256)
                   for px, py in l]
            d.line(pts, fill=col, width=1)


def grid_png(rows, labels, path, header):
    """rows: list (per row) of list of cells; a cell = (layers, origin)."""
    ncol = max(len(r) for r in rows)
    W, H = 130 + ncol * CELL, 24 + len(rows) * CELL
    img = Image.new("RGB", (W, H), "white")
    d = ImageDraw.Draw(img)
    font = ImageFont.load_default()
    d.text((6, 6), header, fill=(0, 0, 0), font=font)
    for r, (cells, lab) in enumerate(zip(rows, labels)):
        y = 24 + r * CELL
        d.text((6, y + CELL // 2 - 6), lab, fill=(0, 0, 0), font=font)
        for c, (layers, origin) in enumerate(cells):
            draw_cell(img, 130 + c * CELL, y, layers, origin)
    img.save(path)


def centred_origin(lines):
    lo, hi = extent(lines)
    return (128 - (lo[0] + hi[0]) / 2, 128 - (lo[1] + hi[1]) / 2)


# ------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prep", default="data/prep")
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--temp", type=float, default=0.5)
    ap.add_argument("--sim_n", type=int, default=6900)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--extra_temps", default="0.3", help="comma list; extra sample grids")
    ap.add_argument("--only_grids", action="store_true", help="skip losses (re-render only)")
    ap.add_argument("--pen_temps", default="0.75,1.0",
                    help="comma list; extra grids/completions with this pen-state temperature "
                         "(mixture temperature stays --temp / the extra temps)")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    dev = torch.device("cuda")
    torch.backends.cuda.matmul.allow_tf32 = False
    torch.backends.cudnn.allow_tf32 = False
    s = torch.load(args.ckpt, map_location=dev)
    a = s["args"]
    meta = json.load(open(os.path.join(args.prep, "meta.json")))
    cats = meta["categories"]
    scale = s["scale"]
    model = SketchDecoder(a["H"], a["quant"]).to(dev)
    model.load_state_dict(s["model"])
    model.eval()
    name = f"H={a['H']} {a['quant']} step {s['step']}"
    met = {"name": name, "H": a["H"], "quant": a["quant"], "step": s["step"],
           "nparam": s.get("nparam")}

    if args.only_grids:
        mp = os.path.join(args.out, "metrics.json")
        if os.path.exists(mp):
            met = json.load(open(mp))
    else:
        losses(model, args, met, scale, dev, a)
    gen = torch.Generator(device=dev)
    render(model, args, met, scale, dev, cats, name, gen, a)


def losses(model, args, met, scale, dev, a):
    val = GPUData(args.prep, "valid", scale, dev)
    met["val_full"] = val_loss(model, val)
    print("val", met["val_full"], flush=True)
    sub = GPUData(args.prep, "valid", scale, dev, max_n=args.sim_n, seed=2)
    met["sim_subset_n"] = int(len(sub.idx))
    met["sim_cudnn"] = val_loss(model, sub)
    met["sim_step_float"] = step_loss(model, sub)
    met["sim_step_h_int8"] = step_loss(model, sub, hq=hq_int8)
    print("sim", met["sim_cudnn"], met["sim_step_float"], met["sim_step_h_int8"], flush=True)
    if a["quant"] != "fp":
        A0, L1 = auto_A(model)
        met["A_auto"], met["max_row_L1"] = A0, L1
        met["mac_auto"] = mac_loss(model, sub, A=A0, wrap=True)
        met["mac_sim"] = [mac_loss(model, sub, A=A, wrap=w)
                          for A, w in ((127, True), (127, False), (63, True), (31, True))]
        print("mac_auto", met["mac_auto"], "mac_sim", met["mac_sim"], flush=True)
    test = GPUData(args.prep, "test", scale, dev)
    met["test_full"] = val_loss(model, test)
    del val, sub


def render(model, args, met, scale, dev, cats, name, gen, a):
    # sample grids: 20 categories x 6 samples (same seed at every temperature)
    ids = [cats.index(c) for c in GRID_CATS]
    temps = [args.temp] + [float(t) for t in args.extra_temps.split(",") if t.strip()]
    pts = [float(t) for t in args.pen_temps.split(",") if t.strip()]
    variants = [(t, None) for t in temps] + [(t, pt) for t in temps for pt in pts]
    for temp, pt in variants:
        gen.manual_seed(args.seed)
        samp = sample(model, [c for c in ids for _ in range(6)], scale, temp, gen=gen,
                      pen_temp=pt)
        lens = [len(m) for m in samp]
        sfx = ("" if temp == args.temp else f"_t{temp:g}") + ("" if pt is None else f"_pen{pt:g}")
        met["grid_mean_moves" + sfx] = float(np.mean(lens))
        met["grid_hit_max" + sfx] = int(sum(l >= 250 for l in lens))
        for part, sl in (("a", slice(0, 10)), ("b", slice(10, 20))):
            rows, labels = [], []
            for ci in range(20)[sl]:
                cells = []
                for j in range(6):
                    lines = polylines(samp[ci * 6 + j])
                    cells.append(([(lines, (0, 0, 0))], centred_origin(lines)))
                rows.append(cells)
                labels.append(GRID_CATS[ci])
            grid_png(rows, labels, os.path.join(args.out, f"grid_{part}{sfx}.png"),
                     f"{name}  samples, temperature {temp:g}"
                     + ("" if pt is None else f", pen-state temperature {pt:g}"))

    # completions of held-out test drawings: first 40% of strokes given
    xy = np.load(os.path.join(args.prep, "test_xy.npy"))
    pen = np.load(os.path.join(args.prep, "test_pen.npy"))
    off = np.load(os.path.join(args.prep, "test_off.npy"))
    tcat = np.load(os.path.join(args.prep, "test_cat.npy"))
    rng = np.random.default_rng(11)
    prompts, originals, pcats, origins = [], [], [], []
    for cname in COMP_CATS:
        c = cats.index(cname)
        for i in rng.permutation(np.flatnonzero(tcat == c)):
            mv = np.column_stack([xy[off[i]:off[i + 1]], pen[off[i]:off[i + 1]]]).astype(np.int64)
            ends = np.flatnonzero(mv[:, 2] == 1)
            if len(ends) >= 4:
                k = max(1, int(round(0.4 * len(ends))))
                prompts.append(mv[:ends[k - 1] + 1])
                originals.append(mv)
                pcats.append(c)
                lo, hi = extent(polylines(mv))
                origins.append((-lo[0] + (255 - (hi[0] - lo[0])) / 2,
                                -lo[1] + (255 - (hi[1] - lo[1])) / 2))
                break
    NC = 4
    for pt in [None] + pts:
        gen.manual_seed(args.seed + 1)
        comp = sample(model, [c for c in pcats for _ in range(NC)], scale, args.temp,
                      prompts=[p for p in prompts for _ in range(NC)], gen=gen, pen_temp=pt)
        sfx = "" if pt is None else f"_pen{pt:g}"
        render_completions(args, met, cats, name, prompts, originals, pcats, origins, comp,
                           NC, pt, sfx)
    export(model, args, met, scale, a)


def render_completions(args, met, cats, name, prompts, originals, pcats, origins, comp, NC,
                       pt, sfx):
    out_frame = 0
    for part, sl in (("a", slice(0, 10)), ("b", slice(10, 20))):
        rows, labels = [], []
        for i in range(len(prompts))[sl]:
            pl = polylines(prompts[i])
            cells = [([(polylines(originals[i]), (150, 150, 150))], origins[i]),
                     ([(pl, (0, 0, 0))], origins[i])]
            for j in range(NC):
                end = prompts[i][:, :2].sum(0)
                cl = polylines(comp[i * NC + j], start=tuple(end), first_down=False)
                lo, hi = extent(pl + cl)
                o = origins[i]
                if lo[0] + o[0] < -1 or lo[1] + o[1] < -1 or hi[0] + o[0] > 256 or hi[1] + o[1] > 256:
                    out_frame += 1
                cells.append(([(pl, (0, 0, 0)), (cl, (220, 0, 0))], origins[i]))
            rows.append(cells)
            labels.append(cats[pcats[i]])
        grid_png(rows, labels, os.path.join(args.out, f"complete_{part}{sfx}.png"),
                 f"{name}  original | first 40% of strokes | {NC} completions "
                 f"(red), temperature {args.temp}"
                 + ("" if pt is None else f", pen-state temperature {pt:g}"))
    met["completions_leaving_frame" + sfx] = f"{out_frame}/{len(prompts) * NC}"
    met["completion_mean_moves" + sfx] = float(np.mean([len(c) for c in comp]))


def export(model, args, met, scale, a):
    if a["quant"] != "fp":
        ex = {}
        for nm, k in (("wh", model.kind_wh), ("wo", model.kind_wo), ("wx", "int8"), ("wc", "int4"),
                      ("w0", "int4"), ("emb", "int8")):
            c, sc = export_codes(getattr(model, nm), k)
            ex[nm + "_codes"], ex[nm + "_scale"] = c, sc
        for nm in ("b0", "b_ih", "b_hh", "bo"):
            ex[nm] = getattr(model, nm).detach().cpu().numpy()
        ex["scale_factor"] = np.float64(scale)
        np.savez_compressed(os.path.join(args.out, "export.npz"), **ex)
    json.dump(met, open(os.path.join(args.out, "metrics.json"), "w"), indent=1)
    print(json.dumps(met, indent=1))


if __name__ == "__main__":
    main()
