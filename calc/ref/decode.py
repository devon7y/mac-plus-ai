#!/usr/bin/env python3
"""Scribble Calc: stroke grouping (segmentation), the reference line height, and grammar decoding.
This file is the specification for app/recog.c; both use integers only.

Segmentation (strokes in time order; each stroke joins at most one earlier group, except that a
bar may also swallow dot groups):
    bbox per stroke; href = reference height (below); tiny = max(w, h) <= max(3, href // 4)
    flat  = not tiny and w >= 2 * h (a bar: '-', the strokes of '=', the bar of a divide sign)
    tall  = neither (digits, parentheses, the strokes of '+' and 'x' ...)
    A new stroke s joins group g when (tol = 2 px; ov = x overlap; "at the top of" = the bar's
    middle y lies in the top quarter of the tall thing):
      s tiny,  g has a bar but no tall stroke : s.cx within the bar span (+-tol) and
                                  2 * |s.cy - bar cy| <= href (the dots of a divide sign)
      s tiny,  g tiny           : |s.cx - g.cx| <= max(href // 6, 3)
      s tiny,  g has a tall stroke : s's centre lies inside g's box (+-0) and in its top 3/4 (a stray dot)
      s flat,  g tiny           : g.cx within s's span (+-tol) and 2 * |g.cy - s.cy| <= href
                                  (dots written before the bar)
      s flat,  g not tiny       : s.cx within g's span (+-tol), or g has a tall stroke, s is at the
                                  top of g and s comes near a stroke of g (the bar of a 5)
      s tall,  g tiny           : g's centre lies inside s's box and in its top 3/4
      s tall,  g has no tall stroke : s.cx within the middle 70% of g's span, or g's bars are at the
                                  top of s and a stroke of g comes near s
      s tall,  g tall           : 10 * ov >= 7 * min(s.w, g.w), or |s.cx - g.cx| * 20 <= 7 * max(s.w, g.w) + 20*tol,
                                  or 4 * ov >= min(s.w, g.w) and s crosses or touches a stroke of g
      otherwise                 : no
    Among candidate groups the one with the most x overlap (ties: the latest) wins; a flat stroke
    that joins (or starts) a group without tall strokes also absorbs every other tiny group whose
    cx lies within its span and whose cy is within href / 2 of the stroke's cy. A group holds at most GRPSTK strokes (a full group is no candidate
    and absorbs nothing more).
    "near": a point of one stroke within R = max(3, href // 10) of a point of the other, or the two
    polylines cross. Groups are read left to right by their bbox centre x.
href: the median height of strokes taller than 8 px (the upper median), at least 16; 32 if none.
minD for a group's raster = href // 2.

Decoding: Viterbi over the grammar (app/dec.h tokens). States:
    0 START (operand expected), 1 UNARY (after a unary minus), 2 INT, 3 DOT (a '.' with no digits
    yet), 4 FRAC, 5 CLOSE (after ')'), 6 END (after '='), 7 TAIL (anything written after the '=',
    PEN_TAIL per symbol; it is not part of the expression); paren depth 0..MAXD.
Symbol scores are the classifier's scores (1/8 nat). An '=' with open parentheses closes them
automatically at a cost of PEN_AUTOCLOSE each. When the last symbol is not '=', any state may end
the line (the user is still writing).
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from strokes import CLASSES, raster   # noqa: E402

MAXD = 8
GRPSTK = 12                 # strokes per group at most
PEN_AUTOCLOSE = 24          # 3 nats (scores are 1/8 nat) per parenthesis closed by "="
PEN_TAIL = 32               # 4 nats per symbol after the "="
NEG = -10 ** 9
T_PLUS, T_MINUS, T_TIMES, T_DIV, T_EQ, T_LPAR, T_RPAR, T_POINT = 10, 11, 12, 13, 14, 15, 16, 17
START, UNARY, INT, DOT, FRAC, CLOSE, END, TAIL = range(8)
NST = 8


def bbox(s):
    return int(s[:, 0].min()), int(s[:, 1].min()), int(s[:, 0].max()), int(s[:, 1].max())


def ref_height(strokes):
    hs = sorted(int(s[:, 1].max() - s[:, 1].min()) for s in strokes if s[:, 1].max() - s[:, 1].min() > 8)
    if not hs:
        return 32
    return max(16, hs[len(hs) // 2])


def orient(ax, ay, bx, by, cx, cy):
    v = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
    return (v > 0) - (v < 0)


def seg_touch(a, b, c, d):
    """Do segments ab and cd intersect (touching counts)? Integer orientation tests."""
    if max(a[0], b[0]) < min(c[0], d[0]) or max(c[0], d[0]) < min(a[0], b[0]) or \
            max(a[1], b[1]) < min(c[1], d[1]) or max(c[1], d[1]) < min(a[1], b[1]):
        return False
    o1, o2 = orient(*a, *b, *c), orient(*a, *b, *d)
    o3, o4 = orient(*c, *d, *a), orient(*c, *d, *b)
    if o1 * o2 <= 0 and o3 * o4 <= 0:
        if o1 == 0 and o2 == 0:                  # collinear: the boxes overlap (checked above)
            return True
        return True
    return False


def crosses(s, t):
    """Does polyline s cross or touch polyline t (one-point strokes are degenerate segments)?"""
    sa = [(int(p[0]), int(p[1])) for p in s]
    ta = [(int(p[0]), int(p[1])) for p in t]
    if len(sa) == 1:
        sa = sa * 2
    if len(ta) == 1:
        ta = ta * 2
    for i in range(len(sa) - 1):
        for j in range(len(ta) - 1):
            if seg_touch(sa[i], sa[i + 1], ta[j], ta[j + 1]):
                return True
    return False


def near(s, t, r):
    """A point of s within r of a point of t, or the polylines cross."""
    a = np.asarray(s, np.int64)
    b = np.asarray(t, np.int64)
    d = ((a[:, None, :] - b[None, :, :]) ** 2).sum(2)
    return bool((d <= r * r).any()) or crosses(s, t)


def segment(strokes):
    """strokes: list of int arrays (n, 2) in time order. Returns (groups sorted by x, href);
    a group is a sorted list of stroke indices."""
    href = ref_height(strokes)
    tiny_t = max(3, href // 4)
    near_r = max(3, href // 10)
    tol = 2
    info = []
    for s in strokes:
        x0, y0, x1, y1 = bbox(s)
        w, h = x1 - x0, y1 - y0
        tiny = max(w, h) <= tiny_t
        flat = (not tiny) and w >= 2 * h
        info.append(dict(x0=x0, x1=x1, y0=y0, y1=y1, w=w, h=h, cx2=x0 + x1, tiny=tiny, flat=flat))
    groups = []
    for i, si in enumerate(info):
        cands = []
        cx = si["cx2"] // 2
        for gi, g in enumerate(groups):
            if g is None or len(g["strokes"]) >= GRPSTK:
                continue
            gw = g["x1"] - g["x0"]
            ov = min(si["x1"], g["x1"]) - max(si["x0"], g["x0"])
            ok = False
            if si["tiny"]:
                if g["bar"] is not None and not g["hastall"]:
                    ok = g["bar"][0] - tol <= cx <= g["bar"][1] + tol and \
                        2 * abs((si["y0"] + si["y1"]) // 2 - (g["bary"][0] + g["bary"][1]) // 2) <= href
                elif g["tiny"]:
                    ok = abs(si["cx2"] - g["cx2"]) <= 2 * max(href // 6, 3)
                elif g["hastall"]:
                    cy = (si["y0"] + si["y1"]) // 2
                    ok = g["x0"] <= cx <= g["x1"] and g["y0"] <= cy and 4 * (cy - g["y0"]) <= 3 * (g["y1"] - g["y0"])
            elif si["flat"]:
                if g["tiny"]:
                    ok = si["x0"] - tol <= g["cx2"] // 2 <= si["x1"] + tol and \
                        2 * abs((g["y0"] + g["y1"]) // 2 - (si["y0"] + si["y1"]) // 2) <= href
                else:
                    ok = g["x0"] - tol <= cx <= g["x1"] + tol
                    if not ok and g["hastall"] and 4 * ((si["y0"] + si["y1"]) // 2 - g["y0"]) <= g["y1"] - g["y0"]:
                        ok = any(near(strokes[i], strokes[k], near_r) for k in g["strokes"])
            else:
                if g["tiny"]:
                    gx, gy = g["cx2"] // 2, (g["y0"] + g["y1"]) // 2
                    ok = si["x0"] <= gx <= si["x1"] and si["y0"] <= gy and 4 * (gy - si["y0"]) <= 3 * si["h"]
                elif not g["hastall"]:
                    ok = 20 * (g["x0"] - cx) <= -3 * gw and 20 * (cx - g["x1"]) <= -3 * gw
                    if not ok and g["bar"] is not None and 4 * ((g["bary"][0] + g["bary"][1]) // 2 - si["y0"]) <= si["h"]:
                        ok = any(near(strokes[i], strokes[k], near_r) for k in g["strokes"])
                else:
                    ok = 10 * ov >= 7 * min(si["w"], gw) or \
                        20 * abs(si["cx2"] - g["cx2"]) <= 2 * (7 * max(si["w"], gw) + 20 * tol)
                    if not ok and 4 * ov >= min(si["w"], gw):
                        ok = any(crosses(strokes[i], strokes[k]) for k in g["strokes"])
            if ok:
                cands.append((ov, gi))
        if cands:
            best = max(ov for ov, k in cands)
            gi = max(k for ov, k in cands if ov == best)
            g = groups[gi]
            add_stroke(g, i, si)
        else:
            g = dict(strokes=[], x0=si["x0"], x1=si["x1"], y0=si["y0"], y1=si["y1"], bar=None, bary=None,
                     tiny=True, hastall=False, cx2=si["cx2"])
            add_stroke(g, i, si)
            groups.append(g)
            gi = len(groups) - 1
        if si["flat"] and not g["hastall"]:
            for gj, g2 in enumerate(groups):
                if gj != gi and g2 is not None and g2["tiny"] and si["x0"] - tol <= g2["cx2"] // 2 <= si["x1"] + tol \
                        and 2 * abs((g2["y0"] + g2["y1"]) // 2 - (si["y0"] + si["y1"]) // 2) <= href \
                        and len(g["strokes"]) + len(g2["strokes"]) <= GRPSTK:
                    for k in g2["strokes"]:
                        add_stroke(g, k, info[k])
                    groups[gj] = None
    gs = [g for g in groups if g is not None]
    gs.sort(key=lambda g: (g["x0"] + g["x1"], min(g["strokes"])))
    return [sorted(g["strokes"]) for g in gs], href


def add_stroke(g, i, si):
    g["strokes"].append(i)
    g["x0"], g["x1"] = min(g["x0"], si["x0"]), max(g["x1"], si["x1"])
    g["y0"], g["y1"] = min(g["y0"], si["y0"]), max(g["y1"], si["y1"])
    g["cx2"] = g["x0"] + g["x1"]
    g["tiny"] = g["tiny"] and si["tiny"]
    if si["flat"]:
        if g["bar"] is None:
            g["bar"], g["bary"] = (si["x0"], si["x1"]), (si["y0"], si["y1"])
        else:
            g["bar"] = (min(g["bar"][0], si["x0"]), max(g["bar"][1], si["x1"]))
            g["bary"] = (min(g["bary"][0], si["y0"]), max(g["bary"][1], si["y1"]))
    if not si["tiny"] and not si["flat"]:
        g["hastall"] = True


def raised_flags(strokes, groups, href):
    """Which groups may be superscripts (an exponent); the decoder decides. Groups in reading order.
    The anchor is the nearest earlier group that is not allowed raised, not tiny, not flat
    (w < 2h) and at least href * 3 / 5 high. Against it (a = anchor box, ah its height), a group
    "looks raised" when it is not tiny or flat, 5h <= 4 ah, 5 (y1 - a.y0) <= 3 ah and
    5 (y0 - a.y0) <= ah. Group i is allowed raised when
      - the group just before it is the anchor, it looks raised and 2 (x0 - a.x1) <= ah, or
      - the group just before it is allowed raised, 2 (x0 - prev.x1) <= ah, and it looks raised or
        it is tiny or flat with 3 (cy - a.y0) <= ah (a '.' or '-' inside the exponent)."""
    tiny_t = max(3, href // 4)
    box = []
    for gr in groups:
        pts = np.concatenate([strokes[k] for k in gr])
        box.append((int(pts[:, 0].min()), int(pts[:, 1].min()), int(pts[:, 0].max()), int(pts[:, 1].max())))
    out = [False] * len(groups)
    anchor = -1
    for i, (x0, y0, x1, y1) in enumerate(box):
        w, h = x1 - x0, y1 - y0
        tiny = max(w, h) <= tiny_t
        flat = not tiny and w >= 2 * h
        if anchor >= 0 and i > 0:
            a0, ay0, a1, ay1 = box[anchor]
            ah = ay1 - ay0
            looks = (not tiny and not flat and 5 * h <= 4 * ah and 5 * (y1 - ay0) <= 3 * ah and 5 * (y0 - ay0) <= ah)
            if anchor == i - 1:
                out[i] = looks and 2 * (x0 - a1) <= ah
            elif out[i - 1]:
                near_prev = 2 * (x0 - box[i - 1][2]) <= ah
                out[i] = near_prev and (looks or ((tiny or flat) and 3 * ((y0 + y1) // 2 - ay0) <= ah))
        if not out[i] and not tiny and not flat and h * 5 >= href * 3:
            anchor = i
    return out


# ---------------------------------------------------------------- grammar decoding

def step(state, depth, c):
    """Grammar transition: returns (state, depth, penalty) or None."""
    if state in (END, TAIL):
        return TAIL, 0, PEN_TAIL
    if c <= 9:
        if state in (START, UNARY, INT):
            return INT, depth, 0
        if state in (DOT, FRAC):
            return FRAC, depth, 0
        return None
    if c == T_POINT:
        if state in (START, UNARY):
            return DOT, depth, 0
        if state == INT:
            return FRAC, depth, 0
        return None
    if c in (T_PLUS, T_TIMES, T_DIV):
        return (START, depth, 0) if state in (INT, FRAC, CLOSE) else None
    if c == T_MINUS:
        if state in (INT, FRAC, CLOSE):
            return START, depth, 0
        if state == START:
            return UNARY, depth, 0
        return None
    if c == T_LPAR:
        if depth >= MAXD:
            return None
        if state in (START, UNARY, INT, FRAC, CLOSE):
            return START, depth + 1, 0
        return None
    if c == T_RPAR:
        return (CLOSE, depth - 1, 0) if state in (INT, FRAC, CLOSE) and depth > 0 else None
    if c == T_EQ:
        return (END, 0, PEN_AUTOCLOSE * depth) if state in (INT, FRAC, CLOSE) else None
    return None


RAISE_BONUS = 16           # 2 nats for reading an allowed symbol as a superscript


def viterbi(scores, forced=None, allow=None):
    """scores: (n, 18) ints; forced: dict symbol index -> class; allow: per symbol, True if its
    shape and place allow a superscript (raised_flags). The search state is (grammar state, paren
    depth, inside an exponent). A symbol read as a superscript gets RAISE_BONUS. Entering an exponent
    needs a finished operand (INT, FRAC, CLOSE) and restarts at START; inside it '(' ')' '=' are
    not allowed; leaving it needs a finished operand inside and continues at CLOSE. A line may end
    inside an exponent. Returns (classes, raised flags, total); ties keep the lower (state, choice)."""
    n = len(scores)
    if n == 0:
        return [], [], 0
    D = MAXD + 1
    S = NST * D * 2
    best = np.full(S, NEG, np.int64)
    best[START * D] = 0
    back = []
    for i in range(n):
        nb = np.full(S, NEG, np.int64)
        bp = np.full((S, 2), -1, np.int64)
        classes = [forced[i]] if forced and i in forced else range(len(CLASSES))
        modes = (0, 1) if allow is not None and allow[i] else (0,)
        for st in range(S):
            if best[st] == NEG:
                continue
            e_prev, rest = divmod(st, NST * D)
            state0, depth = divmod(rest, D)
            for e in modes:
                state = state0
                if e != e_prev and state not in (END, TAIL):
                    if state not in (INT, FRAC, CLOSE):
                        continue
                    state = START if e else CLOSE
                for c in classes:
                    if e and c in (T_LPAR, T_RPAR, T_EQ):
                        continue
                    r = step(state, depth, c)
                    if r is None:
                        continue
                    ns, nd, pen = r
                    ee = 0 if ns in (END, TAIL) else e
                    k = ee * NST * D + ns * D + nd
                    v = best[st] + int(scores[i][c]) - pen + (RAISE_BONUS if e else 0)
                    if v > nb[k]:
                        nb[k] = v
                        bp[k] = (st, c + 18 * e)
        best = nb
        back.append(bp)
    k = int(np.argmax(best))          # any final state (the line may still be in progress)
    total = int(best[k])
    if total <= NEG // 2:             # no valid reading (e.g. a forced symbol breaks the grammar)
        return ([forced[i] if forced and i in forced else int(np.argmax(scores[i])) for i in range(n)],
                [False] * n, NEG)
    out, ra = [], []
    for i in range(n - 1, -1, -1):
        st, ce = back[i][k]
        out.append(int(ce) % 18)
        ra.append(int(ce) >= 18)
        k = int(st)
    return out[::-1], ra[::-1], total


def group_raster(strokes, group, href):
    return raster([strokes[k] for k in group], href // 2)
