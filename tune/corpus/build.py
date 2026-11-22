"""Build the Tune training corpus from the raw ABC collections (tune/data/raw, see fetch.py).

  python3 -I tune/corpus/build.py      -> tune/data/corpus.json, prints statistics

Each tune is parsed (abcparse.py), given a type, meter class and mode, re-centred on its tonic
(key-invariant: the app transposes to the chosen key), converted to tokens (vocab.py) and checked
against the grammar (vocab.State). Tunes that don't fit are dropped and counted by reason.
"""
import collections
import glob
import hashlib
import json
import os
import sys
from fractions import Fraction as Fr

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import abcparse as A          # noqa: E402
import vocab as V             # noqa: E402

RAW = os.path.join(HERE, "..", "data", "raw")
OUT = os.path.join(HERE, "..", "data", "corpus.json")

REELISH = {"reel", "hornpipe", "strathspey"}


def oneill_type(x, r):
    r = r.lower()
    if "waltz" in r:
        return "waltz"
    if x <= 700:
        return "air"
    if x <= 1115:
        return "jig"
    if x <= 1175:
        return "slipjig"
    if x <= 1555:
        return "reel"
    if x <= 1780:
        return "hornpipe"
    if x <= 1800:
        return None           # long (set) dances: irregular lengths
    return "march"


def rhythm_type(r):
    r = r.lower().strip().rstrip(".")
    if not r:
        return None
    if "slip" in r:
        return "slipjig"
    if "jig" in r:
        return "jig"
    if "reel" in r:
        return "reel"
    if "hornpipe" in r or "clog" in r:
        return "hornpipe"
    if "strathspey" in r or "fling" in r:
        return "strathspey"
    if "waltz" in r:
        return "waltz"
    if "march" in r:
        return "march"
    if "polka" in r:
        return "polka"
    if "air" in r or "song" in r:
        return "air"
    return None


def title_type(title):
    w = title.lower().replace(".", " ").replace('"', " ").replace("-", "").split()
    for k in ("reel", "jig", "hornpipe", "strathspey", "waltz", "march", "polka"):
        if k in w:
            return rhythm_type(k)
    if "valse" in w or "mazurka" in w or "varsoviana" in w:
        return "waltz"
    if "quickstep" in w:
        return "march"
    if "galop" in w:
        return "polka"
    return None


def load():
    out = []
    for f in sorted(glob.glob(os.path.join(RAW, "oneills1850", "*.abc"))):
        for t in A.parse_tunes(open(f, encoding="latin-1").read(), "oneills1850"):
            try:
                x = int(t["X"])
            except ValueError:
                continue
            t["type"] = oneill_type(x, t["R"])
            t["id"] = "oneills1850/%d" % x
            out.append(t)
    f = os.path.join(RAW, "ryan_cole", "Ryan-Cole.abc")
    for i, t in enumerate(A.parse_tunes(open(f, encoding="latin-1").read(), "ryan_cole")):
        t["type"] = rhythm_type(t["R"]) or rhythm_type(t["T"].split("--")[-1])
        t["id"] = "ryan_cole/%d" % i
        out.append(t)
    for f in sorted(glob.glob(os.path.join(RAW, "kerr", "*.abc"))):
        vol = os.path.basename(f)[:3]
        for t in A.parse_tunes(open(f, encoding="latin-1").read(), "kerr"):
            t["type"] = title_type(t["R"]) or rhythm_type(t["R"]) or title_type(t["T"])
            t["id"] = "kerr/%s/%s" % (vol, t["X"])
            out.append(t)
    return out


def meter_class(meter, ttype):
    num, den = meter
    if (num, den) in ((4, 4), (2, 2)):
        return (4, 4), 1
    if (num, den) == (2, 4) and ttype in REELISH:
        return (4, 4), 2          # 2/4 written in sixteenths = 4/4 in eighths
    if (num, den) in V.METERS:
        return (num, den), 1
    return None, 0


