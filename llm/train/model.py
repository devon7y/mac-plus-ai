"""Pixel model for training: the float network of llm/ref/SPEC.md section 1, with the QAT
fake-quantization of SPEC section 10. 6,692,640 parameters (asserted by build()).

  6 pre-norm layers, width 160, no biases. Attention: 4 query heads / 2 kv heads of 40 (query head h
  reads kv head h // 2), ALiBi slopes 1/4, 1/16, 1/64, 1/256, sliding window of 64 tokens including
  the current one. FFN: router (4 x 160) picks the argmax expert; output = gate * W_down(silu(W_gate n)
  * W_up n) with gate = softmax(router logits)[top] (Switch). Tied 2,048 x 160 embedding / head.
  RMSNorm with gains, eps 1e-6.

quant=True (ternary QAT, BitNet b1.58) fake-quantizes, all with straight-through estimators:
  weights   wq wk wv wo w_gate w_up w_down: absmean ternary per matrix; router: per-matrix absmax +-127;
            embedding: per-row absmax +-7 (also used as the input embedding)
  activations (symmetric per-vector absmax, round to nearest):
            RMSNorm outputs into QKV and into router+experts +-127 per token; q +-117 per token per head;
            k, v +-7 per token per kv head; attention output (into W_o) +-127 per token;
            silu(gate)*up (into W_down) +-127 per token; final RMSNorm output (into the head) +-58.
quant=False is the full-precision twin (same parameters, no fake quantization).

Parameter names follow export.py's float layout (tok_emb, layers.{l}.wq, ...); expert matrices are
stored stacked (layers.{l}.w_gate [4,512,160] etc.) and split by to_float_weights().
"""
import math
from dataclasses import dataclass

import torch
import torch.nn as nn
import torch.nn.functional as F


@dataclass
class Config:
    vocab: int = 2048
    d: int = 160
    n_layer: int = 6
    n_head: int = 4
    n_kv: int = 2
    head_dim: int = 40
    n_exp: int = 4
    d_ff: int = 512
    window: int = 64
    quant: bool = True
    qa: int = 127        # norm outputs, attention output, silu*up
    qq: int = 117        # query codes
    qkv: int = 7         # key/value codes
    qf: int = 58         # final-norm codes
    qe: int = 7          # embedding codes (per row)
    qr: int = 127        # router weights (per matrix)
    norm_eps: float = 1e-6
    aux_coef: float = 0.01


EXPECTED_PARAMS = 6_692_640
ALIBI_SLOPES = [2.0 ** -2, 2.0 ** -4, 2.0 ** -6, 2.0 ** -8]


def f32(x):
    """Upcast half types to float32; keep float64 (used by the exactness tests)."""
    return x if x.dtype == torch.float64 else x.float()


def ste(x, xq):
    return x + (xq - x).detach()


def fq(x, Q, dim=-1):
    """Symmetric per-vector absmax fake-quantization to integers in [-Q, Q] (SPEC model.fq)."""
    m = x.detach().abs().amax(dim=dim, keepdim=True)
    m = torch.where(m > 0, m, torch.ones_like(m))
    return ste(x, torch.round(x * (Q / m)) * (m / Q))


def tern(w):
    """BitNet b1.58 absmean: alpha = mean|W|, clip(round(W / alpha), -1, 1) * alpha."""
    a = w.detach().abs().mean() + 1e-12
    return ste(w, torch.round(w / a).clamp(-1, 1) * a)


def fq_tensor(w, Q):
    m = w.detach().abs().max() + 1e-12
    return ste(w, torch.round(w / (m / Q)).clamp(-Q, Q) * (m / Q))


def rmsnorm(x, g, eps):
    xf = f32(x)
    return (xf * torch.rsqrt(xf.pow(2).mean(-1, keepdim=True) + eps) * g).to(x.dtype)


