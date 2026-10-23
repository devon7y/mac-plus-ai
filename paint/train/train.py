#!/usr/bin/env python3
"""Train the class-conditional sketch-rnn decoder (see sketch_model.py) on all 345 categories.

  python train.py --prep data/prep --out runs/h256_tern --H 256 --quant tern --steps 60000

Teacher forcing on next-move prediction; per-point loss = offset NLL (mixture of 20
bivariate Gaussians, normalised units, targets dequantised with uniform +-0.5 px noise)
+ pen-state cross-entropy, averaged over real moves plus the end token. Resumes from
<out>/ckpt.pt if present. Logs JSON lines to <out>/log.jsonl.
"""
import argparse
import json
import math
import os
import time

import numpy as np
import torch

from sketch_model import GPUData, SketchDecoder, distill_loss, seq_loss


def evaluate(model, data, B=2048, seed=1234):
    model.eval()
    gen = torch.Generator(device=data.device)
    gen.manual_seed(seed)
    tot = np.zeros(3)
    with torch.no_grad():
        for b in data.batches(B, None, shuffle=False):
            inp, tgt, n, cat = data.make(b, dequant=True, gen=gen)
            o = model(inp, cat)
            a, c, k = seq_loss(o, tgt, n)
            tot += [a.item(), c.item(), k.item()]
    model.train()
    return {"loss": (tot[0] + tot[1]) / tot[2], "off": tot[0] / tot[2], "pen": tot[1] / tot[2]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prep", default="data/prep")
    ap.add_argument("--out", required=True)
    ap.add_argument("--H", type=int, default=256)
    ap.add_argument("--quant", default="fp", choices=["fp", "tern", "int4", "tern_o4", "tern_g64", "tern_g128", "tern_r2o"])
    ap.add_argument("--steps", type=int, default=60000)
    ap.add_argument("--batch", type=int, default=1024)
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--lr_min", type=float, default=2e-5)
    ap.add_argument("--warmup", type=int, default=1000)
    ap.add_argument("--stretch", type=float, default=0.1)
    ap.add_argument("--clip", type=float, default=1.0)
    ap.add_argument("--skip_gn", type=float, default=50.0,
                    help="skip the update if the gradient norm is non-finite or above this")
    ap.add_argument("--eval_every", type=int, default=2500)
    ap.add_argument("--val_n", type=int, default=34500)
    ap.add_argument("--max_hours", type=float, default=1e9, help="save and exit after this")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--init", default=None, help="start from this checkpoint's weights (QAT fine-tune)")
    ap.add_argument("--teacher", default=None, help="checkpoint of a teacher for distillation")
    ap.add_argument("--distill", type=float, default=1.0, help="weight of the output distillation term")
    ap.add_argument("--distill_h", type=float, default=0.0,
                    help="weight of the per-step hidden-state MSE (mean over units) to the teacher")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    torch.backends.cuda.matmul.allow_tf32 = True
    torch.backends.cudnn.allow_tf32 = True
    dev = torch.device("cuda")
    meta = json.load(open(os.path.join(args.prep, "meta.json")))
    scale = meta["scale_factor"]
    torch.manual_seed(args.seed)

    t0 = time.time()
    train = GPUData(args.prep, "train", scale, dev)
    val = GPUData(args.prep, "valid", scale, dev, max_n=args.val_n, seed=1)
    print(f"data loaded {time.time() - t0:.0f}s: train {len(train.idx)} drawings, "
          f"{train.xy.shape[0]} moves; val subset {len(val.idx)}; scale {scale:.3f}", flush=True)

    model = SketchDecoder(args.H, args.quant).to(dev)
    nparam = sum(p.numel() for p in model.parameters())
    opt = torch.optim.Adam(model.parameters(), lr=args.lr)
    step, ck = 0, os.path.join(args.out, "ckpt.pt")
    if os.path.exists(ck):
        s = torch.load(ck, map_location=dev)
        model.load_state_dict(s["model"])
        opt.load_state_dict(s["opt"])
        step = s["step"]
        print(f"resumed at step {step}", flush=True)
    elif args.init:
        model.load_state_dict(torch.load(args.init, map_location=dev)["model"])
        print(f"initialised from {args.init}", flush=True)
    teacher = None
    if args.teacher:
        ts = torch.load(args.teacher, map_location=dev)
        teacher = SketchDecoder(ts["args"]["H"], ts["args"]["quant"]).to(dev)
        teacher.load_state_dict(ts["model"])
        teacher.eval()
        for p in teacher.parameters():
            p.requires_grad_(False)
        print(f"distilling from {args.teacher} (weight {args.distill})", flush=True)
    print(f"H={args.H} quant={args.quant} params={nparam}", flush=True)
    log = open(os.path.join(args.out, "log.jsonl"), "a")

    def lr_at(s):
        if s < args.warmup:
            return args.lr * (s + 1) / args.warmup
        p = min(1.0, (s - args.warmup) / max(1, args.steps - args.warmup))
        return args.lr_min + 0.5 * (args.lr - args.lr_min) * (1 + math.cos(math.pi * p))

    def save():
        torch.save({"model": model.state_dict(), "opt": opt.state_dict(), "step": step,
                    "args": vars(args), "scale": scale, "nparam": nparam}, ck + ".tmp")
        os.replace(ck + ".tmp", ck)

    rng = np.random.default_rng(args.seed + step)
    gen = torch.Generator(device=dev)
    gen.manual_seed(args.seed + step)
    batches, bi = train.batches(args.batch, rng), 0
    acc, tk, tl = np.zeros(3), time.time(), time.time()
    skipped = 0
    t_start = time.time()
    while step < args.steps:
        if bi >= len(batches):
            batches, bi = train.batches(args.batch, rng), 0
        inp, tgt, n, cat = train.make(batches[bi], stretch=args.stretch, dequant=True, gen=gen)
        bi += 1
        for g in opt.param_groups:
            g["lr"] = lr_at(step)
        o, y = model(inp, cat, return_h=True)
        a, c, k = seq_loss(o, tgt, n)
        loss = (a + c) / k
        if teacher is not None:
            with torch.no_grad():
                ot, yt = teacher(inp, cat, return_h=True)
            loss = loss + args.distill * distill_loss(o, ot, n) / k
            if args.distill_h > 0:
                m = (torch.arange(y.shape[1], device=dev)[None] < n[:, None]).float()
                loss = loss + args.distill_h * (((y - yt) ** 2).mean(-1) * m).sum() / k
        opt.zero_grad(set_to_none=True)
        loss.backward()
        gn = torch.nn.utils.clip_grad_norm_(model.parameters(), args.clip)
        if torch.isfinite(gn) and gn <= args.skip_gn:
            opt.step()
        else:
            skipped += 1
        step += 1
        acc += [a.item(), c.item(), k.item()]
        if step % 200 == 0:
            dt = time.time() - tl
            rec = {"step": step, "lr": lr_at(step), "train_loss": (acc[0] + acc[1]) / acc[2],
                   "train_off": acc[0] / acc[2], "train_pen": acc[1] / acc[2],
                   "gnorm": float(gn), "skipped": skipped, "steps_per_s": 200 / dt, "moves_per_s": acc[2] / dt,
                   "elapsed_h": (time.time() - t0) / 3600}
            print(json.dumps(rec), flush=True)
            log.write(json.dumps(rec) + "\n")
            log.flush()
            acc, tl = np.zeros(3), time.time()
        if step % args.eval_every == 0 or step == args.steps:
            v = evaluate(model, val)
            rec = {"step": step, **{"val_" + kk: vv for kk, vv in v.items()}}
            print(json.dumps(rec), flush=True)
            log.write(json.dumps(rec) + "\n")
            log.flush()
            save()
            tl = time.time()
            if (time.time() - t_start) / 3600 > args.max_hours:
                print("time budget reached; saved", flush=True)
                break
    save()
    if step >= args.steps:
        torch.save({"model": model.state_dict(), "step": step, "args": vars(args),
                    "scale": scale, "nparam": nparam}, os.path.join(args.out, "final.pt"))
        print("finished", flush=True)


if __name__ == "__main__":
    main()
