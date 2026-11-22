#!/usr/bin/env python3
"""Train / evaluate one Guess My Doodle classifier (float, or QAT from a float checkpoint).

  train.py --prep DIR --arch SPEC --out DIR [--cats FILE] [--steps N] [--qat --init CKPT]
Writes OUT/ckpt.pt and OUT/metrics.json (top-1/top-3 on the validation split for whole drawings
and for prefixes of 25/50/75% of the points, plus per-class accuracy).
"""
import argparse
import json
import math
import os
import time

import torch
import torch.nn.functional as F

import gnet
import qd


def evaluate(net, split, fracs=(1.0, 0.75, 0.5, 0.25), B=4096):
    net.eval()
    ncls = len(split.cats)
    res = {}
    with torch.no_grad():
        for fr in fracs:
            c1 = torch.zeros(ncls, device=split.device)
            c3 = torch.zeros(ncls, device=split.device)
            n = torch.zeros(ncls, device=split.device)
            for img, lab, _ in qd.eval_batches(split, B, fr):
                with torch.autocast("cuda", dtype=torch.bfloat16, enabled=img.is_cuda):
                    lo = net(img)
                top = lo.float().topk(3, 1).indices
                h1 = (top[:, 0] == lab).float()
                h3 = (top == lab[:, None]).any(1).float()
                n.index_add_(0, lab, torch.ones_like(h1))
                c1.index_add_(0, lab, h1)
                c3.index_add_(0, lab, h3)
            res[str(fr)] = dict(top1=float(c1.sum() / n.sum()), top3=float(c3.sum() / n.sum()),
                                per_class_top1=(c1 / n.clamp(min=1)).tolist(),
                                per_class_top3=(c3 / n.clamp(min=1)).tolist())
    net.train()
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prep", required=True)
    ap.add_argument("--arch", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--cats", default="")
    ap.add_argument("--steps", type=int, default=30000)
    ap.add_argument("--bs", type=int, default=1024)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--wd", type=float, default=1e-4)
    ap.add_argument("--frac_full", type=float, default=0.5)
    ap.add_argument("--qat", action="store_true")
    ap.add_argument("--init", default="")
    ap.add_argument("--smooth", type=float, default=0.05)
    ap.add_argument("--max_valid", type=int, default=0, help="validation drawings per class (0 = all)")
    ap.add_argument("--max_hours", type=float, default=100)
    ap.add_argument("--eval_only", action="store_true")
    ap.add_argument("--afull", action="store_true", help="activation ceilings 127 everywhere (gnet.Net)")
    ap.add_argument("--teacher", default="", help="float checkpoint: distil from its soft predictions")
    ap.add_argument("--kd", type=float, default=0.7, help="weight of the distillation loss")
    ap.add_argument("--kd_t", type=float, default=2.0, help="distillation temperature")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    cats = [c.strip() for c in open(a.cats) if c.strip()] if a.cats else None
    t0 = time.time()
    va = qd.Split(a.prep, os.environ.get("GUESS_SPLIT", "valid"), cats, dev, a.max_valid or None)
    ncls = len(va.cats)
    net = gnet.Net(a.arch, ncls, a.afull).to(dev)
    if a.init:
        sd = torch.load(a.init, map_location=dev)
        net.load_state_dict(sd["model"], strict=False)
    if a.qat:
        net.set_quant(True)
    print(f"arch {a.arch} classes {ncls} params {sum(p.numel() for p in net.parameters())} "
          f"cost {gnet.summary(a.arch, ncls)}", flush=True)
    teacher = None
    if a.teacher:
        tsd = torch.load(a.teacher, map_location=dev)
        teacher = gnet.Net(tsd["arch"], ncls).to(dev)
        teacher.load_state_dict(tsd["model"])
        teacher.set_quant(tsd.get("qat", False))
        teacher.eval()
        for p_ in teacher.parameters():
            p_.requires_grad_(False)
    if not a.eval_only:
        tr = qd.Split(a.prep, "train", cats, dev)
        print(f"data loaded in {time.time() - t0:.0f}s: train {tr.n} valid {va.n}", flush=True)
        opt = torch.optim.AdamW(net.parameters(), lr=a.lr, weight_decay=a.wd)
        warm = min(1000, a.steps // 20)
        sched = torch.optim.lr_scheduler.LambdaLR(
            opt, lambda s: min(1, (s + 1) / warm) * 0.5 * (1 + math.cos(math.pi * min(s, a.steps) / a.steps)))
        if a.qat:          # LSQ initialisation from one batch
            with torch.no_grad():
                img, lab = qd.batch(tr, a.bs, a.frac_full)
                net(img)
        net.train()
        t1 = time.time()
        run = 0.0
        for step in range(a.steps):
            if a.qat and step == int(a.steps * 0.9):      # freeze BN statistics for the end
                for m in net.modules():
                    if isinstance(m, (torch.nn.BatchNorm1d, torch.nn.BatchNorm2d)):
                        m.eval()
            img, lab = qd.batch(tr, a.bs, a.frac_full)
            with torch.autocast("cuda", dtype=torch.bfloat16, enabled=dev == "cuda"):
                lo = net(img)
            loss = F.cross_entropy(lo.float(), lab, label_smoothing=a.smooth)
            if teacher is not None:
                with torch.no_grad(), torch.autocast("cuda", dtype=torch.bfloat16, enabled=dev == "cuda"):
                    tl = teacher(img).float()
                T = a.kd_t
                kd = F.kl_div(F.log_softmax(lo.float() / T, 1), F.softmax(tl / T, 1), reduction="batchmean") * T * T
                loss = (1 - a.kd) * loss + a.kd * kd
            opt.zero_grad(set_to_none=True)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(net.parameters(), 2.0)
            opt.step()
            sched.step()
            lv = loss.item()
            run = 0.98 * run + 0.02 * lv if step else lv
            if step % 1000 == 0 or step == a.steps - 1:
                el = time.time() - t1
                print(f"step {step} loss {run:.4f} lr {sched.get_last_lr()[0]:.2e} {el:.0f}s", flush=True)
                if el > a.max_hours * 3600:
                    print("time limit", flush=True)
                    break
        torch.save(dict(model=net.state_dict(), arch=a.arch, cats=va.cats, qat=a.qat, afull=a.afull),
                   os.path.join(a.out, "ckpt.pt"))
    res = evaluate(net, va)
    out = dict(arch=a.arch, qat=a.qat, steps=a.steps, ncls=ncls, cats=va.cats, cost=gnet.summary(a.arch, ncls),
               eval=res, minutes=(time.time() - t0) / 60)
    json.dump(out, open(os.path.join(a.out, "metrics.json"), "w"))
    print("RESULT " + json.dumps({fr: (round(r["top1"], 4), round(r["top3"], 4)) for fr, r in res.items()}), flush=True)


if __name__ == "__main__":
    main()
