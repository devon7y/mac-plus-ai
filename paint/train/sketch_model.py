"""Class-conditional sketch-rnn decoder for Doodle's "Finish it" (shared by train/eval).

Model (per pen move):
  input  x_t = stroke-5 (dx/S, dy/S, p_down, p_up, p_end), S = global scale factor (meta.json)
  GRU    PyTorch/cuDNN formulation (reset gate applied after the recurrent matmul):
           r = sig(Wx_r x + Wc_r e + b_ir + Wh_r h + b_hr)
           z = sig(Wx_z x + Wc_z e + b_iz + Wh_z h + b_hz)
           n = tanh(Wx_n x + Wc_n e + b_in + r * (Wh_n h + b_hn))
           h' = (1 - z) * n + z * h
         e = category embedding (345 x 32); Wc e + b_i is constant per drawing, so the Mac
         computes it once (3H values) and each step only multiplies the 5-column Wx.
  h0     tanh(W0 e + b0) (once per drawing)
  output y = Wo h' + bo, 123 values: 6 blocks of 20 mixture components
         [pi logits | mu_x | mu_y | log sigma_x | log sigma_y | rho (pre-tanh)] then 3 pen logits.

Quantization-aware training (fake quant, straight-through estimator), per output row:
  Wh (3H x H) and Wo (123 x H), the per-step bulk of the work:
     'tern': ternary {-1,0,1} * s_row, s_row = mean|W_row| (absmean, BitNet b1.58 style)
     'int4': codes -8..7 * s_row, s_row = max|W_row| / 7 (symmetric absmax)
     'fp'  : float
     'tern_o4': ternary Wh, 4-bit Wo
  Wx (3H x 5, per step but tiny): int8 codes -127..127, s_row = max|W_row|/127 (tern/int4)
  Wc (3H x 32), W0 (H x 32): once per drawing, int4 as above (tern/int4)
  embedding table E (345 x 32): int8 per row (tern/int4)
  biases: float (int16/int32 fixed point on the Mac).
Activations are float in training (cuDNN GRU); eval.py has a step-by-step simulation with
quantized hidden state to measure that gap.
"""
import math

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

NCAT, EMB, NMIX = 345, 32, 20
NOUT = 6 * NMIX + 3


# ------------------------------------------------------------------ quantizers (STE)
# Straight-through estimator on the rounding only: the clamp and the per-row scale stay in
# the autograd graph, so weights already saturated at the end of the code range get no
# outward push and the effect of a weight on its row's scale is seen. (A first version passed
# the gradient straight through everything; all its QAT runs diverged after 40-57K steps as
# latent weights grew without bound. Forward values are identical.)
def _round_ste(x):
    return x + (x.round() - x).detach()


def q_tern(w):
    s = w.abs().mean(dim=1, keepdim=True).clamp_min(1e-8)
    return torch.clamp(_round_ste(w / s), -1, 1) * s


