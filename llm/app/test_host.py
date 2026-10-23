#!/usr/bin/env python3
"""Differential test: the C engine (host build) against the Python reference (llm/ref).
Builds llm/build/app/pxhost, exports a random model with the stand-in tokenizer, then compares
per-token logits (hash of all 2,048 scaled ints), experts, sampling and tokenization.
Usage: python3 llm/app/test_host.py [n_tokens]"""
import os
import random
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
LLM = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(LLM, "ref"))
import export  # noqa: E402
import model  # noqa: E402

BUILD = os.path.join(LLM, "build", "app")
os.makedirs(BUILD, exist_ok=True)
HOST = os.path.join(BUILD, "pxhost")
TOKJ = os.path.join(LLM, "build", "ref", "test_tokenizer.json")


def fnv(K):
    h = 2166136261
    for v in K:
        h ^= int(v) & 0xFFFFFFFF
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 80
    subprocess.run(["python3", os.path.join(HERE, "gen_tables.py")], check=True)
    subprocess.run(["cc", "-O2", "-o", HOST] + [os.path.join(HERE, f) for f in
                   ("host_main.c", "engine.c", "kernels_c.c", "tables.c")], check=True)
    if not os.path.exists(TOKJ):
        subprocess.run(["python3", os.path.join(LLM, "ref", "make_test_tokenizer.py"), TOKJ], check=True)
    tok = export.load_tokenizer(TOKJ)
    ok = True
    for seed, es in ((2, 0.08), (0, 0.35)):
        pm = export.export(export.random_float_weights(seed, es))
        pm["merges"] = export.tokenizer_merges(tok)
        path = os.path.join(BUILD, f"test_model_{seed}.pxm")
        export.write_model(pm, path, budget=None)
        ids = [int(t) for t in np.random.default_rng(seed + 9).integers(0, model.V, n)]
        im = model.IntModel(pm)
        exp = []
        for t in ids:
            K, sig = im.step(t)
            exp.append((list(im.experts), sig, fnv(K)))
        for variant in ("W4", "B5"):
            out = subprocess.run([HOST, path, variant, "logits"] + [str(t) for t in ids],
                                 capture_output=True, text=True).stdout.splitlines()
            bad = 0
            for (ex, sig, h), line in zip(exp, out):
                f = line.split()
                got = ([int(x) for x in f[3:9]], (int(f[10]), int(f[11])), int(f[13], 16))
                bad += got != (ex, sig, h)
            good = bad == 0 and len(out) == len(ids)
            ok &= good
            print(f"model seed {seed} {variant}: {len(out)} tokens teacher-forced, {bad} mismatches "
                  f"-> {'OK' if good else 'FAIL'}")
        prompt = [1, 2, 3, 500, 1000, 1500, 2000]
        for (s, T, k) in ((12345, 256, 16), (7, 205, 16), (99, 0, 1)):
            want = model.generate(model.IntModel(pm), prompt, 40, seed=s, temp256=T, topk=k)
            got = subprocess.run([HOST, path, "W4", "gen", str(s), str(T), str(k), "40"] + list(map(str, prompt)),
                                 capture_output=True, text=True).stdout.split()
            good = [int(x) for x in got] == want
            ok &= good
            print(f"model seed {seed} generation seed {s} T {T}/256 k {k}: {'OK' if good else 'FAIL'} "
                  f"({len(set(want))} distinct of 40)")
    # file variants: raw (SE), partially Huffman-coded (budget), fully coded: same model
    pm = export.export(export.random_float_weights(2, 0.08))
    pm["merges"] = export.tokenizer_merges(tok)
    full = export.to_bytes(pm)
    ids = [5, 77, 1999, 3, 1024]
    ref = None
    for name, data in (("raw", export.to_bytes(pm, huff=False)), ("partly coded", export.to_bytes(pm, budget=len(full) + 5000)),
                       ("fully coded", full)):
        p = os.path.join(BUILD, "variant.pxm")
        open(p, "wb").write(data)
        for variant in ("W4", "B5"):
            out = subprocess.run([HOST, p, variant, "logits"] + list(map(str, ids)), capture_output=True, text=True).stdout
            ref = ref or out
            good = out == ref and out.startswith("tok")
            ok &= good
            print(f"model file {name} ({len(data):,} bytes) {variant}: {'OK' if good else 'FAIL'}")
    # tokenizer
    rnd = random.Random(5)
    words = [w.strip() for w in open(os.path.join(LLM, "data_pipeline", "vocab.txt")) if w.strip()]
    lines = ["hi pixel", "<user> what's 2+2?<pixel> Beep! It is 4.<end>", "  double  spaces  ",
             "Tabs\tand é unicode", "don't 'quote' me", "x" * 300, "", " ", "a1b2c3!!", "<user><end>"]
    for _ in range(300):
        s = "".join(rnd.choice([" ", "", "  "]) + rnd.choice(words + list("!?.,'0123456789<>-()"))
                    for _ in range(rnd.randint(1, 15)))
        if rnd.random() < 0.3:
            s = s.capitalize()
        lines.append(s)
        path = os.path.join(BUILD, "test_model_2.pxm")
    lines = [l[:511] for l in lines]          # the Mac encodes at most 511 bytes per call
    got = subprocess.run([HOST, path, "W4", "enc"], input=("\n".join(lines) + "\n").encode("latin-1"),
                         capture_output=True).stdout.decode().split("\n")
    bad = 0
    for line, g in zip(lines, got):
        want = tok.encode(line)
        if [int(x) for x in g.split()] != want:
            bad += 1
            if bad <= 3:
                print("  enc mismatch:", repr(line), want[:12], g[:60])
    good = bad == 0
    ok &= good
    print(f"tokenizer: {len(lines)} strings encoded, {bad} mismatches -> {'OK' if good else 'FAIL'}")
    ids = tok.encode("Hello, I am Pixel! I love floppy disks.")
    dec = subprocess.run([HOST, path, "W4", "dec"] + list(map(str, ids)), capture_output=True, text=True).stdout
    good = dec.rstrip("\n") == tok.decode(ids)
    ok &= good
    print(f"decode: {dec.strip()!r} -> {'OK' if good else 'FAIL'}")
    # corrupted files must be rejected (16 = file byte sum, 17 = decoded-model checksum, 14 = Huffman)
    data = bytearray(open(path, "rb").read())
    for name, mutate in (("flipped bit", lambda d: d.__setitem__(700000, d[700000] ^ 4)),
                         ("swapped bytes", lambda d: d.__setitem__(slice(900000, 900002), d[900000:900002][::-1]))):
        d = bytearray(data)
        mutate(d)
        if d == data:
            continue
        bad_path = os.path.join(BUILD, "corrupt.pxm")
        open(bad_path, "wb").write(d)
        out = subprocess.run([HOST, bad_path, "W4", "logits", "5"], capture_output=True, text=True).stdout
        good = out.startswith("load error")
        ok &= good
        print(f"corrupt model ({name}): {out.strip()} -> {'OK' if good else 'FAIL'}")
    print("HOST ENGINE BIT-EXACT" if ok else "HOST ENGINE DIFFERS")
    return ok


if __name__ == "__main__":
    sys.exit(0 if main() else 1)
