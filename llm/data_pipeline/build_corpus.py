#!/usr/bin/env python3
"""Turn clean chats (filter.py output) into training text: one chat per line in the marker format

    <user> hi pixel<pixel> Beep! Hello, friend!<end><user> what are you<pixel> I am Pixel, a little computer.<end>

Every turn's text is preceded by one space (so the first word uses the same space-prefixed tokens as
later words). Pixel's text is never changed. User turns get light synthetic noise so the model sees
real typing: lowercase (30%), trailing punctuation dropped (25%), one typo (10%), ALL CAPS for short
turns (2%), a stretched word like 'hiii' (4%). The noise is seeded by the chat id (deterministic).
Split: 1% of chats (by md5 of id) go to val.

Truecasing (added 2026-10-16): the Qwen generation came out ~99.8% lower case for both roles, so every
turn is first truecased (sentence starts, I/I'm/I'll..., Pixel, Mac, Chicago, days, months, unambiguous
first names); then the user noise above re-creates the lowercase typists. --no-truecase disables it.

Usage: python3 build_corpus.py clean.jsonl out_dir [--no-noise]
Writes out_dir/train.txt, out_dir/val.txt
"""
import hashlib
import json
import os
import random
import re
import sys

KEYS = ["qwertyuiop", "asdfghjkl", "zxcvbnm"]
NEIGH = {}
for r, row in enumerate(KEYS):
    for c, ch in enumerate(row):
        n = []
        for dr in (-1, 0, 1):
            rr = r + dr
            if 0 <= rr < 3:
                for dc in (-1, 0, 1):
                    cc = c + dc
                    if (dr or dc) and 0 <= cc < len(KEYS[rr]):
                        n.append(KEYS[rr][cc])
        NEIGH[ch] = n


def typo(word, rng):
    if len(word) < 3:
        return word
    i = rng.randrange(1, len(word))
    op = rng.random()
    if op < 0.3:  # swap
        i = min(i, len(word) - 1)
        return word[:i - 1] + word[i] + word[i - 1] + word[i + 1:]
    if op < 0.55:  # drop
        return word[:i] + word[i + 1:]
    if op < 0.75:  # double
        return word[:i] + word[i] + word[i:]
    ch = word[i].lower()
    if ch in NEIGH:
        return word[:i] + rng.choice(NEIGH[ch]) + word[i + 1:]
    return word


def noisy(t, rng):
    if rng.random() < 0.10:
        ws = t.split(" ")
        cand = [k for k, w in enumerate(ws) if re.fullmatch(r"[A-Za-z]{3,}[.,!?]?", w)]
        if cand:
            k = rng.choice(cand)
            m = re.fullmatch(r"([A-Za-z]+)([.,!?]?)", ws[k])
            ws[k] = typo(m.group(1), rng) + m.group(2)
            t = " ".join(ws)
    if rng.random() < 0.04:
        m = re.search(r"\b(hi|hey|yay|no|so|wow|yes|please|ok|okay)\b", t, re.I)
        if m:
            w = m.group(0)
            t = t[:m.start()] + w + w[-1] * rng.randint(1, 3) + t[m.end():]
    r = rng.random()
    if r < 0.02 and len(t.split()) <= 5:
        t = t.upper()
    elif r < 0.32:
        t = t.lower()
    if rng.random() < 0.25:
        t = t.rstrip(".!?")
    return t.strip() or "hi"


_NAMES_AMBIG = set("""will mark rose ruby grace daisy holly ivy jack max pat bill rob ray rex nick jay jade
frank poppy bella una ana lea eva dev sid kai lin mei wei tara hana asha sam ben tim joe dan gus hank
pete eli luke dave kim mike matt nate nell pam tess tina toby tony vera wes zack billy bob bobby
""".split())
_PROPER = {w: w.capitalize() for w in """pixel pixel's mac mac's macintosh chicago monday tuesday wednesday
thursday friday saturday sunday january february april june july august september october november
december christmas halloween easter""".split()}
_PROPER.update({"i": "I", "i'm": "I'm", "i'll": "I'll", "i've": "I've", "i'd": "I'd"})
try:
    _vb = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "build_vocab.py")).read()
    for _n in _vb.split('NAMES = """')[1].split('"""')[0].split():
        if _n not in _NAMES_AMBIG:
            _PROPER.setdefault(_n, _n.capitalize())
except Exception:
    pass
_WORD = re.compile(r"[A-Za-z]+(?:'[A-Za-z]+)*")
_SENT_START = re.compile(r"(^|[.!?]\s+)([^A-Za-z]*)([a-z])")


def truecase(t):
    t = _WORD.sub(lambda m: _PROPER.get(m.group(0), m.group(0)) if m.group(0).islower() else m.group(0), t)
    return _SENT_START.sub(lambda m: m.group(1) + m.group(2) + m.group(3).upper(), t)


def chat_line(turns, rng, noise=True, tc=True):
    parts = []
    for role, t in turns:
        if tc:
            t = truecase(t)
        if role == "user":
            parts.append("<user> " + (noisy(t, rng) if noise else t))
        else:
            parts.append("<pixel> " + t + "<end>")
    return "".join(parts)


def main():
    src, out = sys.argv[1], sys.argv[2]
    noise = "--no-noise" not in sys.argv
    tc = "--no-truecase" not in sys.argv
    os.makedirs(out, exist_ok=True)
    n = {"train": 0, "val": 0}
    with open(os.path.join(out, "train.txt"), "w") as ft, open(os.path.join(out, "val.txt"), "w") as fv:
        for line in open(src):
            rec = json.loads(line)
            h = int(hashlib.md5(rec["id"].encode()).hexdigest(), 16)
            rng = random.Random(h)
            split = "val" if h % 100 == 0 else "train"
            (fv if split == "val" else ft).write(chat_line(rec["turns"], rng, noise, tc) + "\n")
            n[split] += 1
    print(n)


if __name__ == "__main__":
    main()
