#!/usr/bin/env python3
"""Guess My Doodle model files ('GMDL' version 2, big-endian; read by app/engine.c and ref.py).

  'GMDL' u32, version u16 (2), G u16, ncls u16, ntens u16, nlay u16, reserved u16
  tensors: ntens x (H u16, W u16, C u16, pad u16)     pad = 1: stored with a zero border
                                                      (the input of a 3x3 layer)
  layers:  nlay x (kind u16, src u16, dst u16, k u16, stride u16, cin u16, cout u16, A i16, len u32)
  layer data, in order, each padded to an even length:
    T (table): int16 [512][cout], the activations for every 3x3 binary pattern
    D, C, L:   cout x (M i16, S i16, B i32), then the ternary weights:
               D: 2 bits each, 4 per byte, first weight in the top bits (00 = 0, 01 = +1, 11 = -1),
                  order [c][ky][kx];
               C, L: chunk codes: output channels in chunks of 7 (the last one may be short); for
                  each chunk and each input i (C: i = (ky, kx, cin), L: i = cin) one code
                  sum_j (w[7q + j][i] + 1) 3^j (missing rows count as 0 weights), 12 bits, two codes
                  per 3 bytes (big-endian 24 bits, first code on top; an odd last code is padded
                  with ZERO7 = 1093)
  names: ncls Pascal strings (length byte + bytes), padded to an even length

Tensor 0 is the 32x32 binary input (C = 1); kinds: T=1 (3x3 table conv on the binary input),
P=2 (2x2 max pool), D=3 (depthwise 3x3), C=4 (KxK conv, stride 1 or 2), G=5 (global max),
L=6 (fully connected on the src tensor flattened channels-last). A = -1: raw output (logits).
"""
import struct

import numpy as np

KINDS = {"T": 1, "P": 2, "D": 3, "C": 4, "G": 5, "L": 6}
KNAMES = {v: k for k, v in KINDS.items()}
CODE = {0: 0, 1: 1, -1: 3}


ZERO7 = 1093
P3 = 3 ** np.arange(7)


def fan_in(l):
    if l["kind"] == "C":
        return l["k"] * l["k"] * l["cin"]
    return l["cin"] if l["kind"] == "L" else 9


def pack_codes(w, cout, fan):
    w = np.asarray(w, np.int64).reshape(cout, fan)
    nch = (cout + 6) // 7
    wp = np.zeros((nch * 7, fan), np.int64)
    wp[:cout] = w
    codes = ((wp.reshape(nch, 7, fan) + 1) * P3[None, :, None]).sum(1).ravel()
    if len(codes) & 1:
        codes = np.append(codes, ZERO7)
    c = codes.reshape(-1, 2)
    v = c[:, 0] << 12 | c[:, 1]
    return np.stack([v >> 16, (v >> 8) & 255, v & 255], 1).astype(np.uint8).tobytes()