class Layer(nn.Module):
    def __init__(self, c):
        super().__init__()
        self.c = c
        d, hd = c.d, c.head_dim
        u = lambda o, i: nn.Parameter(torch.randn(o, i) / math.sqrt(i))  # noqa: E731
        self.attn_norm = nn.Parameter(torch.ones(d))
        self.wq = u(c.n_head * hd, d)
        self.wk = u(c.n_kv * hd, d)
        self.wv = u(c.n_kv * hd, d)
        self.wo = u(d, c.n_head * hd)
        self.ffn_norm = nn.Parameter(torch.ones(d))
        self.router = nn.Parameter(torch.randn(c.n_exp, d) * 0.02)
        self.w_gate = nn.Parameter(torch.randn(c.n_exp, c.d_ff, d) / math.sqrt(d))
        self.w_up = nn.Parameter(torch.randn(c.n_exp, c.d_ff, d) / math.sqrt(d))
        self.w_down = nn.Parameter(torch.randn(c.n_exp, d, c.d_ff) / math.sqrt(c.d_ff))

    # --- weights as the forward pass sees them
    def W(self, name):
        w = getattr(self, name)
        if not self.c.quant:
            return w
        if name == "router":
            return fq_tensor(w, self.c.qr)
        if w.dim() == 3:
            return torch.stack([tern(w[e]) for e in range(w.shape[0])])
        return tern(w)

    def qa(self, x, Q, dim=-1):
        return fq(x, Q, dim) if self.c.quant else x

    def qkv(self, x):
        """x: (B, T, d) residual -> q (B, H, T, hd), k, v (B, KVH, T, hd), quantized as cached."""
        c = self.c
        B, T, _ = x.shape
        n = self.qa(rmsnorm(x, self.attn_norm, c.norm_eps), c.qa)
        q = F.linear(n, self.W("wq")).view(B, T, c.n_head, c.head_dim)
        k = F.linear(n, self.W("wk")).view(B, T, c.n_kv, c.head_dim)
        v = F.linear(n, self.W("wv")).view(B, T, c.n_kv, c.head_dim)
        q, k, v = self.qa(q, c.qq), self.qa(k, c.qkv), self.qa(v, c.qkv)
        return q.transpose(1, 2), k.transpose(1, 2), v.transpose(1, 2)

    def attn_out(self, y):
        """y: (B, H, T, hd) head outputs -> residual update (B, T, d)."""
        B, H, T, hd = y.shape
        o = self.qa(y.transpose(1, 2).reshape(B, T, H * hd), self.c.qa)
        return F.linear(o, self.W("wo"))

    def moe(self, x):
        c = self.c
        B, T, d = x.shape
        n = self.qa(rmsnorm(x, self.ffn_norm, c.norm_eps), c.qa).reshape(-1, d)
        rl = F.linear(f32(n), f32(self.W("router")))
        probs = rl.softmax(-1)
        top_p, top_i = probs.max(-1)
        wg, wu, wd = self.W("w_gate"), self.W("w_up"), self.W("w_down")
        # dense: every expert on every token (static shapes), then select the routed one
        h = F.silu(torch.einsum("nd,efd->nef", n, wg)) * torch.einsum("nd,efd->nef", n, wu)
        h = self.qa(h, c.qa)                       # per token (and expert), 512 values
        y = torch.einsum("nef,edf->ned", h, wd)
        onehot = F.one_hot(top_i, c.n_exp)
        out = torch.einsum("ned,ne->nd", y, (onehot * top_p[:, None]).to(y.dtype))
        frac = onehot.float().mean(0)
        aux = c.n_exp * (frac * probs.mean(0)).sum()
        return out.view(B, T, d).to(x.dtype), aux, frac.detach()


def alibi_bias(c, T, device, dtype):
    i = torch.arange(T, device=device)
    dist = (i[:, None] - i[None, :]).to(torch.float64)
    slopes = torch.tensor(ALIBI_SLOPES, device=device, dtype=torch.float64)[:, None, None]
    bias = -slopes * dist[None]
    ok = (dist >= 0) & (dist < c.window)
    bias = torch.where(ok[None], bias, torch.full_like(bias, float("-inf")))
    return bias.to(dtype)  # (H, T, T)


