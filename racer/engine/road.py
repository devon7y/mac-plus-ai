#!/usr/bin/env python3
"""Pseudo-3D road model: the bit-exact Python reference for the 68000 renderer.

Shared by Endurance (racer/) and Rail Runner (runner/). Everything here uses the same
integer arithmetic as the assembly (road.s), so tables generated from this file and
frames rendered by `render()` match what the Mac draws.

Coordinates: playfield W x H pixels, row 0 at the top. The horizon is row HY; road rows
are y = HY+1 .. H-1, indexed by k = y - HY (1 = farthest, KB = H-1-HY = nearest).

Per frame ("pass 1", `geometry()`), walking rows from the bottom (k = KB) up:
    zw = camz + zrow[k]                       world depth of the row (32-bit)
    while zw >= seg_end: next segment          (curvature ddx, 16.16 px/row^2)
    dx += ddx ; x += dx                        screen-space curve accumulation (16.16)
    cx[k]   = (x >> 16) + W/2                  road centre in pixels
    phase[k]= (zw >> STRIPE_SHIFT) & 1          stripe parity (rumble/grass/markers alternate)
Initial x = -camx << 16 and dx = camx * RECIP (RECIP = round(65536 / KB)) put the camera
`camx` pixels right of the road centre at the bottom row, fading linearly to 0 at the horizon.
cx is clamped to [-CXL, W + CXL] here (beyond that the row looks the same); the 68000 code
doesn't clamp, its decode table is wide enough for any curve the game generates.

Per row ("pass 2", `render()`), the row is a fixed sequence of spans. A *layout* lists
boundary offsets from cx (per-row tables, sorted) and the pattern slot of the span that
starts at each boundary. Span patterns are 32-bit longs from a pattern set chosen per row
by (phase, row parity): set = patterns[phase][y & 1] = [slot0, slot1, ...].
"""
from dataclasses import dataclass, field
import numpy as np

M32 = 0xFFFFFFFF


def s32(v):
    v &= M32
    return v - (1 << 32) if v & 0x80000000 else v


@dataclass
class RoadConfig:
    W: int = 384               # playfield width, multiple of 32
    H: int = 240               # playfield height
    HY: int = 80               # horizon row
    ZNEAR: int = 260           # world depth of the bottom row
    STRIPE_SHIFT: int = 6      # stripe length = 64 world units
    KSTRIPE: int = 18          # rows with k < KSTRIPE never alternate (avoids far aliasing)
    HWB: float = 0.40          # road half-width at the bottom row, fraction of W
    RWB: float = 0.045         # rumble strip width at the bottom, fraction of W
    MWB: float = 0.012         # lane marker width at the bottom, fraction of W
    MARKERS: tuple = (-1/3, 1/3)   # lane marker centres, fraction of the half-width
    MG: int = 2                # invisible margin (longs) on each side of every buffer row
    KMARK: int = 0             # lane markers only on rows k >= KMARK (0 = automatic: rows where
                               # markers are >= 64 px from each other and the edges: fast path)
    # layouts: name -> list of (offset name, slot of the span that starts there)
    slots: tuple = ("grass", "rumble", "road", "marker")

    @property
    def KB(self):
        return self.H - 1 - self.HY

    @property
    def ZS(self):
        return self.ZNEAR * self.KB

    @property
    def RECIP(self):
        return round(65536 / self.KB)

    @property
    def MAXOFF(self):
        return int(round(self.HWB * self.W)) + int(round(self.RWB * self.W)) + 1

    @property
    def CXL(self):
        return self.MAXOFF + 32


