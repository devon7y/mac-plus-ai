#!/usr/bin/env python3
"""Fixed-point lookup tables shared by the 68000 code (gen_bench.py), the bit-exact Python
reference (ref.py) and quantization-aware training (paint/train/). Every table is a list of
signed 16-bit integers. Each entry is evaluated at the MIDPOINT of its input bin.

Index conventions (v is the clamped input value, >> is an arithmetic shift = floor):
  SIG [1024]  sigmoid, Q14 out.   v = pre-activation Q10 clamped to [-8192, 8191];  i = (v >> 4) + 512
  TANH[1024]  tanh,    Q14 out.   v = Q10 clamped to [-4096, 4095];  i = (v >> 3) + 512
              (also used for tanh(c) of an LSTM cell: c in Q12 clamped to [-16384, 16383]; i = (c >> 5) + 512)
  SECH[1024]  sech = sqrt(1 - tanh^2), Q14, same index as TANH
  EXPS[768]   exp, Q10 out, saturating at 32767.  v = Q10 clamped to [-8192, 4095];  i = (v >> 4) + 512
  EXPP[512]   exp(-d), Q11 out (2048 = 1.0).  d = Q10 difference >= 0;  i = min(d >> 5, 511)
  RTAB[1024]  sqrt(-2 ln u), Q12 out, u = (i + 0.5) / 1024 (Box-Muller radius)
  COS [1024]  cos(2 pi (i + 0.5) / 1024), Q14 out; sin uses index (i + 768) & 1023
"""
import math


def _r(x):
    return int(math.floor(x + 0.5))


def _clip16(x):
    return max(-32768, min(32767, x))


SIG = [_r(16384 / (1 + math.exp(-((i - 512 + 0.5) / 64)))) for i in range(1024)]
TANH = [_r(16384 * math.tanh((i - 512 + 0.5) / 128)) for i in range(1024)]
SECH = [_r(16384 / math.cosh((i - 512 + 0.5) / 128)) for i in range(1024)]
EXPS = [_clip16(_r(1024 * math.exp((i - 512 + 0.5) / 64))) for i in range(768)]
EXPP = [_r(2048 * math.exp(-(i + 0.5) / 32)) for i in range(512)]
RTAB = [_r(4096 * math.sqrt(-2 * math.log((i + 0.5) / 1024))) for i in range(1024)]
COS = [_r(16384 * math.cos(2 * math.pi * (i + 0.5) / 1024)) for i in range(1024)]

TABLES = {"SIG": SIG, "TANH": TANH, "SECH": SECH, "EXPS": EXPS, "EXPP": EXPP, "RTAB": RTAB, "COS": COS}
CENTER = {"SIG": 512, "TANH": 512, "SECH": 512, "EXPS": 512, "EXPP": 0, "RTAB": 0, "COS": 0}

assert all(-32768 <= v <= 32767 for t in TABLES.values() for v in t)
assert max(SIG) < 16384 and min(SIG) > 0 and max(abs(v) for v in TANH) < 16384
