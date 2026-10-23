#!/usr/bin/env python3
"""Doodle's model file ("Doodle Model", type 'DMDL', creator 'Ddle'): integers only, big-endian.

Header (64 bytes):
   0 'DMD1'            4 header size (64)     8 H     10 kind_wh   12 kind_wo   14 cell (0 GRU)
  16 A     18 AO       20 SHR    22 SHO       24 SHA  26 SHB       28 FQ        30 NCAT
  32 NEMB  34 NOUT     36 names offset (long) 40 data offset (long) 44 SHX (0 = 6)
  46 flags (bit 0: two-term output layer, "tern_r2o")  48 SHO2   50..63 reserved (0)
  kind: 0 = ternary (stored one byte per group of 4 columns: idx = sum (w_j+1)*3^j),
        2 = 4-bit (one byte per pair of columns: c = (w1+8)*16 + (w2+8))
Names: NCAT Pascal strings (length byte + Mac Roman text), then padding to an even offset.
Data, in this order (R = 3H rows; "words" are int16):
  U  R x groups bytes      WY NOUT x groups bytes     M 2H words (r, z rows)
  MN 2H words ((m, bhn) per n row)                     MY NOUT words      BY NOUT words
  B0 4H words (fold bias: 3H gate rows, then H rows for h0)               MC 4H words
  WC 4H x NEMB bytes (int8)  EMB NCAT x NEMB words     P 3 x R words (pen columns)
  WDX R words              WDY R words
  with flag bit 0 only:  WY2 NOUT x groups bytes (same kind as WY)   MY2 NOUT words
The meaning of every number is in FORMAT.md.
"""
import struct

import numpy as np

MAGIC = b"DMD1"
HDR = 64
TERN, INT4 = 0, 2


def groups(kind, H):
    return H // 4 if kind == TERN else H // 2


def encode_rows(W, kind):
    W = np.asarray(W, np.int64)
    if kind == TERN:
        assert W.min() >= -1 and W.max() <= 1
        g = (W + 1).reshape(W.shape[0], -1, 4)
        return (g[..., 0] + 3 * g[..., 1] + 9 * g[..., 2] + 27 * g[..., 3]).astype(np.uint8)
    assert W.min() >= -8 and W.max() <= 7
    g = (W + 8).reshape(W.shape[0], -1, 2)
    return (g[..., 0] * 16 + g[..., 1]).astype(np.uint8)


