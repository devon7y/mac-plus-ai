#!/usr/bin/env python3
"""Dream network architectures: a config -> a flat layer program that the Python integer
reference (ref.py), the Mac engine and the training code all follow.

The network is a small U-Net working on space-to-depth patches: a 64x64 picture becomes a
16x16 grid of 4x4 patches (16 channels), a 32x32 picture an 8x8 grid. Levels halve the grid
(2x2 space-to-depth + 1x1 conv) and widen the channels. Every block is

    x = LIN(dw3x3(h))           depthwise 3x3, ternary taps, per-channel shift + bias
    y = RELU(pw_e(x))           1x1 expand C -> E, ternary
    h = RES(h + pw_p(y))        1x1 project E -> C, ternary, added into the residual stream

Every layer ends in the same requantisation ("RQ"): v = (acc >> k_c) + b_c, then a 64 KB
table maps v to the stored value (ReLU clamp, signed clamp, residual clamp), see ref.py.
The bias b_c carries the subject and noise-level conditioning (folded once per step).

Tensors are int16, channels-last, with a one-pixel zero border: storage (H+2) x (W+2) x C.
"""
import json
import math
import sys

# ----------------------------------------------------------------------------- configs
# levels: list of (grid side, channels, expansion width, encoder blocks, decoder blocks);
# the last level is the middle (its blocks run once, enc + dec counted together).
CONFIGS = {
    # 64x64, four levels down to 2x2: the "large" candidate (about 1M parameters).
    "A64L": dict(img=64, patch=4, levels=[(16, 32, 64, 1, 1), (8, 64, 128, 2, 2), (4, 128, 256, 2, 2),
                                          (2, 256, 512, 2, 0)], relu_max=127),
    # The M1 pick (approved 2026-10-16): A64L with the 2x2 level at 192/384 (922K params, ~2.0 MB RAM).
    "A64L2": dict(img=64, patch=4, levels=[(16, 32, 64, 1, 1), (8, 64, 128, 2, 2), (4, 128, 256, 2, 2),
                                           (2, 192, 384, 2, 0)], relu_max=127),
    # A64L2 with 4-bit in/out layers (input at Q6) and two-term gains (pilot QAT ablation, 2026-10-17).
    "A64L2Q": dict(img=64, patch=4, levels=[(16, 32, 64, 1, 1), (8, 64, 128, 2, 2), (4, 128, 256, 2, 2),
                                            (2, 192, 384, 2, 0)], relu_max=127, io_q4=True, gain2=True),
    # Capacity fallback: A64L levels (2x2 at 256/512) with 4-bit in/out and two-term gains.
    "A64LQ": dict(img=64, patch=4, levels=[(16, 32, 64, 1, 1), (8, 64, 128, 2, 2), (4, 128, 256, 2, 2),
                                           (2, 256, 512, 2, 0)], relu_max=127, io_q4=True, gain2=True),
    # 64x64, three levels down to 4x4: the "medium" candidate (fallback).
    "A64M": dict(img=64, patch=4, levels=[(16, 32, 64, 1, 1), (8, 64, 128, 2, 2), (4, 128, 256, 3, 0)],
                 relu_max=127),
    # 64x64, small: fewer blocks.
    "A64S": dict(img=64, patch=4, levels=[(16, 24, 48, 1, 1), (8, 48, 96, 1, 1), (4, 96, 192, 2, 0)],
                 relu_max=127),
    # A64M with 4-bit (-7..7) weights in the 1x1 convs: the ternary-vs-4-bit comparison.
    "A64M_q4": dict(img=64, patch=4, levels=[(16, 32, 64, 1, 1), (8, 64, 128, 2, 2), (4, 128, 256, 3, 0)],
                    relu_max=127, wq="q4"),
    # 32x32 for the 1 MB Plus (W4 table kernels; ~300K params).
    "A32S": dict(img=32, patch=4, levels=[(8, 32, 64, 1, 1), (4, 64, 128, 2, 2), (2, 128, 256, 2, 0)],
                 relu_max=127),
    "A32SQ": dict(img=32, patch=4, levels=[(8, 32, 64, 1, 1), (4, 64, 128, 2, 2), (2, 128, 256, 2, 0)],
                  relu_max=127, io_q4=True, gain2=True),
    # 32x32, larger (does not fit a 1 MB Plus with JIT code).
    "A32": dict(img=32, patch=4, levels=[(8, 48, 96, 1, 1), (4, 96, 192, 2, 2), (2, 192, 384, 2, 0)],
                relu_max=127),
}

