#!/usr/bin/env python3
"""Helpers around student.py (which stays untouched): optimiser step scales for integer-unit
parameters, 16-bit overflow statistics on real batches, checkpoint save/load (export.py's format)."""
import os
import sys

import torch
import torch.nn.functional as F

from student import DreamStudent, Q_IN, gather_s2d, scatter_d2s, tab_range  # noqa: E402  (adds engine/)
import student as _student  # noqa: E402
import arch  # noqa: E402

# PyTorch's CPU conv2d switches to NNPACK (Winograd/FFT, not exact for integers) at batch >= 16, which
# breaks qat == int on the CPU. Plain direct/GEMM convolutions are exact for these integer sums.
torch.backends.nnpack.set_flags(False)


def layer_unit(l):
    """Typical pre-activation size of a layer's output (the scale calibrate() aims for)."""
    r = tab_range(l["tab"])
    if l["mode"] == "raw":
        return 0.5 * Q_IN
    if l["kind"] == "dw" and l["center"]:
        return r[1] / 3
    if l["mode"] == "addh":
        return 0.05 * r[1]
    if l["mode"] == "seth":
        return 0.15 * r[1]
    return r[1] / 3


def bias_units(m):
    u = torch.zeros(m.ctot)
    for i, l in enumerate(m.L):
        a, b = m.offs[i]
        u[a:b] = layer_unit(l)
    return u


def param_groups(m, lr, bias_mult=1.0, w_mult=1.0, lam_mult=1.0):
    """ScaledAdamW groups: biases, U and the time MLP's last layer step in activation units; the
    weights, gains and embeddings step in their own units."""
    u = bias_units(m).to(next(m.parameters()).device)
    g = []
    g.append(dict(params=[m.b0], scale=u * bias_mult))
    g.append(dict(params=[m.U], scale=(u / 4)[None, :] * bias_mult))
    g.append(dict(params=[m.tmlp[2].weight], scale=(u / 8)[:, None] * bias_mult))
    g.append(dict(params=[m.tmlp[2].bias], scale=u * bias_mult))
    g.append(dict(params=[p for p in m.W if p.requires_grad], scale=None, lr=lr * w_mult))
    g.append(dict(params=list(m.lam), scale=None, lr=lr * lam_mult))
    g.append(dict(params=[m.E] + list(m.tmlp[0].parameters()), scale=None))
    for x in g:
        x.setdefault("lr", lr)
        x["base_lr"] = x["lr"]
    return g


