"""Pack a split into one compressed file for a slow link, and unpack it back into the plain split
files (images64.npy, images32.npy, labels.npy, meta.jsonl, subjects.txt).

  python pack_split.py pack   data/full   data/full.pack.npz
  python pack_split.py unpack data/full.pack.npz data/full      (needs only numpy)
"""
import gzip
import io
import json
import os
import sys

import numpy as np


def pack(split, out):
    meta = open(os.path.join(split, 'meta.jsonl'), 'rb').read()
    subjects = open(os.path.join(split, 'subjects.txt'), 'rb').read()
    np.savez_compressed(out, images64=np.load(os.path.join(split, 'images64.npy')),
                        images32=np.load(os.path.join(split, 'images32.npy')),
                        labels=np.load(os.path.join(split, 'labels.npy')),
                        meta_gz=np.frombuffer(gzip.compress(meta, 6), np.uint8),
                        subjects=np.frombuffer(subjects, np.uint8))


def unpack(src, split):
    d = np.load(src)
    os.makedirs(split, exist_ok=True)
    for k in ('images64', 'images32', 'labels'):
        np.save(os.path.join(split, k + '.npy'), d[k])
    open(os.path.join(split, 'meta.jsonl'), 'wb').write(gzip.decompress(d['meta_gz'].tobytes()))
    open(os.path.join(split, 'subjects.txt'), 'wb').write(d['subjects'].tobytes())
    print(split, d['images64'].shape, d['labels'].shape)


if __name__ == '__main__':
    {'pack': pack, 'unpack': unpack}[sys.argv[1]](sys.argv[2], sys.argv[3])
