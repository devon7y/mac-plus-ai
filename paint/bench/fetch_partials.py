#!/usr/bin/env python3
"""Fetch a few real Quick, Draw! drawings (Google, CC BY 4.0) and keep the first half of their
strokes as "partial drawings" for the sample sheet (sample_sheet.py).

Reads only the first ~150 KB of each category's public "simplified" ndjson file (HTTP range
request; plain JSON, already scaled to a 0-255 box and RDP-simplified, like the training data),
picks the first recognized drawing with at least MIN_STROKES strokes, and writes
paint/bench/data/partials.json: [{category, key_id, strokes: [[x...], [y...]] (prompt half),
total_strokes}]. Usage: fetch_partials.py [cat house ...]
"""
import json
import os
import sys
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
import argparse

URL = "https://storage.googleapis.com/quickdraw_dataset/full/simplified/{}.ndjson"
ap = argparse.ArgumentParser()
ap.add_argument("cats", nargs="*", default=["cat", "house", "bicycle", "face", "tree", "airplane"])
ap.add_argument("--n", type=int, default=1, help="drawings per category")
ap.add_argument("--every", type=int, default=0, help="instead of names: every k-th category of categories.txt")
ap.add_argument("--out", default=os.path.join(HERE, "data", "partials.json"))
args = ap.parse_args()
OUT = args.out
CATS = args.cats
if args.every:
    allc = [l.strip() for l in open(os.path.join(HERE, "..", "app", "data", "categories.txt")) if l.strip()]
    CATS = allc[::args.every]
MIN_STROKES = 4


def first_drawings(cat, nbytes=150_000):
    req = urllib.request.Request(URL.format(urllib.parse.quote(cat)), headers={"Range": f"bytes=0-{nbytes}"})
    data = urllib.request.urlopen(req, timeout=60).read().decode("utf-8", "replace")
    for line in data.split("\n")[:-1]:                 # the last line may be cut off
        try:
            yield json.loads(line)
        except json.JSONDecodeError:
            continue


out = []
for cat in CATS:
    got = 0
    for d in first_drawings(cat):
        strokes = d.get("drawing", [])
        if d.get("recognized") and len(strokes) >= MIN_STROKES and d.get("word") == cat:
            k = max(1, len(strokes) // 2)
            out.append({"category": cat, "key_id": d.get("key_id"), "total_strokes": len(strokes),
                        "strokes": [[[int(v) for v in s[0]], [int(v) for v in s[1]]] for s in strokes[:k]]})
            got += 1
            if got >= args.n:
                break
    if got < args.n:
        print("only", got, "suitable drawings for", cat)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
json.dump({"source": "Google Quick, Draw! dataset (simplified), CC BY 4.0, "
                     "https://github.com/googlecreativelab/quickdraw-dataset", "partials": out},
          open(OUT, "w"), indent=0)
print(f"{OUT}: {len(out)} partial drawings ({', '.join(p['category'] for p in out)})")
