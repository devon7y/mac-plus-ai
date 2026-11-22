"""A small ABC parser for the Tune corpus: just enough of ABC 2.1 for single-voice fiddle tunes.

parse_tunes(text) -> list of dicts {X, T, M, L, K, R, body (list of lines), src}
parse_body(tune)  -> (meter (num, den), key (tonic letter, tonic pc, mode, signature dict),
                      events) or raises Skip(reason)

events: ('N', letter_index (0=C..6=B), octave (4 = middle C's), midi, dur) for notes,
        ('R', dur) rests, ('T3',) a triplet of the next three notes (durations are the written ones;
        they sound 2/3 as long), ('B', kind) bar lines with kind in
        '|', '||', '|]', '|:', ':|', '::', '1', '2' (first/second ending starts at this bar line,
        possibly combined with a repeat end, which is emitted first as ':|').
dur is a Fraction of a whole note. Decorations, slurs, grace notes, chord symbols, annotations
and ties across bar lines are dropped; ties within a bar merge equal notes; chords keep their top note.
"""
import re
from fractions import Fraction as Fr


class Skip(Exception):
    pass


LETTERS = "CDEFGAB"
LETTER_PC = [0, 2, 4, 5, 7, 9, 11]
# modes as offsets of the tonic within the major scale of the signature
MODE_ALIASES = {
    "": "maj", "maj": "maj", "major": "maj", "ion": "maj", "ionian": "maj",
    "m": "min", "min": "min", "minor": "min", "aeo": "min", "aeolian": "min",
    "dor": "dor", "dorian": "dor", "mix": "mix", "mixolydian": "mix",
    "phr": "phr", "phrygian": "phr", "lyd": "lyd", "lydian": "lyd", "loc": "loc", "locrian": "loc",
}
# semitones from the major-scale tonic of the signature to the mode's tonic
MODE_SHIFT = {"maj": 0, "dor": 2, "phr": 4, "lyd": 5, "mix": 7, "min": 9, "loc": 11}
# order of sharps / flats
SHARPS = "FCGDAEB"
FLATS = "BEADGCF"
MAJOR_SHARPS = {"C": 0, "G": 1, "D": 2, "A": 3, "E": 4, "B": 5, "F#": 6, "C#": 7,
                "F": -1, "Bb": -2, "Eb": -3, "Ab": -4, "Db": -5, "Gb": -6, "Cb": -7}
NAME_PC = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def parse_key(s):
    """K: field -> (tonic letter index, tonic pc, mode, signature {letter index: alteration})."""
    s = s.split("%")[0].strip()
    if not s or s.lower() in ("none",) or s.startswith("H"):
        raise Skip("key " + s)
    m = re.match(r"([A-Ga-g])([#b]?)\s*([A-Za-z]*)\s*(.*)$", s)
    if not m:
        raise Skip("key " + s)
    letter = m.group(1).upper()
    acc = m.group(2)
    mode_s = m.group(3).lower()
    rest = m.group(4)
    mode = None
    for k in sorted(MODE_ALIASES, key=len, reverse=True):
        if k and mode_s.startswith(k[:3]) and len(k) >= 3 and mode_s[:3] == k[:3]:
            mode = MODE_ALIASES[k]
            break
    if mode is None:
        if mode_s in ("", ):
            mode = "maj"
        elif mode_s == "m" or mode_s.startswith("m") and not mode_s.startswith("mix") and not mode_s.startswith("maj"):
            mode = "min"
        else:
            raise Skip("mode " + mode_s)
    tonic_pc = (NAME_PC[letter] + (1 if acc == "#" else -1 if acc == "b" else 0)) % 12
    # the major key whose signature this mode uses
    maj_pc = (tonic_pc - MODE_SHIFT[mode]) % 12
    # find a major-key name with that pc (prefer few accidentals)
    best = None
    for name, n in MAJOR_SHARPS.items():
        pc = (NAME_PC[name[0]] + (1 if name[1:] == "#" else -1 if name[1:] == "b" else 0)) % 12
        if pc == maj_pc and (best is None or abs(n) < abs(best)):
            best = n
    if best is None:
        raise Skip("key " + s)
    sig = {}
    if best > 0:
        for c in SHARPS[:best]:
            sig[LETTERS.index(c)] = 1
    elif best < 0:
        for c in FLATS[:-best]:
            sig[LETTERS.index(c)] = -1
    # explicit accidentals in the K: field (e.g. "K:D =c")
    for am in re.finditer(r"(\^\^|\^|__|_|=)([A-Ga-g])", rest):
        a = {"^": 1, "^^": 2, "_": -1, "__": -2, "=": 0}[am.group(1)]
        sig[LETTERS.index(am.group(2).upper())] = a
    return LETTERS.index(letter), tonic_pc, mode, sig


