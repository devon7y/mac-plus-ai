#!/usr/bin/env python3
"""bwsave.py: Blockworld save files on the host. The reference for the game's format 7 (app/save.c),
its world run stream (app/world.c world_pack / world_unpack) and its LZ codec (app/lz.c).

    bwsave.py info FILE [--blocks OUT]     header, sizes, checksum; unpack and check (FILE: a save
                                           as on the Mac's disk, or MacBinary); OUT: its blocks
    bwsave.py make --seed S --x X --z Z --yaw Y [--aim A] [--time T] --out FILE.bin
                                           the world of seed S (engine/world.py, the game's
                                           generator) with the player standing at (X, Z), in
                                           format 7 (tighter LZ parse than the Mac's), MacBinary
                                           "Blockworld Default" (type BWwd, creator BWrd: its Finder icon)

Format 7 (all big-endian, 68000 layout):
  Header   'BWSV', version 7, number of blocks (128*128*48), SaveState (52 bytes: seed, px py pz (8.8),
           yaw pitch, time, health food sel, bed bx by bz, sx sy sz (longs), dist)
  Header7  rawlen, packlen, checksum (rotate-left-1-and-add over big-endian longs of the payload,
           zero-padded), the 7 array sizes
  payload  packlen bytes, LZ-packed (see lz_unpack) to rawlen bytes: the arrays (inventory 144,
           armour 16, chests 5568, furnaces 624, objects 2048, mobs 432, dropped items 768 bytes),
           then the world: a dictionary of its distinct columns (see pack_world)
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BW = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(BW, "engine"))

WX, WY = 128, 48
NBLOCKS_TYPES = 29                     # world.h NBLOCKS
AIR, ROCK, BEDROCK = 0, 3, 12
FLOOR = 2
LAST = 0x80
SECTIONS = (("inv", 144), ("inv_arm", 16), ("chests", 5568), ("furns", 624), ("objs", 2048),
            ("mobs", 432), ("drops", 768))
ARRAYS = sum(n for _, n in SECTIONS)
MAGIC, VERSION = 0x42575356, 7
STATE = ">4l2hl7h3lh"                 # SaveState, 52 bytes
STATE_FIELDS = ("seed", "px", "py", "pz", "yaw", "pitch", "time", "health", "food", "sel", "bed",
                "bx", "by", "bz", "sx", "sy", "sz", "dist")
assert struct.calcsize(STATE) == 52
LZ_MINM, LZ_HBITS = 3, 12


# ---- the world part: a dictionary of distinct columns -----------------------------------------

CWX = WX // 4


def coarse_map(b):
    """the highest first top (1 if none) of each 4 x 4 columns (world.c bw_coarse)"""
    ch = bytearray(CWX * CWX)
    for c in range(WX * WX):
        top = WY
        while top > 0 and b[c * WY + top - 1] == AIR:
            top -= 1
        i = (c >> 9) * CWX + ((c & (WX - 1)) >> 2)
        ch[i] = max(ch[i], top or 1)
    return bytes(ch)


def col_record(col):
    """a column's record: its top, then its runs top-down (type, length; the run reaching y 0:
    type | 0x80, no length)"""
    top = WY
    while top > 0 and col[top - 1] == AIR:
        top -= 1
    out = bytearray([top])
    y = top
    while y > 0:
        t = col[y - 1]
        k = y - 1
        while k > 0 and col[k - 1] == t:
            k -= 1
        if k == 0:
            out.append(t | LAST)
            break
        out += bytes((t, y - k))
        y = k
    return bytes(out)


def pack_world(b):
    """blocks ((x * WX + z) * WY + y) -> the coarse map (1024), the number of distinct columns (2),
    each column's id (2 each, by first appearance), the distinct columns' records"""
    ids, index, recs = {}, bytearray(), bytearray()
    for c in range(WX * WX):
        col = bytes(b[c * WY:(c + 1) * WY])
        if col not in ids:
            ids[col] = len(ids)
            recs += col_record(col)
        index += struct.pack(">H", ids[col])
    return coarse_map(b) + struct.pack(">H", len(ids)) + bytes(index) + bytes(recs)


def unpack_world(s):
    """-> blocks; raises ValueError if it is malformed (as world_unpack_c)"""
    n = struct.unpack(">H", s[1024:1026])[0]
    if not 1 <= n <= WX * WX:
        raise ValueError(f"{n} columns")
    index = struct.unpack(f">{WX * WX}H", s[1026:1026 + 2 * WX * WX])
    r = 1026 + 2 * WX * WX
    cols = []
    b = bytearray()
    for c in range(WX * WX):
        i = index[c]
        if i > len(cols) or i >= n:
            raise ValueError(f"column {c}: id {i}")
        if i == len(cols):
            top = s[r]
            r += 1
            if top > WY:
                raise ValueError(f"column {c}: top {top}")
            col = bytearray(WY)
            y = top
            while y > 0:
                t = s[r]
                r += 1
                if t & LAST:
                    t &= ~LAST
                    k = 0
                else:
                    k = y - s[r]
                    r += 1
                    if not 0 < k < y:
                        raise ValueError(f"column {c}: run to {k} from {y}")
                if t >= NBLOCKS_TYPES or (y == top and t == AIR):
                    raise ValueError(f"column {c}: type {t}")
                col[k:y] = bytes([t]) * (y - k)
                y = k
            cols.append(bytes(col))
        b += cols[i]
    if len(cols) != n or r != len(s):
        raise ValueError(f"{len(cols)} of {n} columns; records end at {r} of {len(s)}")
    if s[:1024] != coarse_map(b):
        raise ValueError("the coarse map")
    return bytes(b)


# ---- LZ ----------------------------------------------------------------------------------------

def _hash(d, i):
    return ((d[i] << 6) ^ (d[i + 1] << 3) ^ d[i + 2]) & ((1 << LZ_HBITS) - 1)


def _put_len(o, r):
    while r >= 255:
        o.append(255)
        r -= 255
    o.append(r)


def _emit(o, lit, mlen, off):
    m = mlen - LZ_MINM if mlen else 0
    o.append((min(len(lit), 15) << 4) | min(m, 15))
    if len(lit) >= 15:
        _put_len(o, len(lit) - 15)
    o += lit
    if mlen:
        o += bytes((off >> 8, off & 255))
        if m >= 15:
            _put_len(o, m - 15)


def lz_pack_mac(d):
    """lz.c's lz_pack, byte for byte (greedy, one position per hash; positions inside a match are
    not remembered)"""
    n = len(d)
    ht = [-1] * (1 << LZ_HBITS)
    o = bytearray()
    i = anchor = 0
    while i + LZ_MINM <= n:
        h = _hash(d, i)
        c = ht[h]
        ht[h] = i
        if c >= 0 and i - c <= 65535 and d[c:c + 3] == d[i:i + 3]:
            ln = LZ_MINM
            while i + ln < n and d[c + ln] == d[i + ln]:
                ln += 1
            _emit(o, d[anchor:i], ln, i - c)
            i = anchor = i + ln
        else:
            i += 1
    if anchor < n:
        _emit(o, d[anchor:n], 0, 0)
    return bytes(o)


def lz_pack_best(d, chain=64):
    """the same format, tighter: hash chains, a lazy step (prefer a longer match one byte on)"""
    n = len(d)
    head, prev = {}, [-1] * n

    def insert(p):
        if p + 3 <= n:
            k = d[p:p + 3]
            prev[p] = head.get(k, -1)
            head[k] = p

    def longest(i):
        best, boff = 0, 0
        if i + 3 > n:
            return 0, 0
        p, k = head.get(d[i:i + 3], -1), 0
        while p >= 0 and i - p <= 65535 and k < chain:
            ln = 0
            while i + ln < n and d[p + ln] == d[i + ln]:
                ln += 1
            if ln > best:
                best, boff = ln, i - p
                if i + ln == n:
                    break
            p, k = prev[p], k + 1
        return best, boff

    o = bytearray()
    i = anchor = 0
    while i < n:
        ln, off = longest(i)
        if ln >= LZ_MINM:
            insert(i)
            ln2, _ = longest(i + 1)
            if ln2 > ln + 1:                 # a literal now, the longer match next
                i += 1
                continue
            _emit(o, d[anchor:i], ln, off)
            for j in range(i + 1, i + ln):
                insert(j)
            i = anchor = i + ln
        else:
            insert(i)
            i += 1
    if anchor < n:
        _emit(o, d[anchor:n], 0, 0)
    return bytes(o)


def lz_unpack(s, n):
    o = bytearray()
    i = 0
    while len(o) < n:
        t = s[i]
        i += 1
        lit = t >> 4
        if lit == 15:
            while True:
                v = s[i]
                i += 1
                lit += v
                if v != 255:
                    break
        if lit > n - len(o) or i + lit > len(s):
            raise ValueError("literals past the end")
        o += s[i:i + lit]
        i += lit
        if len(o) >= n:
            break
        off = (s[i] << 8) | s[i + 1]
        i += 2
        m = t & 15
        if m == 15:
            while True:
                v = s[i]
                i += 1
                m += v
                if v != 255:
                    break
        m += LZ_MINM
        if not 0 < off <= len(o) or m > n - len(o):
            raise ValueError("bad match")
        for _ in range(m):
            o.append(o[-off])
    return bytes(o)


def checksum(p):
    s = 0
    for i in range(0, len(p), 4):
        v = int.from_bytes(p[i:i + 4].ljust(4, b"\0"), "big")
        s = (((s << 1) | (s >> 31)) + v) & 0xFFFFFFFF
    return s


# ---- files ---------------------------------------------------------------------------------------

def write_save(state, blocks, arrays=None, best=True):
    arrays = arrays if arrays is not None else bytes(ARRAYS)
    assert len(arrays) == ARRAYS
    raw = arrays + pack_world(blocks)
    pk = (lz_pack_best if best else lz_pack_mac)(raw)
    st = struct.pack(STATE, *(state.get(f, 0) for f in STATE_FIELDS))
    h = struct.pack(">3l", MAGIC, VERSION, WX * WX * WY) + st
    h7 = struct.pack(">2lL", len(raw), len(pk), checksum(raw))
    h7 += struct.pack(">7l", *(n for _, n in SECTIONS))
    return h + h7 + pk


def read_save(data):
    """-> dict(version, state, arrays, blocks, rawlen, packlen, stream)"""
    if data[0] == 0 and len(data) > 128 and data[65:69] == b"BWwd":                       # MacBinary
        n = struct.unpack(">I", data[83:87])[0]
        data = data[128:128 + n]
    magic, version, nb = struct.unpack(">3l", data[:12])
    if magic != MAGIC or nb != WX * WX * WY:
        raise ValueError("not a Blockworld save")
    state = dict(zip(STATE_FIELDS, struct.unpack(STATE, data[12:64])))
    out = dict(version=version, state=state)
    if version >= 7:
        rawlen, packlen, s = struct.unpack(">2lL", data[64:76])
        sizes = struct.unpack(">7l", data[76:104])
        if sizes != tuple(n for _, n in SECTIONS):
            raise ValueError(f"array sizes {sizes}")
        raw = lz_unpack(data[104:104 + packlen], rawlen)
        if checksum(raw) != s:
            raise ValueError("checksum")
        out.update(rawlen=rawlen, packlen=packlen, arrays=raw[:ARRAYS], stream=raw[ARRAYS:],
                   blocks=unpack_world(raw[ARRAYS:]))
    else:
        p = 64 + ARRAYS - (768 if version < 6 else 0) - (432 if version < 5 else 0)
        out["arrays"] = data[64:p]
        b = bytearray()
        while len(b) < WX * WX * WY:
            b += bytes([data[p + 1]]) * data[p]
            p += 2
        out["blocks"] = bytes(b[:WX * WX * WY])
    return out


def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def macbinary(name, ftype, creator, data, locked=False):
    h = bytearray(128)
    nm = name.encode("mac_roman")
    h[1] = len(nm)
    h[2:2 + len(nm)] = nm
    h[65:69], h[69:73] = ftype, creator
    h[81] = 1 if locked else 0                   # protected
    h[83:87] = struct.pack(">I", len(data))
    import time
    mac_now = int(time.time()) + 2082844800 + time.localtime().tm_gmtoff   # (seconds since 1904, local)
    h[91:95] = h[95:99] = struct.pack(">I", mac_now & 0xFFFFFFFF)
    h[122] = h[123] = 129
    h[124:126] = struct.pack(">H", crc16_xmodem(bytes(h[:124])))
    return bytes(h) + data + b"\0" * ((-len(data)) % 128)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info")
    p.add_argument("file")
    p.add_argument("--blocks")
    p = sub.add_parser("make")
    p.add_argument("--seed", type=int, required=True)
    p.add_argument("--x", type=float, required=True, help="the player's feet: block x (+ 0.5: its middle)")
    p.add_argument("--z", type=float, required=True)
    p.add_argument("--yaw", type=int, required=True, help="0..1023 (0: +x, 256: +z)")
    p.add_argument("--aim", type=int, default=0, help="the view's y-shear in rows (+ up)")
    p.add_argument("--time", type=int, default=1000, help="time of day (0 sunrise, 6000 noon)")
    p.add_argument("--out", required=True)
    p.add_argument("--name", default="Blockworld Default")
    p.add_argument("--fast", action="store_true", help="the Mac's greedy packer (quicker)")
    a = ap.parse_args()
    if a.cmd == "info":
        d = read_save(open(a.file, "rb").read())
        print("version", d["version"], "state", d["state"])
        if d["version"] >= 7:
            print(f"payload {d['rawlen']} bytes (arrays {ARRAYS}, world stream {len(d['stream'])}), packed {d['packlen']}")
        if a.blocks:
            open(a.blocks, "wb").write(d["blocks"])
        return
    import world as W
    b, hm = W.generate(a.seed)
    b = bytes(b)
    x8, z8 = int(a.x * 256), int(a.z * 256)
    xi, zi = x8 >> 8, z8 >> 8
    y = WY - 1
    while y > 0 and b[(xi * WX + zi) * WY + y] in (AIR,):
        y -= 1
    y8 = (y + 1) << 8
    st = dict(seed=a.seed, px=x8, py=y8, pz=z8, yaw=a.yaw & 1023, pitch=a.aim, time=a.time, health=20,
              food=20, sx=x8, sy=y8, sz=z8)
    assert unpack_world(pack_world(b)) == b
    data = write_save(st, b, best=not a.fast)
    back = read_save(data)
    assert back["blocks"] == b
    open(a.out, "wb").write(macbinary(a.name, b"BWwd", b"BWrd", data, locked=True))
    print(f"seed {a.seed}: player at ({x8 / 256:.2f}, {y8 / 256:.2f}, {z8 / 256:.2f}) yaw {a.yaw} aim {a.aim} time {a.time}; "
          f"stream {back['rawlen'] - ARRAYS} + arrays {ARRAYS} -> {back['packlen']} bytes packed; file {len(data)} bytes -> {a.out}")


if __name__ == "__main__":
    main()