def q_tern_g(w, g):
    """Ternary with one absmean scale per group of g columns in each row (simulation of a
    possible Mac variant: g partial sums per row, each times its own scale)."""
    r, c = w.shape
    wg = w.view(r, c // g, g)
    s = wg.abs().mean(dim=2, keepdim=True).clamp_min(1e-8)
    return (torch.clamp(_round_ste(wg / s), -1, 1) * s).view(r, c)


def q_tern_r2(w):
    """Two-term ("residual") ternary: s1*T1 + s2*T2, T2 ternary-quantizes w - s1*T1.
    On the Mac: two W4 passes over the same hq and two per-row multipliers."""
    q1 = q_tern(w)
    return q1 + q_tern(w - q1)


def q_int(w, bits):
    qmax = 2 ** (bits - 1) - 1
    s = w.abs().amax(dim=1, keepdim=True).clamp_min(1e-8) / qmax
    return torch.clamp(_round_ste(w / s), -qmax - 1, qmax) * s


def q_int4(w):
    return q_int(w, 4)


def q_int8(w):
    return q_int(w, 8)


def q_none(w):
    return w


def export_codes(w, kind):
    """Integer codes and per-row scales as the Mac would store them (numpy)."""
    w = w.detach().float().cpu()
    if kind == "tern_r2":
        c1, s1 = export_codes(w, "tern")
        c2, s2 = export_codes(w - torch.from_numpy(c1).float() * torch.from_numpy(s1)[:, None], "tern")
        return np.stack([c1, c2]), np.stack([s1, s2])             # [2, rows, H], [2, rows]
    if kind.startswith("tern_g"):
        g = int(kind[6:])
        r, cc = w.shape
        wg = w.view(r, cc // g, g)
        s = wg.abs().mean(dim=2, keepdim=True).clamp_min(1e-8)
        c = (wg / s).round().clamp(-1, 1).view(r, cc)
        return c.to(torch.int8).numpy(), s.squeeze(2).numpy()     # scales [rows, groups]
    if kind == "tern":
        s = w.abs().mean(dim=1, keepdim=True).clamp_min(1e-8)
        c = (w / s).round().clamp(-1, 1)
    else:
        bits = {"int4": 4, "int8": 8}[kind]
        qmax = 2 ** (bits - 1) - 1
        s = w.abs().amax(dim=1, keepdim=True).clamp_min(1e-8) / qmax
        c = (w / s).round().clamp(-qmax - 1, qmax)
    return c.to(torch.int8).numpy(), s.squeeze(1).numpy()


# ------------------------------------------------------------------ model
class SketchDecoder(nn.Module):
    def __init__(self, H=256, quant="fp"):
        super().__init__()
        self.H, self.quant = H, quant
        k = 1.0 / math.sqrt(H)
        u = lambda *shape: nn.Parameter(torch.empty(*shape).uniform_(-k, k))
        self.emb = nn.Parameter(torch.randn(NCAT, EMB) * 0.5)
        self.w0 = u(H, EMB)
        self.b0 = nn.Parameter(torch.zeros(H))
        self.wx = u(3 * H, 5)
        self.wc = u(3 * H, EMB)
        self.wh = u(3 * H, H)
        self.b_ih = u(3 * H)
        self.b_hh = u(3 * H)
        self.wo = nn.Parameter(torch.empty(NOUT, H).uniform_(-k, k))
        self.bo = nn.Parameter(torch.zeros(NOUT))
        # per-matrix kinds: quant = 'fp' | 'tern' | 'int4' | 'tern_o4' (ternary Wh, 4-bit Wo)
        # 'tern_g64' / 'tern_g128': Wh ternary with per-group scales, Wo ternary per row
        # 'tern_r2o': Wh ternary, Wo two-term ternary
        self.kind_wh = {"fp": None, "tern": "tern", "int4": "int4", "tern_o4": "tern",
                        "tern_g64": "tern_g64", "tern_g128": "tern_g128",
                        "tern_r2o": "tern"}[quant]
        self.kind_wo = {"fp": None, "tern": "tern", "int4": "int4", "tern_o4": "int4",
                        "tern_g64": "tern", "tern_g128": "tern", "tern_r2o": "tern_r2"}[quant]
        qk = {None: q_none, "tern": q_tern, "int4": q_int4, "tern_r2": q_tern_r2,
              "tern_g64": lambda w: q_tern_g(w, 64), "tern_g128": lambda w: q_tern_g(w, 128)}
        self.qwh, self.qwo = qk[self.kind_wh], qk[self.kind_wo]
        self.qsmall = q_none if quant == "fp" else q_int4
        self.q8 = q_none if quant == "fp" else q_int8

    def weights(self):
        """Fake-quantized weights used for both training and evaluation."""
        return dict(emb=self.q8(self.emb), w0=self.qsmall(self.w0), wx=self.q8(self.wx),
                    wc=self.qsmall(self.wc), wh=self.qwh(self.wh), wo=self.qwo(self.wo))

    def start(self, cat, W=None):
        """Per-drawing constants: initial hidden state and category input term."""
        W = W or self.weights()
        e = W["emb"][cat]                                   # [B,32]
        h0 = torch.tanh(e @ W["w0"].t() + self.b0)          # [B,H]
        return h0, e

    def forward(self, x5, cat, return_h=False):
        """Teacher-forced pass. x5 [B,T,5] float, cat [B] long -> outputs [B,T,123]
        (and the hidden states [B,T,H] if return_h)."""
        W = self.weights()
        h0, e = self.start(cat, W)
        B, T, _ = x5.shape
        inp = torch.cat([x5, e[:, None, :].expand(B, T, EMB)], dim=2).contiguous()
        w_ih = torch.cat([W["wx"], W["wc"]], dim=1).contiguous()
        y, _ = torch._VF.gru(inp, h0[None].contiguous(),
                             [w_ih, W["wh"].contiguous(), self.b_ih, self.b_hh],
                             True, 1, 0.0, self.training, False, True)
        o = y @ W["wo"].t() + self.bo
        return (o, y) if return_h else o

    # step-by-step pieces (sampling, priming, activation-quantization simulation)
    def step_consts(self, cat):
        W = self.weights()
        h0, e = self.start(cat, W)
        gc = e @ W["wc"].t() + self.b_ih                    # constant input term [B,3H]
        return W, h0, gc

    def step(self, W, gc, h, x5, hq=None):
        """One GRU step. hq: optional function quantizing the hidden state (simulation)."""
        H = self.H
        gi = x5 @ W["wx"].t() + gc
        gh = h @ W["wh"].t() + self.b_hh
        r = torch.sigmoid(gi[:, :H] + gh[:, :H])
        z = torch.sigmoid(gi[:, H:2 * H] + gh[:, H:2 * H])
        n = torch.tanh(gi[:, 2 * H:] + r * gh[:, 2 * H:])
        h = (1 - z) * n + z * h
        if hq is not None:
            h = hq(h)
        return h, h @ W["wo"].t() + self.bo


# ------------------------------------------------------------------ loss
def split_params(o):
    pi, mx, my, lsx, lsy, rr = o[..., :6 * NMIX].split(NMIX, dim=-1)
    lsx = lsx.clamp(-8.0, 4.0)
    lsy = lsy.clamp(-8.0, 4.0)
    rho = torch.tanh(rr) * 0.999
    return pi, mx, my, lsx, lsy, rho, o[..., 6 * NMIX:]


def mdn_nll(o, dx, dy):
    """Negative log-likelihood of offsets (dx, dy) under the mixture, per position."""
    pi, mx, my, lsx, lsy, rho, _ = split_params(o)
    zx = (dx[..., None] - mx) * torch.exp(-lsx)
    zy = (dy[..., None] - my) * torch.exp(-lsy)
    om = 1 - rho * rho
    q = (zx * zx + zy * zy - 2 * rho * zx * zy) / om
    logn = -math.log(2 * math.pi) - lsx - lsy - 0.5 * torch.log(om) - 0.5 * q
    return -torch.logsumexp(F.log_softmax(pi, dim=-1) + logn, dim=-1)


def seq_loss(o, tgt, n):
    """o [B,T,123]; tgt [B,T,5]; n [B] = number of targets per drawing (moves + end).
    Returns (sum offset NLL over real moves, sum pen CE over n positions, count n)."""
    B, T, _ = o.shape
    ar = torch.arange(T, device=o.device)[None]
    m_pen = (ar < n[:, None]).float()
    m_xy = (ar < (n - 1)[:, None]).float()
    nll = mdn_nll(o, tgt[..., 0], tgt[..., 1])
    ce = F.cross_entropy(o[..., 6 * NMIX:].reshape(-1, 3), tgt[..., 2:].argmax(-1).reshape(-1),
                         reduction="none").view(B, T)
    return (nll * m_xy).sum(), (ce * m_pen).sum(), m_pen.sum()


def distill_loss(o, ot, n):
    """Match a teacher's per-step output distribution (sum over the n positions): KL on the
    mixture weights and pen states, plus each component's mean / log sigma / rho weighted by
    the teacher's mixture weight (components keep their identity: the student starts from
    the teacher)."""
    B, T, _ = o.shape
    m = (torch.arange(T, device=o.device)[None] < n[:, None]).float()
    pi, mx, my, lsx, lsy, rho, pen = split_params(o)
    pit, mxt, myt, lsxt, lsyt, rhot, pent = split_params(ot)
    lpt = F.log_softmax(pit, -1)
    wt = lpt.exp()
    kl_pi = (wt * (lpt - F.log_softmax(pi, -1))).sum(-1)
    lqt = F.log_softmax(pent, -1)
    kl_pen = (lqt.exp() * (lqt - F.log_softmax(pen, -1))).sum(-1)
    dx = ((mx - mxt) * torch.exp(-lsxt)).pow(2).clamp(max=100.0)
    dy = ((my - myt) * torch.exp(-lsyt)).pow(2).clamp(max=100.0)
    comp = dx + dy + (lsx - lsxt).pow(2) + (lsy - lsyt).pow(2) + (rho - rhot).pow(2)
    return ((kl_pi + kl_pen + 0.5 * (wt * comp).sum(-1)) * m).sum()


# ------------------------------------------------------------------ data
class GPUData:
    """All drawings of one split resident on the GPU; length-bucketed batches."""

    def __init__(self, prep, split, scale, device, max_n=None, seed=0):
        xy = np.load(f"{prep}/{split}_xy.npy")
        pen = np.load(f"{prep}/{split}_pen.npy")
        off = np.load(f"{prep}/{split}_off.npy")
        cat = np.load(f"{prep}/{split}_cat.npy").astype(np.int64)
        lens = np.diff(off)
        idx = np.arange(len(lens))
        if max_n is not None and max_n < len(idx):   # fixed stratified subset (eval)
            rng = np.random.default_rng(seed)
            per = max(1, max_n // NCAT)
            idx = np.concatenate([rng.permutation(np.flatnonzero(cat == c))[:per]
                                  for c in range(NCAT)])
        self.idx = idx
        self.device, self.scale = device, scale
        self.xy = torch.from_numpy(xy).to(device)
        self.pen = torch.from_numpy(pen).to(device)
        self.off = torch.from_numpy(off[:-1]).to(device)
        self.lens_np, self.cat_np = lens, cat
        self.lens = torch.from_numpy(lens).to(device)
        self.cat = torch.from_numpy(cat).to(device)

    def batches(self, B, rng, shuffle=True, bucket=16):
        """Yield index arrays; drawings grouped by length so padding stays small."""
        idx = self.idx
        L = self.lens_np[idx]
        out = []
        for lo in range(0, int(L.max()) + 1, bucket):
            b = idx[(L >= lo) & (L < lo + bucket)]
            if len(b) == 0:
                continue
            if shuffle:
                b = rng.permutation(b)
            out += [b[i:i + B] for i in range(0, len(b), B)]
        if shuffle:
            out = [out[i] for i in rng.permutation(len(out))]
        return out

    def make(self, bidx, stretch=0.0, dequant=False, gen=None):
        """Build (inputs [B,T,5], targets [B,T,5], n [B], cat [B]) for drawing indices.
        T = max moves + 1. targets: moves then the end token (padding = end token)."""
        d = self.device
        bi = torch.as_tensor(bidx, device=d)
        L = self.lens[bi]
        T = int(L.max()) + 1
        ar = torch.arange(T - 1, device=d)[None]
        valid = ar < L[:, None]
        pos = (self.off[bi][:, None] + ar).clamp(max=self.xy.shape[0] - 1)
        xy = self.xy[pos].float() * valid[..., None]
        lift = self.pen[pos].float() * valid
        Bn = len(bidx)
        if stretch > 0:
            s = 1 + (torch.rand(Bn, 1, 2, device=d, generator=gen) * 2 - 1) * stretch
            xy = torch.round(xy * s)
        tgt = torch.zeros(Bn, T, 5, device=d)
        tgt[:, :T - 1, :2] = xy / self.scale
        tgt[:, :T - 1, 2] = (1 - lift) * valid
        tgt[:, :T - 1, 3] = lift
        tgt[:, :T - 1, 4] = (~valid).float()
        tgt[:, T - 1, 4] = 1
        inp = torch.zeros_like(tgt)
        inp[:, 0, 2] = 1                                   # S0 = (0,0,1,0,0)
        inp[:, 1:] = tgt[:, :-1]
        if dequant:                                        # uniform noise on targets only
            noise = (torch.rand(Bn, T, 2, device=d, generator=gen) - 0.5) / self.scale
            tgt = tgt.clone()
            tgt[..., :2] += noise * (torch.arange(T, device=d)[None, :, None] < L[:, None, None])
        return inp, tgt, L + 1, self.cat[bi]
