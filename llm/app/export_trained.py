#!/usr/bin/env python3
"""Export a trained checkpoint to a Pixel model file (format v3) with its tokenizer.

    /opt/anaconda3/bin/python3 llm/app/export_trained.py --ckpt llm/train/checkpoints/best.pt \\
        --tok llm/train/tokenizer.json --out llm/build/app/pixel_trained.pxm

Uses the training agent's llm/train/model.py (read-only) for the checkpoint layout and
llm/ref/export.py for the packing. Needs torch (the anaconda Python has it).
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LLM = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(LLM, "ref"))
import export as EX  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--ckpt", required=True)
ap.add_argument("--tok", required=True)
ap.add_argument("--out", required=True)
a = ap.parse_args()

import importlib.util  # noqa: E402
import torch  # noqa: E402
spec = importlib.util.spec_from_file_location("train_model", os.path.join(LLM, "train", "model.py"))
TM = importlib.util.module_from_spec(spec)
spec.loader.exec_module(TM)
ck = torch.load(a.ckpt, map_location="cpu", weights_only=False)
sd = ck["model"] if "model" in ck else ck
fw = TM.to_float_weights({k: v for k, v in sd.items()})
pm = EX.export(fw)
pm["merges"] = EX.tokenizer_merges(EX.load_tokenizer(a.tok))
n = EX.write_model(pm, a.out)
print(f"{a.out}: {n:,} bytes (fully Huffman-coded; make_disks.py re-encodes to fit the floppies)")
