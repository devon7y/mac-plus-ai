"""Export: float weights (PyTorch layout) -> packed integer model -> Pixel model file (B5).

Float weight layout (a dict of numpy arrays; nn.Linear weights are [out, in]):
  tok_emb                         [2048, 160]   tied input embedding / output head
  layers.{l}.attn_norm            [160]         RMSNorm gains
  layers.{l}.wq                   [160, 160]    4 query heads x 40 (head h = rows 40h..40h+39)
  layers.{l}.wk, layers.{l}.wv    [80, 160]     2 key/value heads x 40 (query head h uses kv head h//2)
  layers.{l}.wo                   [160, 160]
  layers.{l}.ffn_norm             [160]
  layers.{l}.router               [4, 160]
  layers.{l}.experts.{e}.w_gate   [512, 160]    SwiGLU: down(silu(gate x) * up x)
  layers.{l}.experts.{e}.w_up     [512, 160]
  layers.{l}.experts.{e}.w_down   [160, 512]
  final_norm                      [160]
"""
import math
import struct

import numpy as np

import kernels as K
import sf
from model import D, FF, H, HD, KVH, L, NE, QA, QF, QKV, QQ, V, W, alibi_table

MAGIC, VERSION, HDR = b"PIXL", 1, 64
N_LAYER_SF = 7 + 3 * NE


def ternarize(w):
    """BitNet b1.58 absmean: alpha = mean|w|, T = clip(round(w / alpha), -1, 1)."""
    a = float(np.abs(w).mean()) + 1e-12
    T = np.clip(np.round(w / a), -1, 1).astype(np.int8)
    return T, sf.from_float(a)


def gains(g):
    gs = sf.from_float(max(float(np.abs(g).max()), 1e-12) / 32767)
    gi = np.clip(np.round(g / sf.value(gs)), -32767, 32767).astype(np.int64)
    return gi, gs


def export(fw):
    """Float weights -> packed integer model (the dict IntModel consumes)."""
    E = np.asarray(fw["tok_emb"], dtype=np.float64)
    s = np.maximum(np.abs(E).max(axis=1), 1e-12) / QKV
    codes = np.clip(np.round(E / s[:, None]), -QKV, QKV).astype(np.int8)
    emb_e = int(math.ceil(math.log2(s.max() / 32767)))
    emb_m = np.clip(np.round(s / 2.0 ** emb_e), 1, 32767).astype(np.int64)
    pm = {"emb_codes": codes, "emb_m": emb_m, "emb_e": emb_e, "alibi": alibi_table(W), "layers": []}
    pm["g_final"], pm["gs_final"] = gains(np.asarray(fw["final_norm"], dtype=np.float64))
    for l in range(L):
        p = f"layers.{l}."
        lw = {}
        lw["g_attn"], lw["gs_attn"] = gains(fw[p + "attn_norm"])
        lw["g_ffn"], lw["gs_ffn"] = gains(fw[p + "ffn_norm"])
        tq, lw["a_q"] = ternarize(fw[p + "wq"])
        tk, lw["a_k"] = ternarize(fw[p + "wk"])
        tv, lw["a_v"] = ternarize(fw[p + "wv"])
        lw["wqkv"] = np.concatenate([tq, tk, tv])
        lw["wo"], lw["a_o"] = ternarize(fw[p + "wo"])
        R = np.asarray(fw[p + "router"], dtype=np.float64)
        lw["s_r"] = sf.from_float(max(float(np.abs(R).max()), 1e-12) / 127)
        lw["router"] = np.clip(np.round(R / sf.value(lw["s_r"])), -127, 127).astype(np.int8)
        lw["experts"] = []
        for e in range(NE):
            pe = p + f"experts.{e}."
            ex = {}
            ex["wg"], ex["a_g"] = ternarize(fw[pe + "w_gate"])
            ex["wu"], ex["a_u"] = ternarize(fw[pe + "w_up"])
            ex["wd"], ex["a_d"] = ternarize(fw[pe + "w_down"])
            lw["experts"].append(ex)
        pm["layers"].append(lw)
    return pm