def choose_key(notes, key):
    """-> (tonic letter index, tonic pc, mode, alterations) minimising accidentals."""
    k_li, k_pc, k_mode, _ = key
    last = notes[-1]
    fin_li, fin_pc = last[1], last[3] % 12
    cands = {(k_li, k_pc), (fin_li, fin_pc)}
    best = None
    n = len(notes)
    for (li, pc) in cands:
        for mode in V.MODES:
            steps = V.MODE_STEPS[mode]
            alt = 0
            for e in notes:
                deg = (e[1] - li) % 7
                exp = (pc + steps[deg]) % 12
                if (e[3] - exp) % 12 != 0:
                    alt += 1
            score = alt + (0 if (li, pc) == (fin_li, fin_pc) else 0.03 * n) \
                + (0 if (li, pc) == (k_li, k_pc) else 0.01 * n) + (0 if mode == k_mode else 0.005 * n)
            if best is None or score < best[0]:
                best = (score, li, pc, mode, alt)
    return best[1:]


def tonic_ref(li, pc):
    """MIDI of the tonic placed in [55, 66] (G3..F#4), and its letter octave."""
    for m in range(55, 67):
        if m % 12 == pc:
            nat = A.LETTER_PC[li]
            acc = (pc - nat + 6) % 12 - 6
            octave = (m - nat - acc) // 12 - 1
            return m, octave
    raise ValueError


def merge_bars(kinds, at_end):
    s = set(kinds)
    if "2" in s:
        return [V.E2]
    if "1" in s:
        return [V.RE, V.E1] if ":|" in s else [V.E1]
    if "::" in s or (":|" in s and "|:" in s):
        return [V.RER]
    if ":|" in s:
        return [V.RE]
    if "|:" in s:
        return [V.RS]
    if "||" in s or "|]" in s:
        return [V.DBL]
    return [V.BAR]


def pad_pickups(out, mc):
    """A section that starts with a short (pickup) bar after a complete bar: pad that bar with a
    rest at its start, so that every plain bar line closes a full bar."""
    for _ in range(8):
        st = V.State(mc)
        start, after_sec = 1, False
        fixed = False
        for i in range(1, len(out)):
            tk = out[i]
            if tk in (V.BAR, V.E1) and st.started and st.pos < st.blen and after_sec \
                    and not st.trip:
                need = st.blen - st.pos
                pads = []
                for d in sorted(V.DURS, reverse=True):
                    while need >= d:
                        pads.append(V.rest_tok(d))
                        need -= d
                if need:
                    return out
                out = out[:start] + pads + out[start:]
                fixed = True
                break
            was_full = st.pos >= st.blen
            st.step(tk)
            if tk in V.SECTION_TOKENS or tk in (V.BAR, V.E1) or tk >= V.PICK0 and tk < V.REST0:
                start = i + 1
                after_sec = tk in V.SECTION_TOKENS and was_full
        if not fixed:
            return out
    return out


