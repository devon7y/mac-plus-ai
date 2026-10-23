"""Blockworld column renderer: the bit-exact reference for the 68000 version.

Integer arithmetic only. The screen is NC columns of COLW pixels by H rows. For each column a
2D ray is marched across the x/z grid (DDA, perpendicular distances in 8.8 blocks). Every cell it
enters contributes the faces of its vertical runs (segments) as vertical pieces:

    top face     [Y(top, far),  Y(top, near))   when the eye is above the run and air is above it
    side face    [Y(top, near), Y(bot, near))   (not for the eye's own cell)
    bottom face  [Y(bot, near), Y(bot, far))    when the eye is below the run and air is below it

with Y(h, d) = H0 - (((h << 8) - eye) * RECIP[d] >> RSH[d]): for d < DSPLIT (2 blocks),
RECIP[d] = (F << 12) // max(d, DMIN) and RSH = 12; from DSPLIT on, RECIP[d] = (F << 16) // d and
RSH = 16 (on the 68000 a swap instead of a 12-bit shift).
A piece only paints rows of the column that are still uncovered (front to back), so nearer
cells hide farther ones. Within a cell: the segment across the eye level first (its side face
is nearest), then the segments above the eye bottom-up, then those below it top-down. Rows still uncovered at the end get the sky / haze / fog background.
Extra 1-row pieces (painted just before their face, so they win) draw block edges: the far edge
of a top or bottom face that is at least LMIN rows tall, and the top edge of a side face above
the eye. Fog: level 1 for cells starting at least DMAX - 3 blocks away. A turf side face starts with a quarter-block grass band.

Beyond the view distance (if rows are still uncovered), NFAR samples every FARSTEP blocks along
the ray (positions stepped from the one at DMAX) read a coarse map of the highest surface per 4x4 blocks (world.coarse_heights; outside
the world the sea) and paint distant hills: [Y(height, d), H) in two light tones.

Each painted row also gets a 16-bit surface key (0 for the background and for pieces at fog
level >= EDGEFOG (1); otherwise the face's plane: 0x1000 | 2 * top height, 0x2000 | 2 * bottom height,
0x3000 | x plane + 32, 0x4000 | z plane + 32). Vertical edges: the leftmost pixel of column c
(c >= 1) is black on every row where its key differs from column c - 1's.

Pixels: bit 7 - (x & 7) of PAT[id][y & 7] for the column's pattern id (see patterns.py), then
the vertical edges. The order of pieces within one cell doesn't matter (they never overlap);
across cells it is front to back.
"""
import math

import numpy as np

import patterns as PT
import world as W

SIN = [int(round(16384 * math.sin(2 * math.pi * i / 1024))) for i in range(1024)]   # Q14
COS = [SIN[(i + 256) & 1023] for i in range(1024)]

NOFACE, XFACE, ZFACE = 0, 1, 2
KTOP, KBOT, KX, KZ = 0x1000, 0x2000, 0x3000, 0x4000
DSPLIT = 513             # projection switches from a 12-bit to a 16-bit shift here