def unpack_codes(b, cout, fan):
    nch = (cout + 6) // 7
    n = nch * fan
    a = np.frombuffer(b, np.uint8)[:3 * ((n + 1) // 2)].astype(np.int64).reshape(-1, 3)
    v = a[:, 0] << 16 | a[:, 1] << 8 | a[:, 2]
    codes = np.stack([v >> 12, v & 4095], 1).ravel()[:n].reshape(nch, fan)
    trits = (codes[:, None, :] // P3[None, :, None]) % 3 - 1          # (nch, 7, fan)
    return trits.reshape(nch * 7, fan)[:cout].astype(np.int8)


def pack_tern(w):
    w = np.asarray(w, np.int64).ravel()
    codes = np.where(w > 0, 1, np.where(w < 0, 3, 0)).astype(np.uint8)
    pad = (-len(codes)) % 4
    codes = np.concatenate([codes, np.zeros(pad, np.uint8)]).reshape(-1, 4)
    return (codes[:, 0] << 6 | codes[:, 1] << 4 | codes[:, 2] << 2 | codes[:, 3]).astype(np.uint8).tobytes()


def unpack_tern(b, n):
    a = np.frombuffer(b, np.uint8)
    c = np.stack([a >> 6, (a >> 4) & 3, (a >> 2) & 3, a & 3], 1).ravel()[:n]
    return np.where(c == 1, 1, np.where(c == 3, -1, 0)).astype(np.int8)


def nweights(l):
    if l["kind"] == "C":
        return l["cout"] * l["k"] * l["k"] * l["cin"]
    if l["kind"] == "D":
        return l["cout"] * 9
    if l["kind"] == "L":
        return l["cout"] * l["cin"]
    return 0


def write(path, m):
    """m: dict(G, tensors=[(H, W, C, pad)], layers=[dict(kind, src, dst, k, stride, cin, cout, A,
    table | (M, S, B, w))], names=[str])."""
    out = bytearray()
    lay = m["layers"]
    datas = []
    for l in lay:
        if l["kind"] == "T":
            d = np.asarray(l["table"], ">i2").tobytes()
        elif l["kind"] in ("D", "C", "L"):
            hdr = b"".join(struct.pack(">hhi", int(a), int(b), int(c)) for a, b, c in zip(l["M"], l["S"], l["B"]))
            if l["kind"] == "D":
                d = hdr + pack_tern(l["w"])
            else:
                d = hdr + pack_codes(l["w"], l["cout"], fan_in(l))
        else:
            d = b""
        if len(d) & 1:
            d += b"\0"
        datas.append(d)
    out += struct.pack(">4sHHHHHH", b"GMDL", 2, m["G"], len(m["names"]), len(m["tensors"]), len(lay), 0)
    for t in m["tensors"]:
        out += struct.pack(">HHHH", *t)
    for l, d in zip(lay, datas):
        out += struct.pack(">HHHHHHHhI", KINDS[l["kind"]], l["src"], l["dst"], l.get("k", 0), l.get("stride", 1),
                           l.get("cin", 0), l.get("cout", 0), l.get("A", 0), len(d))
    for d in datas:
        out += d
    nm = b"".join(bytes([len(s)]) + s.encode("ascii") for s in m["names"])
    if len(nm) & 1:
        nm += b"\0"
    out += nm
    open(path, "wb").write(bytes(out))
    return len(out)


def read(path):
    b = open(path, "rb").read()
    magic, ver, G, ncls, nt, nl, _ = struct.unpack_from(">4sHHHHHH", b, 0)
    assert magic == b"GMDL" and ver == 2
    o = 16
    tensors = []
    for _ in range(nt):
        tensors.append(struct.unpack_from(">HHHH", b, o))
        o += 8
    hdrs = []
    for _ in range(nl):
        hdrs.append(struct.unpack_from(">HHHHHHHhI", b, o))
        o += 20
    layers = []
    for kind, src, dst, k, stride, cin, cout, A, ln in hdrs:
        l = dict(kind=KNAMES[kind], src=src, dst=dst, k=k, stride=stride, cin=cin, cout=cout, A=A)
        d = b[o:o + ln]
        o += ln
        if l["kind"] == "T":
            l["table"] = np.frombuffer(d, ">i2").astype(np.int64).reshape(512, cout)
        elif l["kind"] in ("D", "C", "L"):
            hdr = np.frombuffer(d[:8 * cout], np.dtype([("M", ">i2"), ("S", ">i2"), ("B", ">i4")]))
            l["M"], l["S"], l["B"] = (hdr[f].astype(np.int64) for f in ("M", "S", "B"))
            if l["kind"] == "D":
                l["w"] = unpack_tern(d[8 * cout:], nweights(l))
            else:
                l["w"] = unpack_codes(d[8 * cout:], cout, fan_in(l)).ravel()
        layers.append(l)
    names = []
    for _ in range(ncls):
        n = b[o]
        names.append(b[o + 1:o + 1 + n].decode("ascii"))
        o += 1 + n
    return dict(G=G, tensors=tensors, layers=layers, names=names, size=len(b))


def write_macbinary(src, out_bin, name, ftype=b"GMDL", creator=b"GsDd"):
    """Wrap a data file as MacBinary (for hcopy -m / build_disk.py --extra)."""
    import os
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "mactools"))
    import rsrc
    name = name.encode("ascii") if isinstance(name, str) else name
    hdr = bytearray(128)
    hdr[1] = len(name)
    hdr[2:2 + len(name)] = name
    hdr[65:69], hdr[69:73] = ftype, creator
    rsrc.write_macbinary(out_bin, bytes(hdr), open(src, "rb").read(), b"")


if __name__ == "__main__":
    import sys
    if len(sys.argv) >= 5 and sys.argv[1] == "macbinary":       # macbinary SRC OUT.bin "Mac Name" [TYPE]
        write_macbinary(sys.argv[2], sys.argv[3], sys.argv[4], (sys.argv[5] if len(sys.argv) > 5 else "GMDL").encode())
    else:
        print("usage: model_file.py macbinary <src> <out.bin> <mac name> [type]")
