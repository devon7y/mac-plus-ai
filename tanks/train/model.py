"""Tank Duel policies: a small MLP actor with ternary quantization-aware training, its exact
integer form (what the Mac runs), and the float critic used only for training.

Actor: NOBS inputs (obs / 127) -> H -> H -> 15 logits in four heads: drive 3, turn 3, turret 7, fire 2.
Hidden activations are clamp(relu(z), 0, A), fake-quantized to 127 steps when quant is on.

Integer form (export_int / int_forward): per layer a ternary matrix T (-1/0/+1), an int16
multiplier M and an int32 offset B per output, and a shift SH per layer:
    acc = T . x                (x int, |acc| < 2^15)
    hidden: y = clamp((acc * M + B) >> SH, 0, 127)
    output: z = acc * M + B    (int32), action = first argmax in each head
"""
import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

HEADS = (3, 3, 7, 2)
NOUT = sum(HEADS)
ACT_MAX = 4.0


class TernLinear(nn.Module):
    def __init__(self, nin, nout):
        super().__init__()
        self.weight = nn.Parameter(torch.empty(nout, nin))
        self.bias = nn.Parameter(torch.zeros(nout))
        nn.init.kaiming_uniform_(self.weight, a=5 ** 0.5)

    def ternary(self):
        w = self.weight
        delta = 0.7 * w.abs().mean(dim=1, keepdim=True)
        t = (w.abs() > delta).float() * torch.sign(w)
        alpha = (w.abs() * t.abs()).sum(1) / t.abs().sum(1).clamp(min=1)
        return t, alpha

    def forward(self, x, quant):
        if not quant:
            return F.linear(x, self.weight, self.bias)
        t, alpha = self.ternary()
        wq = alpha[:, None] * t
        w = self.weight + (wq - self.weight).detach()
        return F.linear(x, w, self.bias)


def fake_act(z, quant):
    y = torch.clamp(F.relu(z), 0, ACT_MAX)
    if not quant:
        return y
    s = ACT_MAX / 127
    return y + (torch.round(y / s) * s - y).detach()


class Actor(nn.Module):
    def __init__(self, nobs=40, hidden=32):
        super().__init__()
        self.hidden = hidden
        self.l1 = TernLinear(nobs, hidden)
        self.l2 = TernLinear(hidden, hidden)
        self.l3 = TernLinear(hidden, NOUT)
        self.quant = False

    def forward(self, obs):
        x = obs.float() / 127
        h = fake_act(self.l1(x, self.quant), self.quant)
        h = fake_act(self.l2(h, self.quant), self.quant)
        return self.l3(h, self.quant)


class Critic(nn.Module):
    def __init__(self, nobs=40, hidden=128):
        super().__init__()
        self.net = nn.Sequential(nn.Linear(nobs, hidden), nn.Tanh(), nn.Linear(hidden, hidden), nn.Tanh(),
                                 nn.Linear(hidden, 1))

    def forward(self, obs):
        return self.net(obs.float() / 127).squeeze(-1)


def split_heads(logits):
    return torch.split(logits, HEADS, dim=-1)


def sample(logits, greedy=False):
    acts, logp, ent = [], 0, 0
    for lg in split_heads(logits):
        d = torch.distributions.Categorical(logits=lg)
        a = lg.argmax(-1) if greedy else d.sample()
        acts.append(a)
        logp = logp + d.log_prob(a)
        ent = ent + d.entropy()
    return torch.stack(acts, -1), logp, ent


def evaluate(logits, acts):
    logp, ent = 0, 0
    for k, lg in enumerate(split_heads(logits)):
        d = torch.distributions.Categorical(logits=lg)
        logp = logp + d.log_prob(acts[:, k])
        ent = ent + d.entropy()
    return logp, ent


# ---------------------------------------------------------------------- integer form

def export_int(actor):
    """the actor's exact integer form: list of layers {T int8 (out, in), M int32, B int64, SH}"""
    layers = []
    mods = [actor.l1, actor.l2, actor.l3]
    with torch.no_grad():
        for k, m in enumerate(mods):
            t, alpha = m.ternary()
            t = t.numpy().astype(np.int8)
            alpha = alpha.numpy().astype(np.float64)
            b = m.bias.numpy().astype(np.float64)
            s_in = 1 / 127 if k == 0 else ACT_MAX / 127
            last = k == len(mods) - 1
            s_out = 1.0 if last else ACT_MAX / 127
            mult = alpha * s_in / s_out
            nin = t.shape[1]
            assert nin * 127 < 32768
            def fits(sh):
                return (np.abs(np.round(mult * 2 ** sh)).max() <= 32767
                        and np.abs(b / s_out * 2 ** sh).max() + nin * 127 * 32767 <= 2 ** 30)
            if not last and fits(16):
                sh = 16          # the Mac then takes the high word (swap) instead of shifting
            else:
                sh = 30
                while sh > 0 and not fits(sh):
                    sh -= 1
            M = np.round(mult * 2 ** sh).astype(np.int64)
            B = np.round(b / s_out * 2 ** sh).astype(np.int64)
            if not last:
                B += 1 << (sh - 1)        # round half up
            layers.append(dict(T=t, M=M, B=B, SH=sh, last=last))
    return layers


def int_forward(layers, obs):
    """integer actor: obs (n, NOBS) ints -> (logits int64 (n, 15), actions (n, 4))"""
    x = np.asarray(obs, dtype=np.int64)
    for L in layers:
        acc = x @ L["T"].T.astype(np.int64)
        assert np.abs(acc).max(initial=0) < 32768
        z = acc * L["M"][None, :] + L["B"][None, :]
        if L["last"]:
            x = z
        else:
            x = np.clip(z >> L["SH"], 0, 127)
    acts = []
    o = 0
    for h in HEADS:
        acts.append(np.argmax(x[:, o:o + h], axis=1))
        o += h
    return x, np.stack(acts, 1)


def save_int(layers, path):
    d = {}
    for k, L in enumerate(layers):
        for f in ("T", "M", "B"):
            d[f"{f}{k}"] = L[f]
        d[f"SH{k}"] = np.array(L["SH"])
    np.savez(path, **d)


def load_int(path):
    z = np.load(path)
    layers = []
    k = 0
    while f"T{k}" in z:
        layers.append(dict(T=z[f"T{k}"], M=z[f"M{k}"], B=z[f"B{k}"], SH=int(z[f"SH{k}"]), last=False))
        k += 1
    layers[-1]["last"] = True
    return layers