def parse_meter(s):
    s = s.split("%")[0].strip()
    if s in ("C",):
        return (4, 4)
    if s in ("C|",):
        return (2, 2)
    m = re.match(r"(\d+)\s*/\s*(\d+)$", s)
    if not m:
        raise Skip("meter " + s)
    return (int(m.group(1)), int(m.group(2)))


def parse_len(s):
    s = s.split("%")[0].strip()
    m = re.match(r"(\d+)\s*/\s*(\d+)$", s)
    if not m:
        raise Skip("length " + s)
    return Fr(int(m.group(1)), int(m.group(2)))


def parse_tunes(text, src=""):
    tunes = []
    cur = None
    for line in text.splitlines():
        line = line.rstrip("\r")
        if re.match(r"X:", line):
            if cur:
                tunes.append(cur)
            cur = {"X": line[2:].strip(), "T": "", "M": None, "L": None, "K": None, "R": "",
                   "body": [], "src": src, "inbody": False}
            continue
        if cur is None:
            continue
        if not line.strip():
            if cur["inbody"]:
                tunes.append(cur)
                cur = None
            continue
        if line.startswith("%"):
            continue
        fm = re.match(r"([A-Za-z]):(.*)$", line)
        if fm and not cur["inbody"]:
            f, v = fm.group(1), fm.group(2).strip()
            if f == "T" and not cur["T"]:
                cur["T"] = v
            elif f in ("M", "L", "R"):
                cur[f] = v
            elif f == "K":
                cur["K"] = v
                cur["inbody"] = True
            continue
        if cur["inbody"]:
            cur["body"].append(line)
    if cur:
        tunes.append(cur)
    for t in tunes:
        del t["inbody"]
    return [t for t in tunes if t["K"] is not None and t["body"]]


DUR_RE = re.compile(r"(\d*)(/*)(\d*)")


def read_dur(s, i):
    """ABC length suffix at s[i:] -> (multiplier Fraction, new index)."""
    m = DUR_RE.match(s, i)
    num, slashes, den = m.group(1), m.group(2), m.group(3)
    n = int(num) if num else 1
    if slashes:
        if den:
            d = int(den) * (2 ** (len(slashes) - 1))
        else:
            d = 2 ** len(slashes)
    else:
        if den:
            raise Skip("dur")
        d = 1
    return Fr(n, d), m.end()


