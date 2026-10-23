"""Bit-exact emulation of the 68000 kernels in llm/bench/gen_bench.py, plus the storage formats.

Every routine here follows the assembly step by step with 16-bit wraparound, so it reproduces the
benchmark's dumped buffers exactly (verify_dumps.py). The model (model.py) computes the same numbers
as exact integer dot products; test_ref.py checks both paths agree on real activations, which holds
because each kernel's true result fits in 16 bits (SPEC.md section 4).
"""
import numpy as np


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def groups(n, k):
    return -(-n // k)


# ------------------------------------------------------------------ ternary group tables
def build_group_table(x, k):
    """bld_k: 3**k entries; entry idx = sum_j (d_j - 1) * x_j with idx = sum_j d_j 3**j.
    Literal replica of the incremental 68000 algorithm (16-bit registers)."""
    x = [s16(int(v)) for v in x]
    c = [x[0]]                         # c_0 = x0 (register d1 is never modified)
    S = x[0]
    for j in range(1, k):
        c.append(s16(x[j] - 2 * S))    # c_j = x_j - 2*(x_0 + ... + x_{j-1})
        S = s16(S + x[j])
    val = s16(-S)
    out = []
    n = 3 ** k
    for idx in range(n):
        out.append(val)
        if idx < n - 1:
            j, t = 0, idx
            while t % 3 == 2:          # lowest base-3 digit below 2
                t //= 3
                j += 1
            val = s16(val + c[j])
    return out


def build_tables(x, k, n_groups):
    """Tables for all groups of the input vector x (padded with zeros to n_groups*k).
    Returns a list of per-group tables (lists of 3**k signed words)."""
    xx = list(x) + [0] * (n_groups * k - len(x))
    return [build_group_table(xx[g * k:(g + 1) * k], k) for g in range(n_groups)]


def lut_interleaved(tables):
    """W layout: word (idx*G + g) holds group g's entry idx (stride G*2 bytes)."""
    G, n = len(tables), len(tables[0])
    lut = [0] * (G * n)
    for g, t in enumerate(tables):
        for idx, v in enumerate(t):
            lut[idx * G + g] = v
    return lut


def lut_contiguous(tables):
    """B layout: group g's 3**k entries are consecutive."""
    return [v for t in tables for v in t]


def mv_W(codes, lut, G):
    """mv_W: per row, d1 += word at (lut + 32768 + code). codes: [rows][G] int16."""
    out = []
    for row in codes:
        d1 = 0
        for c in row:
            d1 = s16(d1 + lut[(int(c) + 32768) // 2])
        out.append(d1)
    return out


def mv_B4(codes, lut):
    """mv_B4: byte code = idx*2; table pointer advances 162 bytes (81 words) per group."""
    out = []
    for row in codes:
        d1 = 0
        for g, c in enumerate(row):
            d1 = s16(d1 + lut[g * 81 + int(c) // 2])
        out.append(d1)
    return out


def mv_B5(codes, lut):
    """mv_B5: byte code = idx; table pointer advances 486 bytes (243 words) per group."""
    out = []
    for row in codes:
        d1 = 0
        for g, c in enumerate(row):
            d1 = s16(d1 + lut[g * 243 + int(c)])
        out.append(d1)
    return out


# ------------------------------------------------------------------ ternary storage formats
def trit_index(w):
    """Group of ternary weights (-1/0/+1) -> index sum (w_j + 1) * 3**j."""
    return sum((int(v) + 1) * 3 ** j for j, v in enumerate(w))


def trits_of(idx, k):
    return [(idx // 3 ** j) % 3 - 1 for j in range(k)]


def pad_rows(T, k):
    T = np.asarray(T, dtype=np.int8)
    rows, n = T.shape
    G = groups(n, k)
    P = np.zeros((rows, G * k), dtype=np.int8)
    P[:, :n] = T
    return P, G


def _indices(T, k):
    P, G = pad_rows(T, k)
    pw = (3 ** np.arange(k)).astype(np.int64)
    return ((P.reshape(P.shape[0], G, k).astype(np.int64) + 1) * pw).sum(axis=2), G


def pack_W(T, k):
    """W4/W5 runtime codes: int16 (idx*G + g)*2 - 32768, shape [rows][G]."""
    idx, G = _indices(T, k)
    g = np.arange(G, dtype=np.int64)
    return ((idx * G + g) * 2 - 32768).astype(np.int16)


def pack_B4(T):
    """B4 runtime codes: byte idx*2 (0..160)."""
    idx, _ = _indices(T, 4)
    return (idx * 2).astype(np.uint8)


def pack_B5(T):
    """B5 codes (runtime and file format): byte idx (0..242), padding weights are 0."""
    idx, _ = _indices(T, 5)
    return idx.astype(np.uint8)


def unpack_B5(codes, n):
    """B5 bytes [rows][G] -> ternary matrix [rows][n]."""
    codes = np.asarray(codes, dtype=np.int64)
    k = 5
    digits = np.stack([(codes // 3 ** j) % 3 - 1 for j in range(k)], axis=2)
    return digits.reshape(codes.shape[0], -1)[:, :n].astype(np.int8)


def w4_from_B5(codes5, n):
    """Load-time expansion the Mac performs: B5 file bytes -> W4 runtime codes."""
    return pack_W(unpack_B5(codes5, n), 4)


# ------------------------------------------------------------------ 4-bit pair tables
def build_pair_table(h1, h2):
    """bld_pair: entry c = (w1+8)*16 + (w2+8) holds w1*h1 + w2*h2 (incremental, 16-bit)."""
    h1, h2 = s16(int(h1)), s16(int(h2))
    d0 = s16(-s16(s16(h1 + h2) << 3))
    d4 = s16(h1 - s16(s16(h2 << 4) - h2))
    out = []
    for row in range(16):
        for col in range(16):
            out.append(d0)
            if col < 15:
                d0 = s16(d0 + h2)
            elif row < 15:
                d0 = s16(d0 + d4)
    return out


def build_pair_tables(h):
    """bpairs: one 256-entry table per pair (h[2p], h[2p+1]), consecutive."""
    lut = []
    for p in range(len(h) // 2):
        lut += build_pair_table(h[2 * p], h[2 * p + 1])
    return lut


def pair_codes(codes4):
    """4-bit codes [rows][2P] (-8..7) -> 16-bit pair codes p*512 + c*2 - 32768."""
    c = np.asarray(codes4, dtype=np.int64)
    rows, n = c.shape
    P = n // 2
    cc = (c[:, 0::2] + 8) * 16 + (c[:, 1::2] + 8)
    return (np.arange(P) * 512 + cc * 2 - 32768).astype(np.int16)


def nibble_codes(pc):
    """Inverse of pair_codes (verify.py's decoding)."""
    pc = np.asarray(pc, dtype=np.int64)
    P = pc.shape[1]
    c = (pc + 32768) // 2 - np.arange(P) * 256
    out = np.empty((pc.shape[0], 2 * P), dtype=np.int64)
    out[:, 0::2] = c // 16 - 8
    out[:, 1::2] = c % 16 - 8
    return out


# ------------------------------------------------------------------ softmax and weighted sum
def exp_table():
    """init_expt: EXPT[i] ~ 255*exp(-i/16) by the 8.8 fixed-point iteration (256 words)."""
    e, out = 65280, []
    for _ in range(256):
        out.append(e >> 8)
        e = (e * 61566) >> 16
    return out


EXPT = exp_table()


def softmax_index(zmax, z):
    """Index into EXPT: (zmax - z) as an unsigned word, >> 2 (1/16 nat steps), clamped to 255."""
    return min(((zmax - z) & 0xFFFF) >> 2, 255)


def softmax_bench(scores):
    """The benchmark's softmax: p = EXPT[index], ptab entries (i-8)*p for i = 0..15 (words)."""
    mx = -32768
    for s in scores:
        if s > mx:
            mx = s
    ptab = []
    for s in scores:
        p = EXPT[softmax_index(mx, s)]
        ptab += ptab_entries(p)
    return ptab


def ptab_entries(u):
    """16 words (i-8)*u built by repeated addition (entry 0, code -8, is never used by the model)."""
    out, d3 = [], s16(-(s16(u << 3)))
    for i in range(16):
        out.append(d3)
        d3 = s16(d3 + u)
    return out


def wsum(vbytes, ptab, hd=40):
    """wsum: acc[d] = sum_t ptab[t][vbyte[t][d] / 2] (16-bit). vbytes: ctx*hd pre-doubled codes."""
    ctx = len(vbytes) // hd
    acc = [0] * hd
    for t in range(ctx):
        for d in range(hd):
            acc[d] = s16(acc[d] + ptab[t * 16 + vbytes[t * hd + d] // 2])
    return acc


def vbytes_of(codes):
    """V cache bytes: pre-doubled 4-bit codes 2*(code+8)."""
    return [2 * (int(c) + 8) for c in codes]
