"""Bit-exact integer reference of Pixel's forward pass and sampling (SPEC.md), plus the float model.

IntModel runs exactly the integer arithmetic the Mac will run: per-token scales are SFs (sf.py),
all dot products are exact integers whose kernel partial sums provably fit in 16 bits (checked at
run time with assertions), and the only lookup tables are EXP12 and SILU (generated here, embedded
in the Mac app).

FloatModel is the same network in float64, in three flavours: plain float, and "qat" (activation
fake-quantization mirroring the integer path) — what training (M3) should simulate.
"""
import math

import numpy as np

import kernels as K
import sf

# ------------------------------------------------------------------ architecture (fixed)
D, L, H, KVH, HD, FF, NE, V = 160, 6, 4, 2, 40, 512, 4, 2048
W = 64                      # memory window (tokens)
QA = 127                    # int8 activations into ternary matvecs
QQ = 117                    # query codes: 40 dims * 7 * 117 = 32760 <= 32767
QKV = 7                     # 4-bit symmetric key/value codes
QF = 58                     # final-norm codes: 80 dims * 7 * 58 = 32480 per vocab chunk
UMAX_BITS = 12              # wsum weights: sum u <= 2**12 + 32
RES_SHIFT = 16              # residual stream: int32, real = r / 2**16
ALIBI_SLOPES = [2.0 ** -2, 2.0 ** -4, 2.0 ** -6, 2.0 ** -8]
SILU_HALF = 512             # SILU table covers gi in [-512, 512) (gi in 1/64 units)
EPS = 1e-6                  # float model RMSNorm epsilon (integer path: none, see SPEC)
EXP_N, EXP_ONE = 512, 4095  # EXP12 table: 512 entries, 1/32-nat steps, 12-bit values
TOPK_MAX = 16               # sampler candidates (keeps the draw within 16x16-bit multiplies)


def exp12_table():
    """EXP12[i] = round(4095 * exp(-i/32)), i = 0..511 (zero from i = 289 on)."""
    out = []
    for i in range(EXP_N):
        v = EXP_ONE * math.exp(-i / 32.0)
        assert abs(v - math.floor(v) - 0.5) > 1e-6     # no rounding ties: platform-independent
        out.append(int(math.floor(v + 0.5)))
    return out


EXP12 = exp12_table()


def exp_index(d):
    """Softmax/gate/sampler index from a non-negative difference d in 1/64-nat units:
    round half up to 1/32 nat, clamped to 511 (d >= 1022 -> 511; never adds past 16 bits)."""
    return EXP_N - 1 if d >= 2 * EXP_N - 2 else (d + 1) >> 1


def silu_table():
    """SILU[j] = round(64 * silu((j - 512) / 64)), j = 0..1023 (int16)."""
    out = []
    for j in range(2 * SILU_HALF):
        g = (j - SILU_HALF) / 64.0
        v = 64.0 * g / (1.0 + math.exp(-g))
        assert abs(v - math.floor(v) - 0.5) > 1e-6     # no rounding ties
        out.append(int(math.floor(v + 0.5)))
    return out


SILU = silu_table()


def silu64(gi):
    """silu of gate values in 1/64 units, exact beyond the table (identity above, 0 below)."""
    gi = np.asarray(gi, dtype=np.int64)
    out = np.where(gi >= SILU_HALF, gi, 0)
    inside = (gi >= -SILU_HALF) & (gi < SILU_HALF)
    tab = np.array(SILU, dtype=np.int64)
    out = np.where(inside, tab[np.clip(gi + SILU_HALF, 0, 2 * SILU_HALF - 1)], out)
    return out


def bitlen(v):
    return int(v).bit_length()


def clamp(x, lo, hi):
    return np.clip(x, lo, hi) if isinstance(x, np.ndarray) else max(lo, min(hi, x))


def alibi_table(w=W):
    return np.array([[int(math.floor(64 * s * d + 0.5)) for d in range(w)] for s in ALIBI_SLOPES], dtype=np.int64)


# ------------------------------------------------------------------ RNG
class XorShift32:
    """xorshift32 (13, 17, 5), exactly the benchmark's `rand`. Seed 0 is replaced by 0x2545F491."""

    def __init__(self, seed):
        self.x = (seed & 0xFFFFFFFF) or 0x2545F491

    def next(self):
        x = self.x
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.x = x
        return x


