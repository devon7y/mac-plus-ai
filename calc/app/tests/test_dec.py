#!/usr/bin/env python3
"""test_dec.py - randomized and hand-written tests for calc/app/dec.c (Scribble Calc arithmetic).

Run (takes a few seconds; rebuilds the host driver when dec.c, dec.h or test_dec.c change):
    python3 -I calc/app/tests/test_dec.py              # seed 1, default case counts
    python3 -I calc/app/tests/test_dec.py 7 3          # seed 7, 3x the case counts
The driver is calc/build/dectest/test_dec (host clang, -fsanitize=address,undefined).

What is checked (all must match exactly):
  * dec_add/sub/mul/div and dec_from_digits against Python's decimal module with
    Context(prec=18, ROUND_HALF_EVEN, Emax=999, Emin=-999): same value (sign, coefficient with
    trailing zeros stripped, exponent) or the same error (Overflow, division by zero).
  * dec_pow against a Python model of the same repeated-squaring algorithm (see dec.h).
  * dec_format / dec_to_text against an independent formatter built on decimal's ROUND_HALF_UP.
  * dec_eval on random expression trees (rendered to tokens with minimal or extra parentheses,
    implicit multiplication, unary minus, powers) evaluated directly with the same context,
    and on random/mutated token strings against a mirror parser (syntax errors, error position).
  * Every successful result is a valid Dec (m < 10^18, exponent in range, zero is +0 e0).
"""
import os
import random
import subprocess
import sys
from decimal import (Context, Decimal, DivisionByZero, InvalidOperation, Overflow,
                     ROUND_HALF_EVEN, ROUND_HALF_UP)

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(APP))
BUILD = os.path.join(ROOT, 'calc', 'build', 'dectest')
EXE = os.path.join(BUILD, 'test_dec')
SRCS = [os.path.join(APP, 'dec.c'), os.path.join(HERE, 'test_dec.c')]
DEPS = SRCS + [os.path.join(APP, 'dec.h')]

PREC, EMAX, EMIN = 18, 999, -999
ETINY = EMIN - PREC + 1
CTX = Context(prec=PREC, rounding=ROUND_HALF_EVEN, Emax=EMAX, Emin=EMIN, clamp=0,
              traps=[InvalidOperation, DivisionByZero, Overflow])
WIDE = Context(prec=60, rounding=ROUND_HALF_EVEN, Emax=10**6, Emin=-10**6)
OK, DIV0, OVF, DOMAIN, SYNTAX = range(5)
MAXDEPTH = 32


# ---------------------------------------------------------------- build / run

def build():
    global EXE
    if os.environ.get('DECTEST_EXE'):       # use a prebuilt driver (e.g. a mutated dec.c)
        EXE = os.environ['DECTEST_EXE']
        return
    os.makedirs(BUILD, exist_ok=True)
    if os.path.exists(EXE) and os.path.getmtime(EXE) > max(os.path.getmtime(p) for p in DEPS):
        return
    cmd = ['/usr/bin/clang', '-std=c99', '-O1', '-g', '-Wall', '-Wextra', '-pedantic',
           '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-o', EXE] + SRCS
    subprocess.run(cmd, check=True)


def run(lines):
    out = subprocess.run([EXE], input='\n'.join(lines) + '\n', capture_output=True, text=True)
    if out.returncode != 0:
        sys.stderr.write(out.stderr[-4000:])
        raise SystemExit('driver crashed (exit %d)' % out.returncode)
    res = out.stdout.split('\n')
    assert len(res) >= len(lines), (len(res), len(lines))
    return res[:len(lines)]


# ---------------------------------------------------------------- values

class Err(Exception):
    def __init__(self, code):
        Exception.__init__(self, code)
        self.code = code


def guard(fn, *args):
    try:
        return fn(*args)
    except Overflow:
        raise Err(OVF)
    except (DivisionByZero, InvalidOperation):
        raise Err(DIV0)


def dec_of(f):
    s, m, e = f
    return Decimal((s, tuple(int(c) for c in str(m)), e))


def canon(d):
    """(sign, coefficient without trailing zeros, exponent); zero -> (0, 0, 0)"""
    if d.is_zero():
        return (0, 0, 0)
    s, digs, ex = d.as_tuple()
    c = int(''.join(map(str, digs)))
    while c % 10 == 0:
        c //= 10
        ex += 1
    return (s, c, ex)