def tokenize(t):
    meter, key, ev = A.parse_body(t)
    mc, scale = meter_class(meter, t["type"])
    if mc is None:
        raise A.Skip("meter class %s" % (meter,))
    notes = [e for e in ev if e[0] == "N"]
    if len(notes) < 16:
        raise A.Skip("short")
    li, pc, mode, alt = choose_key(notes, key)
    if alt > 0.15 * len(notes):
        raise A.Skip("chromatic")
    tref, toct = tonic_ref(li, pc)
    steps = V.MODE_STEPS[mode]
    # events -> raw token list (bars still as lists of kinds)
    toks = []
    pending = []
    for e in ev:
        if e[0] == "B":
            pending.append(e[1])
            continue
        if pending:
            toks.append(("B", pending))
            pending = []
        if e[0] == "T3":
            toks.append(("T3",))
            continue
        dur = e[-1] * 48 * scale
        if dur.denominator != 1 or int(dur) not in V.DURS:
            raise A.Skip("dur %s" % dur)
        d = int(dur)
        if e[0] == "R":
            toks.append(("R", d))
            continue
        deg = (e[1] - li) + 7 * (e[2] - toct)
        exp = tref + 12 * (deg // 7) + steps[deg % 7]
        a = e[3] - exp
        if a not in (-1, 0, 1):
            raise A.Skip("alteration %d" % a)
        if not V.DEG_MIN <= deg <= V.DEG_MAX:
            raise A.Skip("range %d" % deg)
        toks.append(("N", deg, d, a))
    # trailing bar lines
    end_rep = pending and any(k in (":|", "::") for k in pending)
    # drop leading bar lines
    while toks and toks[0][0] == "B":
        toks.pop(0)
    # triplets: the 3 notes must share a nominal duration
    out = []
    blen = V.bar_ticks(mc)
    # pickup = length before the first bar line
    first = 0
    k = 0
    trip = 0
    while k < len(toks) and toks[k][0] != "B":
        x = toks[k]
        if x[0] == "T3":
            trip = 3
        elif x[0] in ("N", "R"):
            d = x[2] if x[0] == "N" else x[1]
            first += d * 2 // 3 if trip else d
            if trip:
                trip -= 1
        k += 1
    if first > blen:
        raise A.Skip("first bar long")
    pick = 0 if first == blen else first
    if pick not in V.PICKS:
        raise A.Skip("pickup %d" % pick)
    out = [V.BOS, V.PICK0 + V.PICKS.index(pick)]
    for x in toks:
        if x[0] == "B":
            out += merge_bars(x[1], False)
        elif x[0] == "T3":
            out.append(V.T3)
        elif x[0] == "R":
            out.append(V.rest_tok(x[1]))
        else:
            if x[3] == 1:
                out.append(V.UP)
            elif x[3] == -1:
                out.append(V.DOWN)
            out.append(V.note_tok(x[1], x[2]))
    if end_rep:
        out.append(V.RE)
    out.append(V.EOS)
    out = pad_pickups(out, mc)
    # grammar check
    st = V.State(mc)
    for i, tk in enumerate(out[1:]):
        ok = st.allowed()
        if not ok[tk]:
            raise A.Skip("grammar %s at %d (pos %d/%d)" % (V.tok_name(tk), i, st.pos, st.blen))
        st.step(tk)
    return {"meter": list(mc), "mode": mode, "tonic_pc": pc, "tonic_li": li, "toks": out,
            "bars": sum(1 for x in out if x in (V.BAR, V.E1)) + 1}


def main():
    tunes = load()
    skipped = collections.Counter()
    kept = []
    seen = {}
    for t in tunes:
        if t["type"] is None:
            skipped["no type"] += 1
            continue
        try:
            r = tokenize(t)
        except A.Skip as e:
            reason = str(e).split(" at ")[0][:24]
            skipped[reason] += 1
            continue
        # duplicates: same type and the same first 24 tokens
        sig = (t["type"], tuple(r["toks"][:26]))
        if sig in seen:
            skipped["duplicate"] += 1
            continue
        seen[sig] = t["id"]
        r.update(id=t["id"], src=t["src"], title=t["T"], type=t["type"])
        h = int(hashlib.md5(t["id"].encode()).hexdigest()[:8], 16)
        r["split"] = "val" if h % 20 == 0 else "train"
        kept.append(r)
    json.dump(kept, open(OUT, "w"))
    print("parsed", len(tunes), "kept", len(kept))
    for k, v in skipped.most_common():
        print("  skip %-26s %d" % (k, v))
    by = collections.Counter((r["type"]) for r in kept)
    print("types", dict(by))
    print("meters", collections.Counter(tuple(r["meter"]) for r in kept))
    print("modes", collections.Counter(r["mode"] for r in kept))
    print("sources", collections.Counter(r["src"] for r in kept))
    ntok = sum(len(r["toks"]) for r in kept)
    print("tokens", ntok, "mean", ntok / len(kept), "max", max(len(r["toks"]) for r in kept))
    deg = collections.Counter()
    dur = collections.Counter()
    for r in kept:
        for x in r["toks"]:
            i = V.tok_info(x)
            if i[0] == "note":
                deg[i[1]] += 1
                dur[i[2]] += 1
    print("degrees", sorted(deg.items()))
    print("durs", sorted(dur.items()))
    print("vocab", V.VOCAB)


if __name__ == "__main__":
    main()
