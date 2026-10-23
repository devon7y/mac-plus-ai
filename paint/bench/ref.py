#!/usr/bin/env python3
"""Bit-exact Python reference of Doodle's 68000 decoder step and sampler (integer arithmetic
only). The 68000 code in gen_bench.py (and later the app) must produce identical numbers.
See FORMAT.md for the meaning of every quantity. All ">>" are arithmetic shifts (floor).

Weight matrices are passed already DECODED to small integers (ternary -1/0/+1 or 4-bit
-8..7); decode_* turn the Mac's stored codes into such matrices.
"""
import numpy as np

from tables import COS, EXPP, EXPS, RTAB, SECH, SIG, TANH

SIG_, TANH_, SECH_ = (np.array(t, np.int64) for t in (SIG, TANH, SECH))
NOUT, NMIX = 123, 20


def wrap16(x):
    return ((np.asarray(x, np.int64) + 32768) & 0xFFFF) - 32768


def clip(x, lo, hi):
    return np.clip(np.asarray(x, np.int64), lo, hi)


# ------------------------------------------------------------------ weight code decoding
def ternary_digits(idx):
    """idx (0..80) -> 4 weights (d_j - 1), j = 0..3, idx = sum d_j 3^j."""
    idx = np.asarray(idx, np.int64)
    return np.stack([(idx // 3 ** j) % 3 - 1 for j in range(4)], axis=-1)


def decode_w4(codes, rows, G):
    """16-bit codes: (g*81 + idx)*2, relative to the start of the lookup table."""
    c = (np.asarray(codes, np.int64) & 0xFFFF).reshape(rows, G)
    g = np.arange(G)
    idx = c // 2 - g * 81
    assert (c % 2 == 0).all() and (idx >= 0).all() and (idx < 81).all(), "bad W4 code"
    return ternary_digits(idx).reshape(rows, G * 4)


def decode_b4(codes, rows, G):
    """byte codes: idx*2."""
    c = np.asarray(codes, np.int64).reshape(rows, G)
    assert (c % 2 == 0).all() and (c < 162).all(), "bad B4 code"
    return ternary_digits(c // 2).reshape(rows, G * 4)


def p4_segments(H):
    """Order of pair codes in a P4 row: [(first pair, count), ...]. Pairs >= 128 use a second
    64 KB table window and come FIRST in each row."""
    P = H // 2
    return [(128, P - 128), (0, 128)] if P > 128 else [(0, P)]


def decode_p4(codes, rows, H):
    """16-bit codes: (p'*256 + c)*2 - 32768 where p' is the pair index within its window and
    c = (w1+8)*16 + (w2+8) holds the two 4-bit weights of pair p."""
    c = np.asarray(codes, np.int64).reshape(rows, H // 2)
    W = np.zeros((rows, H), np.int64)
    col = 0
    for first, n in p4_segments(H):
        v = (c[:, col:col + n] + 32768) & 0xFFFF
        assert (v % 2 == 0).all()
        v //= 2
        pp = v // 256
        assert (pp == np.arange(n)).all(), "bad P4 window index"
        cc = v % 256
        for q in range(n):
            p = first + q
            W[:, 2 * p] = cc[:, q] // 16 - 8
            W[:, 2 * p + 1] = cc[:, q] % 16 - 8
        col += n
    return W


# ------------------------------------------------------------------ RNG
def xorshift(s):
    s ^= (s << 13) & 0xFFFFFFFF
    s ^= s >> 17
    s ^= (s << 5) & 0xFFFFFFFF
    return s


# ------------------------------------------------------------------ network pieces
def matvec(W, hq):
    """16-bit wraparound accumulation, exactly what the lookup-table kernels compute."""
    return wrap16(np.asarray(W, np.int64) @ np.asarray(hq, np.int64))


def p4_split(H):
    """Columns summed in the P4 kernel's second accumulator: the last 64 pairs in kernel order
    (pairs >= 128 first, then 0..127)."""
    P = H // 2
    order = list(range(128, P)) + list(range(min(128, P)))
    b = np.zeros(H, bool)
    for p in order[-64:]:
        b[2 * p] = b[2 * p + 1] = True
    return b


def matvec_p4(W, hq):
    """P4 kernel: two 16-bit wraparound partial sums, added in 32 bits and clamped to int16."""
    W, hq = np.asarray(W, np.int64), np.asarray(hq, np.int64)
    b = p4_split(W.shape[1])
    return clip(wrap16(W[:, ~b] @ hq[~b]) + wrap16(W[:, b] @ hq[b]), -32768, 32767)


def mat(p, name, hq):
    """The matrix-vector product as the Mac's kernel for that matrix's storage computes it."""
    kind = p.get("kind_wh" if name == "U" else "kind_wo", 0)
    return matvec_p4(p[name], hq) if kind == 2 else matvec(p[name], hq)


def fold(WE, B0, emb):
    """Category embedding folded into a per-drawing bias: E = clip16(B0 + ((WE @ emb) >> 7))."""
    return clip(np.asarray(B0, np.int64) + ((np.asarray(WE, np.int64) @ np.asarray(emb, np.int64)) >> 7),
                -32768, 32767)


def inp(p, pen, dx, dy):
    return ((p["WDX"] * dx + p["WDY"] * dy) >> p.get("SHX", 6)) + p["E"] + p["P"][pen]


def sig_rows(S, M, cx, sh=12):
    pre = clip(cx + ((S * M) >> sh), -8192, 8191)
    return SIG_[(pre >> 4) + 512]


def tanh_rows(S, M, cx, sh=12):
    pre = clip(cx + ((S * M) >> sh), -4096, 4095)
    return TANH_[(pre >> 3) + 512]


def gru_step(p, st):
    """One GRU recurrence (PyTorch form: n = tanh(Wx + b_in + r*(U_n h + b_hn))).
    Rows of U: r [0,H), z [H,2H), n [2H,3H). p['M']: 2H multipliers (r, z rows);
    p['MN']: H x (m, bhn) for the n rows. Returns intermediates and updates st in place."""
    H = p["H"]
    S = mat(p, "U", st["hq"])
    cx = inp(p, st["pen"], st["dx"], st["dy"])
    sh = p.get("SHR", 12)
    g_rz = sig_rows(S[:2 * H], p["M"], cx[:2 * H], sh)
    r, z = g_rz[:H], g_rz[H:]
    mn = p["MN"]
    u = clip(((S[2 * H:] * mn[:, 0]) >> sh) + mn[:, 1], -32768, 32767)
    t = (r * u) >> 14
    pre = clip(cx[2 * H:] + t, -4096, 4095)
    n = TANH_[(pre >> 3) + 512]
    h = st["h"]
    hn = n + ((z * (h - n)) >> 14)
    st["h"] = hn
    st["hq"] = (hn * p["A"]) >> 14
    return {"S": S, "CX": cx, "GV": np.concatenate([r, z, n])}


def lstm_step(p, st):
    """LSTM: rows i [0,H), f [H,2H), o [2H,3H) sigmoid; g [3H,4H) tanh. c in Q12."""
    H = p["H"]
    S = mat(p, "U", st["hq"])
    cx = inp(p, st["pen"], st["dx"], st["dy"])
    sh = p.get("SHR", 12)
    sg = sig_rows(S[:3 * H], p["M"][:3 * H], cx[:3 * H], sh)
    g = tanh_rows(S[3 * H:], p["M"][3 * H:], cx[3 * H:], sh)
    i, f, o = sg[:H], sg[H:2 * H], sg[2 * H:]
    c = clip(((f * st["c"]) >> 14) + ((i * g) >> 16), -32768, 32767)
    th = TANH_[(clip(c, -16384, 16383) >> 5) + 512]
    hn = (o * th) >> 14
    st["c"] = c
    st["h"] = hn
    st["hq"] = (hn * p["A"]) >> 14
    return {"S": S, "CX": cx, "GV": np.concatenate([sg, g])}


def out_layer(p, hq):
    """O = clip16(BY + ((SY*MY) >> SHO) [+ ((SY2*MY2) >> SHO2) for a two-term output layer]),
    both products 32-bit, arithmetic shifts, summed in 32 bits, clipped once."""
    SY = mat(p, "WY", hq)
    acc = p["BY"] + ((SY * p["MY"]) >> p.get("SHO", 12))
    if p.get("WY2") is not None:
        acc = acc + ((mat(p, "WY2", hq) * p["MY2"]) >> p["SHO2"])
    return SY, clip(acc, -32768, 32767)


# ------------------------------------------------------------------ sampling
def softmax_pick(logits, invt, seed):
    """Temperature softmax via the EXPP table, then pick from 16-bit cumulative sums."""
    l = [(int(q) * invt) >> 8 for q in logits]
    mx = max(l)
    pr = [EXPP[min((mx - v) >> 5, 511)] for v in l]
    total = sum(pr)
    seed = xorshift(seed)
    t = ((seed & 0xFFFF) * total) >> 16
    cum = 0
    for j, v in enumerate(pr):
        cum += v
        if t < cum:
            return j, seed
    return len(pr) - 1, seed


def exps(raw, hlt):
    v = min(max(int(raw) + hlt, -8192), 4095)
    return EXPS[(v >> 4) + 512]


def to_px(x, fq):
    x = min(max(x, -32768), 32767)
    d = (x * fq + 32768) >> 16
    return min(max(d, -255), 255)


def sample(O, sc):
    """sc: dict with seed, invt (Q8 1/temperature), hlt (Q10 0.5*ln temperature), fq
    (pixels per Q10 unit * 64). Output layout of O (as paint/train/sketch_model.py): pi logits
    [0,20), mu_x [20,40), mu_y [40,60), log sigma_x [60,80), log sigma_y [80,100),
    rho (pre-tanh) [100,120), pen logits [120,123).
    Random numbers drawn in this order: component, Box-Muller, pen. Returns the SAMP record."""
    O = [int(v) for v in O]
    k, sc["seed"] = softmax_pick(O[0:20], sc["invt"], sc["seed"])
    s1 = exps(O[60 + k], sc["hlt"])
    s2 = exps(O[80 + k], sc["hlt"])
    rv = min(max(O[100 + k], -4096), 4095)
    ri = (rv >> 3) + 512
    rho, cc = TANH[ri], SECH[ri]
    sc["seed"] = r = xorshift(sc["seed"])
    R = RTAB[r >> 22]
    i2 = (r >> 12) & 1023
    n1 = (R * COS[i2]) >> 14
    n2 = (R * COS[(i2 + 768) & 1023]) >> 14
    x = O[20 + k] + ((s1 * n1) >> 12)
    w = ((rho * n1) >> 14) + ((cc * n2) >> 14)
    y = O[40 + k] + ((s2 * w) >> 12)
    pen, sc["seed"] = softmax_pick(O[120:123], sc.get("invtp", sc["invt"]), sc["seed"])
    dx, dy = to_px(x, sc["fq"]), to_px(y, sc["fq"])
    return {"k": k, "pen": pen, "dx": dx, "dy": dy, "s1": s1, "s2": s2, "rho": rho, "c": cc,
            "n1": n1, "n2": n2, "x": x, "y": y}


def full_step(p, st, sc):
    """Recurrence + output layer + sampling; the sampled point becomes the next input.
    The output layer reads its own quantization of h, hq_o = (h*AO) >> 14 (= hq when AO = A)."""
    inter = (gru_step if p["cell"] == "gru" else lstm_step)(p, st)
    SY, O = out_layer(p, (st["h"] * p.get("AO", p["A"])) >> 14)
    s = sample(O, sc)
    st["pen"], st["dx"], st["dy"] = s["pen"], s["dx"], s["dy"]
    inter.update({"SY": SY, "O": O, "samp": s})
    return inter


# ================================================================== M4: the app's pipeline
# (model file -> per-drawing fold -> prompt -> priming -> sampling), bit-exact with the Mac.
def temp_consts(t_mix, t_pen):
    """(INVT, HLT, INVTP): mixture temperature (component pick, sigma * sqrt(t)) and pen temperature."""
    import math
    return (int(round(256 / t_mix)), int(round(512 * math.log(t_mix))), int(round(256 / t_pen)))


# The app's Finish menu settings (paint/app/model.c must hold the same numbers):
# Pen temperature 1.0: sharpening the pen softmax made strokes ~1.4x too long and 7% of drawings
# loop to the 200-move cap (h384_tern_r2o, 96 drawings); at pen 1.0: 0% and 6.5 moves per stroke.
# P4 H=256 model (h256_int4_ft, 96 drawings from scratch + 100 real partial drawings each):
# pen 0.75 gives no loops, the fewest early stops (20%) and the longest completions.
TEMP_SETTINGS = {0: (0.3, 0.75), 1: (0.5, 0.75), 2: (0.8, 0.75)}    # (mixture, pen)
TEMPS = {k: temp_consts(*v) for k, v in TEMP_SETTINGS.items()}
MAX_ROWS, MAX_GEN = 250, 200


def model_params(m):
    """Model-file dict (model_file.read) -> parameters for gru_step / out_layer."""
    return {"cell": "gru", "H": m["H"], "A": m["A"], "AO": m["AO"], "SHR": m["SHR"], "SHO": m["SHO"],
            "SHX": m.get("SHX", 6), "WY2": m.get("WY2"), "MY2": m.get("MY2"), "SHO2": m.get("SHO2", 0),
            "kind_wh": m["kind_wh"], "kind_wo": m["kind_wo"],
            "U": m["U"], "WY": m["WY"], "M": m["M"], "MN": m["MN"], "MY": m["MY"], "BY": m["BY"],
            "P": m["P"], "WDX": m["WDX"], "WDY": m["WDY"]}


def fold2(m, cat):
    """Per-drawing constants: E (3H, Q10) and the initial state h0 (H, Q14)."""
    acc = np.asarray(m["WC"], np.int64) @ np.asarray(m["EMB"][cat], np.int64)
    v = clip(acc >> m["SHA"], -32768, 32767)
    e = clip(np.asarray(m["B0"], np.int64) + ((v * np.asarray(m["MC"], np.int64)) >> m["SHB"]), -32768, 32767)
    H = m["H"]
    return e[:3 * H], TANH_[(clip(e[3 * H:], -4096, 4095) >> 3) + 512]


def rdp_keep(pts):
    """Integer Ramer-Douglas-Peucker, epsilon 2 (paint/train/prep_data.py's rdp, exactly):
    keep the farthest point from the chord (first one on ties) if its distance > 2."""
    n = len(pts)
    keep = [False] * n
    if n < 3:
        return [True] * n
    keep[0] = keep[-1] = True
    stack = [(0, n - 1)]
    while stack:
        s, e = stack.pop()
        if e <= s + 1:
            continue
        ax, ay = pts[s]
        bx, by = pts[e]
        dx, dy = bx - ax, by - ay
        L2 = dx * dx + dy * dy
        best, bi = -1, -1
        for i in range(s + 1, e):
            px, py = pts[i]
            v = (px - ax) ** 2 + (py - ay) ** 2 if L2 == 0 else (dx * (py - ay) - dy * (px - ax)) ** 2
            if v > best:
                best, bi = v, i
        if (best > 4) if L2 == 0 else (best > 4 * L2):
            keep[bi] = True
            stack.append((s, bi))
            stack.append((bi, e))
    return keep


def prompt_rows(points):
    """points: [(x, y, lift)] in frame coordinates as the Mac records them (lift = 1 on a
    stroke's last point). Per stroke: drop consecutive duplicates, drop strokes with fewer
    than 2 points, RDP. Returns (start point, rows [(dx, dy, pen)]) with pen 0 = pen stays
    down, 1 = lifted after this point (stroke-5 p1 / p2); None if nothing is left."""
    strokes, cur = [], []
    for x, y, lift in points:
        if not cur or (x, y) != cur[-1]:
            cur.append((x, y))
        if lift:
            strokes.append(cur)
            cur = []
    if cur:
        strokes.append(cur)
    pts, lifts = [], []
    for st in strokes:
        if len(st) < 2:
            continue
        k = rdp_keep(st)
        kept = [p for p, f in zip(st, k) if f]
        pts += kept
        lifts += [0] * (len(kept) - 1) + [1]
    if len(pts) < 2:
        return None
    rows = [(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1], lifts[i]) for i in range(1, len(pts))]
    return pts[0], rows[:MAX_ROWS - 1]


def finish_it(m, rows, cat, seed, temp=1, max_rows=MAX_ROWS, max_gen=MAX_GEN):
    # temp: a Finish menu setting (0, 1, 2) or a (mixture, pen) temperature pair
    """The Mac's Finish it: fold, feed the start token and the prompt rows, then sample until
    the pen state says 'end' (2) or a length limit. Returns the generated rows [(dx, dy, pen)]."""
    p = model_params(m)
    p["E"], h0 = fold2(m, cat)
    st = {"h": h0, "hq": (h0 * m["A"]) >> 14, "pen": 0, "dx": 0, "dy": 0}
    invt, hlt, invtp = TEMPS[temp] if isinstance(temp, int) else temp_consts(*temp)
    sc = {"seed": seed if seed else 0x2545F491, "invt": invt, "hlt": hlt, "invtp": invtp, "fq": m["FQ"]}
    inputs = [(0, 0, 0)] + list(rows)
    for dx, dy, pen in inputs[:-1]:
        st["pen"], st["dx"], st["dy"] = pen, dx, dy
        gru_step(p, st)
    st["pen"], st["dx"], st["dy"] = inputs[-1][2], inputs[-1][0], inputs[-1][1]
    gen = []
    while True:
        s = full_step(p, st, sc)["samp"]
        gen.append((s["dx"], s["dy"], s["pen"]))
        if s["pen"] == 2 or len(gen) >= max_gen or len(rows) + len(gen) >= max_rows:
            return gen