def parse_body(t):
    meter = parse_meter(t["M"] or "4/4")
    if t["L"]:
        unit = parse_len(t["L"])
    else:
        unit = Fr(1, 16) if meter[0] / meter[1] < 0.75 else Fr(1, 8)
    key = parse_key(t["K"])
    sig = dict(key[3])
    # join lines; handle field lines inside the body
    body = []
    for line in t["body"]:
        line = line.split("%")[0]
        fm = re.match(r"([A-Za-z]):(.*)$", line)
        if fm:
            f, v = fm.group(1), fm.group(2).strip()
            if f in ("w", "W", "N", "P", "T", "R", "Q", "I", "H", "Z", "B", "S", "O", "C", "D", "F", "G"):
                continue
            if f == "K":
                k2 = parse_key(v)
                if k2[1:3] != key[1:3] or k2[3] != key[3]:
                    raise Skip("key change")
                continue
            if f == "M":
                if parse_meter(v) != meter:
                    raise Skip("meter change")
                continue
            if f == "L":
                body.append("[L:%s]" % v)
                continue
            if f == "V":
                raise Skip("voices")
            continue
        body.append(line)
    s = " ".join(x.rstrip("\\ ") if x.rstrip().endswith("\\") else x for x in body)

    events = []
    acc_bar = {}            # (letter, octave) -> alteration within the bar
    i = 0
    n = len(s)
    pending_triplet = 0
    broken = None           # pending broken-rhythm factor for the next note
    tie = False
    last_note_idx = None
    while i < n:
        c = s[i]
        if c in " \t`$y\\":
            i += 1
            continue
        if c == '"':
            j = s.find('"', i + 1)
            if j < 0:
                raise Skip("quote")
            i = j + 1
            continue
        if c == "!" or c == "+":
            j = s.find(c, i + 1)
            if j < 0:
                raise Skip("deco")
            i = j + 1
            continue
        if c == "{":
            j = s.find("}", i + 1)
            if j < 0:
                raise Skip("grace")
            i = j + 1
            continue
        if c in "~.HIJLMNOPQSTUVWhijklmnopqrstuvw":
            i += 1
            continue
        if c == "(":
            m = re.match(r"\((\d)(?::(\d*))?(?::(\d*))?", s[i:])
            if m:
                p = int(m.group(1))
                q = m.group(2)
                if p != 3 or (q and q != "2") or (m.group(3) and m.group(3) != "3"):
                    raise Skip("tuplet %d" % p)
                pending_triplet = 3
                events.append(("T3",))
                i += m.end()
                continue
            i += 1          # slur
            continue
        if c == ")":
            i += 1
            continue
        if c == "-":
            tie = True
            i += 1
            continue
        if c in "<>":
            j = i
            while j < n and s[j] == c:
                j += 1
            k = j - i
            if k > 2 or last_note_idx is None:
                raise Skip("broken")
            ev = events[last_note_idx]
            if c == ">":
                f1, f2 = (Fr(3, 2), Fr(1, 2)) if k == 1 else (Fr(7, 4), Fr(1, 4))
            else:
                f1, f2 = (Fr(1, 2), Fr(3, 2)) if k == 1 else (Fr(1, 4), Fr(7, 4))
            events[last_note_idx] = ev[:-1] + (ev[-1] * f1,)
            broken = f2
            i = j
            continue
        if c == "[":
            m = re.match(r"\[([KLMVPIQNRT]):([^\]]*)\]", s[i:])
            if m:
                f, v = m.group(1), m.group(2)
                if f == "L":
                    unit = parse_len(v)
                elif f == "K":
                    k2 = parse_key(v)
                    if k2[1:3] != key[1:3] or k2[3] != key[3]:
                        raise Skip("key change")
                elif f == "M":
                    if parse_meter(v) != meter:
                        raise Skip("meter change")
                elif f == "V":
                    raise Skip("voices")
                i += m.end()
                continue
            m = re.match(r"\[(\d)", s[i:])
            if m:       # [1 / [2 ending after a bar line
                events.append(("B", m.group(1)))
                i += m.end()
                while i < n and s[i] in ",-0123456789":
                    i += 1
                continue
            if i + 1 < n and s[i + 1] == "|":
                bm = re.match(r"\[\|+(:*)", s[i:])
                events.append(("B", "|:" if bm.group(1) else "||"))
                acc_bar = {}
                i += bm.end()
                continue
            # chord: collect notes up to ]
            j = s.find("]", i)
            if j < 0:
                raise Skip("chord")
            inner = s[i + 1:j]
            notes = list(re.finditer(r"(\^\^|\^|__|_|=)?([A-Ga-g])([',]*)(\d*/*\d*)", inner))
            if not notes:
                i = j + 1
                continue
            best = None
            for nm in notes:
                ev = make_note(nm.group(1), nm.group(2), nm.group(3), sig, acc_bar, commit=False)
                mul, _ = read_dur(nm.group(4) or "", 0)
                if best is None or ev[2] > best[0][2]:
                    best = (ev, mul, nm)
            ev, mul, nm = best
            ev = make_note(nm.group(1), nm.group(2), nm.group(3), sig, acc_bar, commit=True)
            i = j + 1
            mul2, i = read_dur(s, i)
            dur = unit * mul * mul2
            i, last_note_idx, broken, tie, pending_triplet = add_note(
                events, ev, dur, broken, tie, pending_triplet, last_note_idx, i)
            continue
        if c in "|:":
            m = re.match(r"(:*)(\|\]|\|+|\]|)(:*)(\[?\d)?", s[i:])
            tok = m.group(0)
            if not tok:
                i += 1
                continue
            lead, mid, trail, ending = m.group(1), m.group(2), m.group(3), m.group(4)
            if lead and trail:
                kinds = ["::"]
            elif lead:
                kinds = [":|"]
            elif trail:
                kinds = ["|:"]
            elif mid in ("|]",) or mid == "]":
                kinds = ["|]"]
            elif mid.startswith("||"):
                kinds = ["||"]
            elif mid == "|":
                kinds = ["|"]
            else:
                kinds = ["|"]
            if not mid and lead and not trail:
                kinds = [":|"]
            if ending:
                d = ending.lstrip("[")
                if kinds == ["|"]:
                    kinds = [d]
                else:
                    kinds = kinds + [d]
            for k in kinds:
                events.append(("B", k))
            acc_bar = {}
            tie = False
            i += m.end()
            while i < n and s[i] in ",-0123456789" and ending:
                i += 1
            continue
        if c in "^_=" or c.upper() in LETTERS:
            m = re.match(r"(\^\^|\^|__|_|=)?([A-Ga-g])([',]*)", s[i:])
            if not m:
                raise Skip("note at %r" % s[i:i + 8])
            ev = make_note(m.group(1), m.group(2), m.group(3), sig, acc_bar, commit=True)
            i += m.end()
            mul, i = read_dur(s, i)
            dur = unit * mul
            i, last_note_idx, broken, tie, pending_triplet = add_note(
                events, ev, dur, broken, tie, pending_triplet, last_note_idx, i)
            continue
        if c in "zx":
            i += 1
            mul, i = read_dur(s, i)
            dur = unit * mul
            if broken is not None:
                dur *= broken
                broken = None
            events.append(("R", dur))
            last_note_idx = len(events) - 1
            tie = False
            if pending_triplet:
                pending_triplet -= 1
            continue
        if c in "ZX":
            raise Skip("multibar rest")
        raise Skip("char %r %r" % (c, s[max(0, i - 12):i + 6]))
    return meter, key, events


def make_note(acc, letter, octs, sig, acc_bar, commit):
    li = LETTERS.index(letter.upper())
    octave = 4 if letter.isupper() else 5
    for o in octs:
        octave += 1 if o == "'" else -1
    if acc:
        a = {"^": 1, "^^": 2, "_": -1, "__": -2, "=": 0}[acc]
        if commit:
            acc_bar[(li, octave)] = a
    else:
        a = acc_bar.get((li, octave), sig.get(li, 0))
    midi = 12 * (octave + 1) + LETTER_PC[li] + a
    return ("N", li, octave, midi)


def add_note(events, ev, dur, broken, tie, pending_triplet, last_note_idx, i):
    if broken is not None:
        dur *= broken
        broken = None
    if tie and last_note_idx is not None and events[last_note_idx][0] == "N" \
            and events[last_note_idx][3] == ev[3] and not pending_triplet:
        prev = events[last_note_idx]
        events[last_note_idx] = prev[:4] + (prev[4] + dur,)
        tie = False
        return i, last_note_idx, broken, tie, pending_triplet
    tie = False
    events.append(ev + (dur,))
    if pending_triplet:
        pending_triplet -= 1
    return i, len(events) - 1, broken, tie, pending_triplet