class Pixel(nn.Module):
    def __init__(self, c):
        super().__init__()
        self.c = c
        self.tok_emb = nn.Parameter(torch.randn(c.vocab, c.d) * 0.05)
        self.layers = nn.ModuleList([Layer(c) for _ in range(c.n_layer)])
        self.final_norm = nn.Parameter(torch.ones(c.d))

    def n_params(self):
        return sum(p.numel() for p in self.parameters())

    def table(self):
        return fq(self.tok_emb, self.c.qe) if self.c.quant else self.tok_emb

    def head(self, x, E):
        nf = rmsnorm(x, self.final_norm, self.c.norm_eps)
        if self.c.quant:
            nf = fq(nf, self.c.qf)
        return F.linear(nf, E.to(nf.dtype))

    def forward(self, idx):
        c = self.c
        B, T = idx.shape
        E = self.table()
        x = F.embedding(idx, E)
        bias = alibi_bias(c, T, idx.device, torch.float32 if x.dtype != torch.float64 else x.dtype)
        aux_total, fracs = 0.0, []
        for L in self.layers:
            q, k, v = L.qkv(x)
            rep = c.n_head // c.n_kv
            y = F.scaled_dot_product_attention(q, k.repeat_interleave(rep, 1), v.repeat_interleave(rep, 1),
                                               attn_mask=bias.to(q.dtype))
            x = x + L.attn_out(y)
            m, aux, frac = L.moe(x)
            x = x + m
            aux_total = aux_total + aux
            fracs.append(frac)
        return self.head(x, E), aux_total / c.n_layer, torch.stack(fracs)

    # ---- incremental decoding with a rolling 64-entry KV cache (what the Mac does) ----
    @torch.no_grad()
    def step(self, tok, cache):
        """tok: (B,) ids. cache from new_cache(): per-layer keys/values (B, KVH, n, hd), n <= window-1,
        oldest first. Returns logits (B, V)."""
        c = self.c
        E = self.table()
        x = F.embedding(tok[:, None], E)
        rep = c.n_head // c.n_kv
        for li, L in enumerate(self.layers):
            q, k, v = L.qkv(x)
            if cache["k"][li] is not None:
                k = torch.cat([cache["k"][li], k], 2)
                v = torch.cat([cache["v"][li], v], 2)
            n = k.shape[2]  # distance of key j = n - 1 - j
            dist = torch.arange(n - 1, -1, -1, device=x.device, dtype=torch.float64)
            slopes = torch.tensor(ALIBI_SLOPES, device=x.device, dtype=torch.float64)[:, None]
            bias = (-slopes * dist[None])[None, :, None, :].to(q.dtype)  # (1, H, 1, n)
            y = F.scaled_dot_product_attention(q, k.repeat_interleave(rep, 1), v.repeat_interleave(rep, 1),
                                               attn_mask=bias)
            cache["k"][li] = k[:, :, -(c.window - 1):]
            cache["v"][li] = v[:, :, -(c.window - 1):]
            x = x + L.attn_out(y)
            m, _, _ = L.moe(x)
            x = x + m
        cache["pos"] += 1
        return self.head(x, E)[:, 0]

    def new_cache(self):
        return {"k": [None] * self.c.n_layer, "v": [None] * self.c.n_layer, "pos": 0}


def build(c):
    m = Pixel(c)
    n = m.n_params()
    assert n == EXPECTED_PARAMS, f"param count {n:,} != {EXPECTED_PARAMS:,}"
    return m


def param_breakdown(m):
    tern_n = sum(p.numel() for n, p in m.named_parameters()
                 if n.split(".")[-1] in ("wq", "wk", "wv", "wo", "w_gate", "w_up", "w_down"))
    router = sum(p.numel() for n, p in m.named_parameters() if n.endswith("router"))
    norms = sum(p.numel() for n, p in m.named_parameters() if n.endswith("norm"))
    return {"ternary": tern_n, "router": router, "norms": norms, "vocab_table": m.tok_emb.numel(),
            "total": tern_n + router + norms + m.tok_emb.numel()}


def to_float_weights(state_dict):
    """Model state dict -> export.py's float layout (dict of float64 numpy arrays)."""
    sd = {k: v.detach().double().cpu().numpy() for k, v in state_dict.items()}
    sd = {k[len("_orig_mod."):] if k.startswith("_orig_mod.") else k: v for k, v in sd.items()}
    fw = {"tok_emb": sd["tok_emb"], "final_norm": sd["final_norm"]}
    n_layer = 1 + max(int(k.split(".")[1]) for k in sd if k.startswith("layers."))
    for l in range(n_layer):
        p = f"layers.{l}."
        for k in ("attn_norm", "wq", "wk", "wv", "wo", "ffn_norm", "router"):
            fw[p + k] = sd[p + k]
        for e in range(sd[p + "w_gate"].shape[0]):
            for k in ("w_gate", "w_up", "w_down"):
                fw[p + f"experts.{e}.{k}"] = sd[p + k][e]
    return fw
