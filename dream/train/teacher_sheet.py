#!/usr/bin/env python3
"""Sample sheets for EVERY subject present in a teacher checkpoint (pages of 30 rows), at chosen guidance
weights: the check that the teacher is clean enough to distil from.

  python teacher_sheet.py <ema.pt> --out <dir> [--w 3] [--seeds 6] [--steps 64]
"""
import argparse
import os

import torch

from distill import load_teacher
from train_teacher import sheets


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ckpt")
    ap.add_argument("--out", required=True)
    ap.add_argument("--w", default="3")
    ap.add_argument("--seeds", type=int, default=6)
    ap.add_argument("--steps", type=int, default=64)
    a = ap.parse_args()
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    t, ck = load_teacher(a.ckpt, dev, compile=False)
    present = ck.get("present", list(range(len(ck["names"]))))
    tag = f"it{ck['step']:06d}_all"
    ps = sheets(t, ck["names"], a.out, tag, dev, ws=tuple(float(x) for x in a.w.split(",")), seeds=a.seeds,
                steps=a.steps, max_rows=30, subj=present)
    print(f"{len(present)} subjects, step {ck['step']}: {ps}")


if __name__ == "__main__":
    main()
