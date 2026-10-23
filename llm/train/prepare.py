#!/usr/bin/env python3
"""Tokenize corpus text (one chat per line) into training arrays.

Writes, for each split in {train, val}:
  <split>.bin         uint16 token ids, all chats concatenated
  <split>_starts.npy  int64 offset of each chat's first token (<user>)
  <split>_pixel.bin   uint8, 1 where the token belongs to a Pixel reply (text after <pixel> up to and
                      including <end>), used for the Pixel-only loss

Usage: python3 prepare.py tokenizer.json corpus_dir out_dir [workers]
"""
import os
import sys
from multiprocessing import Pool

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tokenizer as T  # noqa: E402

TOK = None


def init(path):
    global TOK
    TOK = T.Tokenizer.load(path)


def enc(lines):
    out = []
    for line in lines:
        ids = TOK.encode(line.rstrip("\n"))
        role, pm = 0, []
        for t in ids:
            if t == T.PIXEL:
                pm.append(0)
                role = 1
            elif t == T.END:
                pm.append(1)
                role = 0
            elif t == T.USER:
                pm.append(0)
                role = 0
            else:
                pm.append(role)
        out.append((ids, pm))
    return out


def main():
    tok_path, src, out = sys.argv[1], sys.argv[2], sys.argv[3]
    workers = int(sys.argv[4]) if len(sys.argv) > 4 else os.cpu_count()
    os.makedirs(out, exist_ok=True)
    for split in ("train", "val"):
        lines = open(os.path.join(src, f"{split}.txt")).readlines()
        chunks = [lines[i:i + 2000] for i in range(0, len(lines), 2000)]
        ids_all, pm_all, starts = [], [], []
        n = 0
        with Pool(workers, initializer=init, initargs=(tok_path,)) as pool:
            for res in pool.imap(enc, chunks, chunksize=1):
                for ids, pm in res:
                    starts.append(n)
                    n += len(ids)
                    ids_all.append(np.asarray(ids, dtype=np.uint16))
                    pm_all.append(np.asarray(pm, dtype=np.uint8))
        ids = np.concatenate(ids_all)
        pm = np.concatenate(pm_all)
        ids.tofile(os.path.join(out, f"{split}.bin"))
        pm.tofile(os.path.join(out, f"{split}_pixel.bin"))
        np.save(os.path.join(out, f"{split}_starts.npy"), np.asarray(starts, dtype=np.int64))
        print(f"{split}: chats {len(starts):,} tokens {len(ids):,} pixel-tokens {int(pm.sum()):,} "
              f"tokens/chat {len(ids) / max(len(starts), 1):.1f}", flush=True)


if __name__ == "__main__":
    main()
