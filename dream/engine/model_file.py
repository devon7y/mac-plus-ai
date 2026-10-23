#!/usr/bin/env python3
"""Read and write Dream model files (FORMAT.md), and make random test models.

    model_file.py random <cfg> <out.drm> [--subjects N] [--steps N] [--seed S]
    model_file.py info <file.drm>
"""
import math
import struct
import sys

import numpy as np

import arch

KIND = {"pw": 0, "dw": 1}
MODE = {"relu": 0, "lin": 1, "seth": 2, "addh": 3, "raw": 4}


def table_names(L):
    names = []
    for l in L:
        if l["tab"] != "raw" and l["tab"] not in names:
            names.append(l["tab"])
    return names


def table_range(n):
    if n.startswith("relu"):
        return 0, int(n[4:])
    if n == "lin":
        return -arch.LIN_MAX, arch.LIN_MAX
    if n.startswith("lin"):
        return -int(n[3:]), int(n[3:])
    if n == "res":
        return -arch.RES_MAX, arch.RES_MAX
    raise ValueError(n)


def cosine_grid(n):
    """t_0 = 1 > ... > t_n = 0, uniform in the cosine angle."""
    return [1.0 - i / n for i in range(n + 1)]


def sampler_consts(ts, c=1.0):
    """C1, C2 (Q14) and t (Q15) per step from the time grid (FORMAT.md); with c < 1 also C3 (Q14),
    the fresh-noise coefficient: x' = alpha' x0 + sigma' (c eps_hat + sqrt(1 - c^2) z).
    c = 1 is deterministic DDIM (no C3 in the file)."""
    out = []
    for i in range(len(ts) - 1):
        a0, s0 = math.cos(math.pi * ts[i] / 2), math.sin(math.pi * ts[i] / 2)
        a1, s1 = math.cos(math.pi * ts[i + 1] / 2), math.sin(math.pi * ts[i + 1] / 2)
        if s1 < 1e-9:
            c1, c2, c3 = 0.0, 1.0, 0.0
        else:
            c1 = c * s1 / s0
            c2 = a1 - c * a0 * s1 / s0
            c3 = math.sqrt(max(0.0, 1 - c * c)) * s1
        e = (int(round(16384 * c1)), int(round(16384 * c2)), int(round(32767 * ts[i])))
        out.append(e if c >= 1.0 else e + (int(round(16384 * c3)),))
    return out


def pack_ternary(w):
    flat = (np.asarray(w, dtype=np.int64).reshape(-1) + 1)
    assert flat.min() >= 0 and flat.max() <= 2, "weights must be ternary"
    pad = (-len(flat)) % 5
    flat = np.concatenate([flat, np.ones(pad, dtype=np.int64)])   # pad with w = 0
    g = flat.reshape(-1, 5)
    return (g * np.array([1, 3, 9, 27, 81])).sum(1).astype(np.uint8).tobytes()


CHUNK = 7                 # the JIT's output chunk (accumulators d1..d7)
ZERO7 = sum(3 ** j for j in range(CHUNK))   # the code of 7 zero weights (1093)


def pack_chunks(w):
    """1x1-conv weights (cout, cin) -> chunk-major 12-bit codes: for each chunk of 7 output rows, for
    each input i, code = sum_j (w[o0+j][i] + 1) 3^j (rows past cout count as 0); two codes per 3
    bytes (a >> 4, (a & 15) << 4 | b >> 8, b & 255), padded with ZERO7."""
    w = np.asarray(w, dtype=np.int64)
    cout, cin = w.shape
    codes = []
    for o0 in range(0, cout, CHUNK):
        blk = np.zeros((CHUNK, cin), dtype=np.int64)
        m = min(CHUNK, cout - o0)
        blk[:m] = w[o0:o0 + m]
        codes.extend(((blk + 1) * (3 ** np.arange(CHUNK))[:, None]).sum(0).tolist())
    if len(codes) % 2:
        codes.append(ZERO7)
    c = np.array(codes, dtype=np.int64).reshape(-1, 2)
    out = np.stack([c[:, 0] >> 4, ((c[:, 0] & 15) << 4) | (c[:, 1] >> 8), c[:, 1] & 255], 1)
    return out.astype(np.uint8).tobytes()


