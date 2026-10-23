#!/usr/bin/env python3
"""Train Dream's float teacher (teacher.py): v-prediction, cosine schedule, classifier-free guidance
dropout, AdamW, EMA, bf16 autocast. Resumable (out/last.pt). Writes out/log.jsonl, out/ema.pt and
per-subject sample sheets (DDIM, several guidance scales) to out/sheets/.

  python train_teacher.py --data synth:500 --out runs/teacher_synth --iters 20000
  python train_teacher.py --data <scratch>/macplus_dream/data/pilot/split --out runs/teacher_pilot
"""
import argparse
import json
import math
import os
import time

import numpy as np
import torch
import torch.nn.functional as F

from common import EMA, GPUData, alpha_sigma, get_split, grid_image, to_u8
from teacher import TeacherUNet, sample


def sheets(model, names, out, tag, device, ws=(1.0, 2.0, 3.0, 5.0), seeds=8, steps=64, max_rows=24,
           levels=None, subj=None):
    """One PNG per guidance scale: rows = subjects, columns = seeds (same noise in every sheet)."""
    os.makedirs(out, exist_ok=True)
    model.eval()
    K = len(names)
    subj = list(range(K)) if subj is None else subj
    paths = []
    for c0 in range(0, len(subj), max_rows):
        ss = subj[c0:c0 + max_rows]
        y = torch.tensor([s for s in ss for _ in range(seeds)], device=device)
        g = torch.Generator(device=device).manual_seed(1234)
        R = model.cfg["img"]
        noise = torch.randn(len(ss) * seeds, 1, R, R, generator=g, device=device)
        for w in ws:
            x = sample(model, y, steps=steps, w=w, noise=noise)
            a = to_u8(x[:, 0], levels)
            rows = [[a[r * seeds + j] for j in range(seeds)] for r in range(len(ss))]
            p = os.path.join(out, f"{tag}_w{w:g}_s{c0:03d}{'_q16' if levels else ''}.png")
            grid_image(rows, scale=2, labels=[names[s] for s in ss]).save(p)
            paths.append(p)
    model.train()
    return paths


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--iters", type=int, default=20000)
    ap.add_argument("--bs", type=int, default=256)
    ap.add_argument("--lr", type=float, default=2e-4)
    ap.add_argument("--warmup", type=int, default=1000)
    ap.add_argument("--wd", type=float, default=0.0)
    ap.add_argument("--ema", type=float, default=0.9995)
    ap.add_argument("--ch", type=int, default=96)
    ap.add_argument("--mults", default="1,2,3,4")
    ap.add_argument("--nres", type=int, default=2)
    ap.add_argument("--res", type=int, default=64, help="picture size (64, or 32 for the 1 MB build)")
    ap.add_argument("--attn_res", default="16,8")
    ap.add_argument("--dropout", type=float, default=0.1)
    ap.add_argument("--cfg_drop", type=float, default=0.1)
    ap.add_argument("--sample_every", type=int, default=5000)
    ap.add_argument("--save_every", type=int, default=2000)
    ap.add_argument("--max_minutes", type=float, default=1e9, help="save and stop after this wall time")
    ap.add_argument("--compile", action="store_true")
    ap.add_argument("--channels_last", action="store_true", help="slower on H100 and crashes with --compile")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--sheet_rows", type=int, default=24)
    ap.add_argument("--sheet_steps", type=int, default=64)
    ap.add_argument("--val", help="held-out split: EMA v-loss on it (fixed noise, 5 noise levels) every --val_every")
    ap.add_argument("--val_every", type=int, default=5000)
    a = ap.parse_args()

    T0 = time.time()
    torch.manual_seed(a.seed)
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    torch.backends.cuda.matmul.allow_tf32 = True
    torch.backends.cudnn.allow_tf32 = True
    os.makedirs(a.out, exist_ok=True)
    split = get_split(a.data, res=a.res)
    print(f"data {a.data}: {len(split)} images, {len(split.present)} of {split.K} subjects present", flush=True)
    data = GPUData(split, dev)
    ints = lambda v: tuple(int(x) for x in v.replace("-", ",").split(","))   # "1,2,3" or "1-2-3" (sbatch)
    model = TeacherUNet(split.K, ch=a.ch, mults=ints(a.mults), nres=a.nres, attn_res=ints(a.attn_res), img=a.res,
                        dropout=a.dropout).to(dev)
    print(f"teacher {sum(p.numel() for p in model.parameters()) / 1e6:.1f}M params {model.cfg}", flush=True)
    if a.channels_last:
        model = model.to(memory_format=torch.channels_last)
    opt = torch.optim.AdamW(model.parameters(), lr=a.lr, betas=(0.9, 0.99), weight_decay=a.wd,
                            fused=dev == "cuda")
    ema = EMA(model, a.ema)
    step = 0
    last = os.path.join(a.out, "last.pt")
    if os.path.exists(last):
        ck = torch.load(last, map_location=dev)
        model.load_state_dict(ck["model"])
        opt.load_state_dict(ck["opt"])
        ema.shadow = ck["ema"]
        step = ck["step"]
        print(f"resumed at step {step}", flush=True)
    fwd = torch.compile(model) if a.compile else model
    meta = dict(cfg=model.cfg, names=split.names, orig=split.orig, present=split.present, data=a.data,
                args=vars(a))

    def save(path, ema_only=False):
        d = dict(meta, step=step, ema=ema.state_dict())
        if not ema_only:
            d.update(model=model.state_dict(), opt=opt.state_dict())
        torch.save(d, path + ".tmp")
        os.replace(path + ".tmp", path)

    logf = open(os.path.join(a.out, "log.jsonl"), "a")
    val = None
    if a.val:
        vs = get_split(a.val, res=a.res)
        g = torch.Generator().manual_seed(99)
        vx = torch.tensor(vs.images, dtype=torch.uint8)
        val = dict(x0=(vx.float() / 7.5 - 1)[:, None].to(dev), y=torch.tensor(vs.labels).to(dev),
                   eps=torch.randn(len(vs), 1, a.res, a.res, generator=g).to(dev))
        print(f"val {a.val}: {len(vs)} images", flush=True)

    @torch.no_grad()
    def val_loss(mod):
        mod.eval()
        out = []
        for tv in (0.1, 0.3, 0.5, 0.7, 0.9):
            tot = 0.0
            for b in range(0, len(val["y"]), 512):
                x0v, yv, ev = val["x0"][b:b + 512], val["y"][b:b + 512], val["eps"][b:b + 512]
                tt = torch.full((len(yv),), tv, device=dev)
                al, si = alpha_sigma(tt)
                al, si = al[:, None, None, None], si[:, None, None, None]
                with torch.autocast("cuda", dtype=torch.bfloat16, enabled=dev == "cuda"):
                    pv = mod(al * x0v + si * ev, tt, yv).float()
                tot += float((pv - (al * ev - si * x0v)).pow(2).mean((1, 2, 3)).sum())
            out.append(round(tot / len(val["y"]), 5))
        mod.train()
        return out
    ema_model = TeacherUNet(split.K, **{k: v for k, v in model.cfg.items() if k != "n_classes"}).to(dev)
    model.train()
    acc = dict(loss=0.0, n=0, lb=np.zeros(5), nb=np.zeros(5))
    t_last = time.time()
    while step < a.iters:
        x0, y = data.batch(a.bs)
        B = x0.shape[0]
        # stratified t in [0, 1]
        t = (torch.arange(B, device=dev) + torch.rand(B, device=dev)) / B
        t = t[torch.randperm(B, device=dev)]
        drop = torch.rand(B, device=dev) < a.cfg_drop
        y = torch.where(drop, torch.full_like(y, split.K), y)
        eps = torch.randn_like(x0)
        al, si = alpha_sigma(t)
        al, si = al[:, None, None, None], si[:, None, None, None]
        xt = al * x0 + si * eps
        if a.channels_last:
            xt = xt.contiguous(memory_format=torch.channels_last)
        v = al * eps - si * x0
        lr = a.lr * min(1.0, (step + 1) / a.warmup)
        for g in opt.param_groups:
            g["lr"] = lr
        with torch.autocast("cuda", dtype=torch.bfloat16, enabled=dev == "cuda"):
            pred = fwd(xt, t, y)
        per = (pred.float() - v).pow(2).mean((1, 2, 3))
        loss = per.mean()
        opt.zero_grad(set_to_none=True)
        loss.backward()
        gnorm = torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step()
        ema.update(model, step)
        step += 1
        acc["loss"] += loss.item() if step % 25 == 0 else 0.0
        acc["n"] += 1 if step % 25 == 0 else 0
        if step % 25 == 0:
            bi = (t * 5).long().clamp(max=4).cpu().numpy()
            pl = per.detach().cpu().numpy()
            np.add.at(acc["lb"], bi, pl)
            np.add.at(acc["nb"], bi, 1)
        if step % 500 == 0:
            dt = time.time() - t_last
            t_last = time.time()
            rec = dict(step=step, loss=acc["loss"] / max(1, acc["n"]), lr=lr, gnorm=float(gnorm),
                       loss_t=[round(float(x), 5) for x in acc["lb"] / np.maximum(1, acc["nb"])],
                       it_s=500 / dt, min=(time.time() - T0) / 60)
            print(json.dumps(rec), flush=True)
            logf.write(json.dumps(rec) + "\n")
            logf.flush()
            acc = dict(loss=0.0, n=0, lb=np.zeros(5), nb=np.zeros(5))
        if step % a.save_every == 0 or step == a.iters:
            save(last)
        if val is not None and (step % a.val_every == 0 or step == a.iters):
            ema_model.load_state_dict(ema.state_dict())
            vl = val_loss(ema_model)
            rec = dict(step=step, val_ema_loss_t=vl, val_ema_loss=round(sum(vl) / len(vl), 5),
                       train_loss_t_recent=[round(float(x), 5) for x in acc["lb"] / np.maximum(1, acc["nb"])])
            print(json.dumps(rec), flush=True)
            logf.write(json.dumps(rec) + "\n")
            logf.flush()
        if step % a.sample_every == 0 or step == a.iters:
            save(os.path.join(a.out, "ema.pt"), ema_only=True)
            ema_model.load_state_dict(ema.state_dict())
            ps = sheets(ema_model, split.names, os.path.join(a.out, "sheets"), f"it{step:06d}", dev,
                        max_rows=a.sheet_rows, subj=split.present[:a.sheet_rows], steps=a.sheet_steps)
            print(f"sheets: {ps}", flush=True)
        if (time.time() - T0) / 60 > a.max_minutes:
            save(last)
            save(os.path.join(a.out, "ema.pt"), ema_only=True)
            print(f"time limit at step {step}", flush=True)
            break
    save(os.path.join(a.out, "ema.pt"), ema_only=True)
    print(f"done step {step} in {(time.time() - T0) / 60:.1f} min", flush=True)


if __name__ == "__main__":
    main()
