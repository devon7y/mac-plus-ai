#!/usr/bin/env python3
"""Pick the categories the classifier recognises best (greedy elimination).

  select_cats.py --prep DIR --ckpt RUN/ckpt.pt --out DIR [--per 1000] [--exclude FILE]

Runs the (all-345) model on `per` validation drawings per category, whole and half drawn, then
repeatedly drops the category with the lowest accuracy, where accuracy is measured with the arg
max restricted to the categories still in the set (an estimate of what retraining on the subset
gives). Score per category = 0.6 x top-1 (whole) + 0.4 x top-1 (half). Writes ranking.json (the
drop order and the overall accuracy at every set size) and cats_<N>.txt for a few sizes.
"""
import argparse
import json
import os

import torch

import gnet
import qd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prep", required=True)
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--per", type=int, default=1000)
    ap.add_argument("--exclude", default="")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    sd = torch.load(a.ckpt, map_location=dev)
    cats = sd["cats"]
    net = gnet.Net(sd["arch"], len(cats)).to(dev)
    net.load_state_dict(sd["model"])
    net.set_quant(sd.get("qat", False))
    net.eval()
    va = qd.Split(a.prep, "valid", cats, dev, a.per)
    logits = {}
    with torch.no_grad():
        for fr in (1.0, 0.5):
            out, labs = [], []
            for img, lab, _ in qd.eval_batches(va, 4096, fr):
                out.append(net(img).float())
                labs.append(lab)
            logits[fr] = torch.cat(out)
            lab = torch.cat(labs)
    excl = set()
    if a.exclude:
        excl = {c.strip() for c in open(a.exclude) if c.strip() and not c.startswith("#")}
    alive = torch.tensor([c not in excl for c in cats], device=dev)
    K = len(cats)
    order, curve = [], []
    while alive.sum() > 20:
        accs = {}
        for fr, lo in logits.items():
            m = lo.masked_fill(~alive[None], -1e9)
            pred = m.argmax(1)
            top3 = m.topk(3, 1).indices
            ok = (pred == lab).float()
            ok3 = (top3 == lab[:, None]).any(1).float()
            sel = alive[lab]
            n = torch.zeros(K, device=dev).index_add_(0, lab, sel.float())
            c = torch.zeros(K, device=dev).index_add_(0, lab, ok * sel)
            accs[fr] = (c / n.clamp(min=1), float((ok * sel).sum() / sel.sum()), float((ok3 * sel).sum() / sel.sum()))
        score = 0.6 * accs[1.0][0] + 0.4 * accs[0.5][0]
        score = score.masked_fill(~alive, 1e9)
        n_alive = int(alive.sum())
        curve.append(dict(n=n_alive, full_top1=accs[1.0][1], full_top3=accs[1.0][2],
                          half_top1=accs[0.5][1], half_top3=accs[0.5][2]))
        if n_alive in (100, 120, 150, 180, 200, 250):
            keep = [c for c, al in zip(cats, alive.tolist()) if al]
            open(os.path.join(a.out, f"cats_{n_alive}.txt"), "w").write("\n".join(keep) + "\n")
        worst = int(score.argmin())
        order.append(dict(cat=cats[worst], full=float(accs[1.0][0][worst]), half=float(accs[0.5][0][worst])))
        alive[worst] = False
    json.dump(dict(curve=curve, drop_order=order, excluded=sorted(excl)), open(os.path.join(a.out, "ranking.json"), "w"),
              indent=0)
    for c in curve:
        if c["n"] % 25 == 0 or c["n"] == curve[0]["n"]:
            print(f"N={c['n']:3d} whole top1 {c['full_top1']:.3f} top3 {c['full_top3']:.3f}  "
                  f"half top1 {c['half_top1']:.3f} top3 {c['half_top3']:.3f}")


if __name__ == "__main__":
    main()