class Cfg:
    def __init__(self, nc=96, colw=4, h=288, dmax=16, f=256, dmin=40, lines=1, lmin=3,
                 haze=24, fog=None, edges=1, edgefog=1, nfar=8, farstep=4):
        self.NC, self.COLW, self.H = nc, colw, h
        self.W = nc * colw
        self.F = f
        self.DMAX = dmax * 256
        self.DMIN = dmin
        self.LINES = lines          # 0 none, 1 far edges of top/bottom faces + wall tops
        self.LMIN = lmin            # no edge line on a face smaller than this (rows)
        self.HAZEH = haze           # rows of haze above the horizon
        self.EDGES = edges          # vertical edges between different surfaces
        self.EDGEFOG = edgefog      # no vertical edges from this fog level on
        self.NFAR, self.FARSTEP = nfar, farstep * 256      # distant hills: samples, spacing (8.8)
        self.PW = (self.W * 8192) // f                      # camera plane half-width, Q14
        self.CAMX = [((2 * c + 1 - nc) * 16384) // nc for c in range(nc)]
        self.DSPLIT = DSPLIT
        dlast = self.DMAX + nfar * self.FARSTEP
        self.RECIP = [((f << 12) // max(d, dmin)) if d < DSPLIT else ((f << 16) // d) for d in range(dlast + 1)]
        self.RSH = [12 if d < DSPLIT else 16 for d in range(dlast + 1)]
        assert max(self.RECIP) < 32768
        if fog is None:
            fog = (dmax - 3,)                               # 2 levels: far for the last 3 blocks
        self.FOG = [sum(i >= b for b in fog) for i in range(dmax + 1)]   # by whole blocks


class Cam:
    def __init__(self, x, y, z, yaw, pitch=0):
        self.x, self.y, self.z = x, y, z      # 8.8 blocks; y is the eye height
        self.yaw = yaw & 1023                 # 1024 per turn; 0 looks along +x, 256 along +z
        self.pitch = pitch                    # horizon shift in rows (y-shear); + looks up


def make_seg_lookup(segs):
    def at(x, z):
        if 0 <= x < W.WX and 0 <= z < W.WX:
            return segs[x * W.WX + z]
        return W.OCEAN
    return at


def render_cols(seg_at, cam, cfg, stats=None, farh=None, shade=(0, 0, 0, 0)):
    """Return (ids, keys): ids[c][y] pattern ids, keys[c][y] surface keys.
    shade[k]: the dither OR'ed into the fog field (0 none, 2 dim, 3 dark) for light class k (0 open
    sky, 1 under a tree, 2 covered, 3 torch-lit). A top face takes its segment's class; a side the
    light of the cell in front of it (class 0 if the segment's top is above that cell's first top,
    else 1 if that first segment is leaves or a log, else 2; a torch-lit segment stays 3); a bottom
    face the class of the next segment down (2 if none)."""
    H, NC = cfg.H, cfg.NC
    H0 = H // 2 + cam.pitch
    RECIP, RSH, DMAX, FOG = cfg.RECIP, cfg.RSH, cfg.DMAX, cfg.FOG
    LINES, LMIN, EDGEFOG = cfg.LINES, cfg.LMIN, cfg.EDGEFOG
    hrel = [(h << 8) - cam.y for h in range(W.WY + 1)]
    dirx, dirz = COS[cam.yaw], SIN[cam.yaw]
    plx = (-SIN[cam.yaw] * cfg.PW) >> 14
    plz = (COS[cam.yaw] * cfg.PW) >> 14
    mx0, mz0 = cam.x >> 8, cam.z >> 8
    fx, fz = cam.x & 255, cam.z & 255
    ids, keys = [], []
    ncells = npieces = 0
    for c in range(NC):
        rdx = dirx + ((plx * cfg.CAMX[c]) >> 14)
        rdz = dirz + ((plz * cfg.CAMX[c]) >> 14)
        adx, adz = abs(rdx), abs(rdz)
        ddx = 0x7FFF if adx < 128 else min(0x7FFF, (1 << 22) // adx)
        ddz = 0x7FFF if adz < 128 else min(0x7FFF, (1 << 22) // adz)
        if rdx > 0:
            sx, sdx = 1, ((256 - fx) * ddx) >> 8
        else:
            sx, sdx = -1, (fx * ddx) >> 8
        if rdz > 0:
            sz, sdz = 1, ((256 - fz) * ddz) >> 8
        else:
            sz, sdz = -1, (fz * ddz) >> 8
        col = [-1] * H
        kcol = [0] * H
        left = [H]

        def paint(y0, y1, pid, key):
            if y0 < 0:
                y0 = 0
            if y1 > H:
                y1 = H
            for y in range(y0, y1):
                if col[y] < 0:
                    col[y] = pid
                    kcol[y] = key
                    left[0] -= 1

        mx, mz = mx0, mz0
        dn = 0
        face = NOFACE
        rn, sn = RECIP[0], RSH[0]
        xplane = KX + 32 + (sx < 0)          # x plane of an x face entered at cell mx: + mx
        zplane = KZ + 32 + (sz < 0)
        psky, pcov = 0, 2                    # the previous cell: open sky from its first top up;
                                             # the shade below that (1 under a tree, else 2)
        while True:
            if sdx < sdz:
                df, nxt = sdx, XFACE
            else:
                df, nxt = sdz, ZFACE
            last = df >= DMAX
            if last:
                df = DMAX
            rf, sf = RECIP[df], RSH[df]
            fog = FOG[dn >> 8]
            var = ((mx ^ mz) & 1) << 4
            keyon = fog < EDGEFOG
            ncells += 1
            segs = seg_at(mx, mz)
            ns = len(segs)
            # within a cell, nearer pieces first: the segment across the eye level (its side is
            # at the near edge), then the ones above the eye bottom-up, then the rest top-down
            above = [i for i in range(ns) if hrel[segs[i][1]] > 0]
            rest = [i for i in range(ns) if hrel[segs[i][1]] <= 0]
            order = rest[:1] + above[::-1] + rest[1:] if rest and hrel[segs[rest[0]][0]] > 0 else above[::-1] + rest
            for i in order:
                top, bot, t, fl, cls = segs[i]
                tr, br = hrel[top], hrel[bot]
                tbase = t << 5
                if tr < 0 and fl & W.TOPEXP:                 # top face (air above): its own light
                    sh = fog | shade[cls]
                    k = (KTOP | (top << 1)) if keyon else 0
                    yf = H0 - ((tr * rf) >> sf)
                    yn = H0 - ((tr * rn) >> sn)
                    if LINES and yn - yf >= LMIN:
                        paint(yf, yf + 1, PT.LINE + sh, k)
                    paint(yf, yn, tbase + var + sh, k)
                    npieces += 1
                if face:                                     # side face: the light in front of it
                    sh = fog | shade[3 if cls == 3 else (0 if top > psky else pcov)]
                    if face == XFACE:
                        k = (xplane + mx) if keyon else 0
                        sid = tbase + var + sh + 4
                    else:
                        k = (zplane + mz) if keyon else 0
                        sid = tbase + var + sh + 8
                    y0 = H0 - ((tr * rn) >> sn)
                    y1 = H0 - ((br * rn) >> sn)
                    if LINES and tr > 0:
                        paint(y0, y0 + 1, PT.LINE + sh, k)
                    if t == W.TURF:
                        yb = H0 - (((tr - 64) * rn) >> sn)
                        paint(y0, yb, (PT.TURFBAND_B if var else PT.TURFBAND) + sh, k)
                    paint(y0, y1, sid, k)
                    npieces += 1
                if br > 0 and fl & W.BOTEXP:                 # bottom face: the light of the air below
                    sh = fog | shade[segs[i + 1][4] if i + 1 < ns else 2]
                    k = (KBOT | (bot << 1)) if keyon else 0
                    yn = H0 - ((br * rn) >> sn)
                    yf = H0 - ((br * rf) >> sf)
                    if LINES and yf - yn >= LMIN:
                        paint(yf - 1, yf, PT.LINE + sh, k)
                    paint(yn, yf, tbase + var + sh + 12, k)
                    npieces += 1
            psky = segs[0][0] if segs else 0
            pcov = 1 if segs and segs[0][2] in (W.LEAVES, W.LOG) else 2
            if last or left[0] == 0:
                break
            if nxt == XFACE:
                mx += sx
                sdx += ddx
            else:
                mz += sz
                sdz += ddz
            face = nxt
            dn, rn, sn = df, rf, sf
        if farh is not None and left[0]:                     # distant hills (coarse heights)
            px = cam.x + ((rdx * cfg.DMAX) >> 14)            # stepped: x = start + k * step
            pz = cam.z + ((rdz * cfg.DMAX) >> 14)
            stx, stz = (rdx * cfg.FARSTEP) >> 14, (rdz * cfg.FARSTEP) >> 14
            for k in range(1, cfg.NFAR + 1):
                d = cfg.DMAX + k * cfg.FARSTEP
                px += stx
                pz += stz
                hx, hz = px >> 10, pz >> 10
                y = H0 - (((farh(hx, hz) << 8) - cam.y) * RECIP[d] >> RSH[d])
                paint(y, y + 1, PT.FARLINE, 0)                # a dotted silhouette edge
                paint(y, H, PT.FARHILL + (k > cfg.NFAR // 2), 0)
        for y in range(H):                                   # background
            if col[y] < 0:
                col[y] = PT.SKY if y < H0 - cfg.HAZEH else (PT.HAZE if y < H0 else PT.FOGBG)
        ids.append(col)
        keys.append(kcol)
    if stats is not None:
        stats["cells"] = ncells
        stats["pieces"] = npieces
    return ids, keys


def to_bitmap(ids, keys, cfg, pat=PT.PAT):
    """H x W array of 0/1 (1 = black)."""
    H, W_ = cfg.H, cfg.W
    a = np.array(ids, dtype=np.int32).T                    # H x NC
    patarr = np.array(pat, dtype=np.uint8)                  # NIDS x 8
    rowbytes = patarr[a, (np.arange(H) & 7)[:, None]]       # H x NC
    xs = np.arange(W_)
    bits = ((rowbytes[:, xs // cfg.COLW] >> (7 - (xs & 7))) & 1).astype(np.uint8)
    if cfg.EDGES:
        k = np.array(keys, dtype=np.int32).T
        ys, cs = np.nonzero(k[:, 1:] != k[:, :-1])
        bits[ys, (cs + 1) * cfg.COLW] = 1
    return bits


def to_bitmap_moving(ids, keys, cfg, pat=PT.PAT, step=2):
    """Moving frames: only the even columns, each drawn 8 px wide (its pattern byte), the leftmost
    pixel black where its key differs from the previous even column's. step 4 (the Full size):
    columns 0, 4, 8, ... drawn 16 px wide (the pattern byte twice)."""
    import copy
    c8 = copy.copy(cfg)
    c8.COLW = 4 * step
    return to_bitmap(ids[::step], keys[::step], c8, pat)


def render(seg_at, cam, cfg, stats=None, farh=None, moving=False, shade=(0, 0, 0, 0)):
    """moving: 0 still, 1 (True) moving (8 px columns), 2 moving at the Full size (16 px)."""
    ids, keys = render_cols(seg_at, cam, cfg, stats, farh, shade)
    if moving:
        return to_bitmap_moving(ids, keys, cfg, step=4 if moving == 2 else 2)
    return to_bitmap(ids, keys, cfg)
