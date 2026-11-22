"""Memorisation check: does the integer model (as the Mac runs it) reproduce training tunes?

  python3 -I tune/ref/memcheck.py tune/build/model.npz [--n 120] [--out tune/build/samples]

Composes n tunes (types and modes as the app would pick them, seeds 1..n) and, for each, finds
  - the longest run of consecutive notes (pitch degree, accidental, duration) that also occurs in
    some corpus tune (train and validation), and which tune it is;
  - the corpus tune sharing the most 4-note patterns (Jaccard) - the "nearest tune".
Writes a summary table, the ABC of every tune, and a JSON with all numbers.
"""
import argparse
import collections
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "corpus"))
import abcout  # noqa: E402
import tuneref as R  # noqa: E402
import vocab as V  # noqa: E402

DATA = os.path.join(HERE, "..", "data", "corpus.json")
APP_TYPES = ["reel", "jig", "hornpipe", "waltz", "march", "air"]


def notes_of(toks):
    out, alt = [], 0
    for t in toks:
        i = V.tok_info(t)
        if i[0] == "up":
            alt = 1
        elif i[0] == "down":
            alt = -1
        elif i[0] == "note":
            out.append((i[1], alt, i[2]))
            alt = 0
        elif i[0] == "rest":
            out.append(("r", 0, i[1]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("--n", type=int, default=120)
    ap.add_argument("--out", default=os.path.join(HERE, "..", "build", "samples"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    corpus = json.load(open(DATA))
    cnotes = [notes_of(r["toks"]) for r in corpus]
    # k-gram index for the longest common run; 4-grams for similarity
    grams4 = []
    idx4 = collections.defaultdict(set)
    for j, ns in enumerate(cnotes):
        g = set(tuple(ns[i:i + 4]) for i in range(len(ns) - 3))
        grams4.append(g)
        for x in g:
            idx4[x].add(j)
    kgram = {}

    def kset(k):
        if k not in kgram:
            d = {}
            for j, ns in enumerate(cnotes):
                for i in range(len(ns) - k + 1):
                    d.setdefault(tuple(ns[i:i + k]), j)
            kgram[k] = d
        return kgram[k]

    m = R.Model(a.model)
    rows = []
    abc_all = []
    for s in range(1, a.n + 1):
        typ = APP_TYPES[(s - 1) % len(APP_TYPES)]
        r = (s >> 5) % 100
        mode = "maj" if r < 66 else "min" if r < 80 else "dor" if r < 91 else "mix"
        meter, toks = R.compose(m, typ, mode, s, maxbars=32)
        ns = notes_of(toks)
        # longest common run (binary search on k with exact k-gram sets)
        lo, hi, best_j = 0, min(len(ns), 64), -1
        while lo < hi:
            k = (lo + hi + 1) // 2
            d = kset(k)
            hit = next((d[tuple(ns[i:i + k])] for i in range(len(ns) - k + 1) if tuple(ns[i:i + k]) in d), None)
            if hit is not None:
                lo, best_j = k, hit
            else:
                hi = k - 1
        g = set(tuple(ns[i:i + 4]) for i in range(len(ns) - 3))
        cnt = collections.Counter()
        for x in g:
            for j in idx4.get(x, ()):
                cnt[j] += 1
        near, jac = -1, 0.0
        for j, c in cnt.most_common(30):
            jj = c / len(g | grams4[j])
            if jj > jac:
                near, jac = j, jj
        bars = sum(1 for t in toks if t in (V.BAR, V.E1)) + 1
        rows.append({"seed": s, "type": typ, "mode": mode, "meter": "%d/%d" % meter, "tokens": len(toks),
                     "notes": len(ns), "bars": bars, "longest_run": lo,
                     "longest_run_tune": corpus[best_j]["title"] if best_j >= 0 else "",
                     "nearest": corpus[near]["title"] if near >= 0 else "", "nearest_jaccard": round(jac, 3)})
        key = {"maj": "D", "min": "Em", "dor": "Ador", "mix": "Dmix"}[mode]
        tonic = key.rstrip("mdorix") or key[0]
        abc_all.append(abcout.to_abc(toks, meter, mode, tonic, typ, "%s, seed %d" % (typ.capitalize(), s), s))
    json.dump(rows, open(os.path.join(a.out, "memcheck.json"), "w"), indent=1)
    open(os.path.join(a.out, "samples.abc"), "w").write("\n\n".join(abc_all) + "\n")
    runs = sorted(r["longest_run"] for r in rows)
    jacs = sorted(r["nearest_jaccard"] for r in rows)
    print("tunes", len(rows), "mean tokens %.0f" % (sum(r["tokens"] for r in rows) / len(rows)),
          "mean bars %.1f" % (sum(r["bars"] for r in rows) / len(rows)))
    print("longest run of notes shared with a corpus tune: median %d, 90%% %d, max %d" %
          (runs[len(runs) // 2], runs[int(len(runs) * 0.9)], runs[-1]))
    print("nearest-tune 4-gram Jaccard: median %.3f, 90%% %.3f, max %.3f" %
          (jacs[len(jacs) // 2], jacs[int(len(jacs) * 0.9)], jacs[-1]))
    worst = max(rows, key=lambda r: r["longest_run"])
    print("worst:", worst)
    # the same statistic for corpus tunes against the rest of the corpus (a baseline)
    base = []
    for j in range(0, len(corpus), max(1, len(corpus) // 120)):
        ns = cnotes[j]
        others = None
        best = 0
        for k in (8, 12, 16, 24, 32, 48):
            d = {}
            for jj, ns2 in enumerate(cnotes):
                if jj == j:
                    continue
                for i in range(len(ns2) - k + 1):
                    d.setdefault(tuple(ns2[i:i + k]), jj)
            if any(tuple(ns[i:i + k]) in d for i in range(len(ns) - k + 1)):
                best = k
            else:
                break
        base.append(best)
        if len(base) >= 40:
            break
    base.sort()
    print("baseline (corpus tunes vs the rest, run >= k for k in 8..48): median %d, 90%% %d" %
          (base[len(base) // 2], base[int(len(base) * 0.9)]))


if __name__ == "__main__":
    main()
