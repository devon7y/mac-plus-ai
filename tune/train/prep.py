"""Turn tune/data/corpus.json into model inputs: per tune, per step: (sym, dur, pos, trip, bar, sec)
of the token just read, the next token (target) and the grammar mask for it.

  python3 -I tune/train/prep.py   -> tune/data/prep.npz
"""
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "corpus"))
import vocab as V  # noqa: E402

DATA = os.path.join(HERE, "..", "data")


def tune_arrays(r):
    toks = r["toks"]
    st = V.State(tuple(r["meter"]))
    n = len(toks) - 1
    feats = np.zeros((n, 10), np.int16)
    tgt = np.zeros(n, np.int16)
    mask = np.zeros((n, V.VOCAB), bool)
    for t in range(n):
        st.step(toks[t])
        sym, dur = V.tok_inputs(toks[t])
        feats[t] = (sym, dur) + st.feats() + st.lookback()
        tgt[t] = toks[t + 1]
        ok = st.allowed()
        assert ok[toks[t + 1]], (r["id"], t)
        mask[t] = ok
    return feats, tgt, mask


def cond_ids(r):
    return (V.TYPES.index(r["type"]), V.METERS.index(tuple(r["meter"])), V.MODES.index(r["mode"]))


def main():
    corpus = json.load(open(os.path.join(DATA, "corpus.json")))
    F, T, M, C, off, split = [], [], [], [], [0], []
    for r in corpus:
        f, t, m = tune_arrays(r)
        F.append(f); T.append(t); M.append(m)
        C.append(cond_ids(r))
        off.append(off[-1] + len(t))
        split.append(1 if r["split"] == "val" else 0)
    np.savez_compressed(os.path.join(DATA, "prep.npz"), feats=np.concatenate(F), tgt=np.concatenate(T),
                        mask=np.packbits(np.concatenate(M), axis=1), cond=np.array(C, np.int16),
                        off=np.array(off, np.int64), split=np.array(split, np.int8))
    print(len(corpus), "tunes", off[-1], "steps", sum(split), "val tunes")


if __name__ == "__main__":
    main()
