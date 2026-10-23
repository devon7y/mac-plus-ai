#!/usr/bin/env python3
"""The chat protocol the Mac app runs (bit-exact reference for its differential test).

One turn: encode "<user> " + text + "<pixel>" with the tokenizer (specials included), feed those
tokens (logits only for the last), then sample until a marker token (<end>, or <pad>/<user>/<pixel>,
which also end the turn) or max_new tokens (the app uses 32). Every sampled token is
fed back; <end> is fed without computing logits, and when max_new is reached without <end> an <end>
is fed too, so the memory always holds whole turns like the training text
("<user> hi<pixel> Beep! Hello!<end>"). "reset" clears the memory (the RNG continues).

Usage: chat.py <model.pxm> <seed> <temp256> <topk> <max_new> "line 1" "line 2" ...
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import export  # noqa: E402
import model  # noqa: E402

END = 3


def encode(pm, text):
    """Tokenizer on token ids (equivalent to llm/train/tokenizer.py; see export.tokenizer_merges)."""
    import re
    rank = {(a, b): r for r, (a, b) in enumerate(pm["merges"])}
    out = []
    specials = ["<pad>", "<user>", "<pixel>", "<end>"]
    for part in re.split("(" + "|".join(re.escape(s) for s in specials) + ")", text):
        if not part:
            continue
        if part in specials:
            out.append(specials.index(part))
            continue
        part = "".join(c if 32 <= ord(c) <= 126 else "?" for c in part)
        for ch in re.findall(r" ?[A-Za-z']+| ?[0-9]| ?[^A-Za-z0-9' ]| ", part):
            syms = [ord(c) - 32 + 4 for c in ch]
            while len(syms) > 1:
                best = min(((rank[p], p) for p in zip(syms, syms[1:]) if p in rank), default=None)
                if best is None:
                    break
                r, (a, b) = best
                new, i = [], 0
                while i < len(syms):
                    if i + 1 < len(syms) and syms[i] == a and syms[i + 1] == b:
                        new.append(99 + r)
                        i += 2
                    else:
                        new.append(syms[i])
                        i += 1
                syms = new
            out += syms
    return out


def chat_turn(m, rng, pm, text, temp256, topk, max_new):
    ids = encode(pm, "<user> " + text + "<pixel>")
    for i, t in enumerate(ids):
        res = m.step(t, want_logits=(i == len(ids) - 1))
    out = []
    while True:
        t = model.sample(*res, rng, temp256, topk)
        out.append(t)
        if t < 4:                      # <end> (or any other marker) ends the turn; <end> is fed
            m.step(END, want_logits=False)
            return out
        if len(out) == max_new:
            m.step(t, want_logits=False)
            m.step(END, want_logits=False)
            return out
        res = m.step(t)


def run_script(pm, seed, temp256, topk, max_new, lines):
    m, rng = model.IntModel(pm), model.XorShift32(seed)
    replies = []
    for line in lines:
        if line.strip() == "reset":
            m.reset()
            replies.append([])
            continue
        replies.append(chat_turn(m, rng, pm, line, temp256, topk, max_new))
    return replies


def detok(pm, ids):
    def s(t):
        if t < 4:
            return ""
        if t < 99:
            return chr(t - 4 + 32)
        a, b = pm["merges"][t - 99]
        return s(a) + s(b)
    return "".join(s(t) for t in ids)


if __name__ == "__main__":
    pm = export.read_model(sys.argv[1])
    seed, temp, k, mx = map(int, sys.argv[2:6])
    for line, rep in zip(sys.argv[6:], run_script(pm, seed, temp, k, mx, sys.argv[6:])):
        print(f"> {line}\nPixel:{detok(pm, rep)}  {rep}")
