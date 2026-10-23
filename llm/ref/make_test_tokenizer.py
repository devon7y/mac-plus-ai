#!/usr/bin/env python3
"""Stand-in tokenizer until the real one exists: train llm/train/tokenizer.py (the training agent's
BPE, used read-only) on synthetic chats built from llm/data_pipeline/vocab.txt.
Usage: make_test_tokenizer.py [out.json]   (default llm/build/ref/test_tokenizer.json)"""
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LLM = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(LLM, "train"))
import tokenizer as T  # noqa: E402


def synthetic_lines(n=20000, seed=7):
    words = [w.strip() for w in open(os.path.join(LLM, "data_pipeline", "vocab.txt")) if w.strip()]
    rnd = random.Random(seed)
    lines = []
    for _ in range(n):
        parts = []
        for _ in range(rnd.randint(1, 3)):
            u = " ".join(rnd.choice(words) for _ in range(rnd.randint(1, 8)))
            p = " ".join(rnd.choice(words) for _ in range(rnd.randint(2, 12)))
            parts.append(f"<user> {u}{rnd.choice(['', '?', '!'])}<pixel> {p.capitalize()}{rnd.choice(['.', '!', '?'])} "
                         f"I have {rnd.randint(0, 99)} disks.<end>")
        lines.append("".join(parts))
    return lines


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(LLM, "build", "ref", "test_tokenizer.json")
    tok = T.train(synthetic_lines(), verbose=False)
    tok.save(out)
    print(f"{len(tok.merges)} merges, {len(tok.tokens)} tokens -> {out}")
