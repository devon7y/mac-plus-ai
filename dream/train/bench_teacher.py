#!/usr/bin/env python3
"""Throughput of the teacher on this GPU: training it/s (batch 256) and guided sampling evals/s, for
plain / channels_last / torch.compile variants. Also times the student's training step."""
import time

import torch

from teacher import TeacherUNet


def bench_train(model, B=256, n=30, cl=False, comp=False):
    dev = "cuda"
    m = model.to(dev)
    if cl:
        m = m.to(memory_format=torch.channels_last)
    f = torch.compile(m) if comp else m
    opt = torch.optim.AdamW(m.parameters(), lr=1e-4, fused=True)
    x = torch.randn(B, 1, 64, 64, device=dev)
    if cl:
        x = x.contiguous(memory_format=torch.channels_last)
    t = torch.rand(B, device=dev)
    y = torch.randint(0, 20, (B,), device=dev)
    for i in range(n + 5):
        if i == 5:
            torch.cuda.synchronize()
            t0 = time.time()
        with torch.autocast("cuda", dtype=torch.bfloat16):
            loss = f(x, t, y).float().pow(2).mean()
        opt.zero_grad(set_to_none=True)
        loss.backward()
        opt.step()
    torch.cuda.synchronize()
    return n / (time.time() - t0)


@torch.no_grad()
def bench_infer(model, B=2048, n=10, cl=False, comp=False):
    dev = "cuda"
    m = model.to(dev).eval()
    if cl:
        m = m.to(memory_format=torch.channels_last)
    f = torch.compile(m) if comp else m
    x = torch.randn(B, 1, 64, 64, device=dev)
    t = torch.rand(B, device=dev)
    y = torch.randint(0, 20, (B,), device=dev)
    for i in range(n + 3):
        if i == 3:
            torch.cuda.synchronize()
            t0 = time.time()
        with torch.autocast("cuda", dtype=torch.bfloat16):
            f(x, t, y)
    torch.cuda.synchronize()
    return n * B / (time.time() - t0)


def main():
    torch.backends.cuda.matmul.allow_tf32 = True
    torch.backends.cudnn.allow_tf32 = True
    torch.backends.cudnn.benchmark = True
    for cl, comp in ((False, False), (True, False), (False, True), (True, True)):
        torch.manual_seed(0)
        tr = bench_train(TeacherUNet(20), cl=cl, comp=comp)
        inf = bench_infer(TeacherUNet(20), cl=cl, comp=comp)
        print(f"channels_last={cl} compile={comp}: train {tr:.2f} it/s (bs 256), inference {inf:.0f} img/s",
              flush=True)
    # student training step (float and qat), batch 256
    torch.backends.cuda.matmul.allow_tf32 = False
    torch.backends.cudnn.allow_tf32 = False
    from student import DreamStudent
    m = DreamStudent("A64L2", n_subjects=250).cuda()
    x = torch.randn(256, 1, 64, 64, device="cuda") * 512
    s = torch.randint(0, 250, (256,), device="cuda")
    t = torch.rand(256, device="cuda")
    m.calibrate(x, s, t)
    opt = torch.optim.Adam(m.parameters(), lr=1e-4)
    for mode in ("float", "qat"):
        if mode == "qat":
            m.to_qat()
        for i in range(25):
            if i == 5:
                torch.cuda.synchronize()
                t0 = time.time()
            loss = (m(x, s, t) / 512).pow(2).mean()
            opt.zero_grad()
            loss.backward()
            opt.step()
        torch.cuda.synchronize()
        print(f"student {mode}: {20 / (time.time() - t0):.1f} it/s (bs 256)", flush=True)


if __name__ == "__main__":
    main()
