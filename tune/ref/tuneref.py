"""Tune's integer inference: the exact arithmetic of the Mac app (tune/app/engine.c, gru68k.s), in
Python. See tune/ref/SPEC.md.

  python3 -I tune/ref/tuneref.py tune/build/model.npz --type reel --mode maj --seed 1 [--n 3]

Model: tune/ref/export.py. Grammar and inputs: tune/corpus/vocab.py.
"""
import argparse
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "corpus"))
import vocab as V  # noqa: E402

FEAT_NAMES = ["sym", "dur", "pos", "trip", "bar", "sec", "lasym", "ladur", "lbsym", "lbdur"]
COND_NAMES = ["typ", "met", "mode"]
NROWS = len(FEAT_NAMES) + 1            # rows summed per step (features + the tune's condition row)
TCLAMP = 32767 // NROWS                # each table entry; a sum of NROWS never overflows 16 bits
HONE = 16256                           # h = 1.0 (127 * 128, so hq = (h + 64) >> 7 is in -127..127)


def make_tables():
    """sigmoid (Q15) and tanh (h units) over the whole int16 range in steps of 8 (1/64 in Q9),
    indexed by (p >> 3) + 4096; exp (Q8) for sampling."""
    sig, tanh = [], []
    for i in range(8192):
        xv = ((i - 4096) * 8 + 4) / 512.0
        sig.append(min(32767, int(round(32768 / (1 + math.exp(-xv))))))
        tanh.append(int(round(HONE * math.tanh(xv))))
    exp = [int(round(256 * math.exp(-k / 32.0))) for k in range(256)]
    return sig, tanh, exp


SIG, TANH, EXP = make_tables()
SIG_A = np.array(SIG, np.int64)
TANH_A = np.array(TANH, np.int64)

# meters the app offers per type; the seed picks one when there are several
TYPE_METERS = {"reel": [(4, 4)], "jig": [(6, 8)], "hornpipe": [(4, 4)], "waltz": [(3, 4)],
               "march": [(2, 4), (6, 8)], "air": [(3, 4), (4, 4), (6, 8)], "slipjig": [(9, 8)],
               "strathspey": [(4, 4)], "polka": [(2, 4)]}


def sat16(v):
    return np.clip(v, -32768, 32767)


def wrap16(v):
    return ((v + 32768) & 0xFFFF) - 32768


def scale(acc, M, k):
    """wrap16(((acc * M) >> 16) << k): the high word of a muls.w product, shifted with asl.w."""
    return wrap16(((acc * M) >> 16) << k)


class Model:
    def __init__(self, path):
        z = np.load(path)
        self.L = int(z["L"])
        self.H = [int(z["H%d" % l]) for l in range(self.L)]
        self.T = {k: z["T_" + k].astype(np.int64) for k in FEAT_NAMES + COND_NAMES}
        self.W_hh = [z["W_hh%d" % l].astype(np.int64) for l in range(self.L)]
        self.M_hh = [z["M_hh%d" % l].astype(np.int64) for l in range(self.L)]
        self.k_hh = [int(z["k_hh%d" % l]) for l in range(self.L)]
        self.bhn = [z["bhn%d" % l].astype(np.int64) for l in range(self.L)]
        self.W_ih = [None] + [z["W_ih%d" % l].astype(np.int64) for l in range(1, self.L)]
        self.M_ih = [None] + [z["M_ih%d" % l].astype(np.int64) for l in range(1, self.L)]
        self.k_ih = [None] + [int(z["k_ih%d" % l]) for l in range(1, self.L)]
        self.b_ih = [None] + [z["b_ih%d" % l].astype(np.int64) for l in range(1, self.L)]
        self.W_out = z["W_out"].astype(np.int64)
        self.M_out = z["M_out"].astype(np.int64)
        self.k_out = int(z["k_out"])
        self.B_out = z["B_out"].astype(np.int64)

    def cond_row(self, typ, met, mode):
        c = self.T["typ"][typ] + self.T["met"][met] + self.T["mode"][mode]
        return np.clip(c, -TCLAMP, TCLAMP)


def gru(x, acc, M, k, bhn, h):
    """One GRU layer update. x: input pre-activations (Q9, 3H), acc: W_hh . hq (3H). -> h (new)."""
    H = len(h)
    gh = scale(acc, M, k)
    p = sat16(x[:H] + gh[:H])
    r = SIG_A[(p >> 3) + 4096]
    p = sat16(x[H:2 * H] + gh[H:2 * H])
    z = SIG_A[(p >> 3) + 4096]
    v = sat16(gh[2 * H:] + bhn)
    p = sat16(x[2 * H:] + ((r * v) >> 15))
    n = TANH_A[(p >> 3) + 4096]
    return n + ((z * (h - n)) >> 15)


