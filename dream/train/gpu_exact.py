#!/usr/bin/env python3
"""Is the qat forward exact on this GPU? Compares, layer by layer, the student's qat forward on CUDA with
the same forward on the CPU (exact: test_student.py / export check), and tests the suspects one by one:
the power-of-two gains (torch.pow on CUDA) and the cuDNN convolutions.

  python gpu_exact.py <qat checkpoint>
"""
import sys

import torch

from distill import Grid, eval_noise, quant_in, student_sample
from student_tools import load_student


def layer_diffs(m_gpu, m_cpu, xq, s, t):
    m_gpu(xq.cuda(), s.cuda(), t.cuda())
    m_cpu(xq, s, t)
    out = []
    for name in m_cpu.last:
        d = (m_gpu.last[name].cpu() - m_cpu.last[name]).abs()
        out.append((name, float(d.max()), int((d > 0).sum())))
    return out


def main():
    path = sys.argv[1]
    torch.backends.cuda.matmul.allow_tf32 = False
    torch.backends.cudnn.allow_tf32 = False
    m_cpu, ck = load_student(path, "cpu")
    m_gpu, _ = load_student(path, "cuda")
    m_cpu.eval(), m_gpu.eval()
    G = Grid(len(ck["ts"]) - 1, ck["grid"]["kind"])
    pres = ck["present"][:8]
    S = [s for s in pres for _ in range(8)]
    z = eval_noise([1 + j for _ in pres for j in range(8)])
    s = torch.tensor(S)
    xs, _ = student_sample(m_cpu, z, s, G)
    # 1. gains: torch.pow(2, -k) on CUDA vs exact
    k = torch.arange(-3, 16, dtype=torch.float32)
    g_cuda = torch.pow(2.0, -k.cuda()).cpu()
    g_exact = torch.ldexp(torch.ones_like(k), (-k).int())
    print("pow(2,-k) on CUDA exact:", torch.equal(g_cuda, g_exact),
          "| exp2 on CUDA exact:", torch.equal(torch.exp2(-k.cuda()).cpu(), g_exact))
    # 1b. the per-step bias (b0 + subject + time) on GPU vs CPU, and its parts
    t = torch.full((len(S),), G.t[3])
    with torch.no_grad():
        bg, bc = m_gpu.bias(s.cuda(), t.cuda()).cpu(), m_cpu.bias(s, t)
        tg = torch.round(m_gpu.time_bias(t.cuda())).cpu()
        tc = torch.round(m_cpu.time_bias(t))
        tr = m_cpu.time_bias(t)
        sg, sc = m_gpu.subject_bias(s.cuda()).cpu(), m_cpu.subject_bias(s)
    print(f"bias GPU vs CPU: {int((bg != bc).sum())} of {bc.numel()} differ (max {float((bg - bc).abs().max())}); "
          f"rounded time bias differs in {int((tg != tc).sum())}; subject bias differs in {int((sg != sc).sum())}; "
          f"time-bias values within 1e-3 of .5: {int(((tr - tr.floor() - 0.5).abs() < 1e-3).sum())}")
    # 1c. manual layer-by-layer forward with the CPU bias fed to both devices
    import arch
    import torch.nn.functional as F
    from student import scatter_d2s, tab_range

    def manual(m, xq, bias):
        p = arch.CONFIGS[m.cfg]["patch"]
        x = torch.round(F.pixel_unshuffle(xq.clamp(-arch.IN_MAX, arch.IN_MAX), p))
        tens, outs = {"xt": x}, []
        for i, l in enumerate(m.L):
            acc = m.conv(i, tens[l["src"]], m.layer_weights(i))
            a_, b_ = m.offs[i]
            v = torch.floor(acc * m.gains(i)[None, :, None, None]) + bias[:, a_:b_, None, None]
            if l["kind"] == "pw" and l["scatter"] == "d2s":
                v = scatter_d2s(v, l["cout"] // 4)
            if l["mode"] == "addh":
                v = tens[l["dst"]] + v
            r = tab_range(l["tab"])
            tens[l["dst"]] = v if r is None else v.clamp(r[0], r[1])
            outs.append((l["tag"], acc, tens[l["dst"]]))
        return outs
    with torch.no_grad():
        xq = quant_in(xs[3])
        og = manual(m_gpu, xq.cuda(), bc.cuda())
        oc = manual(m_cpu, xq, bc)
    bad = [(tg_[0], float((ag.cpu() - ac).abs().max()), float((vg.cpu() - vc).abs().max()))
           for tg_, (_, ag, vg), (_, ac, vc) in zip(og, og, oc) if not torch.equal(vg.cpu(), vc)]
    print("same bias on both devices -> first differing layers (tag, acc diff, out diff):", bad[:5])
    # 2. layer by layer on a mid-trajectory state
    for i in (0, 3, 5):
        t = torch.full((len(S),), G.t[i])
        xq = quant_in(xs[i])
        d = layer_diffs(m_gpu, m_cpu, xq, s, t)
        first = next(((n, mx, c) for n, mx, c in d if mx > 0), None)
        print(f"step {i}: first differing tensor {first}; all: {[(n, mx) for n, mx, c in d if mx > 0][:8]}")
    # 3. with cuDNN off
    with torch.backends.cudnn.flags(enabled=False):
        t = torch.full((len(S),), G.t[3])
        d = layer_diffs(m_gpu, m_cpu, quant_in(xs[3]), s, t)
        print("cudnn disabled, step 3 differing:", [(n, mx) for n, mx, c in d if mx > 0][:8])
    # 4. full sampler GPU vs CPU
    xg, _ = student_sample(m_gpu, z.cuda(), s.cuda(), G)
    dd = (xg[-1].cpu() - xs[-1]).abs() * 512
    print(f"sampler GPU vs CPU qat: maxdiff {float(dd.max())} Q9, {int((dd > 0).sum())} pixels differ")


if __name__ == "__main__":
    main()
