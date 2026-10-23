#!/usr/bin/env python3
"""Train the Pixel model (ternary QAT or full-precision twin) on prepared token arrays.

Batches: sequences of --seq+1 tokens starting at random chat starts (so each sequence begins like a
fresh session and usually runs into the next chat, exercising the rolling 64-token window).
Loss: cross-entropy over all tokens (user-token weight --user-weight) + aux_coef * Switch balance loss.
Logs JSON lines to <out>/log.jsonl; checkpoints <out>/last.pt and <out>/best.pt (by val loss).

Usage: python train.py --data <prepared dir> --out <dir> [--fp] [--steps N] ...
"""
import argparse
import json
import math
import os
import sys
import time
from dataclasses import asdict

import numpy as np
import torch
import torch.nn.functional as F

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from model import Config, build, param_breakdown  # noqa: E402


def load_split(d, split, dev):
    ids = np.fromfile(os.path.join(d, f"{split}.bin"), dtype=np.uint16)
    pm = np.fromfile(os.path.join(d, f"{split}_pixel.bin"), dtype=np.uint8)
    starts = np.load(os.path.join(d, f"{split}_starts.npy"))
    return (torch.from_numpy(ids.astype(np.int32)).to(dev), torch.from_numpy(pm).to(dev),
            torch.from_numpy(starts).to(dev))


def get_batch(data, B, L, gen):
    ids, pm, starts = data
    ok = starts[starts + L + 1 <= ids.numel()]
    s = ok[torch.randint(0, ok.numel(), (B,), device=ids.device, generator=gen)]
    idx = s[:, None] + torch.arange(L + 1, device=ids.device)[None]
    toks = ids[idx].long()
    return toks[:, :-1], toks[:, 1:], pm[idx[:, 1:]].bool()


@torch.no_grad()
def evaluate(model, data, B, L, n_batches, seed=1234):
    model.eval()
    g = torch.Generator(device=data[0].device)
    g.manual_seed(seed)
    tot = totp = n = npx = 0.0
    for _ in range(n_batches):
        x, y, m = get_batch(data, B, L, g)
        with torch.autocast(x.device.type, dtype=torch.bfloat16):
            logits, _, _ = model(x)
        l = F.cross_entropy(logits.float().reshape(-1, logits.shape[-1]), y.reshape(-1), reduction="none")
        tot += l.sum().item()
        n += l.numel()
        mf = m.reshape(-1)
        totp += l[mf].sum().item()
        npx += mf.sum().item()
    model.train()
    return tot / n, totp / max(npx, 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--fp", action="store_true", help="full-precision twin (no fake quantization)")
    ap.add_argument("--steps", type=int, default=10000)
    ap.add_argument("--batch", type=int, default=256)
    ap.add_argument("--seq", type=int, default=256)
    ap.add_argument("--window", type=int, default=64)
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--min-lr-frac", type=float, default=0.05)
    ap.add_argument("--warmup", type=int, default=500)
    ap.add_argument("--wd", type=float, default=0.05)
    ap.add_argument("--user-weight", type=float, default=1.0)
    ap.add_argument("--aux", type=float, default=0.01)
    ap.add_argument("--eval-every", type=int, default=500)
    ap.add_argument("--eval-batches", type=int, default=40)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--compile", action="store_true")
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    torch.backends.cuda.matmul.allow_tf32 = True
    torch.backends.cudnn.allow_tf32 = True
    os.makedirs(args.out, exist_ok=True)
    cfg = Config(quant=not args.fp, window=args.window, aux_coef=args.aux)
    model = build(cfg).to(dev)
    bd = param_breakdown(model)
    print("params:", json.dumps(bd), flush=True)
    assert bd["total"] == 6_692_640 and bd["ternary"] == 6_359_040 and bd["router"] == 3_840 \
        and bd["norms"] == 2_080 and bd["vocab_table"] == 327_680, bd

    train = load_split(args.data, "train", dev)
    val = load_split(args.data, "val", dev)
    print(f"train tokens {train[0].numel():,} chats {train[2].numel():,}; val tokens {val[0].numel():,}",
          flush=True)

    decay = [p for n, p in model.named_parameters() if p.dim() >= 2]
    no_decay = [p for n, p in model.named_parameters() if p.dim() < 2]
    opt = torch.optim.AdamW([{"params": decay, "weight_decay": args.wd},
                             {"params": no_decay, "weight_decay": 0.0}],
                            lr=args.lr, betas=(0.9, 0.95), eps=1e-8, fused=(dev == "cuda"))
    fwd = torch.compile(model) if args.compile else model

    def lr_at(s):
        if s < args.warmup:
            return args.lr * (s + 1) / args.warmup
        p = (s - args.warmup) / max(1, args.steps - args.warmup)
        return args.lr * (args.min_lr_frac + (1 - args.min_lr_frac) * 0.5 * (1 + math.cos(math.pi * p)))

    g = torch.Generator(device=dev)
    g.manual_seed(args.seed + 1)
    logf = open(os.path.join(args.out, "log.jsonl"), "a")
    best = float("inf")
    t0 = time.time()
    tok_seen = 0
    for step in range(args.steps + 1):
        if step % args.eval_every == 0 or step == args.steps:
            vl, vp = evaluate(model, val, args.batch, args.seq, args.eval_batches)
            rec = {"step": step, "val_loss": round(vl, 4), "val_pixel_loss": round(vp, 4),
                   "tokens": tok_seen, "secs": round(time.time() - t0, 1)}
            print(json.dumps(rec), flush=True)
            logf.write(json.dumps(rec) + "\n")
            logf.flush()
            ck = {"cfg": asdict(cfg), "model": model.state_dict(), "step": step, "val_loss": vl,
                  "val_pixel_loss": vp, "args": vars(args)}
            torch.save(ck, os.path.join(args.out, "last.pt"))
            if vl < best:
                best = vl
                torch.save(ck, os.path.join(args.out, "best.pt"))
            if step == args.steps:
                break
        for gr in opt.param_groups:
            gr["lr"] = lr_at(step)
        x, y, m = get_batch(train, args.batch, args.seq, g)
        with torch.autocast(dev, dtype=torch.bfloat16):
            logits, aux, frac = fwd(x)
        l = F.cross_entropy(logits.float().reshape(-1, logits.shape[-1]), y.reshape(-1), reduction="none")
        if args.user_weight != 1.0:
            w = torch.where(m.reshape(-1), 1.0, args.user_weight)
            loss = (l * w).sum() / w.sum()
        else:
            loss = l.mean()
        total = loss + cfg.aux_coef * aux
        opt.zero_grad(set_to_none=True)
        total.backward()
        gn = torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step()
        tok_seen += x.numel()
        if step % 100 == 0:
            dt = time.time() - t0
            rec = {"step": step, "loss": round(loss.item(), 4), "aux": round(float(aux), 4),
                   "gnorm": round(gn.item(), 3), "lr": round(lr_at(step), 6),
                   "expert_frac_l0": [round(v, 3) for v in frac[0].tolist()],
                   "tok_per_s": round(tok_seen / max(dt, 1e-6))}
            print(json.dumps(rec), flush=True)
            logf.write(json.dumps(rec) + "\n")
    print(f"DONE best_val {best:.4f} secs {time.time() - t0:.0f}", flush=True)


if __name__ == "__main__":
    main()