def chunk_bytes(cout, cin):
    n = -(-cout // CHUNK) * cin
    return (n + 1) // 2 * 3


def unpack_chunks(b, cout, cin):
    v = np.frombuffer(b, dtype=np.uint8).astype(np.int64).reshape(-1, 3)
    a = (v[:, 0] << 4) | (v[:, 1] >> 4)
    bb = ((v[:, 1] & 15) << 8) | v[:, 2]
    codes = np.stack([a, bb], 1).reshape(-1)
    nch = -(-cout // CHUNK)
    codes = codes[:nch * cin].reshape(nch, cin)
    d = np.stack([(codes // 3 ** j) % 3 for j in range(CHUNK)], 1) - 1      # (nch, 7, cin)
    return d.reshape(nch * CHUNK, cin)[:cout].astype(np.int8)


def pack_q4(w):
    """4-bit weights (-7..7), row-major, two per byte: (w0 + 8) << 4 | (w1 + 8)."""
    v = (np.asarray(w, dtype=np.int64).reshape(-1) + 8)
    assert v.min() >= 1 and v.max() <= 15 and len(v) % 2 == 0
    return ((v[0::2] << 4) | v[1::2]).astype(np.uint8).tobytes()


def unpack_q4(b, n):
    v = np.frombuffer(b, dtype=np.uint8).astype(np.int64)
    return (np.stack([v >> 4, v & 15], 1).reshape(-1)[:n] - 8).astype(np.int8)


def unpack_ternary(b, n):
    v = np.frombuffer(b, dtype=np.uint8).astype(np.int64)
    d = np.stack([(v // 3 ** j) % 3 for j in range(5)], 1).reshape(-1)
    return (d[:n] - 1).astype(np.int8)


def weight_count(l):
    if l["kind"] == "pw":
        return l["cin"] * l["cout"]
    return 0 if l["center"] else 9 * l["cout"]


def write(path, M):
    """M: dict with cfg (arch config dict or name), names, layers' w (cout, n) and k, B0, E, U, Mc, BT, samp."""
    T, L = M["T"], M["L"]
    tn = list(T)
    order = ["xt", "out"] + [n for n in tn if n not in ("xt", "out")]
    tabs = table_names(L)
    ctot = sum(l["cout"] for l in L)
    secs = []

    def sec(tag, payload):
        if len(payload) % 2:
            payload += b"\0"
        secs.append(tag.encode() + struct.pack(">I", len(payload)) + payload)

    two = any(np.ndim(k) == 2 for k in M["k"])
    renoise = any(len(t) == 4 for t in M["samp"])
    flags = (1 if M.get("in_shift") else 0) | (2 if two else 0) | (4 if renoise else 0)
    sec("HEAD", struct.pack(">10HI", 2, len(order), len(L), len(tabs), len(M["names"]), M["E"].shape[1],
                            len(M["samp"]), M["img"], M["patch"], flags, ctot))
    sec("TENS", b"".join(struct.pack(">3H", *T[n]) for n in order))
    sec("TABS", b"".join(struct.pack(">2h", *table_range(n)) for n in tabs))
    lay = b""
    for l in L:
        lay += struct.pack(">8B6H", KIND[l["kind"]], MODE[l["mode"]], int(l.get("gather") == "s2d"),
                           int(l.get("scatter") == "d2s"), int(bool(l.get("center"))),
                           255 if l["tab"] == "raw" else tabs.index(l["tab"]), int(l.get("wq") == "q4"), 0,
                           order.index(l["src"]), order.index(l["dst"]), l["cin"], l["cout"], l["grid"], 0)
    sec("LAYR", lay)
    sec("SHFT", b"".join(np.asarray(k if (np.ndim(k) == 2 or not two) else
                                     np.stack([k, np.zeros_like(k)], 1), dtype=np.int8).tobytes() for k in M["k"]))
    wb = b""
    for l, w in zip(L, M["w"]):
        if l["kind"] == "pw" and l.get("wq") == "q4":
            wb += pack_q4(w)
        elif l["kind"] == "pw":
            wb += pack_chunks(w)
        elif weight_count(l):
            wb += pack_ternary(w)
    sec("WGTS", wb)
    sec("BIAS", np.asarray(M["B0"], dtype=">i2").tobytes())
    sec("SUBJ", np.asarray(M["E"], dtype=np.int8).tobytes() + np.asarray(M["U"], dtype=np.int8).tobytes()
        + np.asarray(M["Mc"], dtype=">i2").tobytes())
    sec("TIME", np.asarray(M["BT"], dtype=">i2").tobytes())
    if renoise:
        sec("SAMP", b"".join(struct.pack(">hhHh", *(tuple(s) + (0,))[:4]) for s in M["samp"]))
    else:
        sec("SAMP", b"".join(struct.pack(">hhH", *s) for s in M["samp"]))
    sec("NAME", b"".join(bytes([len(n)]) + n.encode("mac_roman") for n in M["names"]))
    sec("END ", b"")
    data = b"".join(secs)
    open(path, "wb").write(data)
    return len(data)


def read(path):
    data = open(path, "rb").read()
    pos, S = 0, {}
    while pos < len(data):
        tag = data[pos:pos + 4].decode()
        n = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        S[tag] = data[pos + 8:pos + 8 + n]
        pos += 8 + n + (n & 1)
        if tag == "END ":
            break
    (ver, nt, nl, ntab, nsub, D, N, img, patch, flags, ctot) = struct.unpack(">10HI", S["HEAD"])
    assert ver == 2, f"model file version {ver}, expected 2"
    names_t = ["xt", "out"] + [f"t{i}" for i in range(2, nt)]
    T = {names_t[i]: list(struct.unpack(">3H", S["TENS"][6 * i:6 * i + 6])) for i in range(nt)}
    tabs = [struct.unpack(">2h", S["TABS"][4 * i:4 * i + 4]) for i in range(ntab)]
    inv_kind = {v: k for k, v in KIND.items()}
    inv_mode = {v: k for k, v in MODE.items()}
    L = []
    for i in range(nl):
        f = struct.unpack(">8B6H", S["LAYR"][20 * i:20 * i + 20])
        lo_hi = None if f[5] == 255 else tabs[f[5]]
        tab = "raw" if lo_hi is None else (f"relu{lo_hi[1]}" if lo_hi[0] == 0 else
                                           ("res" if lo_hi[1] == arch.RES_MAX else
                                            ("lin" if lo_hi[1] == arch.LIN_MAX else f"lin{lo_hi[1]}")))
        L.append(dict(kind=inv_kind[f[0]], mode=inv_mode[f[1]], gather="s2d" if f[2] else "plain",
                      scatter="d2s" if f[3] else "plain", center=bool(f[4]), tab=tab, wq="q4" if f[6] else "tern",
                      src=names_t[f[8]], dst=names_t[f[9]], cin=f[10], cout=f[11], grid=f[12], tag=f"l{i}"))
    two = bool(flags & 2)
    sh = np.frombuffer(S["SHFT"], dtype=np.int8)
    if two:
        sh = sh[:2 * ctot].reshape(ctot, 2)
    ks, ws, o, wo = [], [], 0, 0
    for l in L:
        ks.append(sh[o:o + l["cout"]].astype(np.int64))
        o += l["cout"]
        n = weight_count(l)
        if l["kind"] == "pw" and l["wq"] == "q4":
            nb = n // 2
            ws.append(unpack_q4(S["WGTS"][wo:wo + nb], n).reshape(l["cout"], -1))
            wo += nb
        elif l["kind"] == "pw":
            nb = chunk_bytes(l["cout"], l["cin"])
            ws.append(unpack_chunks(S["WGTS"][wo:wo + nb], l["cout"], l["cin"]))
            wo += nb
        elif n:
            nb = (n + 4) // 5
            ws.append(unpack_ternary(S["WGTS"][wo:wo + nb], n).reshape(l["cout"], -1))
            wo += nb
        else:
            ws.append(np.ones((l["cout"], 1), dtype=np.int8))
    B0 = np.frombuffer(S["BIAS"][:2 * ctot], dtype=">i2").astype(np.int64)
    sj = S["SUBJ"]
    E = np.frombuffer(sj[:nsub * D], dtype=np.int8).reshape(nsub, D).astype(np.int64)
    U = np.frombuffer(sj[nsub * D:nsub * D + D * ctot], dtype=np.int8).reshape(D, ctot).astype(np.int64)
    Mc = np.frombuffer(sj[nsub * D + D * ctot:nsub * D + D * ctot + 2 * ctot], dtype=">i2").astype(np.int64)
    BT = np.frombuffer(S["TIME"][:2 * N * ctot], dtype=">i2").reshape(N, ctot).astype(np.int64)
    if flags & 4:
        samp = [struct.unpack(">hhHh", S["SAMP"][8 * i:8 * i + 8]) for i in range(N)]
    else:
        samp = [struct.unpack(">hhH", S["SAMP"][6 * i:6 * i + 6]) for i in range(N)]
    names, p = [], 0
    nm = S["NAME"]
    for _ in range(nsub):
        n = nm[p]
        names.append(nm[p + 1:p + 1 + n].decode("mac_roman"))
        p += 1 + n
    return dict(T=T, L=L, k=ks, w=ws, B0=B0, E=E, U=U, Mc=Mc, BT=BT, samp=samp, names=names, img=img,
                patch=patch, ctot=ctot, size=len(data), in_shift=arch.IN_SHIFT_Q4 if flags & 1 else 0,
                flags=flags)


def random_model(cfg, n_subjects=8, n_steps=6, seed=1):
    """A random model with the Mac bench's weights (ref.random_layer) and random conditioning."""
    import ref
    T, L = arch.build(cfg)
    order = ["xt", "out"] + [n for n in T if n not in ("xt", "out")]
    T = {n: T[n] for n in order}
    rs = np.random.RandomState(seed)
    ws, ks, b0 = [], [], []
    for i, l in enumerate(L):
        w, k, b = ref.random_layer(i, l)
        ws.append(w)
        ks.append(k)
        b0.append(b)
    ctot = sum(l["cout"] for l in L)
    D = 32
    c = arch.CONFIGS[cfg]
    return dict(T=T, L=L, w=ws, k=ks, B0=np.concatenate(b0), img=c["img"], patch=c["patch"],
                in_shift=arch.in_shift(cfg)[0],
                E=rs.randint(-127, 128, size=(n_subjects, D)), U=rs.randint(-127, 128, size=(D, ctot)),
                Mc=rs.randint(-300, 300, size=ctot), BT=rs.randint(-40, 40, size=(n_steps, ctot)),
                samp=sampler_consts(cosine_grid(n_steps)), names=[f"subject {i}" for i in range(n_subjects)])


if __name__ == "__main__":
    if sys.argv[1] == "random":
        args = sys.argv[2:]
        opt = {a: args[i + 1] for i, a in enumerate(args) if a.startswith("--")}
        M = random_model(args[0], int(opt.get("--subjects", 8)), int(opt.get("--steps", 6)), int(opt.get("--seed", 1)))
        n = write(args[1], M)
        print(f"wrote {args[1]}: {n:,} bytes")
    else:
        R = read(sys.argv[2])
        print(f"{sys.argv[2]}: {R['size']:,} bytes, {len(R['L'])} layers, {len(R['names'])} subjects, "
              f"{len(R['samp'])} steps, ctot {R['ctot']}, params {sum(w.size for w in R['w'])}")
