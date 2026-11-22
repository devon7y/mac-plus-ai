"""Tune's token vocabulary and the bar/section state machine (shared by the corpus builder, training,
the integer reference and, mirrored in C, the Mac app).

A tune is BOS, PICK(p), then notes, rests, triplet marks and bar lines, then EOS. Pitches are diatonic
degrees of the tune's mode counted from its tonic (degree 0); UP / DOWN before a note raise or lower
it a semitone from the mode's scale. Durations are nominal, in ticks (an eighth note = 6 ticks); the
three notes after T3 sound 2/3 of their nominal length. Conditioning (type, meter, mode) is not in the
token stream: the model gets it as extra inputs.

State after each token (State.step): pos (ticks into the bar), trip (triplet notes still to come),
bar (bar index in the section), sec (section index); these are the model's position inputs.
State.allowed() gives the grammar mask used when sampling.
"""

PAD, BOS, EOS, BAR, DBL, RS, RE, RER, E1, E2, UP, DOWN, T3 = range(13)
BAR_NAMES = {BAR: "|", DBL: "||", RS: "|:", RE: ":|", RER: "::", E1: "|1", E2: ":|2"}
SECTION_TOKENS = (DBL, RS, RE, RER, E2)

PICKS = [0, 3, 6, 9, 12, 18, 24]                  # pickup length in ticks
DURS = [3, 6, 9, 12, 18, 24, 30, 36]              # nominal note/rest lengths in ticks
DEG_MIN, DEG_MAX = -5, 18                          # diatonic degrees from the tonic
NDEG = DEG_MAX - DEG_MIN + 1

PICK0 = 13
REST0 = PICK0 + len(PICKS)
NOTE0 = REST0 + len(DURS)
VOCAB = NOTE0 + NDEG * len(DURS)

TYPES = ["reel", "jig", "hornpipe", "air", "march", "waltz", "slipjig", "strathspey", "polka"]
METERS = [(4, 4), (6, 8), (3, 4), (2, 4), (9, 8), (12, 8), (3, 8)]
MODES = ["maj", "min", "dor", "mix"]
MODE_STEPS = {"maj": [0, 2, 4, 5, 7, 9, 11], "min": [0, 2, 3, 5, 7, 8, 10],
              "dor": [0, 2, 3, 5, 7, 9, 10], "mix": [0, 2, 4, 5, 7, 9, 10]}

MAXPOS = 72        # position input table size (ticks)
MAXBAR = 17       # bar index inputs 0..15, and 16 for a pickup (partial first) bar
HISTBARS = 33     # bars remembered per section for the look-back inputs (-1..31)
MAXSEC = 4


def bar_ticks(meter):
    return meter[0] * 48 // meter[1]


def note_tok(deg, d):
    return NOTE0 + (deg - DEG_MIN) * len(DURS) + DURS.index(d)


def rest_tok(d):
    return REST0 + DURS.index(d)