def dequantize(pm):
    """Packed model -> float weights with exactly the packed values (for the float comparison)."""
    fw = {"tok_emb": pm["emb_codes"] * (pm["emb_m"] * 2.0 ** pm["emb_e"])[:, None],
          "final_norm": pm["g_final"] * sf.value(pm["gs_final"])}
    for l, lw in enumerate(pm["layers"]):
        p = f"layers.{l}."
        fw[p + "attn_norm"] = lw["g_attn"] * sf.value(lw["gs_attn"])
        fw[p + "ffn_norm"] = lw["g_ffn"] * sf.value(lw["gs_ffn"])
        T = lw["wqkv"].astype(np.float64)
        fw[p + "wq"] = T[:D] * sf.value(lw["a_q"])
        fw[p + "wk"] = T[D:D + KVH * HD] * sf.value(lw["a_k"])
        fw[p + "wv"] = T[D + KVH * HD:] * sf.value(lw["a_v"])
        fw[p + "wo"] = lw["wo"] * sf.value(lw["a_o"])
        fw[p + "router"] = lw["router"] * sf.value(lw["s_r"])
        for e, ex in enumerate(lw["experts"]):
            pe = p + f"experts.{e}."
            fw[pe + "w_gate"] = ex["wg"] * sf.value(ex["a_g"])
            fw[pe + "w_up"] = ex["wu"] * sf.value(ex["a_u"])
            fw[pe + "w_down"] = ex["wd"] * sf.value(ex["a_d"])
    return fw


def random_float_weights(seed=0, emb_std=0.35):
    """Random float weights with a lively signal: residual updates of order one per layer and
    logits spread over a few nats (a stand-in until trained weights exist)."""
    rng = np.random.default_rng(seed)
    nrm = rng.standard_normal
    fw = {"tok_emb": nrm((V, D)) * emb_std, "final_norm": 1.0 + 0.2 * nrm(D)}
    for l in range(L):
        p = f"layers.{l}."
        fw[p + "attn_norm"] = 1.0 + 0.2 * nrm(D)
        fw[p + "ffn_norm"] = 1.0 + 0.2 * nrm(D)
        fw[p + "wq"] = nrm((D, D)) * 2.0 / math.sqrt(D)
        fw[p + "wk"] = nrm((KVH * HD, D)) * 2.0 / math.sqrt(D)
        fw[p + "wv"] = nrm((KVH * HD, D)) / math.sqrt(D)
        fw[p + "wo"] = nrm((D, D)) / math.sqrt(D)
        fw[p + "router"] = nrm((NE, D)) / math.sqrt(D)
        for e in range(NE):
            pe = p + f"experts.{e}."
            fw[pe + "w_gate"] = nrm((FF, D)) / math.sqrt(D)
            fw[pe + "w_up"] = nrm((FF, D)) / math.sqrt(D)
            fw[pe + "w_down"] = nrm((D, FF)) / math.sqrt(FF)
    return fw


# ------------------------------------------------------------------ tokenizer
N_BASE = 99          # <pad> <user> <pixel> <end> + 95 printable ASCII characters


def tokenizer_merges(tok):
    """llm/train/tokenizer.py Tokenizer -> merges as (left id, right id), rank order. The Mac merges
    token ids, which equals the Python string algorithm when every token string is unique."""
    assert len(set(tok.tokens)) == len(tok.tokens) and None not in tok.tokens, "duplicate token strings"
    out = []
    for r, (a, b) in enumerate(tok.merges):
        assert tok.id_of[a + b] == N_BASE + r
        out.append((tok.id_of[a], tok.id_of[b]))
    return out