def build_tables(cfg):
    """Per-row tables, index k = 0..KB (k = 0 unused)."""
    KB = cfg.KB
    k = np.arange(KB + 1)
    kk = np.maximum(k, 1)
    zrow = np.minimum(cfg.ZS // kk, 65535)
    zrow[0] = 65535
    hw = np.round(cfg.HWB * cfg.W * k / KB).astype(int)
    rw = np.maximum(np.round(cfg.RWB * cfg.W * k / KB), 1).astype(int)
    mw = np.round(cfg.MWB * cfg.W * k / KB).astype(int)
    mc = [np.round(f * hw).astype(int) for f in cfg.MARKERS]
    # plain layout: G | R | A | R | G
    plain = np.stack([-(hw + rw), -hw, hw, hw + rw], axis=1)
    # marker layout: G | R | A | M | A | M | A | R | G
    m1a = mc[0] - mw // 2
    m2a = mc[1] - mw // 2
    marked = np.stack([-(hw + rw), -hw, m1a, m1a + mw, m2a, m2a + mw, hw, hw + rw], axis=1)
    kmark = cfg.KMARK
    if not kmark:   # first row from which markers are always >= 64 px apart and from the edges
        gap = np.minimum(np.minimum(m1a - (-(hw + rw)), m2a - m1a), (hw) - (m2a + mw))
        ok = gap >= 64
        kmark = KB + 1
        for kk in range(KB, 0, -1):
            if not ok[kk]:
                break
            kmark = kk
    has_mark = (mw >= 1) & (k >= cfg.KSTRIPE) & (k >= kmark)
    stripes = k >= cfg.KSTRIPE
    return dict(zrow=zrow, hw=hw, rw=rw, mw=mw, plain=plain, marked=marked,
                has_mark=has_mark, stripes=stripes,
                plain_slots=[1, 2, 1, 0], marked_slots=[1, 2, 3, 2, 3, 2, 1, 0])


@dataclass
class Track:
    """Segments: list of (length in world units, curvature ddx as 16.16 px/row^2)."""
    segs: list = field(default_factory=list)

    def seg_at(self, z):
        """(index, end z) of the segment containing world z (track loops)."""
        total = sum(s[0] for s in self.segs)
        z %= total
        acc = 0
        for i, (ln, _) in enumerate(self.segs):
            acc += ln
            if z < acc:
                return i, acc
        return len(self.segs) - 1, total


def geometry(cfg, tabs, track, camz, camx=0):
    """Pass 1. Returns cx[k], phase[k], plus the curvature at the player (bottom row)."""
    KB = cfg.KB
    zrow = tabs["zrow"]
    total = sum(s[0] for s in track.segs)
    base = camz - camz % total
    i, end = track.seg_at(camz + int(zrow[KB]))
    end += base
    if end <= camz + int(zrow[KB]):
        end += total
    ddx = track.segs[i][1]
    x = s32(-camx << 16)
    dx = s32(camx * cfg.RECIP)
    cx = np.zeros(KB + 1, dtype=int)
    phase = np.zeros(KB + 1, dtype=int)
    first = True
    for k in range(KB, 0, -1):
        zw = camz + int(zrow[k])
        while zw >= end:
            i = (i + 1) % len(track.segs)
            end += track.segs[i][0]
            ddx = track.segs[i][1]
        if not first:
            dx = s32(dx + ddx)
            x = s32(x + dx)
        first = False
        cx[k] = min(max((x >> 16) + cfg.W // 2, -cfg.CXL), cfg.W + cfg.CXL)
        phase[k] = (zw >> cfg.STRIPE_SHIFT) & 1
    return cx, phase


def pat_bits(p):
    return np.array([(p >> (31 - b)) & 1 for b in range(32)], dtype=np.uint8)


def render_row(W, bounds, slots, patset):
    """One row: spans of patset[slot] between clamped, sorted boundaries."""
    row = np.zeros(W, dtype=np.uint8)
    reps = W // 32
    prev, slot = 0, 0
    for b, nslot in list(zip(bounds, slots)) + [(W, 0)]:
        b = min(max(b, 0), W)
        if b > prev:
            row[prev:b] = np.tile(pat_bits(patset[slot]), reps)[prev:b]
        prev, slot = max(prev, b), nslot
    return row


def render(cfg, tabs, cx, phase, patterns, sky=None):
    """Pass 2 reference. patterns[phase][parity] = list of 4 longs (grass, rumble, road, marker).
    sky: optional (HY+1) x W uint8 array for rows 0..HY."""
    img = np.zeros((cfg.H, cfg.W), dtype=np.uint8)
    if sky is not None:
        img[:cfg.HY + 1] = sky[:cfg.HY + 1]
    for k in range(1, cfg.KB + 1):
        y = cfg.HY + k
        ph = phase[k] if tabs["stripes"][k] else 0
        ps = patterns[ph][y & 1]
        if ph and tabs["has_mark"][k]:
            offs, slots = tabs["marked"][k], tabs["marked_slots"]
        else:
            offs, slots = tabs["plain"][k], tabs["plain_slots"]
        img[y] = render_row(cfg.W, [int(cx[k] + o) for o in offs], slots, ps)
    return img


def rep(b):
    """Replicate a byte into a long."""
    return (b << 24) | (b << 16) | (b << 8) | b


# ---------------------------------------------------------------------------------------------
# Edge pieces (fast path of road_draw). A plain row whose rumble strips are on screen and at
# least two longs apart is drawn as: grass fill | left piece | road fill | right piece | grass
# fill, where a piece is two longs taken from a table built per pattern set:
#   L[rw][a] = bits [0,a) grass, [a,a+rw) rumble, rest road      (a = b1 & 31)
#   R[rw][a] = bits [0,a) road,  [a,a+rw) rumble, rest grass     (a = b3 & 31)
#   M[mw][a] = bits [0,a) road,  [a,a+mw) marker, rest road      (lane markers)
# Entry (w, a) is at byte offset w*256 + a*8. Pattern set in memory (SETSZ = 32 bytes):
#   4 longs (grass, rumble, road, marker), ptr L, ptr R, ptr M, 0.
SETSZ = 32


def _piece(p0, p1, p2, a, w):
    bits = 0
    for i in range(64):
        p = p0 if i < a else (p1 if i < a + w else p2)
        bits = (bits << 1) | ((p >> (31 - (i & 31))) & 1)
    return bits.to_bytes(8, "big")


def piece_tables(tabs, patset):
    """patset = [grass, rumble, road, marker] longs -> (L, R, M) bytes for one pattern set."""
    G, R, A, M = patset
    rwmax, mwmax = int(max(tabs["rw"])), int(max(tabs["mw"]))
    L = b"".join(_piece(G, R, A, a, w) for w in range(rwmax + 1) for a in range(32))
    Rt = b"".join(_piece(A, R, G, a, w) for w in range(rwmax + 1) for a in range(32))
    Mt = b"".join(_piece(A, M, A, a, w) for w in range(mwmax + 1) for a in range(32))
    return L, Rt, Mt
