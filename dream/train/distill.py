#!/usr/bin/env python3
"""Distil the guided teacher into Dream's student (student.py) on a fixed N-step DDIM grid.

Sampler (exactly the Mac's, engine/FORMAT.md + export.mac_sample; the student predicts x0 and there
is no CFG at run time). States are Q9 integers, C1/C2 are the Q14 constants of model_file.sampler_consts:
    x_0 = z (noise in Q9, |.| <= 2047, t_0 = 1)
    for i = 0..N-1:  x0_hat = clamp(student(x_i, s, t_i), +-512)
                     x_{i+1} = clamp((C1_i x_i + C2_i x0_hat + 8192) >> 14, +-2047)
    picture = the last x0_hat.     (C1_i ~ sigma_{i+1}/sigma_i, C2_i ~ alpha_{i+1} - C1_i alpha_i)
Target for a state x_i: the guided teacher (DDIM, K substeps, CFG weight w, x0 clipped) runs from t_i
to t_{i+1} and lands on x_tea; x0_tgt = clamp((x_tea - c1_i x_i) / c2_i, -1, 1) is the x0 whose
(clamped) student step lands closest to it, pixel by pixel.

State sources (mixed per batch, fractions --mix):
    bank  states of precomputed teacher trajectories from noise (the targets come with them)
    data  forward-noised training pictures, x_i = alpha_i x0 + sigma_i eps (online teacher target)
    roll  states reached by the student's own sampler (online teacher target; reduces drift),
          switched on after --roll_start of the iterations
Stages: --stage float (calibrate() then float training) and --stage qat (to_qat() on a float
checkpoint, then the same distillation with a lower learning rate).

  python distill.py --teacher runs/teacher/ema.pt --out runs/student --stage float --iters 20000
  python distill.py --teacher runs/teacher/ema.pt --out runs/student --stage qat --init runs/student/float.pt
"""
import argparse
import json
import math
import os
import sys
import time

import numpy as np
import torch

from common import ScaledAdamW, grid_image, time_grid, to_u8
from student_tools import (cap_subject_scale, load_student, make_student, overflow_stats, param_groups, parse_sx,
                           range_penalty, save_student)
from teacher import TeacherUNet, ddim


# ----------------------------------------------------------------------------- sampler pieces
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "engine"))
import model_file  # noqa: E402


class Grid:
    """Time grid t_0 = 1 > ... > t_N = 0 with float coefficients (c1 on x, c2 on x0) and the Mac's Q14
    integers (C1, C2)."""

    def __init__(self, n, kind):
        self.n, self.kind = n, kind
        self.t = time_grid(n, kind)
        self.alpha = [0.0 if t >= 1 else math.cos(0.5 * math.pi * t) for t in self.t]
        self.sigma = [0.0 if t <= 0 else math.sin(0.5 * math.pi * t) for t in self.t]
        self.c1 = [self.sigma[i + 1] / self.sigma[i] for i in range(n)]
        self.c2 = [self.alpha[i + 1] - self.c1[i] * self.alpha[i] for i in range(n)]
        q = model_file.sampler_consts(self.t)
        self.C1, self.C2 = [c[0] for c in q], [c[1] for c in q]
        self.snr = [(a_ / s_) ** 2 if s_ > 0 else float("inf") for a_, s_ in zip(self.alpha, self.sigma)]

    def info(self):
        return dict(n=self.n, kind=self.kind, t=self.t, alpha=self.alpha, sigma=self.sigma, c1=self.c1,
                    c2=self.c2, C1_q14=self.C1, C2_q14=self.C2)


def quant_in(x):
    """The Mac's input quantisation: x_t in Q9, clamped to +-2047."""
    return torch.round((x * 512.0).clamp(-2047, 2047))


def student_x0(m, x, s, t):
    return m(quant_in(x), s, t) / 512.0