def canon_f(s, m, e):
    if m == 0:
        return (0, 0, 0)
    while m % 10 == 0:
        m //= 10
        e += 1
    return (s, m, e)


def valid(s, m, e):
    if m == 0:
        return s == 0 and e == 0
    return m < 10**PREC and e >= ETINY and e + len(str(m)) - 1 <= EMAX and s in (0, 1)


def fields(s):
    return '%d %d %d' % s


def rcoef(rng):
    r = rng.random()
    if r < 0.4:
        nd = 18
    elif r < 0.6:
        nd = rng.randint(1, 3)
    else:
        nd = rng.randint(1, 18)
    c = rng.randint(10**(nd - 1), 10**nd - 1)
    r = rng.random()
    if r < 0.05:
        c = 10**(nd - 1)
    elif r < 0.12:
        c = 10**nd - 1
    elif r < 0.2 and nd > 1:
        k = rng.randint(1, nd - 1)
        c = max(1, c // 10**k) * 10**k
    return c


def radj(rng):
    r = rng.random()
    if r < 0.55:
        return rng.randint(-12, 12)
    if r < 0.75:
        return rng.randint(EMIN, EMAX)
    if r < 0.87:
        return rng.randint(EMAX - 25, EMAX)
    return rng.randint(ETINY, EMIN + 25)


def mk(s, c, adj):
    adj = max(ETINY, min(EMAX, adj))
    e = adj - len(str(c)) + 1
    if e < ETINY:
        c //= 10**(ETINY - e)
        e = ETINY
        if c == 0:
            c = 1
    return (s, c, e)


def rdec(rng):
    if rng.random() < 0.03:
        return (0, 0, 0)
    return mk(rng.getrandbits(1), rcoef(rng), radj(rng))


def adjof(f):
    return f[2] + len(str(f[1])) - 1 if f[1] else 0


# ---------------------------------------------------------------- binary operations

def pair(rng, op):
    a = rdec(rng)
    r = rng.random()
    s = rng.getrandbits(1)
    if r < 0.35 or a[1] == 0:
        return a, rdec(rng)
    if r < 0.55:                            # nearby exponents (alignment boundaries)
        return a, mk(s, rcoef(rng), adjof(a) + rng.randint(-42, 42))
    if r < 0.62:                            # far apart exponents
        return a, mk(s, rcoef(rng), adjof(a) + rng.choice((-1, 1)) * rng.randint(30, 2100))
    if op in 'as' and r < 0.7:              # b truncated below a's last digit (d > 19), with the
        t = rng.randint(1, 17)              # kept part of b ending in 0...0 or 50...0
        j = rng.randint(1, 18 - t)
        yt = rng.randint(0, 10**(18 - t) - 1) // 10**j * 10**j + rng.choice((0, 5 * 10**(j - 1)))
        mb = yt * 10**t + rng.randint(1, 10**t - 1)
        ea = rng.randint(-20, 20)
        return (rng.getrandbits(1), rng.randint(1, 10**rng.randint(1, 4) - 1), ea), (s, mb, ea - 19 - t)
    if op in 'as' and r < 0.8:              # cancellation
        c = a[1] + rng.randint(-1000, 1000)
        c = min(max(c, 1), 10**PREC - 1)
        sh = rng.randint(0, 3)
        e = a[2] - sh
        if e >= ETINY and c * 10**sh < 10**PREC:
            return a, (s, c * 10**sh, e)
        return a, (s, c, a[2])
    if op in 'as':                          # exact ties and carries at the 19th digit
        c = rng.choice((rng.randint(10**17, 10**18 - 1), 10**18 - 1, 10**17))
        e = max(a[2], ETINY + 3)
        e = min(e, EMAX - 17)
        k = rng.randint(1, 3)
        b = (s, 5 * 10**(k - 1), e - k)
        if rng.random() < 0.5:
            b = (s, rng.randint(1, 9) * 10**(k - 1), e - k)
        return (rng.getrandbits(1), c, e), b
    if op == 'm':                           # products whose dropped part is exactly a half
        b = (s, rng.choice((5, 25, 125, 5 * 10**rng.randint(0, 5), 2, 4, 8)), rng.randint(-5, 5))
        c = rng.randint(10**17, 10**18 - 1)
        return (rng.getrandbits(1), c | rng.getrandbits(1), rng.randint(-30, 30) + rng.choice((0, 0, 980, -980))), b
    # division: exact halves, repeating quotients, a ~ b
    c = rng.randint(10**17, 10**18 - 1) | 1
    d = rng.choice((2, 4, 8, 16, 5, 25, 3, 7, 9, 11, 13, 17, 99, 10**17 + 1, c, c + 2, c - 2))
    if rng.random() < 0.3:
        d = c
    return (rng.getrandbits(1), c, rng.randint(-20, 20)), (s, d, rng.randint(-20, 20))


def ref_binop(op, a, b):
    da, db = dec_of(a), dec_of(b)
    fn = {'a': CTX.add, 's': CTX.subtract, 'm': CTX.multiply, 'd': CTX.divide}[op]
    try:
        return OK, canon(guard(fn, da, db))
    except Err as x:
        return x.code, None


def model_pow(b, x):
    """Same algorithm as dec_pow; raises Err."""
    n = 0
    if not x.is_zero():
        if x != x.to_integral_value():
            raise Err(DOMAIN)
        n = int(x)
        if abs(n) > 9999:
            raise Err(DOMAIN)
    if n == 0:
        return Decimal(1)
    if b.is_zero():
        if n < 0:
            raise Err(DIV0)
        return Decimal(0)
    k, res, p = abs(n), Decimal(1), b
    try:
        while True:
            if k & 1:
                res = CTX.multiply(res, p)
            k >>= 1
            if not k:
                break
            p = CTX.multiply(p, p)
    except Overflow:
        if n < 0:
            return Decimal(0)
        raise Err(OVF)
    if n < 0:
        if res.is_zero():
            raise Err(OVF)
        return guard(CTX.divide, Decimal(1), res)
    return res


def pow_case(rng):
    r = rng.random()
    if r < 0.6:
        nd = rng.randint(1, 4)
        base = mk(rng.getrandbits(1), rng.randint(1, 10**nd - 1), rng.randint(-4, 4))
    elif r < 0.7:                       # close to 1 (long powers stay in range)
        k = rng.randint(1, 17)
        base = (rng.getrandbits(1), 10**k + rng.choice((1, -1, 7, -3)), -k)
    elif r < 0.75:
        base = (0, 0, 0)
    else:
        base = rdec(rng)
    r = rng.random()
    if r < 0.55:
        n = rng.randint(-40, 40)
    elif r < 0.7:
        n = rng.choice((-1, 1)) * rng.randint(40, 9999)
    elif r < 0.75:
        n = rng.choice((9999, -9999, 10000, -10000, 123456))
    else:
        n = None
    if n is None:                       # non-integer or odd forms
        ex = rng.choice(((0, 15, -1), (0, 5, -1), (1, 333333333333333333, -18), (0, 1, 3), (0, 1, 4),
                         (0, 2000, -3), (1, 30, -1), (0, 0, 0), (0, 3, 2), (0, 99999, -1),
                         (1, 999900, -2), (0, 1, 999), (0, 1, -1016), rdec(rng)))
    else:
        sh = rng.randint(0, 3) if n and abs(n) < 10**(18 - 3) else 0
        ex = (1 if n < 0 else 0, abs(n) * 10**sh, -sh)
        if rng.random() < 0.1 and n and n % 10 == 0:
            ex = (ex[0], abs(n) // 10, 1)
    return base, ex


# ---------------------------------------------------------------- formatting reference

def ref_format(d, sig):
    sig = max(1, min(PREC, sig))
    if d.is_zero():
        return (0, 0, '0')
    q = Context(prec=sig, rounding=ROUND_HALF_UP, Emax=10**6, Emin=-10**6).plus(d)
    q = q.normalize(WIDE)
    adj = q.adjusted()
    if -6 <= adj <= 11:
        return (0, 0, format(q, 'f'))
    return (1, adj, format(q.scaleb(-adj, WIDE), 'f'))


def ref_text(d):
    sci, x, s = ref_format(d, 12)
    return s + ('E%d' % x if sci else '')


def fmt_case(rng):
    r = rng.random()
    if r < 0.5:
        return rdec(rng)
    s = rng.getrandbits(1)
    if r < 0.8:                     # around the fixed/scientific boundaries, with carries
        adj = rng.choice((-8, -7, -6, -5, 10, 11, 12, 13, rng.randint(-20, 20)))
        k = rng.randint(1, 18)
        c = int('9' * k + ''.join(rng.choice('0123456789') for _ in range(18 - k)))
        if rng.random() < 0.3:
            c = int('9' * 12 + rng.choice('456') + '0' * 5)
        return mk(s, c, adj)
    if r < 0.9:                     # half-up ties at the 12th digit
        c = rng.randint(10**11, 10**12 - 1) * 10**6 + 5 * 10**5
        return mk(s, c, rng.randint(-15, 15))
    return mk(s, rng.randint(1, 10**rng.randint(1, 18) - 1), rng.randint(-1016, 999))


# ---------------------------------------------------------------- expressions

TOKCH = '0123456789+-*/=().^'
T_PLUS, T_MINUS, T_TIMES, T_DIV, T_EQ, T_LPAR, T_RPAR, T_POINT, T_POW = range(10, 19)
LEVEL = {'add': 1, 'sub': 1, 'mul': 2, 'div': 2, 'imul': 2, 'neg': 3, 'pow': 4, 'num': 5}
OPCH = {'add': '+', 'sub': '-', 'mul': '*', 'div': '/', 'pow': '^'}


def rnum(rng):
    r = rng.random()
    d = lambda k: ''.join(rng.choice('0123456789') for _ in range(k))
    if r < 0.45:
        s = str(rng.randint(0, 10**rng.randint(1, 4) - 1))
    elif r < 0.7:
        s = str(rng.randint(0, 999)) + '.' + d(rng.randint(1, 4))
    elif r < 0.77:
        s = str(rng.randint(0, 99)) + '.'
    elif r < 0.85:
        s = '.' + d(rng.randint(1, 4))
    elif r < 0.93:
        s = d(rng.randint(15, 30))
        if rng.random() < 0.5:
            i = rng.randint(0, len(s))
            s = s[:i] + '.' + s[i:]
    else:
        s = rng.choice(('0', '0.0', '00', '.0', '0.', '1', '2', '10'))
    if rng.random() < 0.05:
        s = '00' + s
    return s


def gen(rng, depth):
    if depth <= 0 or rng.random() < 0.25:
        return ('num', rnum(rng))
    r = rng.random()
    g = lambda: gen(rng, depth - 1)
    if r < 0.17:
        return ('add', g(), g())
    if r < 0.32:
        return ('sub', g(), g())
    if r < 0.47:
        return ('mul', g(), g())
    if r < 0.6:
        return ('div', g(), g())
    if r < 0.7:
        return ('imul', g(), g())
    if r < 0.8:
        return ('neg', g())
    r = rng.random()
    if r < 0.6:
        ex = ('num', str(rng.randint(0, 9)))
    elif r < 0.8:
        ex = ('neg', ('num', str(rng.randint(0, 6))))
    else:
        ex = g()
    return ('pow', g(), ex)


def render(node, minlev, rng):
    k = node[0]
    if k == 'num':
        s = node[1]
    elif k in ('add', 'sub'):
        s = render(node[1], 1, rng) + OPCH[k] + render(node[2], 2, rng)
    elif k in ('mul', 'div'):
        s = render(node[1], 2, rng) + OPCH[k] + render(node[2], 3, rng)
    elif k == 'imul':
        s = render(node[1], 2, rng) + '(' + render(node[2], 1, rng) + ')'
    elif k == 'neg':
        s = '-' + render(node[1], 3, rng)
    else:
        s = render(node[1], 5, rng) + '^' + render(node[2], 3, rng)
    if LEVEL[k] < minlev or rng.random() < 0.04:
        s = '(' + s + ')'
    return s


def ev(node):
    k = node[0]
    if k == 'num':
        return guard(CTX.create_decimal, node[1])
    if k == 'neg':
        return CTX.minus(ev(node[1]))
    a = ev(node[1])
    b = ev(node[2])
    if k == 'add':
        return guard(CTX.add, a, b)
    if k == 'sub':
        return guard(CTX.subtract, a, b)
    if k in ('mul', 'imul'):
        return guard(CTX.multiply, a, b)
    if k == 'div':
        return guard(CTX.divide, a, b)
    return model_pow(a, b)


class Syn(Exception):
    def __init__(self, pos):
        Exception.__init__(self, pos)
        self.pos = pos


def tokens_of(s):
    """token values: the index in TOKCH ('=' is T_EQ), anything else is 99"""
    return [TOKCH.index(c) if c in TOKCH else 99 for c in s]


def mirror_parse(s):
    """Mirror of dec.c's parser: returns an AST or raises Syn(pos)."""
    t = tokens_of(s)
    n = len(t)
    st = {'pos': 0, 'depth': 0}

    def peek():
        return t[st['pos']] if st['pos'] < n else 255

    def unary():
        st['depth'] += 1
        if st['depth'] > MAXDEPTH:
            raise Syn(st['pos'])
        c = peek()
        if c == T_MINUS:
            st['pos'] += 1
            node = ('neg', unary())
        else:
            if c == T_LPAR:
                st['pos'] += 1
                node = expr()
                if peek() != T_RPAR:
                    raise Syn(st['pos'])
                st['pos'] += 1
            else:
                s0, pp, nd = st['pos'], -1, 0
                while peek() <= 9 or peek() == T_POINT:
                    if peek() == T_POINT:
                        if pp >= 0:
                            break
                        pp = nd
                    else:
                        nd += 1
                    st['pos'] += 1
                if nd == 0:
                    st['pos'] = s0
                    raise Syn(s0)
                node = ('num', s[s0:st['pos']])
            if peek() == T_POW:
                st['pos'] += 1
                node = ('pow', node, unary())
        st['depth'] -= 1
        return node

    def term():
        v = unary()
        while True:
            c = peek()
            if c in (T_TIMES, T_DIV):
                st['pos'] += 1
            elif c != T_LPAR:
                return v
            w = unary()
            v = ('div' if c == T_DIV else 'mul', v, w)

    def expr():
        v = term()
        while peek() in (T_PLUS, T_MINUS):
            c = peek()
            st['pos'] += 1
            w = term()
            v = ('add' if c == T_PLUS else 'sub', v, w)
        return v

    node = expr()
    if st['pos'] < n:
        raise Syn(st['pos'])
    return node


def eval_ref(node):
    try:
        return OK, canon(ev(node))
    except Err as x:
        return x.code, None


def fuzz_string(rng, valid_pool):
    r = rng.random()
    alpha = '0123456789+-*/().^=?'
    weights = [3] * 10 + [4, 5, 4, 4, 5, 5, 4, 3, 1, 1]
    if r < 0.35 and valid_pool:
        s = list(rng.choice(valid_pool))
        for _ in range(rng.randint(1, 3)):
            op = rng.random()
            i = rng.randint(0, len(s))
            if op < 0.35 and s:
                del s[min(i, len(s) - 1)]
            elif op < 0.7:
                s.insert(i, rng.choices(alpha, weights)[0])
            elif s:
                s[min(i, len(s) - 1)] = rng.choices(alpha, weights)[0]
        return ''.join(s)
    if r < 0.85:
        return ''.join(rng.choices(alpha, weights, k=rng.randint(0, 14)))
    k = rng.randint(28, 36)
    return rng.choice(('(' * k + '1' + ')' * k, '-' * k + '2', '2^' * k + '1', '(-' * k + '3' + ')' * k,
                       '(' * k + '1', '1' + ')' * k))


# ---------------------------------------------------------------- hand-written cases

HAND = [
    # (expression, rc, sci, exp10, display text, clipboard text)
    ('12*(3.5+4)', OK, 0, 0, '90', '90'),        # 12 x 7.5 (the brief's "=114" is a typo)
    ('12*(3.5+6)', OK, 0, 0, '114', '114'),
    ('12(3.5+4)', OK, 0, 0, '90', '90'),
    ('1/3', OK, 0, 0, '0.333333333333', '0.333333333333'),
    ('2/3', OK, 0, 0, '0.666666666667', '0.666666666667'),
    ('1/3*3', OK, 0, 0, '1', '1'),
    ('0.1+0.2', OK, 0, 0, '0.3', '0.3'),
    ('10/0', DIV0, 0, 0, '0', '0'),
    ('0/0', DIV0, 0, 0, '0', '0'),
    ('99999999999*99999999999', OK, 1, 21, '9.9999999998', '9.9999999998E21'),
    ('123456789012', OK, 0, 0, '123456789012', '123456789012'),
    ('1234567890123', OK, 1, 12, '1.23456789012', '1.23456789012E12'),
    ('0.0000001', OK, 1, -7, '1', '1E-7'),
    ('0.000001', OK, 0, 0, '0.000001', '0.000001'),
    ('-0', OK, 0, 0, '0', '0'),
    ('2^10', OK, 0, 0, '1024', '1024'),
    ('2^-2', OK, 0, 0, '0.25', '0.25'),
    ('-2^2', OK, 0, 0, '-4', '-4'),
    ('2^3^2', OK, 0, 0, '512', '512'),
    ('(1+1)(2+2)', OK, 0, 0, '8', '8'),
    ('2(3+4)', OK, 0, 0, '14', '14'),
    ('(-8)^(1/3)', DOMAIN, 0, 0, '0', '0'),
    ('1.5E15', SYNTAX, 0, 0, '0', '0'),
    ('15*10^14', OK, 1, 15, '1.5', '1.5E15'),
    ('15/10^8', OK, 1, -7, '1.5', '1.5E-7'),
    ('999999999999.5', OK, 1, 12, '1', '1E12'),
    ('-999999999999.4', OK, 0, 0, '-999999999999', '-999999999999'),
    ('10^999*10', OVF, 0, 0, '0', '0'),
    ('10^-999/10^17', OK, 1, -1016, '1', '1E-1016'),
    ('10^-999/10^18', OK, 0, 0, '0', '0'),
]


# ---------------------------------------------------------------- main

def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    scale = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
    rng = random.Random(seed)
    build()
    N = lambda k: int(k * scale)

    lines, checks = [], []

    # binary operations
    for op, cnt in (('a', 30000), ('s', 30000), ('m', 30000), ('d', 30000)):
        for _ in range(N(cnt)):
            a, b = pair(rng, op)
            if rng.random() < 0.5:
                a, b = b, a
            lines.append('B %s %s %s' % (op, fields(a), fields(b)))
            checks.append(('op' + op, (a, b), ref_binop(op, a, b)))
    for _ in range(N(10000)):
        base, ex = pow_case(rng)
        try:
            exp = (OK, canon(model_pow(dec_of(base), dec_of(ex))))
        except Err as x:
            exp = (x.code, None)
        lines.append('B p %s %s' % (fields(base), fields(ex)))
        checks.append(('pow', (base, ex), exp))

    # dec_from_digits
    for _ in range(N(20000)):
        r = rng.random()
        if r < 0.7:
            n = rng.randint(1, 30)
        elif r < 0.9:
            n = rng.randint(30, 70)
        else:
            n = rng.randint(900, 1100)
        lead = rng.randint(0, n - 1) if rng.random() < 0.3 else 0
        body = ''.join(rng.choice('0123456789') for _ in range(n - lead))
        if rng.random() < 0.15:
            body = '9' * len(body)
        if n > 100 and rng.random() < 0.7:
            body = body[:rng.randint(1, 25)].ljust(len(body), '0')
        digits = '0' * lead + body
        pp = rng.randint(0, n) if rng.random() < 0.9 else rng.randint(-1100, n + 1100)
        try:
            exp = (OK, canon(guard(CTX.create_decimal, '%sE%d' % (digits, pp - n))))
        except Err as x:
            exp = (x.code, None)
        lines.append('N %d %s' % (pp, digits))
        checks.append(('digits', (digits, pp), exp))
    lines.append('N 0 12a3')
    checks.append(('digits', ('12a3', 0), (SYNTAX, None)))

    # formatting
    for _ in range(N(20000)):
        a = fmt_case(rng)
        sig = 12 if rng.random() < 0.6 else rng.randint(0, 19)
        lines.append('F %d %s' % (sig, fields(a)))
        checks.append(('format', (a, sig), ref_format(dec_of(a), sig)))
    for _ in range(N(5000)):
        a = fmt_case(rng)
        lines.append('T %s' % fields(a))
        checks.append(('text', a, ref_text(dec_of(a))))

    # expressions: random trees
    pool = []
    for _ in range(N(20000)):
        node = gen(rng, rng.randint(1, 6))
        s = render(node, 1, rng)
        if len(pool) < 3000:
            pool.append(s)
        lines.append('E ' + s)
        checks.append(('eval', s, eval_ref(node)))
    # expressions: fuzzed token strings against the mirror parser
    for _ in range(N(20000)):
        s = fuzz_string(rng, pool)
        try:
            exp = eval_ref(mirror_parse(s))
        except Syn as x:
            exp = (SYNTAX, x.pos)
        lines.append('E ' + s)
        checks.append(('fuzz', s, exp))
    for h in HAND:
        lines.append('E ' + h[0])
        checks.append(('hand', h[0], h))

    out = run(lines)

    fails, counts, rcs = [], {}, {}
    sub = 0
    for (kind, args, exp), got in zip(checks, out):
        counts[kind] = counts.get(kind, 0) + 1
        if kind not in ('format', 'text'):
            rcs.setdefault(kind, [0] * 5)[exp[1] if kind == 'hand' else exp[0]] += 1
            if kind != 'hand' and exp[0] == OK and exp[1][1] and \
                    exp[1][2] + len(str(exp[1][1])) - 1 < EMIN:
                sub += 1
        f = got.split()
        bad = None
        if kind in ('opa', 'ops', 'opm', 'opd', 'pow', 'digits'):
            rc, s, m, e = int(f[0]), int(f[1]), int(f[2]), int(f[3])
            if rc != exp[0]:
                bad = 'rc %d, expected %d (%s)' % (rc, exp[0], exp[1])
            elif rc == OK and not valid(s, m, e):
                bad = 'invalid Dec %s' % got
            elif rc == OK and canon_f(s, m, e) != exp[1]:
                bad = 'got %s, expected %s' % (canon_f(s, m, e), exp[1])
        elif kind == 'format':
            if len(f) != 3 or (int(f[0]), int(f[1]), f[2]) != exp:
                bad = 'got %r, expected %r' % (got, exp)
        elif kind == 'text':
            if got != exp:
                bad = 'got %r, expected %r' % (got, exp)
        else:
            rc, ep, s, m, e = map(int, f[:5])
            sci, x10, disp, txt = int(f[5]), int(f[6]), f[7], f[8]
            if kind == 'hand':
                want = exp
                if (rc, sci, x10, disp, txt) != want[1:]:
                    bad = 'got %r, expected %r' % ((rc, sci, x10, disp, txt), want[1:])
            elif rc != exp[0]:
                bad = 'rc %d errpos %d, expected %r' % (rc, ep, exp)
            elif rc == SYNTAX:
                if ep != exp[1]:
                    bad = 'errpos %d, expected %d' % (ep, exp[1])
            elif ep != -1:
                bad = 'errpos %d for rc %d' % (ep, rc)
            elif rc != OK:
                if (s, m, e) != (0, 0, 0):
                    bad = 'result not zeroed on error: %s' % got
            elif not valid(s, m, e):
                bad = 'invalid Dec %s' % got
            elif canon_f(s, m, e) != exp[1]:
                bad = 'got %s, expected %s' % (canon_f(s, m, e), exp[1])
            else:
                d = Decimal((s, tuple(int(c) for c in str(m)), e))
                if (sci, x10, disp) != ref_format(d, 12) or txt != ref_text(d):
                    bad = 'format %r / %r, expected %r / %r' % ((sci, x10, disp), txt,
                                                                ref_format(d, 12), ref_text(d))
        if bad:
            fails.append((kind, args, bad))

    total = sum(counts.values())
    print('seed %d: %d cases  ' % (seed, total) + '  '.join('%s=%d' % kv for kv in sorted(counts.items())))
    print('expected outcomes (ok/div0/overflow/domain/syntax): ' +
          '  '.join('%s=%s' % (k, '/'.join(map(str, v))) for k, v in sorted(rcs.items())) +
          '  subnormal results=%d' % sub)
    for kind, args, bad in fails[:25]:
        print('FAIL %s %r: %s' % (kind, args, bad))
    print('%d failures' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
