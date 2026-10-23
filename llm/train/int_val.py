#!/usr/bin/env python3
"""Mac-exact validation loss: cross-entropy of the bit-exact integer model (llm/ref IntModel, loaded
from the exported .pxm) next to the torch QAT checkpoint's, on the first N chats of a corpus val.txt
(one fresh session per chat, teacher-forced). Also reports the argmax agreement of the two.

Usage: python3 int_val.py model.pxm ckpt.pt tokenizer.json val.txt [N=300]
"""
import importlib.util
import os
import sys

import numpy as np
import torch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "ref"))
import export as EX  # noqa: E402
import model as RM  # noqa: E402
import sf  # noqa: E402

_spec = importlib.util.spec_from_file_location("pixel_train_model", os.path.join(HERE, "model.py"))
TM = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(TM)
sys.path.append(HERE)
import tokenizer as T  # noqa: E402


def lsm(z):
    z = z - z.max()
    return z - np.log(np.exp(z).sum())


def main():
    pxm, pt, tokf, valf = sys.argv[1:5]
    N = int(sys.argv[5]) if len(sys.argv) > 5 else 300
    pm = EX.read_model(pxm)
    tok = T.Tokenizer.load(tokf)
    ck = torch.load(pt, map_location="cpu", weights_only=False)
    m = TM.build(TM.Config(**ck["cfg"]))
    m.load_state_dict({k.replace("_orig_mod.", ""): v for k, v in ck["model"].items()})
    m = m.eval()
    lines = open(valf).read().splitlines()[:N]
    ci = ct = cip = ctp = n = npx = agree = 0.0
    for line in lines:
        ids = tok.encode(line)
        im = RM.IntModel(pm)
        with torch.no_grad():
            full = m(torch.tensor([ids]))[0][0].double().numpy()
        role = 0
        for i, t in enumerate(ids[:-1]):
            Kl, sig = im.step(int(t))
            zi = Kl.astype(np.float64) * sf.value(sig)
            y = ids[i + 1]
            li, lt = -lsm(zi)[y], -lsm(full[i])[y]
            role = 1 if t == T.PIXEL else (0 if t in (T.USER, T.END) else role)
            ci += li
            ct += lt
            n += 1
            agree += int(np.argmax(zi) == np.argmax(full[i]))
            if role == 1:          # targets inside a Pixel reply (its text and the closing <end>)
                cip += li
                ctp += lt
                npx += 1
    print(f"{os.path.basename(pxm)}: {len(lines)} val chats, {int(n)} tokens | CE all: int {ci / n:.4f} "
          f"torchQAT {ct / n:.4f} | CE Pixel tokens: int {cip / npx:.4f} torchQAT {ctp / npx:.4f} | "
          f"argmax agree {agree / n:.3f}")


if __name__ == "__main__":
    main()