@torch.no_grad()
def student_sample(m, z, s, G, upto=None):
    """The Mac's sampler (integer Q9 states, Q14 update, x0 clamped) for any student mode, from float
    noise z. Returns the states x_0..x_upto (floats = Q9 / 512) and the clamped x0 estimates."""
    B = z.shape[0]
    xq = quant_in(z)
    xs, x0s = [xq / 512.0], []
    for i in range(G.n if upto is None else upto):
        t = torch.full((B,), G.t[i], device=z.device)
        x0q = torch.round(m(xq, s, t)).clamp(-512, 512)
        # exact integer update (C1 * x reaches 3.4e7 > 2^24, so float32 would round)
        xq = ((G.C1[i] * xq.long() + G.C2[i] * x0q.long() + 8192) >> 14).clamp(-2047, 2047).float()
        xs.append(xq / 512.0)
        x0s.append(x0q / 512.0)
    return xs, x0s


def student_rollout_grad(m, z, s, G):
    """The Mac sampler with straight-through rounding, differentiable end to end: the final picture
    (x0 of the last step, in [-1, 1]) for the end-to-end loss."""
    from student import ste_floor, ste_round
    B = z.shape[0]
    xq = ste_round((z * 512.0).clamp(-2047, 2047))
    for i in range(G.n):
        t = torch.full((B,), G.t[i], device=z.device)
        x0 = ste_round(m(xq, s, t)).clamp(-512, 512)
        if i == G.n - 1:
            return x0 / 512.0
        xq = ste_floor((G.C1[i] * xq.double() + G.C2[i] * x0.double() + 8192) / 16384).float().clamp(-2047, 2047)


def teacher_target(teacher, x, s, i, G, K, w):
    """i: (B,) step indices. Teacher DDIM from t_i to t_{i+1} -> the (clamped) x0 target for one
    student step."""
    tg = torch.tensor(G.t, device=x.device)
    c1 = torch.tensor(G.c1, device=x.device)[i][:, None, None, None]
    c2 = torch.tensor(G.c2, device=x.device)[i][:, None, None, None]
    xq = quant_in(x) / 512.0
    x_tea, _ = ddim(teacher, xq, s, tg[i], tg[i + 1], K, w)
    return ((x_tea - c1 * xq) / c2).clamp(-1, 1)


@torch.no_grad()
def make_bank(teacher, present, n, G, K, w, bs, device, seed=0):
    """n teacher trajectories from noise on the student grid (subjects cycling through `present`):
    states (n, N+1, 1, R, R) and subject ids (R = the teacher's picture size)."""
    R = teacher.cfg["img"]
    g = torch.Generator(device="cpu").manual_seed(seed)
    S = torch.tensor(present)[torch.arange(n) % len(present)]
    S = S[torch.randperm(n, generator=g)]
    states = torch.empty(n, G.n + 1, 1, R, R, dtype=torch.float32, device=device)
    for b0 in range(0, n, bs):
        s = S[b0:b0 + bs].to(device)
        x = quant_in(torch.randn(len(s), 1, R, R, generator=g).to(device)) / 512.0
        states[b0:b0 + len(s), 0] = x
        for i in range(G.n):
            x, _ = ddim(teacher, x, s, G.t[i], G.t[i + 1], K, w)
            states[b0:b0 + len(s), i + 1] = x
    return states, S.to(device)


def bank_targets(states, G):
    """Clamped x0 targets for every (trajectory, step): (n, N, 1, R, R)."""
    out = torch.empty_like(states[:, :-1])
    for i in range(G.n):
        xq = quant_in(states[:, i]) / 512.0
        out[:, i] = ((states[:, i + 1] - G.c1[i] * xq) / G.c2[i]).clamp(-1, 1)
    return out


def eval_noise(seeds, cfg="A64L2"):
    """The Mac's own noise for these seeds (export.mac_noise), as floats."""
    from export import mac_noise
    return torch.cat([mac_noise(int(sd), cfg) for sd in seeds]) / 512.0


def load_teacher(path, device, compile=False):
    ck = torch.load(path, map_location=device)
    cfg = dict(ck["cfg"])
    K = cfg.pop("n_classes")
    t = TeacherUNet(K, **cfg).to(device)
    t.load_state_dict(ck["ema"])
    t.eval()
    for p in t.parameters():
        p.requires_grad_(False)
    if compile:                       # ~2.3x faster inference on H100 (measured); one compile per batch shape
        t.forward = torch.compile(t.forward, dynamic=False)
    return t, ck