# ------------------------------------------------------------------ canonical Huffman
def huffman_lengths(freq):
    """Code lengths for symbols with freq > 0 (deterministic ties: frequency, then smallest symbol)."""
    import heapq
    syms = [s for s in range(len(freq)) if freq[s] > 0]
    if len(syms) == 1:
        return [1 if s == syms[0] else 0 for s in range(len(freq))]
    f = list(freq)
    while True:
        heap = [(f[s], s, [s]) for s in syms]
        heapq.heapify(heap)
        depth = [0] * len(freq)
        while len(heap) > 1:
            f1, k1, a = heapq.heappop(heap)
            f2, k2, b = heapq.heappop(heap)
            for s in a + b:
                depth[s] += 1
            heapq.heappush(heap, (f1 + f2, min(k1, k2), a + b))
        if max(depth) <= 15:
            return depth
        top = max(f)                     # too deep: flatten rare counts and retry
        f = [x + top // 4096 + 1 if x else 0 for x in f]


def canonical_codes(lengths):
    order = sorted((l, s) for s, l in enumerate(lengths) if l)
    codes, code, prev = {}, 0, order[0][0]
    for l, s in order:
        code <<= (l - prev)
        prev = l
        codes[s] = (code, l)
        code += 1
    return codes


def huff_encode(symbols, nsym):
    freq = np.bincount(np.asarray(symbols), minlength=nsym).tolist()
    lengths = huffman_lengths(freq)
    codes = canonical_codes(lengths)
    acc, nbits, out = 0, 0, bytearray()
    for s in symbols:
        c, l = codes[s]
        acc = (acc << l) | c
        nbits += l
        while nbits >= 8:
            nbits -= 8
            out.append((acc >> nbits) & 255)
        acc &= (1 << nbits) - 1
    if nbits:
        out.append((acc << (8 - nbits)) & 255)
    return lengths, bytes(out)


def huff_decode(lengths, data, n):
    """Table-driven canonical decoding (same codes as the bit-by-bit definition below)."""
    codes = canonical_codes(lengths)
    ml = max(lengths)
    T = np.zeros(1 << ml, dtype=np.int64)
    for s, (c, l) in codes.items():
        T[c << (ml - l):(c + 1) << (ml - l)] = s * 32 + l
    bits = np.unpackbits(np.frombuffer(data, dtype=np.uint8)).astype(np.int64)
    bits = np.concatenate([bits, np.zeros(ml, dtype=np.int64)])
    win = np.zeros(len(bits) - ml, dtype=np.int64)
    for k in range(ml):
        win = (win << 1) | bits[k:k + len(win)]
    win = T[win].tolist()
    out, p = [], 0
    for _ in range(n):
        e = win[p]
        out.append(e >> 5)
        p += e & 31
    return out


def huff_decode_bitwise(lengths, data, n):
    order = sorted((l, s) for s, l in enumerate(lengths) if l)
    first, count, index = {}, {}, {}
    code, prev, i = 0, order[0][0], 0
    for l, s in order:
        code <<= (l - prev)
        prev = l
        if l not in first:
            first[l], count[l], index[l] = code, 0, i
        count[l] += 1
        code += 1
        i += 1
    syms = [s for _, s in order]
    out, code, l, bit = [], 0, 0, 0
    while len(out) < n:
        b = (data[bit >> 3] >> (7 - (bit & 7))) & 1
        bit += 1
        code, l = (code << 1) | b, l + 1
        if l in first and code - first[l] < count[l]:
            out.append(syms[index[l] + code - first[l]])
            code, l = 0, 0
    return out


# ------------------------------------------------------------------ model file (v2, big-endian)
MAGIC, VERSION, HDR = b"PIXL", 3, 64
N_LAYER_SF = 7 + 3 * NE
FLAG_HUFF_EMB = 1


def _sf(a):
    return struct.pack(">hh", *a)


def _i16(a):
    return np.asarray(a, dtype=">i2").tobytes()


def pack_trits(T):
    """A whole ternary matrix as one trit stream (row-major), 5 trits per byte (no row padding)."""
    flat = np.asarray(T, dtype=np.int8).reshape(1, -1)
    assert flat.shape[1] % 5 == 0
    return K.pack_B5(flat).tobytes()


def unpack_trits(b, rows, n):
    return K.unpack_B5(np.frombuffer(b, dtype=np.uint8).reshape(1, -1), rows * n).reshape(rows, n)


FLAG_HUFF_TERN = 2


def layer_mats(lw):
    m = [pack_trits(lw["wqkv"]), pack_trits(lw["wo"])]
    for ex in lw["experts"]:
        m += [pack_trits(ex["wg"]), pack_trits(ex["wu"]), pack_trits(ex["wd"])]
    return m


def layer_head(lw):
    sfs = [lw["gs_attn"], lw["gs_ffn"], lw["a_q"], lw["a_k"], lw["a_v"], lw["a_o"], lw["s_r"]]
    for ex in lw["experts"]:
        sfs += [ex["a_g"], ex["a_u"], ex["a_d"]]
    b = b"".join(_sf(a) for a in sfs) + _i16(lw["g_attn"]) + _i16(lw["g_ffn"])
    return b + np.asarray(lw["router"], dtype=np.int8).tobytes()


def layer_bytes(lw):
    """Uncompressed layer record (also what the model checksum covers)."""
    return layer_head(lw) + b"".join(layer_mats(lw))


def huff_bits(symbols, codes):
    acc, nbits, out = 0, 0, bytearray()
    for s in symbols:
        c, l = codes[s]
        acc = (acc << l) | c
        nbits += l
        while nbits >= 8:
            nbits -= 8
            out.append((acc >> nbits) & 255)
        acc &= (1 << nbits) - 1
    if nbits:
        out.append((acc << (8 - nbits)) & 255)
    return bytes(out)


LAYER_BYTES = 4 * N_LAYER_SF + 2 * 2 * D + NE * D + (2 * KVH * HD + D) * D // 5 + D * D // 5 + \
    NE * (3 * FF * D // 5)


def merges_bytes(merges):
    b = bytearray()
    for a, c in merges:
        b += bytes([a >> 4, ((a & 15) << 4) | (c >> 8), c & 255])
    return bytes(b) + (b"\0" if len(b) % 2 else b"")


def to_bytes(pm, huff=True, budget=None):
    """Model file. huff: Huffman-code the embedding (over nibble-pair bytes) and the ternary
    matrices; with budget (bytes), only as many matrices as needed to fit (biggest savings first),
    because every coded matrix costs decoding time on the Mac. Raw matrices load much faster."""
    merges = pm.get("merges", [])
    c = (pm["emb_codes"].astype(np.int64) + 8).reshape(V, D)
    pairs = (c[:, 0::2] * 16 + c[:, 1::2]).astype(np.uint8)
    if huff:
        lengths, bits = huff_encode(pairs.reshape(-1).tolist(), 256)
        emb = bytes(lengths) + struct.pack(">I", len(bits)) + bits + (b"\0" if len(bits) % 2 else b"")
    else:
        emb = pairs.tobytes()
    pre = _i16(pm["alibi"]) + _sf(pm["gs_final"]) + _i16(pm["g_final"]) + _i16(pm["emb_m"])
    pre += merges_bytes(merges) + emb
    mats = [layer_mats(lw) for lw in pm["layers"]]
    flags = 0
    coded = set()
    if huff:
        flags |= FLAG_HUFF_EMB
        allb = np.frombuffer(b"".join(m for ms in mats for m in ms), dtype=np.uint8)
        tl = huffman_lengths(np.bincount(allb, minlength=243).tolist())
        tcodes = canonical_codes(tl)
        enc = {(l, i): huff_bits(list(m), tcodes) for l, ms in enumerate(mats) for i, m in enumerate(ms)}
        gain = {k: len(mats[k[0]][k[1]]) - (len(v) + (len(v) & 1)) for k, v in enc.items()}
        order = sorted((k for k in gain if gain[k] > 0), key=lambda k: -gain[k])
        size = len(pre) + 244 + sum(len(layer_head(lw)) + sum(4 + len(m) for m in ms)
                                    for lw, ms in zip(pm["layers"], mats))
        for k in order:
            if budget is not None and HDR + size <= budget:
                break
            coded.add(k)
            size -= gain[k]
        if coded:
            flags |= FLAG_HUFF_TERN
            pre += bytes(tl) + b"\0"
    layers = []
    for l, (lw, ms) in enumerate(zip(pm["layers"], mats)):
        b = layer_head(lw)
        for i, m in enumerate(ms):
            if not flags & FLAG_HUFF_TERN:
                b += m
            elif (l, i) in coded:
                bits = enc[(l, i)]
                b += struct.pack(">I", len(bits)) + bits + (b"\0" if len(bits) % 2 else b"")
            else:
                b += struct.pack(">I", 0) + m
        layers.append(b)
    assert flags & FLAG_HUFF_TERN or all(len(x) == LAYER_BYTES for x in layers)
    body = pre + b"".join(layers)
    hdr = MAGIC + struct.pack(">HH9hh4HIIIIHH", VERSION, HDR, D, L, H, KVH, HD, FF, NE, V, W,
                              pm["emb_e"], QA, QQ, QKV, QF, len(body), sum(body) & 0xFFFFFFFF,
                              0 if flags & FLAG_HUFF_TERN else LAYER_BYTES, HDR + len(pre), len(merges), flags)
    # integrity check over the decoded model: the body of the uncompressed file
    plain = body if not flags else to_bytes(pm, huff=False)[HDR:]
    hdr += struct.pack(">II", *fletcher(plain))
    return hdr.ljust(HDR, b"\0") + body


def fletcher(data):
    """Two 32-bit running sums (s1 += byte; s2 += s1), the model's integrity check."""
    a = np.frombuffer(data, dtype=np.uint8).astype(np.uint64)
    c1 = np.cumsum(a) & 0xFFFFFFFF
    return int(c1[-1]), int(np.sum(c1) & 0xFFFFFFFF)


def write_model(pm, path, huff=True, budget=None):
    data = to_bytes(pm, huff, budget)
    open(path, "wb").write(data)
    return len(data)


def from_bytes(data):
    assert data[:4] == MAGIC
    f = struct.unpack(">HH9hh4HIIIIHH", data[4:56])
    ver, hdr, dims, emb_e, qs = f[0], f[1], f[2:11], f[11], f[12:16]
    body_len, csum, lsize, l0, n_merges, flags = f[16:22]
    assert ver == VERSION and dims == (D, L, H, KVH, HD, FF, NE, V, W) and qs == (QA, QQ, QKV, QF)
    body = data[hdr:]
    assert len(body) == body_len and sum(body) & 0xFFFFFFFF == csum
    ck = struct.unpack(">II", data[56:64])
    o = hdr

    def take(n):
        nonlocal o
        b = data[o:o + n]
        o += n
        return b

    def i16(n):
        return np.frombuffer(take(2 * n), dtype=">i2").astype(np.int64)

    def sfr():
        return tuple(int(v) for v in struct.unpack(">hh", take(4)))

    pm = {"emb_e": emb_e, "alibi": i16(H * W).reshape(H, W)}
    pm["gs_final"] = sfr()
    pm["g_final"] = i16(D)
    pm["emb_m"] = i16(V)
    mb = take(3 * n_merges + (n_merges & 1))
    pm["merges"] = [((mb[3 * i] << 4) | (mb[3 * i + 1] >> 4), ((mb[3 * i + 1] & 15) << 8) | mb[3 * i + 2])
                    for i in range(n_merges)]
    if flags & FLAG_HUFF_EMB:
        lengths = list(take(256))
        nb = struct.unpack(">I", take(4))[0]
        nib = np.array(huff_decode(lengths, take(nb + (nb & 1)), V * D // 2), dtype=np.int64).reshape(V, D // 2)
    else:
        nib = np.frombuffer(take(V * D // 2), dtype=np.uint8).reshape(V, D // 2).astype(np.int64)
    codes = np.empty((V, D), dtype=np.int64)
    codes[:, 0::2], codes[:, 1::2] = nib // 16 - 8, nib % 16 - 8
    pm["emb_codes"] = codes.astype(np.int8)
    tl = list(take(244))[:243] if flags & FLAG_HUFF_TERN else None
    assert o == l0

    def tern(rows, n):
        if tl is None:
            return unpack_trits(take(rows * n // 5), rows, n)
        nb = struct.unpack(">I", take(4))[0]
        if nb == 0:                                     # this matrix is stored raw
            return unpack_trits(take(rows * n // 5), rows, n)
        syms = huff_decode(tl, take(nb + (nb & 1)), rows * n // 5)
        return unpack_trits(bytes(syms), rows, n)
    pm["layers"] = []
    for _ in range(L):
        s = [sfr() for _ in range(N_LAYER_SF)]
        lw = dict(zip(["gs_attn", "gs_ffn", "a_q", "a_k", "a_v", "a_o", "s_r"], s[:7]))
        lw["g_attn"], lw["g_ffn"] = i16(D), i16(D)
        lw["router"] = np.frombuffer(take(NE * D), dtype=np.int8).reshape(NE, D).copy()
        lw["wqkv"] = tern(D + 2 * KVH * HD, D)
        lw["wo"] = tern(D, D)
        lw["experts"] = []
        for e in range(NE):
            ex = {"a_g": s[7 + 3 * e], "a_u": s[8 + 3 * e], "a_d": s[9 + 3 * e]}
            ex["wg"] = tern(FF, D)
            ex["wu"] = tern(FF, D)
            ex["wd"] = tern(D, FF)
            lw["experts"].append(ex)
        pm["layers"].append(lw)
    assert o == len(data)
    if ck != (0, 0):
        assert fletcher(to_bytes(pm, huff=False)[HDR:]) == ck, "model checksum mismatch"
    return pm


def read_model(path):
    return from_bytes(open(path, "rb").read())


def load_tokenizer(path):
    """The training agent's tokenizer (llm/train/tokenizer.py), imported read-only."""
    import os
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "train"))
    import tokenizer as T
    return T.Tokenizer.load(path)