RES_MAX = 3640          # residual stream clamp: 9 * 3640 < 32768, so the dw sums never wrap
LIN_MAX = 127           # signed activations (dw output, level inputs)
IN_MAX = 2047           # x_t in Q9 (|x_t| < 4): 16 * 2047 < 32768
IN_SHIFT_Q4 = 3         # io_q4 configs: the network input is x_t >> 3 (Q6), clamp +-255,
IN_MAX_Q4 = 255         # so a 4-bit input layer stays exact: 16 * 7 * 255 < 32768


def in_shift(cfg):
    c = CONFIGS[cfg] if isinstance(cfg, str) else cfg
    return (IN_SHIFT_Q4, IN_MAX_Q4) if c.get("io_q4") else (0, IN_MAX)


def relu_max_for(fan_in, relu_max=127):
    """Largest ReLU clamp that keeps a ternary sum over fan_in inputs inside int16."""
    return min(relu_max, 32767 // fan_in)


def build(name_or_cfg):
    """Return (tensors, layers) for a config. tensors: name -> [H, W, C]. layers: list of dicts."""
    cfg = CONFIGS[name_or_cfg] if isinstance(name_or_cfg, str) else name_or_cfg
    img, patch = cfg["img"], cfg["patch"]
    levels = cfg["levels"]
    g0 = img // patch
    assert levels[0][0] == g0
    T, L = {}, []

    def tensor(name, g, c):
        T[name] = [g, g, c]
        return name

    def pw(src, dst, cin, cout, grid, mode, gather="plain", scatter="plain", tab=None, tag=""):
        L.append(dict(kind="pw", src=src, dst=dst, cin=cin, cout=cout, grid=grid, mode=mode,
                      gather=gather, scatter=scatter, tab=tab or mode, tag=tag, wq=cfg.get("wq", "tern")))

    def dw(src, dst, c, grid, mode, center_only=False, tag="", tab=None):
        L.append(dict(kind="dw", src=src, dst=dst, cin=c, cout=c, grid=grid, mode=mode,
                      center=center_only, tab=tab or mode, tag=tag))

    def block(lv, g, c, e, tag):
        h, x, y = f"h{lv}", f"x{lv}", f"y{lv}"
        dw(h, x, c, g, "lin", tag=tag + ".dw")
        rm = relu_max_for(e, cfg.get("relu_max", 127))
        pw(x, y, c, e, g, "relu", tab=f"relu{rm}", tag=tag + ".pe")
        pw(y, h, e, c, g, "addh", tab="res", tag=tag + ".pp")

    tensor("xt", g0, patch * patch)
    for lv, (g, c, e, ne, nd) in enumerate(levels):
        tensor(f"h{lv}", g, c)
        tensor(f"x{lv}", g, c)
        tensor(f"y{lv}", g, e)
    tensor("out", g0, patch * patch)

    # encoder
    g, c, e, ne, nd = levels[0]
    pw("xt", "h0", patch * patch, c, g, "seth", tab="res", tag="in")
    if cfg.get("io_q4"):
        L[-1]["wq"] = "q4"
    nlev = len(levels)
    for lv in range(nlev):
        g, c, e, ne, nd = levels[lv]
        for b in range(ne):
            block(lv, g, c, e, f"L{lv}e{b}")
        if lv + 1 < nlev:
            g2, c2 = levels[lv + 1][0], levels[lv + 1][1]
            m = min(LIN_MAX, 32767 // (4 * c))      # the down conv sums 4c of these
            dw(f"h{lv}", f"x{lv}", c, g, "lin", center_only=True, tag=f"L{lv}q",
               tab="lin" if m == LIN_MAX else f"lin{m}")
            pw(f"x{lv}", f"h{lv + 1}", 4 * c, c2, g2, "seth", gather="s2d", tab="res", tag=f"L{lv}down")
    # decoder
    for lv in range(nlev - 2, -1, -1):
        g, c, e, ne, nd = levels[lv]
        gc, cc = levels[lv + 1][0], levels[lv + 1][1]
        dw(f"h{lv + 1}", f"x{lv + 1}", cc, gc, "lin", center_only=True, tag=f"L{lv + 1}q2")
        pw(f"x{lv + 1}", f"h{lv}", cc, 4 * c, gc, "addh", scatter="d2s", tab="res", tag=f"L{lv}up")
        for b in range(nd):
            block(lv, g, c, e, f"L{lv}d{b}")
    g, c, e, ne, nd = levels[0]
    dw("h0", "x0", c, g, "lin", center_only=True, tag="outq")
    pw("x0", "out", c, patch * patch, g, "raw", tab="raw", tag="out")
    if cfg.get("io_q4"):
        L[-1]["wq"] = "q4"
    if cfg.get("gain2"):
        for l in L:
            l["gain2"] = True
    return T, L


# ----------------------------------------------------------------------------- counts
def positions(l):
    return l["grid"] * l["grid"]


def macs(l):
    if l["kind"] == "pw":
        return positions(l) * l["cin"] * l["cout"]
    return positions(l) * l["cout"] * (1 if l["center"] else 9)


def params(l):
    if l["kind"] == "pw":
        return l["cin"] * l["cout"]
    return l["cout"] * (1 if l["center"] else 9)


# Cost model in 68000 cycles (MAME, full 7.83 MHz), from instruction timings; M1 measures it.
DENS = 2 / 3                 # fraction of non-zero ternary weights


def rq_cycles(mode, k=4):
    shift = 6 + 2 * k
    return {"relu": 36, "lin": 36, "seth": 36, "addh": 48, "raw": 12}[mode] + shift


Q4_ADDS = 1.16             # mean add instructions per 4-bit weight (bench distribution, see ref.py)


def pw_cycles_per_pos(l, dens=DENS):
    cin, cout = l["cin"], l["cout"]
    if l.get("wq") == "q4":
        chunks = math.ceil(cout / 6)
        per_chunk = 6 * 4 + cin * (8 + 8) + 8
        adds = cin * cout * Q4_ADDS * 4
        return chunks * per_chunk + adds + cout * rq_cycles(l["mode"]) + 90
    chunks = math.ceil(cout / 7)
    per_chunk = 7 * 4 + cin * 8 + 8          # clears, input loads, a0 reset
    adds = cin * cout * dens * 4
    jumps = (4 if l["gather"] == "s2d" else 0) * 8 * chunks
    return chunks * per_chunk + adds + jumps + cout * rq_cycles(l["mode"]) + 90


def dw_cycles_per_pos(l, dens=DENS):
    taps = 1 if l["center"] else 9 * dens
    return l["cout"] * (taps * 12 + rq_cycles(l["mode"])) + 90


def cycles(l, dens=DENS):
    f = pw_cycles_per_pos if l["kind"] == "pw" else dw_cycles_per_pos
    return positions(l) * f(l, dens)


def jit_bytes(l, dens=DENS):
    if l["kind"] == "pw" and l.get("wq") == "q4":
        chunks = math.ceil(l["cout"] / 6)
        return int(chunks * (12 + 6 * l["cin"] + 4) + 2 * l["cin"] * l["cout"] * Q4_ADDS + l["cout"] * 10 + 8)
    if l["kind"] == "pw":
        chunks = math.ceil(l["cout"] / 7)
        return int(chunks * (14 + 2 * l["cin"] + 4) + 2 * l["cin"] * l["cout"] * dens + l["cout"] * 10 + 8)
    taps = 1 if l["center"] else 9 * dens
    return int(l["cout"] * (4 * taps + 10) + 8)


def summary(name, dens=DENS, clock=7833600, slow=1.5):
    T, L = build(name)
    m = sum(macs(l) for l in L)
    p = sum(params(l) for l in L)
    cyc = sum(cycles(l, dens) for l in L)
    jb = sum(jit_bytes(l, dens) for l in L)
    act = sum((t[0] + 2) * (t[1] + 2) * t[2] * 2 for t in T.values())
    return dict(name=name, layers=len(L), macs=m, params=p, est_s_mame=cyc / clock,
                est_s_real=cyc / clock * slow, jit_kb=jb / 1024, act_kb=act / 1024)


if __name__ == "__main__":
    names = sys.argv[1:] or list(CONFIGS)
    for n in names:
        s = summary(n)
        print(f"{n:6s} layers {s['layers']:3d}  MACs {s['macs'] / 1e6:6.2f}M  params {s['params'] / 1e3:7.1f}K  "
              f"est step {s['est_s_mame']:5.2f} s MAME / {s['est_s_real']:5.2f} s real  "
              f"JIT {s['jit_kb']:6.0f} KB  activations {s['act_kb']:5.0f} KB")
    if len(names) == 1:
        T, L = build(names[0])
        for l in L:
            print(f"  {l['tag']:10s} {l['kind']} {l['src']:>4s}->{l['dst']:4s} grid {l['grid']:2d} "
                  f"{l['cin']:4d}->{l['cout']:4d} {l['mode']:5s} {l.get('gather', '')}/{l.get('scatter', '')} "
                  f"MACs {macs(l) / 1e3:7.1f}K est {cycles(l) / 7.8336e6 * 1000:7.1f} ms")
