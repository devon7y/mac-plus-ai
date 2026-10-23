#!/usr/bin/env python3
"""Pixel's 2,048-token BPE tokenizer (ASCII alphabet, turn markers). Pure Python, no dependencies,
written so the Mac app can reproduce it exactly.

Token ids
  0 <pad>   1 <user>   2 <pixel>   3 <end>
  4..98     the 95 printable ASCII characters 0x20..0x7E (id = byte - 0x20 + 4)
  99..2047  1,949 merges, in merge order (id = 99 + merge rank)

Text model: specials are matched literally first. Every other character must be printable ASCII
(0x20..0x7E); anything else is replaced by '?' (the Mac app should do the same). The text between
specials is split into chunks, scanning left to right, by the first matching rule:
  1. ' '? letters-or-apostrophes+       e.g. " don't", "Pixel's", " hi"
  2. ' '? one digit                     " 1", "9" (numbers are spelled digit by digit)
  3. ' '? one other non-space character " !", ",", " ("
  4. ' '                                a space not followed by anything that rule 1-3 can take
Merges never cross chunk boundaries. Each chunk starts as its characters; then, repeatedly, the
adjacent pair with the lowest merge rank is found and every occurrence of that pair in the chunk is
merged, scanning left to right (non-overlapping), until no adjacent pair has a rank.
"""
import collections
import json
import re
import sys

SPECIALS = ["<pad>", "<user>", "<pixel>", "<end>"]
PAD, USER, PIXEL, END = 0, 1, 2, 3
N_BASE = len(SPECIALS) + 95
VOCAB_SIZE = 2048
SPECIAL_RE = re.compile("(" + "|".join(re.escape(s) for s in SPECIALS) + ")")
CHUNK_RE = re.compile(r" ?[A-Za-z']+| ?[0-9]| ?[^A-Za-z0-9' ]| ")


def clean_text(s):
    return "".join(c if 32 <= ord(c) <= 126 else "?" for c in s)


def chunks(text):
    """Pre-tokenize plain text (no specials)."""
    out = CHUNK_RE.findall(text)
    assert "".join(out) == text, (text, out)
    return out


def split_specials(text):
    for part in SPECIAL_RE.split(text):
        if part:
            yield part


class Tokenizer:
    def __init__(self, merges):
        self.merges = [tuple(m) for m in merges]
        self.rank = {m: i for i, m in enumerate(self.merges)}
        self.id_of = {s: i for i, s in enumerate(SPECIALS)}
        for b in range(32, 127):
            self.id_of[chr(b)] = b - 32 + len(SPECIALS)
        for i, (a, b) in enumerate(self.merges):
            self.id_of[a + b] = N_BASE + i
        self.tokens = [None] * (N_BASE + len(self.merges))
        for s, i in self.id_of.items():
            self.tokens[i] = s
        self.cache = {}

    @classmethod
    def load(cls, path):
        return cls(json.load(open(path))["merges"])

    def save(self, path):
        json.dump({"specials": SPECIALS, "alphabet": "printable ASCII 0x20-0x7E, ids 4-98",
                   "merges": [list(m) for m in self.merges], "tokens": self.tokens}, open(path, "w"))

    def bpe(self, chunk):
        if chunk in self.cache:
            return self.cache[chunk]
        syms = list(chunk)
        while len(syms) > 1:
            best, br = None, None
            for i in range(len(syms) - 1):
                r = self.rank.get((syms[i], syms[i + 1]))
                if r is not None and (br is None or r < br):
                    best, br = (syms[i], syms[i + 1]), r
            if best is None:
                break
            out, i = [], 0
            while i < len(syms):
                if i < len(syms) - 1 and syms[i] == best[0] and syms[i + 1] == best[1]:
                    out.append(best[0] + best[1])
                    i += 2
                else:
                    out.append(syms[i])
                    i += 1
            syms = out
        ids = [self.id_of[s] for s in syms]
        if len(self.cache) < 500000:
            self.cache[chunk] = ids
        return ids

    def encode(self, text):
        ids = []
        for part in split_specials(text):
            if part in self.id_of and part in SPECIALS:
                ids.append(self.id_of[part])
            else:
                for c in chunks(clean_text(part)):
                    ids.extend(self.bpe(c))
        return ids

    def decode(self, ids):
        return "".join(self.tokens[i] if 0 <= i < len(self.tokens) and self.tokens[i] else "" for i in ids)


def train(lines, n_merges=VOCAB_SIZE - N_BASE, verbose=True):
    """Classic BPE on chunk frequencies. Ties: highest count, then the lexicographically smallest
    pair (a, b) (deterministic)."""
    freq = collections.Counter()
    for line in lines:
        for part in split_specials(line.rstrip("\n")):
            if part not in SPECIALS:
                freq.update(chunks(clean_text(part)))
    words = [list(w) for w in freq]
    counts = [freq[w] for w in freq]
    pair_count = collections.Counter()
    where = collections.defaultdict(set)
    for wi, syms in enumerate(words):
        for a, b in zip(syms, syms[1:]):
            pair_count[(a, b)] += counts[wi]
            where[(a, b)].add(wi)
    merges = []
    for step in range(n_merges):
        if not pair_count:
            break
        best = min(pair_count.items(), key=lambda kv: (-kv[1], kv[0]))[0]
        if pair_count[best] <= 0:
            break
        merges.append(best)
        a, b = best
        for wi in list(where[best]):
            syms = words[wi]
            c = counts[wi]
            # remove old pairs of this word
            for p in zip(syms, syms[1:]):
                pair_count[p] -= c
                if pair_count[p] <= 0:
                    del pair_count[p]
            out, i = [], 0
            while i < len(syms):
                if i < len(syms) - 1 and syms[i] == a and syms[i + 1] == b:
                    out.append(a + b)
                    i += 2
                else:
                    out.append(syms[i])
                    i += 1
            words[wi] = out
            for p in zip(out, out[1:]):
                pair_count[p] += c
                where[p].add(wi)
        where.pop(best, None)
        if verbose and step % 200 == 0:
            print(f"merge {step}: {a!r}+{b!r}", flush=True)
    return Tokenizer(merges)


if __name__ == "__main__":
    # python3 tokenizer.py train.txt tokenizer.json [max_lines]
    src, out = sys.argv[1], sys.argv[2]
    lim = int(sys.argv[3]) if len(sys.argv) > 3 else None
    lines = []
    for i, line in enumerate(open(src)):
        if lim and i >= lim:
            break
        lines.append(line)
    tok = train(lines)
    tok.save(out)
    print(f"{len(tok.tokens)} tokens -> {out}")