def tok_info(t):
    """('note', deg, dur) / ('rest', dur) / ('pick', ticks) / ('bar', t) / ('up',) ..."""
    if t >= NOTE0:
        k = t - NOTE0
        return ("note", DEG_MIN + k // len(DURS), DURS[k % len(DURS)])
    if t >= REST0:
        return ("rest", DURS[t - REST0])
    if t >= PICK0:
        return ("pick", PICKS[t - PICK0])
    return ({BOS: "bos", EOS: "eos", UP: "up", DOWN: "down", T3: "t3", PAD: "pad"}.get(t, "bar"), t)


def tok_dur(t):
    i = tok_info(t)
    if i[0] == "note":
        return i[2]
    if i[0] == "rest":
        return i[1]
    return 0


def tok_name(t):
    i = tok_info(t)
    if i[0] == "note":
        return "n%d/%d" % (i[1], i[2])
    if i[0] == "rest":
        return "z/%d" % i[1]
    if i[0] == "pick":
        return "P%d" % i[1]
    if i[0] == "bar":
        return BAR_NAMES[t]
    return i[0].upper()


class State:
    """Bar/section bookkeeping. Integer-only and identical in the C app (tune/app/grammar.c)."""

    def __init__(self, meter):
        self.blen = bar_ticks(meter)
        self.pos = 0
        self.trip = 0         # triplet notes still to come
        self.tdur = 0         # nominal duration of the current triplet's notes
        self.bar = 0
        self.sec = 0
        self.e1pos = -1       # pos/bar saved at E1 (-1: no first ending open)
        self.e1bar = 0
        self.acc = 0          # an UP/DOWN is pending
        self.started = False  # PICK seen
        self.ntok = 0
        self.last = BOS
        self.nbars = 0        # bar lines that closed a full bar
        self.hist = {}        # (section, bar) -> {tick position: note/rest token} (look-back inputs)

    def feats(self):
        """(pos, trip, bar, sec) inputs for the next prediction."""
        bar = MAXBAR - 1 if self.bar < 0 else min(self.bar, MAXBAR - 2)
        return (min(self.pos, MAXPOS - 1), self.trip, bar, min(self.sec, MAXSEC - 1))

    def _key(self, sec, bar):
        return (min(sec, MAXSEC - 1), min(bar, HISTBARS - 2))

    def lookback(self):
        """(sym, dur) of the note or rest that started at this bar position 4 bars ago in this
        section, and at this bar of the previous section; sym NSYM (and dur 0) when there is none."""
        out = []
        for sec, bar in ((self.sec, self.bar - 4), (self.sec - 1, self.bar)):
            t = None
            if sec >= 0 and bar >= -1:
                t = self.hist.get(self._key(sec, bar), {}).get(self.pos)
            out += list(tok_inputs(t)) if t is not None else [NSYM, 0]
        return tuple(out)

    def allowed(self, maxtok=10000, maxbars=1000):
        """Boolean list over the vocabulary: tokens the grammar allows next. maxtok / maxbars cap
        the length: past either, the tune is steered to an end (only EOS or a final repeat)."""
        ok = [False] * VOCAB
        if not self.started:
            for i, p in enumerate(PICKS):
                if p < self.blen:
                    ok[PICK0 + i] = True
            return ok
        room = self.blen - self.pos
        if self.acc:
            self._notes(ok, room, notes_only=True)
            return ok
        if self.trip:
            self._notes(ok, room, notes_only=False)
            ok[UP] = ok[DOWN] = True
            return ok
        if self.ntok >= maxtok or self.nbars >= maxbars:
            if self.pos == 0:
                ok[EOS] = True
                return ok
            if room > 0:
                self._notes(ok, room, notes_only=False)
            else:
                ok[EOS] = ok[RE] = True
            return ok
        if room > 0:
            self._notes(ok, room, notes_only=False)
            ok[UP] = ok[DOWN] = True
            for d in DURS:                       # a triplet of notes of length d fits in 2d
                if 2 * d <= room and d in (3, 6, 12):
                    ok[T3] = True
        if self.last in (RE, DBL, RER):
            ok[EOS] = True
        if self.pos > 0:
            ok[EOS] = True
            for t in SECTION_TOKENS:
                ok[t] = True
            if self.e1pos < 0:
                ok[E2] = False
            if room == 0:
                ok[BAR] = True
                if self.e1pos < 0:
                    ok[E1] = True
        return ok

    def _notes(self, ok, room, notes_only):
        for di, d in enumerate(DURS):
            if self.trip == 3:                   # first note of a triplet: the whole group must fit
                if d not in (3, 6, 12) or 2 * d > room:
                    continue
            elif self.trip:
                if d != self.tdur:
                    continue
            elif d > room:
                continue
            for k in range(NDEG):
                ok[NOTE0 + k * len(DURS) + di] = True
            if not notes_only:
                ok[REST0 + di] = True

    def step(self, t):
        """Advance past token t (assumed allowed)."""
        self.ntok += 1
        self.last = t
        i = tok_info(t)
        kind = i[0]
        if kind == "pick":
            self.started = True
            self.pos = (self.blen - i[1]) % self.blen
            self.bar = -1 if i[1] else 0
            return
        if kind in ("up", "down"):
            self.acc = 1
            return
        if kind == "t3":
            # the next note fixes the triplet's duration
            self.trip = 3
            self.tdur = 0
            return
        if kind in ("note", "rest"):
            d = i[2] if kind == "note" else i[1]
            self.acc = 0
            self.hist.setdefault(self._key(self.sec, self.bar), {})[self.pos] = t
            if self.trip:
                if self.tdur == 0:
                    self.tdur = d
                self.pos += d * 2 // 3
                self.trip -= 1
                if self.trip == 0:
                    self.tdur = 0
            else:
                self.pos += d
            return
        if kind == "bar":
            full = self.pos >= self.blen
            if full or t in (BAR, E1):
                self.nbars += 1
            if t == BAR:
                self.pos = 0
                self.bar += 1
            elif t == E1:
                self.pos = 0
                self.bar += 1
                self.e1pos, self.e1bar = 0, self.bar
            elif t == E2:
                # the second ending replaces the first: back to where E1 started
                self.pos, self.bar = self.e1pos, self.e1bar
                self.e1pos = -1
            else:                         # DBL, RS, RE, RER: a section boundary
                if full:
                    self.pos = 0
                if t != RS or self.bar > 0 or self.pos > 0:
                    self.sec += 1
                self.bar = 0 if self.pos == 0 else -1     # -1: the next notes are a pickup
                self.e1pos = -1
            return
        # bos / eos: nothing


# ---- model inputs: the previous token factorised into a symbol and a duration -----------------
SPECIALS = [BOS, EOS, BAR, DBL, RS, RE, RER, E1, E2, UP, DOWN, T3]
NSYM = NDEG + len(SPECIALS) + len(PICKS) + 1      # degrees, specials, pickups, rest
NDUR = len(DURS) + 1                              # 0 = no duration


def tok_inputs(t):
    """token -> (sym, dur) input ids."""
    i = tok_info(t)
    if i[0] == "note":
        return i[1] - DEG_MIN, 1 + DURS.index(i[2])
    if i[0] == "rest":
        return NSYM - 1, 1 + DURS.index(i[1])
    if i[0] == "pick":
        return NDEG + len(SPECIALS) + PICKS.index(i[1]), 0
    return NDEG + SPECIALS.index(t), 0