# ------------------------------------------------------------------ integer building blocks
class Counter(dict):
    def add(self, k, n=1):
        self[k] = self.get(k, 0) + n


def quant16(x, Q, cnt=None):
    """Symmetric absmax quantization of a 16-bit integer vector to [-Q, Q].
    Returns (codes, F) with codes = round(x * F) and F = Q / max|x| as an SF (F = 0 if x == 0)."""
    x = np.asarray(x, dtype=np.int64)
    M = int(np.abs(x).max(initial=0))
    if cnt is not None:
        cnt.add("mul", len(x))
    if M == 0:
        return np.zeros_like(x), sf.ZERO
    F = sf.div(sf.from_int(Q), sf.from_int(M))
    y = sf.apply(x, F)
    assert np.abs(y).max() <= Q
    return y, F


def normq(r, g, gs, Q, cnt=None):
    """Fixed-point RMSNorm with gain, then absmax quantization to [-Q, Q].
    Returns (y, beta): y * beta ~ r / rms(r) * gain (real units). SPEC.md 5.1."""
    r = np.asarray(r, dtype=np.int64)
    n = len(r)
    if cnt is not None:
        cnt.add("mul", 3 * n)
    M0 = int(np.abs(r).max(initial=0))
    if M0 == 0:
        return np.zeros(n, dtype=np.int64), sf.ZERO
    sh = bitlen(M0) - 15
    t = r >> sh if sh >= 0 else r << -sh
    ss = int(((t * t) >> 8).sum())
    a = t * np.asarray(g, dtype=np.int64)
    Ma = int(np.abs(a).max())
    if Ma == 0:
        return np.zeros(n, dtype=np.int64), sf.ZERO
    s2 = max(0, bitlen(Ma) - 15)
    b = a >> s2
    Mb = int(np.abs(b).max())
    F = sf.div(sf.from_int(Q), sf.from_int(Mb))
    y = sf.apply(b, F)
    assert np.abs(y).max() <= Q
    num = sf.mul(sf.mul(sf.pow2(s2 - 4), gs), sf.SQRT_D)
    den = sf.mul(F, sf.sqrt_int(ss))
    return y, sf.div(num, den)


TRACE = None   # tests set a list here to capture kernel inputs (replayed through kernels.py)


def tern_dot(T, y, chunk=None):
    """Exact ternary matvec. Asserts the 68000 kernel cannot overflow: every chunk of `chunk`
    inputs (a kernel call) has a true partial sum within int16."""
    out = T.astype(np.int64) @ y
    if TRACE is not None:
        TRACE.append(("tern", T, y.copy(), chunk, out.copy()))
    if chunk is None:
        assert np.abs(out).max(initial=0) <= 32767
    else:
        for c0 in range(0, T.shape[1], chunk):
            part = T[:, c0:c0 + chunk].astype(np.int64) @ y[c0:c0 + chunk]
            assert np.abs(part).max(initial=0) <= 32767
    return out


def sat32(x):
    return np.clip(x, -(1 << 31), (1 << 31) - 1)


