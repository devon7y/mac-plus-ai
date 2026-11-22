"""Tune tokens -> ABC text in a chosen key, and -> a list of (midi, ticks) notes for audio.

The Mac app (tune/app/abc.c) writes the same text for Save.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "corpus"))
import vocab as V  # noqa: E402

LETTERS = "CDEFGAB"
LETTER_PC = [0, 2, 4, 5, 7, 9, 11]
NAME_PC = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
MODE_ABC = {"maj": "", "min": "m", "dor": "dor", "mix": "mix"}
MAJOR_SHARPS = {"C": 0, "G": 1, "D": 2, "A": 3, "E": 4, "B": 5, "F#": 6, "C#": 7,
                "F": -1, "Bb": -2, "Eb": -3, "Ab": -4, "Db": -5, "Gb": -6}
MODE_SHIFT = {"maj": 0, "dor": 2, "mix": 7, "min": 9}
TYPE_NAMES = {"reel": "Reel", "jig": "Jig", "hornpipe": "Hornpipe", "waltz": "Waltz", "march": "March",
              "air": "Air", "slipjig": "Slip jig", "strathspey": "Strathspey", "polka": "Polka"}


def key_info(tonic, mode):
    """tonic name like 'D', 'Bb', 'F#' -> (letter index, pc, tonic midi in [55,66], letter octave,
    signature {letter index: alteration})."""
    li = LETTERS.index(tonic[0])
    acc = 1 if tonic[1:] == "#" else -1 if tonic[1:] == "b" else 0
    pc = (NAME_PC[tonic[0]] + acc) % 12
    maj_pc = (pc - MODE_SHIFT[mode]) % 12
    nsh = None
    for name, n in MAJOR_SHARPS.items():
        p2 = (NAME_PC[name[0]] + (1 if name[1:] == "#" else -1 if name[1:] == "b" else 0)) % 12
        if p2 == maj_pc and (nsh is None or abs(n) < abs(nsh)):
            nsh = n
    sig = {}
    if nsh > 0:
        for c in "FCGDAEB"[:nsh]:
            sig[LETTERS.index(c)] = 1
    elif nsh < 0:
        for c in "BEADGCF"[:-nsh]:
            sig[LETTERS.index(c)] = -1
    for m in range(55, 67):
        if m % 12 == pc:
            octave = (m - LETTER_PC[li] - acc) // 12 - 1
            return li, pc, m, octave, sig
    raise ValueError


def note_pitch(deg, alt, mode, tmidi):
    s = V.MODE_STEPS[mode]
    return tmidi + 12 * (deg // 7) + s[deg % 7] + alt


def dur_abc(t):
    return {3: "/", 6: "", 9: "3/2", 12: "2", 18: "3", 24: "4", 30: "5", 36: "6"}[t]


def to_abc(toks, meter, mode, tonic, ttype, title, seed):
    li, pc, tmidi, toct, sig = key_info(tonic, mode)
    lines = ["X: %d" % (seed & 0x7FFFFFFF), "T: %s" % title, "R: %s" % TYPE_NAMES.get(ttype, ttype),
             "C: composed by Tune on a Macintosh Plus (seed %d)" % seed,
             "M: %d/%d" % meter, "L: 1/8", "K: %s%s" % (tonic, MODE_ABC[mode])]
    out = []
    bar_acc = {}
    alt = 0
    last = None
    nbar = 0
    for t in toks:
        i = V.tok_info(t)
        k = i[0]
        if k in ("bos", "pick"):
            continue
        if k == "up":
            alt = 1
            continue
        if k == "down":
            alt = -1
            continue
        if k == "t3":
            out.append("(3")
            continue
        if k == "note":
            deg, d = i[1], i[2]
            midi = note_pitch(deg, alt, mode, tmidi)
            alt = 0
            l = (li + deg) % 7
            octv = toct + (li + deg) // 7
            nat = 12 * (octv + 1) + LETTER_PC[l]
            a = midi - nat
            cur = bar_acc.get((l, octv), sig.get(l, 0))
            accs = ""
            if a != cur:
                accs = {1: "^", -1: "_", 0: "=", 2: "^^", -2: "__"}[a]
                bar_acc[(l, octv)] = a
            name = LETTERS[l]
            if octv >= 5:
                name = name.lower() + "'" * (octv - 5)
            else:
                name = name + "," * (4 - octv)
            out.append(accs + name + dur_abc(d))
            last = t
            continue
        if k == "rest":
            out.append("z" + dur_abc(i[1]))
            continue
        if k == "eos":
            if last not in (V.RE, V.DBL, V.RER):
                out.append("|]")
            break
        if k == "bar":
            out.append(" " + V.BAR_NAMES[t] + " ")
            bar_acc = {}
            last = t
            nbar += 1
            if nbar % 4 == 0 and t not in (V.E1, V.E2):
                out.append("\n")
    body = "".join(out)
    body = "\n".join(x.strip() for x in body.split("\n") if x.strip())
    return "\n".join(lines) + "\n" + body


def to_notes(toks, meter, mode, tonic, repeats=True):
    """-> list of (midi or None for a rest, ticks) in playing order (repeats expanded)."""
    li, pc, tmidi, toct, sig = key_info(tonic, mode)
    # first a flat list of items with bar markers, then expand repeats
    items = []
    alt = 0
    trip = 0
    for t in toks:
        i = V.tok_info(t)
        k = i[0]
        if k == "up":
            alt = 1
        elif k == "down":
            alt = -1
        elif k == "t3":
            trip = 3
        elif k in ("note", "rest"):
            d = i[2] if k == "note" else i[1]
            if trip:
                d = d * 2 // 3
                trip -= 1
            items.append(("n", note_pitch(i[1], alt, mode, tmidi) if k == "note" else None, d))
            alt = 0
        elif k == "bar":
            items.append(("b", t))
    if not repeats:
        return [(x[1], x[2]) for x in items if x[0] == "n"]
    out = []
    start = 0          # index where the current repeat section starts
    i = 0
    done = set()       # repeat-end indices already taken
    e1 = None
    while i < len(items):
        x = items[i]
        if x[0] == "n":
            out.append((x[1], x[2]))
            i += 1
            continue
        t = x[1]
        if t in (V.RS,):
            start = i + 1
        elif t == V.DBL:
            start = i + 1
        elif t == V.E1:
            e1 = i
        elif t in (V.RE, V.RER, V.E2):
            if i not in done:
                done.add(i)
                i = start
                continue
            if t == V.E2 and e1 is not None:
                pass
            start = i + 1
            e1 = None
        if t == V.E1 and any(j in done for j in range(i, len(items)) if items[j][0] == "b" and items[j][1] == V.E2):
            # second time through: skip the first ending up to its E2
            j = i + 1
            while j < len(items) and not (items[j][0] == "b" and items[j][1] == V.E2):
                j += 1
            i = j + 1
            start = i
            e1 = None
            continue
        i += 1
    return out