def decode_rows(b, kind, H):
    b = np.asarray(b, np.int64)
    if kind == TERN:
        return np.stack([(b // 3 ** j) % 3 - 1 for j in range(4)], axis=-1).reshape(b.shape[0], H)
    return np.stack([b // 16 - 8, b % 16 - 8], axis=-1).reshape(b.shape[0], H)


SECTIONS = ["U", "WY", "M", "MN", "MY", "BY", "B0", "MC", "WC", "EMB", "P", "WDX", "WDY"]


def shapes(m):
    H, nout, ncat, nemb = m["H"], m.get("NOUT", 123), len(m["names"]), m.get("NEMB", 32)
    R = 3 * H
    return {"U": ((R, groups(m["kind_wh"], H)), "u1"), "WY": ((nout, groups(m["kind_wo"], H)), "u1"),
            "M": ((2 * H,), ">i2"), "MN": ((H, 2), ">i2"), "MY": ((nout,), ">i2"), "BY": ((nout,), ">i2"),
            "B0": ((4 * H,), ">i2"), "MC": ((4 * H,), ">i2"), "WC": ((4 * H, nemb), "i1"),
            "EMB": ((ncat, nemb), ">i2"), "P": ((3, R), ">i2"), "WDX": ((R,), ">i2"), "WDY": ((R,), ">i2"),
            "WY2": ((nout, groups(m["kind_wo"], H)), "u1"), "MY2": ((nout,), ">i2")}


def write(path, m):
    """m: H, kind_wh, kind_wo, A, AO, SHR, SHO, SHA, SHB, FQ, names, and the integer arrays
    (U and WY as weight matrices, the others as in SECTIONS)."""
    names = b"".join(bytes([len(n)]) + n.encode("mac_roman") for n in m["names"])
    if len(names) % 2:
        names += b"\0"
    sh = shapes(m)
    data = b""
    r2 = m.get("WY2") is not None
    for s in SECTIONS + (["WY2", "MY2"] if r2 else []):
        a = m[s]
        if s == "U":
            a = encode_rows(a, m["kind_wh"])
        elif s in ("WY", "WY2"):
            a = encode_rows(a, m["kind_wo"])
        a = np.asarray(a)
        shape, dt = sh[s]
        assert a.shape == shape, (s, a.shape, shape)
        if dt != "u1":
            info = np.iinfo(np.int16 if "i2" in dt else np.int8)
            assert a.min() >= info.min and a.max() <= info.max, (s, a.min(), a.max())
        data += a.astype(dt).tobytes()
    hdr = MAGIC + struct.pack(">I14hII", HDR, m["H"], m["kind_wh"], m["kind_wo"], 0, m["A"], m["AO"],
                              m["SHR"], m["SHO"], m["SHA"], m["SHB"], m["FQ"], len(m["names"]),
                              m.get("NEMB", 32), m.get("NOUT", 123), HDR, HDR + len(names))
    hdr += struct.pack(">hhh", m.get("SHX", 6), 1 if r2 else 0, m.get("SHO2", 0))
    hdr += bytes(HDR - len(hdr))
    open(path, "wb").write(hdr + names + data)
    return HDR + len(names) + len(data)


def read(path):
    d = open(path, "rb").read()
    assert d[:4] == MAGIC, "not a Doodle model"
    v = struct.unpack(">I14hII", d[4:44])
    m = dict(zip(["hdr", "H", "kind_wh", "kind_wo", "cell", "A", "AO", "SHR", "SHO", "SHA", "SHB", "FQ",
                  "NCAT", "NEMB", "NOUT", "names_off", "data_off"], v))
    m["SHX"] = struct.unpack(">h", d[44:46])[0] or 6
    m["flags"], m["SHO2"] = struct.unpack(">hh", d[46:50])
    pos, names = m["names_off"], []
    for _ in range(m["NCAT"]):
        n = d[pos]
        names.append(d[pos + 1:pos + 1 + n].decode("mac_roman"))
        pos += 1 + n
    m["names"] = names
    pos = m["data_off"]
    sh = shapes(m)
    if not m["flags"] & 1:
        del sh["WY2"], sh["MY2"]
    for s, (shape, dt) in sh.items():
        n = int(np.prod(shape)) * np.dtype(dt).itemsize
        a = np.frombuffer(d[pos:pos + n], dt).reshape(shape).astype(np.int64)
        pos += n
        if s == "U":
            a = decode_rows(a, m["kind_wh"], m["H"])
        elif s in ("WY", "WY2"):
            a = decode_rows(a, m["kind_wo"], m["H"])
        m[s] = a
    assert pos == len(d), (pos, len(d))
    return m


def write_macbinary(dmdl_path, out_bin, name=b"Doodle Model"):
    """Wrap a model file as MacBinary (type 'DMDL', creator 'Ddle') for hcopy -m / build_disk.py --extra."""
    import os
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "mactools"))
    import rsrc
    hdr = bytearray(128)
    hdr[1] = len(name)
    hdr[2:2 + len(name)] = name
    hdr[65:69], hdr[69:73] = b"DMDL", b"Ddle"
    rsrc.write_macbinary(out_bin, bytes(hdr), open(dmdl_path, "rb").read(), b"")


if __name__ == "__main__":
    import sys
    if len(sys.argv) == 4 and sys.argv[1] == "macbinary":
        write_macbinary(sys.argv[2], sys.argv[3])
    else:
        print("usage: model_file.py macbinary <model.dmdl> <out.bin>")