# ------------------------------------------------------------------ the integer model
class IntModel:
    """Packed integer model (see export.py for the layout) + KV cache + the per-token forward pass."""

    def __init__(self, pm):
        self.pm = pm
        self.alibi = np.asarray(pm["alibi"], dtype=np.int64)
        self.reset()

    def reset(self):
        self.pos = 0
        self.cache = [{
            "kc": np.zeros((W, KVH, HD), dtype=np.int64), "sk": [[sf.ZERO] * KVH for _ in range(W)],
            "vc": np.zeros((W, KVH, HD), dtype=np.int64), "sv": [[sf.ZERO] * KVH for _ in range(W)],
        } for _ in range(L)]
        self.cnt = Counter()

    # -- embedding
    def embed(self, tok):
        pm = self.pm
        p = pm["emb_codes"][tok].astype(np.int64) * int(pm["emb_m"][tok])
        s = pm["emb_e"] + RES_SHIFT
        return p << s if s >= 0 else (p + (1 << (-s - 1))) >> -s

    # -- attention for one layer
    def attention(self, l, r, slot, nvalid):
        lw, c, cnt = self.pm["layers"][l], self.cache[l], self.cnt
        y1, b1 = normq(r, lw["g_attn"], lw["gs_attn"], QA, cnt)
        acc = tern_dot(lw["wqkv"], y1)                       # 320 outputs, |acc| <= 20320
        cnt.add("tern_rows", 320)
        accq, acck, accv = acc[:D], acc[D:D + KVH * HD], acc[D + KVH * HD:]
        sq0, sk0, sv0 = sf.mul(lw["a_q"], b1), sf.mul(lw["a_k"], b1), sf.mul(lw["a_v"], b1)
        for j in range(KVH):
            kc, Fk = quant16(acck[j * HD:(j + 1) * HD], QKV, cnt)
            vc, Fv = quant16(accv[j * HD:(j + 1) * HD], QKV, cnt)
            c["kc"][slot, j], c["vc"][slot, j] = kc, vc
            c["sk"][slot][j], c["sv"][slot][j] = sf.div(sk0, Fk), sf.div(sv0, Fv)
        # valid slots and their distances (ring buffer: slot of position p is p % W)
        slots = [(self.pos - d) % W for d in range(nvalid)]
        acco, so = [], []
        for h in range(H):
            j = h // (H // KVH)
            qh, Fq = quant16(accq[h * HD:(h + 1) * HD], QQ, cnt)
            Cq = sf.mul(sf.div(sq0, Fq), sf.ATT_C)
            z = []
            if TRACE is not None:
                kcs = np.stack([c["kc"][t, j] for t in slots])
                TRACE.append(("score", kcs, qh.copy(), kcs @ qh))
            for d, t in enumerate(slots):
                S = int(c["kc"][t, j] @ qh)
                assert abs(S) <= 32767
                zt = sf.apply(S, sf.mul(Cq, c["sk"][t][j])) - int(self.alibi[h, d])
                z.append(clamp(zt, -32768, 32767))
            cnt.add("mul", 2 * nvalid)
            cnt.add("pair_lookups", nvalid * HD // 2)
            zmax = max(z)
            p = [EXP12[exp_index(zmax - zt)] for zt in z]
            P = sum(p)                                          # <= 64 * 4095
            svs = [c["sv"][t][j] for t in slots]
            nz = [s for s in svs if s[0]]
            if not nz:
                acco.append(np.zeros(HD, dtype=np.int64))
                so.append(sf.ZERO)
                continue
            emax = max(s[1] for s in nz)
            w = []
            for pt, s in zip(p, svs):
                k = emax - s[1]
                w.append(0 if s[0] == 0 or k >= 32 else (pt * (s[0] >> 3)) >> k)
            Ws = sum(w)
            cnt.add("mul", nvalid)
            if Ws == 0:
                acco.append(np.zeros(HD, dtype=np.int64))
                so.append(sf.ZERO)
                continue
            sh = max(0, bitlen(Ws) - UMAX_BITS)
            half = (1 << (sh - 1)) if sh else 0
            u = np.array([(wt + half) >> sh for wt in w], dtype=np.int64)
            assert u.sum() * QKV <= 32767
            vcs = np.stack([c["vc"][t, j] for t in slots])
            a = u @ vcs                                         # wsum, |a| <= 7 * sum(u)
            if TRACE is not None:
                TRACE.append(("wsum", vcs, u.copy(), a.copy()))
            assert np.abs(a).max() <= 32767
            cnt.add("wsum_lookups", nvalid * HD)
            acco.append(a)
            so.append(sf.div(sf.pow2(sh + emax + 3), sf.from_int(P)))
        # requantize the 4 heads (different scales) into one int8 vector
        R = [sf.mul(sf.from_int(int(np.abs(a).max())), s) for a, s in zip(acco, so)]
        Mo = sf.ZERO
        for x in R:
            Mo = sf.smax(Mo, x)
        cnt.add("mul", D)
        if Mo[0] == 0:
            return np.zeros(D, dtype=np.int64)
        yo = np.concatenate([clamp(sf.apply(a, sf.div(sf.mul(s, sf.from_int(QA)), Mo)), -QA, QA)
                             for a, s in zip(acco, so)])
        bo = sf.div(Mo, sf.from_int(QA))
        acc_o = tern_dot(lw["wo"], yo)
        cnt.add("tern_rows", D)
        cnt.add("mul", D)
        return sat32(sf.apply(acc_o, sf.mul(sf.mul(lw["a_o"], bo), sf.pow2(RES_SHIFT))))

    # -- mixture-of-experts feed-forward for one layer
    def ffn(self, l, r):
        lw, cnt = self.pm["layers"][l], self.cnt
        y2, b2 = normq(r, lw["g_ffn"], lw["gs_ffn"], QA, cnt)
        rl = lw["router"].astype(np.int64) @ y2               # int32 router logits
        cnt.add("mul", NE * D)
        top = int(np.argmax(rl))                              # first maximum
        sr = sf.mul(sf.mul(lw["s_r"], b2), sf.pow2(6))        # router logit units -> 1/64 nat
        Pg = 0
        for e in range(NE):
            d = sf.apply(1, sf.mul(sf.from_int(int(rl[top] - rl[e])), sr))
            Pg += EXP12[exp_index(min(d, 65535))]
        gate = sf.div(sf.from_int(EXP_ONE), sf.from_int(Pg))
        ex = lw["experts"][top]
        accg, accu = tern_dot(ex["wg"], y2), tern_dot(ex["wu"], y2)
        cnt.add("tern_rows", 2 * FF)
        gi = clamp(sf.apply(accg, sf.mul(sf.mul(ex["a_g"], b2), sf.pow2(6))), -32768, 32767)
        hh = silu64(gi) * accu                                 # |hh| < 2**30
        cnt.add("mul", 2 * FF)
        Mh = int(np.abs(hh).max())
        if Mh == 0:
            return np.zeros(D, dtype=np.int64), top
        s3 = max(0, bitlen(Mh) - 15)
        yh, Fh = quant16(hh >> s3, QA, cnt)
        bh = sf.div(sf.mul(sf.mul(ex["a_u"], b2), sf.pow2(s3 - 6)), Fh)
        accd = tern_dot(ex["wd"], yh, chunk=FF // 2)          # two kernel calls of 256 inputs
        cnt.add("tern_rows", 2 * D)
        cnt.add("mul", D)
        Fd = sf.mul(sf.mul(sf.mul(ex["a_d"], bh), gate), sf.pow2(RES_SHIFT + 1))
        return sat32(sf.apply(accd >> 1, Fd)), top

    # -- one token
    def step(self, tok, want_logits=True):
        """Process one token at position self.pos. Returns (Klogits int array, sigma SF) or None.
        Real logit_v = K[v] * sigma."""
        slot = self.pos % W
        nvalid = min(self.pos + 1, W)
        r = self.embed(tok)
        self.experts = []
        for l in range(L):
            r = sat32(r + self.attention(l, r, slot, nvalid))
            delta, top = self.ffn(l, r)
            self.experts.append(top)
            r = sat32(r + delta)
        self.pos += 1
        self.last_r = r
        if not want_logits:
            return None
        pm = self.pm
        yf, bf = normq(r, pm["g_final"], pm["gs_final"], QF, self.cnt)
        lg = pm["emb_codes"].astype(np.int64) @ yf             # 2 chunks of 80 dims per row
        if TRACE is not None:
            TRACE.append(("vocab", pm["emb_codes"], yf.copy(), lg.copy()))
        for c0 in (0, D // 2):
            part = pm["emb_codes"][:, c0:c0 + D // 2].astype(np.int64) @ yf[c0:c0 + D // 2]
            assert np.abs(part).max() <= 32767
        self.cnt.add("pair_lookups", V * D // 2)
        Kl = (lg >> 1) * pm["emb_m"].astype(np.int64)
        self.cnt.add("mul", V)
        return Kl, sf.mul(sf.pow2(pm["emb_e"] + 1), bf)


def sample(Kl, sigma, rng, temp256=205, topk=16):
    """Pick the next token. temp256 = temperature * 256 (0 = greedy), topk <= 16. SPEC.md 7."""
    if temp256 == 0 or topk <= 1:
        return int(np.argmax(Kl))
    assert topk <= TOPK_MAX
    order = sorted(range(len(Kl)), key=lambda v: (-int(Kl[v]), v))[:topk]
    kmax = int(Kl[order[0]])
    sT = sf.div(sf.mul(sigma, sf.pow2(6 + 8)), sf.from_int(temp256))     # 1/64 nat per unit
    ps = []
    for v in order:
        d = sf.apply(1, sf.mul(sf.from_int(kmax - int(Kl[v])), sT))
        ps.append(EXP12[exp_index(min(d, 65535))])
    tot = sum(ps)                                                        # <= 16 * 4095 < 2**16
    x = rng.next()
    pick = ((x >> 16) * tot) >> 16
    cum = 0
    for v, p in zip(order, ps):
        cum += p
        if cum > pick:
            return v
    raise AssertionError("unreachable")


def generate(model, prompt, n_new, seed=1, temp256=205, topk=16, stop=None):
    """Prompt ingestion + generation. Logits are only computed for the last prompt token."""
    rng = XorShift32(seed)
    out = []
    for i, t in enumerate(prompt):
        res = model.step(t, want_logits=(i == len(prompt) - 1))
    for _ in range(n_new):
        tok = sample(*res, rng, temp256, topk)
        out.append(tok)
        if stop is not None and tok == stop:
            break
        res = model.step(tok)
    return out


# ------------------------------------------------------------------ the float model
def rmsnorm(x, g):
    return x / np.sqrt(np.mean(x * x) + EPS) * g


def fq(x, Q):
    """Fake quantization: symmetric per-vector absmax to [-Q, Q] (what QAT simulates)."""
    m = np.abs(x).max()
    if m == 0:
        return x
    return np.round(x * (Q / m)) * (m / Q)


class FloatModel:
    """Same network in float64. qat=True fake-quantizes activations like the integer path."""

    def __init__(self, fw, qat=False):
        self.fw, self.qat = fw, qat
        self.reset()

    def reset(self):
        self.pos = 0
        self.k = np.zeros((L, W, KVH, HD))
        self.v = np.zeros((L, W, KVH, HD))

    def q(self, x, Q):
        return fq(x, Q) if self.qat else x

    def step(self, tok):
        fw, qq = self.fw, self.q
        slot, nvalid = self.pos % W, min(self.pos + 1, W)
        x = fw["tok_emb"][tok].astype(np.float64)
        slots = [(self.pos - d) % W for d in range(nvalid)]
        for l in range(L):
            p = f"layers.{l}."
            n = qq(rmsnorm(x, fw[p + "attn_norm"]), QA)
            q, k, v = fw[p + "wq"] @ n, fw[p + "wk"] @ n, fw[p + "wv"] @ n
            for j in range(KVH):
                self.k[l, slot, j] = qq(k[j * HD:(j + 1) * HD], QKV)
                self.v[l, slot, j] = qq(v[j * HD:(j + 1) * HD], QKV)
            o = np.zeros(D)
            for h in range(H):
                j = h // (H // KVH)
                qh = qq(q[h * HD:(h + 1) * HD], QQ)
                s = np.array([self.k[l, t, j] @ qh / math.sqrt(HD) - ALIBI_SLOPES[h] * d
                              for d, t in enumerate(slots)])
                pr = np.exp(s - s.max())
                pr /= pr.sum()
                o[h * HD:(h + 1) * HD] = pr @ np.stack([self.v[l, t, j] for t in slots])
            x = x + fw[p + "wo"] @ qq(o, QA)
            n2 = qq(rmsnorm(x, fw[p + "ffn_norm"]), QA)
            rl = fw[p + "router"] @ n2
            e = int(np.argmax(rl))
            gate = 1.0 / np.exp(rl - rl[e]).sum()
            pe = p + f"experts.{e}."
            gg, uu = fw[pe + "w_gate"] @ n2, fw[pe + "w_up"] @ n2
            hh = qq(gg / (1 + np.exp(-gg)) * uu, QA)
            x = x + gate * (fw[pe + "w_down"] @ hh)
        self.pos += 1
        nf = qq(rmsnorm(x, fw["final_norm"]), QF)
        return fw["tok_emb"] @ nf
