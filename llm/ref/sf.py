"""Scale floats (SF): the tiny fixed-point floating format the Mac uses for every per-token scale.

An SF is a pair (m, e) meaning m * 2**e, with m = 0 (the zero scale, e = 0) or 2**14 <= m < 2**15,
so m fits a positive signed 16-bit word and two mantissas multiply with one muls.w/mulu.w.
Every operation is exact integer arithmetic (truncating), defined so the 68000 can reproduce it
bit for bit. See SPEC.md section 3.
"""
import math

import numpy as np

ZERO = (0, 0)
M_LO, M_HI = 1 << 14, 1 << 15


def norm(v, e=0):
    """Normalize a non-negative integer v * 2**e: truncating right shift or exact left shift."""
    v = int(v)
    if v < 0:
        raise ValueError("SF is non-negative")
    if v == 0:
        return ZERO
    b = v.bit_length()
    if b > 15:
        return (v >> (b - 15), e + b - 15)
    return (v << (15 - b), e - (15 - b))


def from_int(v):
    return norm(v, 0)


def pow2(k):
    return (M_LO, k - 14)


def mul(a, b):
    if a[0] == 0 or b[0] == 0:
        return ZERO
    return norm(a[0] * b[0], a[1] + b[1])


def div(a, b):
    """a / b. Quotient (a.m << 15) // b.m lies in (2**14, 2**16): one divu.w. x / 0 := 0."""
    if a[0] == 0 or b[0] == 0:
        return ZERO
    return norm((a[0] << 15) // b[0], a[1] - b[1] - 15)


def sqrt_int(v):
    """sqrt of a positive integer v < 2**32, as an SF: scale v into [2**28, 2**30) by an even shift."""
    v = int(v)
    if v <= 0:
        return ZERO
    b = v.bit_length()
    s = b - 30 if (b - 30) % 2 == 0 else b - 29
    vv = v >> s if s >= 0 else v << -s
    return norm(math.isqrt(vv), s // 2)


def less(a, b):
    """a < b for SFs (zero is the smallest)."""
    if a[0] == 0:
        return b[0] != 0
    if b[0] == 0:
        return False
    return (a[1], a[0]) < (b[1], b[0])


def smax(a, b):
    return b if less(a, b) else a


def apply(x, f):
    """round(x * f) with round-half-up: (x*m + 2**(k-1)) >> k for k = -e > 0, x*m << e otherwise.
    x is an int or an int64 numpy array with |x| <= 32768 (one muls.w). Results are exact integers;
    callers clamp to their target range."""
    m, e = f
    if isinstance(x, np.ndarray):
        if m == 0:
            return np.zeros_like(x, dtype=np.int64)
        assert np.abs(x).max(initial=0) <= 32768, "sf.apply input must be 16-bit"
        p = x.astype(np.int64) * m
        if e >= 0:
            return p << e   # callers keep e small; never reached in practice (see SPEC)
        k = -e
        if k >= 31:
            return np.zeros_like(p)
        return (p + (1 << (k - 1))) >> k
    x = int(x)
    assert abs(x) <= 32768, "sf.apply input must be 16-bit"
    if m == 0:
        return 0
    p = x * m
    if e >= 0:
        return p << e
    k = -e
    return (p + (1 << (k - 1))) >> k if k < 31 else 0


def from_float(f):
    """Export-time only: nearest SF to a positive float (0 -> ZERO)."""
    if f <= 0:
        return ZERO
    fr, ex = math.frexp(f)          # f = fr * 2**ex, 0.5 <= fr < 1
    m = int(round(fr * (1 << 15)))  # in [2**14, 2**15]
    e = ex - 15
    if m == 1 << 15:
        m, e = 1 << 14, e + 1
    return (m, e)


def value(a):
    return a[0] * 2.0 ** a[1]


# Constants used by the forward pass (literal values fixed in SPEC.md; checked at import).
SQRT_D = (25905, -11)        # sqrt(160)
ATT_C = (20724, -11)         # 64 / sqrt(40): attention scores in 1/64-nat units
assert SQRT_D == from_float(math.sqrt(160)), from_float(math.sqrt(160))
assert ATT_C == from_float(64 / math.sqrt(40)), from_float(64 / math.sqrt(40))