def rng_next(s):
    return (s * 1103515245 + 12345) & 0xFFFFFFFF


class Gen:
    """One tune being composed. Integer state only."""

    def __init__(self, m, typ, meter, mode, seed, invT=256, cut=1152, maxbars=40, maxtok=600):
        self.m = m
        self.meter = meter
        self.cond = m.cond_row(V.TYPES.index(typ), V.METERS.index(meter), V.MODES.index(mode))
        self.h = [np.zeros(H, np.int64) for H in m.H]
        self.hq = [np.zeros(H, np.int64) for H in m.H]
        self.st = V.State(meter)
        self.rng = seed & 0xFFFFFFFF
        self.invT, self.cut, self.maxbars, self.maxtok = invT, cut, maxbars, maxtok
        self.toks = [V.BOS]
        self.feed(V.BOS)

    def feed(self, tok):
        """Read token tok: update the grammar state and the GRU layers."""
        m = self.m
        if tok != V.BOS:
            self.st.step(tok)
        f = V.tok_inputs(tok) + self.st.feats() + self.st.lookback()
        x = self.cond.copy()
        for name, idx in zip(FEAT_NAMES, f):
            x = x + m.T[name][idx]
        assert np.abs(x).max() <= 32767
        for l in range(m.L):
            if l > 0:
                acc = m.W_ih[l] @ self.hq[l - 1]
                assert np.abs(acc).max() <= 32767
                x = sat16(scale(acc, m.M_ih[l], m.k_ih[l]) + m.b_ih[l])
            acc = m.W_hh[l] @ self.hq[l]
            assert np.abs(acc).max() <= 32767
            self.h[l] = gru(x, acc, m.M_hh[l], m.k_hh[l], m.bhn[l], self.h[l])
            self.hq[l] = (self.h[l] + 64) >> 7

    def logits(self):
        m = self.m
        acc = m.W_out @ self.hq[-1]
        assert np.abs(acc).max() <= 32767
        return sat16(scale(acc, m.M_out, m.k_out) + m.B_out)

    def sample(self):
        ok = self.st.allowed(self.maxtok, self.maxbars)
        L = self.logits()
        allowed = [v for v in range(V.VOCAB) if ok[v]]
        if len(allowed) == 1:          # forced: no logits, no random number
            return allowed[0]
        Lt = {v: max(-32768, min(32767, (int(L[v]) * self.invT) >> 8)) for v in allowed}
        lmax = max(Lt.values())
        p = []
        for v in allowed:
            d = lmax - Lt[v]
            p.append(EXP[d >> 3] if d <= self.cut else 0)
        P = sum(p)
        self.rng = rng_next(self.rng)
        u = (self.rng >> 16) & 0xFFFF
        r = (u * P) >> 16
        acc = 0
        for v, pv in zip(allowed, p):
            acc += pv
            if r < acc:
                return v
        raise AssertionError

    def run(self):
        while True:
            t = self.sample()
            self.toks.append(t)
            if t == V.EOS:
                return self.toks
            self.feed(t)


def pick_meter(typ, seed):
    ms = TYPE_METERS[typ]
    return ms[(seed >> 3) % len(ms)]


def compose(m, typ, mode, seed, **kw):
    meter = pick_meter(typ, seed)
    g = Gen(m, typ, meter, mode, seed, **kw)
    return meter, g.run()


def nll(m, tunes, limit=None):
    """Mean negative log-likelihood (nats/token, grammar-masked, temperature 1) of the integer
    model on corpus tunes (teacher forcing)."""
    tot, n = 0.0, 0
    for r in tunes[:limit]:
        g = Gen(m, r["type"], tuple(r["meter"]), r["mode"], 1)
        for t in r["toks"][1:]:
            ok = g.st.allowed()
            L = g.logits().astype(np.float64) / 256.0
            al = [v for v in range(V.VOCAB) if ok[v]]
            lv = L[al]
            mx = lv.max()
            lse = mx + math.log(np.exp(lv - mx).sum())
            tot += lse - L[t]
            n += 1
            if t == V.EOS:
                break
            g.feed(t)
    return tot / n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("--type", default="reel")
    ap.add_argument("--mode", default="maj")
    ap.add_argument("--key", default="D")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--n", type=int, default=1)
    ap.add_argument("--invT", type=int, default=256)
    ap.add_argument("--cut", type=int, default=1152)
    a = ap.parse_args()
    sys.path.insert(0, HERE)
    import abcout
    m = Model(a.model)
    for i in range(a.n):
        seed = a.seed + i
        meter, toks = compose(m, a.type, a.mode, seed, invT=a.invT, cut=a.cut)
        print(abcout.to_abc(toks, meter, a.mode, a.key, a.type, "Tune %d" % seed, seed))
        print()


if __name__ == "__main__":
    main()