@torch.no_grad()
def overflow_stats(m, xt_q9, s, t):
    """Mirror of the qat forward that records, per layer, the integer quantities the Mac keeps in
    16 bits: |acc| (weighted sum), |v| = requantised acc + b (must stay inside the 15-bit table index for
    tabled layers), and |old + v| for residual adds. Returns a list of dicts and the worst margins."""
    assert m.mode in ("qat", "int")
    old = m.mode
    m.mode = "qat"
    p = arch.CONFIGS[m.cfg]["patch"]
    x = F.pixel_unshuffle(m.net_input(xt_q9), p) if hasattr(m, "net_input") else \
        torch.round(F.pixel_unshuffle(xt_q9.clamp(-arch.IN_MAX, arch.IN_MAX), p))
    tens = {"xt": x}
    bias = m.bias(s, t)
    g2 = getattr(m, "gain2", False)
    rows = []
    for i, l in enumerate(m.L):
        acc = m.conv(i, tens[l["src"]], m.layer_weights(i))
        a, b = m.offs[i]
        if g2:
            k1, sgn, k2, _ = _student.gain2_codes(m.lam[i])
            v = (torch.floor(acc * torch.pow(2.0, -k1)[None, :, None, None])
                 + sgn[None, :, None, None] * torch.floor(acc * torch.pow(2.0, -k2)[None, :, None, None]))
        else:
            v = torch.floor(acc * m.gains(i)[None, :, None, None])
        v = v + bias[:, a:b, None, None]
        if l["kind"] == "pw" and l["scatter"] == "d2s":
            v = scatter_d2s(v, l["cout"] // 4)
        row = dict(tag=l["tag"], acc=float(acc.abs().max()), v=float(v.abs().max()))
        if l["mode"] == "addh":
            v = tens[l["dst"]] + v
            row["sum"] = float(v.abs().max())
        r = tab_range(l["tab"])
        row["tab"] = l["tab"]
        row["clip_frac"] = float(((v < r[0]) | (v > r[1])).float().mean()) if r else 0.0
        tens[l["dst"]] = v if r is None else v.clamp(r[0], r[1])
        rows.append(row)
    m.mode = old
    worst_acc = max(r["acc"] for r in rows)
    worst_v = max(max(r["v"], r.get("sum", 0)) for r in rows if r["tab"] != "raw")
    return rows, dict(max_acc=worst_acc, max_tab_index=worst_v,
                      acc_margin=32767 - worst_acc, tab_margin=16383 - worst_v)


def range_penalty(m, xt_q9, s, t, T=14000.0):
    """Differentiable penalty on the 16-bit table index: sum over tabled layers of (relu(|v| - T) / T)^2
    per picture, where v is the requantised value before the table clamp (|old + v| for residual adds).
    The qat forward clamps such values (zero gradient), but the Mac's table index is 15-bit and wraps
    beyond +-16383, so nothing else keeps them in range. Returns (penalty, max |v|)."""
    p = arch.CONFIGS[m.cfg]["patch"]
    x = F.pixel_unshuffle(m.net_input(xt_q9), p)
    tens = {"xt": x}
    bias = m.bias(s, t)
    pen = x.new_zeros(())
    vmax = 0.0
    for i, l in enumerate(m.L):
        acc = m.conv(i, tens[l["src"]], m.layer_weights(i))
        a, b = m.offs[i]
        v = acc * m.gains(i)[None, :, None, None] + bias[:, a:b, None, None]
        if l["kind"] == "pw" and l["scatter"] == "d2s":
            v = scatter_d2s(v, l["cout"] // 4)
        if l["mode"] == "addh":
            v = tens[l["dst"]] + v
        r = tab_range(l["tab"])
        if r is None:
            tens[l["dst"]] = v
            continue
        pen = pen + (F.relu(v.abs() - T) / T).pow(2).sum() / v.shape[0]
        vmax = max(vmax, float(v.detach().abs().max()))
        tens[l["dst"]] = v.clamp(r[0], r[1])
    return pen, vmax


@torch.no_grad()
def cap_subject_scale(m, m_max=16383):
    """After to_qat/freeze_subject_quant: shrink the per-channel U scale so the Mac's subject-bias
    multiplier M_c = round(sE sU_c 2^22) stays <= m_max (export.py requires < 16384). U entries beyond
    127 sU_c then saturate in U_int (straight-through), which QAT adapts to. Returns #channels capped."""
    lim = m_max / (float(m.sE) * 2 ** 22)
    n = int((m.sU > lim).sum())
    m.sU.clamp_(max=lim)
    return n


def make_student(cfg, n_subjects, emb_dim=32):
    return DreamStudent(cfg, n_subjects=n_subjects, emb_dim=emb_dim)


def save_student(path, m, ts, extra):
    """Checkpoint in the shape export.py reads: state_dict, cfg, n_subjects, emb_dim, ts (+ extras)."""
    d = dict(state_dict=m.state_dict(), cfg=m.cfg, n_subjects=m.n_subjects, emb_dim=m.emb_dim,
             ts=list(ts), mode=m.mode, theta=m.theta, **extra)
    torch.save(d, path + ".tmp")
    os.replace(path + ".tmp", path)


def parse_sx(spec):
    """'q4=out+in,gain2' -> StudentX options (student_x.py prototypes); '' -> None (plain DreamStudent)."""
    if not spec:
        return None
    kw = {}
    for part in spec.split(","):
        if part.startswith("q4="):
            kw["q4_tags"] = set(part[3:].split("+"))
        elif part == "gain2":
            kw["gain2"] = True
        elif part:
            raise ValueError(part)
    return kw


def build_student(cfg, n_subjects, emb_dim=32, sx=None):
    if sx:
        from student_x import StudentX
        return StudentX(cfg, n_subjects=n_subjects, emb_dim=emb_dim, **sx)
    return DreamStudent(cfg, n_subjects=n_subjects, emb_dim=emb_dim)


def load_student(path, device="cpu", sx=None):
    """sx: StudentX options to use (e.g. when starting QAT from a plain float checkpoint); by default the
    checkpoint's own ('sx' key)."""
    d = torch.load(path, map_location=device)
    sx = sx if sx is not None else d.get("sx")
    m = build_student(d["cfg"], d["n_subjects"], d["emb_dim"], sx).to(device)
    sd = dict(d["state_dict"])
    if sx and "q4s" not in sd:
        sd["q4s"] = m.q4s
    m.load_state_dict(sd)
    m.mode = d["mode"]
    m.theta = d.get("theta", 0.7)
    return m, d