def step_weights(G, kind):
    if kind == "uniform":
        return [1.0] * G.n
    if kind == "tsnr":                    # truncated SNR (Salimans & Ho): max(SNR(t_i), 1)
        return [max(G.snr[i], 1.0) for i in range(G.n)]
    if kind == "sqrt":
        return [math.sqrt(max(G.snr[i], 1.0)) for i in range(G.n)]
    if kind == "land":                    # error of the landing point x_{i+1}
        return [G.c2[i] ** 2 for i in range(G.n)]
    raise ValueError(kind)


# ----------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--teacher", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--stage", choices=["float", "qat"], default="float")
    ap.add_argument("--init", help="student checkpoint to start from (qat: the float student)")
    ap.add_argument("--cfg", default="A64L2")
    ap.add_argument("--emb_dim", type=int, default=32)
    ap.add_argument("--N", type=int, default=6)
    ap.add_argument("--grid", default="angle")
    ap.add_argument("--w", type=float, default=3.0, help="teacher guidance weight baked into the student")
    ap.add_argument("--K", type=int, default=8, help="teacher DDIM substeps per student step")
    ap.add_argument("--bank", type=int, default=0, help="trajectories in the bank (0: 256 per subject)")
    ap.add_argument("--bank_file", help="reuse/save the bank here")
    ap.add_argument("--bank_bs", type=int, default=1024, help="trajectories per teacher batch (x2 with CFG)")
    ap.add_argument("--data", help="training split for 'data' states (default: the teacher's)")
    ap.add_argument("--mix", default="bank:0.7,data:0.1,roll:0.2")
    ap.add_argument("--roll_start", type=float, default=0.3)
    ap.add_argument("--iters", type=int, default=20000)
    ap.add_argument("--bs", type=int, default=256)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--bias_mult", type=float, default=1.0)
    ap.add_argument("--warmup", type=int, default=500)
    ap.add_argument("--weights", default="sqrt")
    ap.add_argument("--e2e", type=float, default=1.0, help="weight of the end-to-end rollout loss (0: off)")
    ap.add_argument("--e2e_bs", type=int, default=64)
    ap.add_argument("--range_pen", type=float, default=0.0,
                    help="QAT: weight of the 16-bit table-index range penalty (student_tools.range_penalty)")
    ap.add_argument("--range_T", type=float, default=24000.0, help="penalise |v| above this (int16 margin)")
    ap.add_argument("--range_bs", type=int, default=64)
    ap.add_argument("--e2e_start", type=float, default=0.0)
    ap.add_argument("--eval_every", type=int, default=2000)
    ap.add_argument("--eval_subj", type=int, default=24)
    ap.add_argument("--eval_seeds", type=int, default=8)
    ap.add_argument("--max_minutes", type=float, default=1e9)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--no_compile", action="store_true", help="do not torch.compile the teacher")
    ap.add_argument("--sx", default="", help="student_x prototype options for QAT, e.g. 'q4=out,gain2'")
    ap.add_argument("--m_max", type=int, default=32767, help="cap on the subject-bias multiplier M (export)")
    ap.add_argument("--drop", default="steak", help="subjects (comma list) the app drops: never distilled")
    ap.add_argument("--qat_cfg", help="QAT into this config from a float checkpoint of the same shapes "
                                      "(e.g. A64L2 float -> A64L2Q: 4-bit in/out, Q6 input, two-term gains)")
    a = ap.parse_args()

    T0 = time.time()
    torch.manual_seed(a.seed)
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    torch.backends.cuda.matmul.allow_tf32 = False      # the student's integer sums must stay exact
    torch.backends.cudnn.allow_tf32 = False
    os.makedirs(a.out, exist_ok=True)
    teacher, tck = load_teacher(a.teacher, dev, compile=dev == "cuda" and not a.no_compile)
    names, orig = tck["names"], tck["orig"]
    Ks = len(names)
    drop = set(x.strip() for x in a.drop.split(",") if x.strip())
    present = [p for p in tck.get("present", list(range(Ks))) if names[p] not in drop]
    present_t = torch.tensor(present, device=dev)
    G = Grid(a.N, a.grid)
    print(f"teacher step {tck['step']}, {Ks} subjects ({len(present)} present); grid {G.info()}", flush=True)
    sw = torch.tensor(step_weights(G, a.weights), device=dev)
    sw = sw / sw.mean()
    print(f"step weights ({a.weights}): {[round(float(x), 3) for x in sw]}", flush=True)

    # ---- bank of teacher trajectories
    nb = a.bank or 256 * len(present)
    tb = time.time()
    if a.bank_file and os.path.exists(a.bank_file):
        bk = torch.load(a.bank_file, map_location=dev)
        states, bS = bk["states"], bk["S"]
        print(f"bank loaded: {states.shape[0]} trajectories", flush=True)
    else:
        states, bS = make_bank(teacher, present, nb, G, a.K, a.w, a.bank_bs, dev, seed=a.seed + 7)
        print(f"bank: {nb} trajectories in {time.time() - tb:.0f} s "
              f"({nb * G.n * a.K * 2 / (time.time() - tb):.0f} teacher evals/s)", flush=True)
        if a.bank_file:
            torch.save(dict(states=states, S=bS, grid=G.info(), w=a.w, K=a.K), a.bank_file)
    btg = bank_targets(states, G)
    for i in range(G.n):
        tgi = btg[:, i]
        print(f"  step {i}: t={G.t[i]:.3f} target mean {tgi.mean():+.3f} std {tgi.std():.3f} "
              f"min {tgi.min():+.2f} max {tgi.max():+.2f}", flush=True)

    # ---- data states (optional)
    data = None
    mix = {k: float(v) for k, v in (kv.split(":") for kv in a.mix.split(","))}
    if mix.get("data", 0) > 0:
        from common import GPUData, get_split
        data = GPUData(get_split(a.data or tck["data"], res=teacher.cfg["img"]), dev)

    # ---- evaluation set: the Mac's noise for seeds 1..eval_seeds (same seeds in every row), teacher
    # endpoints with the same K, w and grid as the bank
    ev_list = present[:a.eval_subj]
    ns = len(ev_list)
    ev_s = torch.tensor([s for s in ev_list for _ in range(a.eval_seeds)], device=dev)
    ev_seeds = [1 + j for _ in range(ns) for j in range(a.eval_seeds)]
    ev_z = eval_noise(ev_seeds, a.cfg).to(dev)          # the Mac's noise, at the student config's size
    with torch.no_grad():
        x = ev_z
        for i in range(G.n):
            x, _ = ddim(teacher, x, ev_s, G.t[i], G.t[i + 1], a.K, a.w)
        ev_tea = x

    # ---- student
    sx = parse_sx(a.sx)
    if a.init:
        m, ick = load_student(a.init, dev, sx=sx)
        print(f"student from {a.init} (mode {m.mode}, step {ick.get('step')})", flush=True)
    else:
        m = make_student(a.cfg, Ks, a.emb_dim).to(dev)
    if a.stage == "float" and not a.init:
        j = torch.randint(0, states.shape[0], (512,), device=dev)
        i = torch.randint(0, G.n, (512,), device=dev)
        xs = states[j, i]
        tt = torch.tensor(G.t, device=dev)[i]
        m.calibrate(quant_in(xs), bS[j], tt)
        print("calibrated", flush=True)
    if a.stage == "qat" and m.mode == "float":
        if a.qat_cfg and a.qat_cfg != m.cfg:
            from student import DreamStudent
            m2 = DreamStudent(a.qat_cfg, n_subjects=m.n_subjects, emb_dim=m.emb_dim).to(dev)
            m2.load_state_dict(m.state_dict())
            m2.adopt_q9_input_weights()
            m2.theta = m.theta
            m = m2
            print(f"float {ick['cfg']} -> {a.qat_cfg}", flush=True)
        m.to_qat()
        nc = cap_subject_scale(m, a.m_max)
        print(f"converted to qat; subject scale capped (M <= {a.m_max}) in {nc} channels", flush=True)
    m.train()
    print(f"student {a.cfg}: {m.n_params() / 1e3:.0f}K Mac params, mode {m.mode}", flush=True)

    opt = ScaledAdamW(param_groups(m, a.lr, bias_mult=a.bias_mult), lr=a.lr)
    tag = a.stage
    logf = open(os.path.join(a.out, f"log_{tag}.jsonl"), "a")

    def evaluate(step, sheets=False):
        m.eval()
        with torch.no_grad():
            xs, x0s = student_sample(m, ev_z, ev_s, G)
            fin = xs[-1]
            mse = (fin.clamp(-1, 1) - ev_tea.clamp(-1, 1)).pow(2).mean().item()
            rec = dict(step=step, eval_mse_vs_teacher=mse)
            if m.mode == "qat":
                # qat must equal the exact integer model (any difference = a 16-bit wrap). int mode
                # needs integer matmuls, so it runs on a CPU copy for the first 32 evaluation pictures.
                import copy
                mi = copy.deepcopy(m).cpu()
                mi.mode = "int"
                xi, _ = student_sample(mi, ev_z[:32].cpu(), ev_s[:32].cpu(), G)
                rec["qat_int_maxdiff"] = float((xi[-1] - fin[:32].cpu()).abs().max()) * 512
                del mi
                tt = torch.full((len(ev_s),), G.t[0], device=dev)
                worst = None
                for i in range(G.n):
                    tt = torch.full((len(ev_s),), G.t[i], device=dev)
                    _, w_ = overflow_stats(m, quant_in(xs[i]), ev_s, tt)
                    worst = w_ if worst is None else {k: max(worst[k], w_[k]) if "max" in k else min(worst[k], w_[k])
                                                      for k in w_}
                rec.update(worst)
                tb_ = torch.round(m.time_bias(torch.tensor(G.t[:-1], device=dev)))
                rec.update(b0_max=float(torch.round(m.b0).abs().max()), tbias_max=float(tb_.abs().max()),
                           M_max=float(m.subject_quant()[2].abs().max()))
        if sheets:
            rows = []
            A, B_ = to_u8(ev_tea[:, 0]), to_u8(fin[:, 0])
            for r in range(ns):
                rows.append([A[r * a.eval_seeds + j] for j in range(a.eval_seeds)] +
                            [B_[r * a.eval_seeds + j] for j in range(a.eval_seeds)])
            p = os.path.join(a.out, f"cmp_{tag}_{step:06d}.png")
            grid_image(rows, scale=2, labels=[names[r] for r in ev_list]).save(p)
            # the step-by-step x0 estimates (what the Mac draws) for the first seed of 8 subjects
            prog = []
            for r in range(min(ns, 8)):
                k = r * a.eval_seeds
                prog.append([to_u8(x0s[i][k, 0]) for i in range(G.n)] + [A[k]])
            grid_image(prog, scale=2, labels=[names[r] for r in ev_list[:8]]).save(
                os.path.join(a.out, f"steps_{tag}_{step:06d}.png"))
            rec["sheet"] = p
        m.train()
        return rec

    extra = lambda step: dict(step=step, grid=G.info(), w=a.w, K=a.K, names=names, orig=orig, present=present, sx=sx,
                              teacher=a.teacher, teacher_step=tck["step"], args=vars(a))
    print(json.dumps(evaluate(0)), flush=True)
    acc = np.zeros(G.n)
    cnt = np.zeros(G.n)
    e2e_acc = [0.0, 0]
    range_acc = [0.0, 0.0]
    tl = time.time()
    step = 0
    n_roll_online = 0
    for step in range(1, a.iters + 1):
        frac = step / a.iters
        lr_f = min(1.0, step / a.warmup) * (0.1 + 0.9 * 0.5 * (1 + math.cos(math.pi * frac)))
        for g in opt.param_groups:
            g["lr"] = g["base_lr"] * lr_f
        B = a.bs
        nr = int(B * mix.get("roll", 0)) if frac >= a.roll_start else 0
        nd = int(B * mix.get("data", 0)) if data is not None else 0
        nbk = B - nr - nd
        X, S, I, TG = [], [], [], []
        # bank
        j = torch.randint(0, states.shape[0], (nbk,), device=dev)
        i = torch.randint(0, G.n, (nbk,), device=dev)
        X.append(states[j, i]); S.append(bS[j]); I.append(i); TG.append(btg[j, i])
        with torch.no_grad():
            if nd:
                x0d, sd = data.batch(nd)
                i = torch.randint(0, G.n, (nd,), device=dev)
                al = torch.tensor(G.alpha, device=dev)[i][:, None, None, None]
                si = torch.tensor(G.sigma, device=dev)[i][:, None, None, None]
                xd = al * x0d + si * torch.randn_like(x0d)
                X.append(xd); S.append(sd); I.append(i)
                TG.append(teacher_target(teacher, xd, sd, i, G, a.K, a.w))
            if nr:
                sr = present_t[torch.randint(0, len(present), (nr,), device=dev)]
                i = torch.randint(1, G.n, (nr,), device=dev)
                m.eval()
                R = teacher.cfg["img"]
                xs_r, _ = student_sample(m, torch.randn(nr, 1, R, R, device=dev), sr, G, upto=int(i.max()))
                m.train()
                xr = torch.stack(xs_r, 1)[torch.arange(nr, device=dev), i]
                X.append(xr); S.append(sr); I.append(i)
                TG.append(teacher_target(teacher, xr, sr, i, G, a.K, a.w))
                n_roll_online += nr
        X, S, I, TG = torch.cat(X), torch.cat(S), torch.cat(I), torch.cat(TG)
        tt = torch.tensor(G.t, device=dev)[I]
        pred = student_x0(m, X, S, tt)
        per = (pred - TG).pow(2).mean((1, 2, 3))
        loss = (per * sw[I]).mean()
        if a.range_pen > 0 and m.mode == "qat":
            k = min(a.range_bs, X.shape[0])
            rp, rmax = range_penalty(m, quant_in(X[:k]), S[:k], tt[:k], a.range_T)
            loss = loss + a.range_pen * rp
            range_acc[0] += rp.item()
            range_acc[1] = max(range_acc[1], rmax)
        if a.e2e > 0 and frac >= a.e2e_start:
            j = torch.randint(0, states.shape[0], (a.e2e_bs,), device=dev)
            fin = student_rollout_grad(m, states[j, 0], bS[j], G)
            e2e = (fin - states[j, G.n].clamp(-1, 1)).pow(2).mean()
            loss = loss + a.e2e * e2e
            e2e_acc[0] += e2e.item()
            e2e_acc[1] += 1
        opt.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(m.parameters(), 10.0)
        opt.step()
        with torch.no_grad():
            np.add.at(acc, I.cpu().numpy(), per.detach().cpu().numpy())
            np.add.at(cnt, I.cpu().numpy(), 1)
        if step % 200 == 0:
            dt = time.time() - tl
            tl = time.time()
            rec = dict(step=step, loss=loss.item(), lr_f=round(lr_f, 4),
                       mse_step=[round(float(x), 5) for x in acc / np.maximum(cnt, 1)],
                       it_s=round(200 / dt, 2), nr=nr, nd=nd, min=round((time.time() - T0) / 60, 1))
            if e2e_acc[1]:
                rec["e2e_mse"] = round(e2e_acc[0] / e2e_acc[1], 5)
            if a.range_pen > 0:
                rec.update(range_pen=round(range_acc[0] / 200, 5), range_vmax=round(range_acc[1]))
                range_acc = [0.0, 0.0]
            acc[:] = 0
            cnt[:] = 0
            e2e_acc = [0.0, 0]
            if step % a.eval_every == 0 or step == a.iters:
                rec.update(evaluate(step, sheets=True))
                save_student(os.path.join(a.out, f"{tag}.pt"), m, G.t, extra(step))
            print(json.dumps(rec), flush=True)
            logf.write(json.dumps(rec) + "\n")
            logf.flush()
        if (time.time() - T0) / 60 > a.max_minutes:
            print(f"time limit at step {step}", flush=True)
            break
    rec = evaluate(step, sheets=True)
    print(json.dumps(dict(final=rec)), flush=True)
    save_student(os.path.join(a.out, f"{tag}.pt"), m, G.t, extra(step))
    if m.mode == "qat":                    # the Mac model file, checked bit-exact against picture.py
        import copy
        import export
        mc = copy.deepcopy(m).cpu()
        try:
            size = export.export(mc, names, G.t, os.path.join(a.out, "dream.drm"), drop=sorted(drop))
            bad = export.check(mc, os.path.join(a.out, "dream.drm"), G.t, n=2, names=names, drop=sorted(drop))
            print(f"export: dream.drm {size:,} bytes, check {'ALL BIT-EXACT' if not bad else 'FAILED'}", flush=True)
        except (AssertionError, KeyError, ValueError) as e:
            print(f"export FAILED: {e}" + (" (student_x prototype: export needs the lead's port)" if sx else ""),
                  flush=True)
    print(f"done in {(time.time() - T0) / 60:.1f} min", flush=True)


if __name__ == "__main__":
    main()
