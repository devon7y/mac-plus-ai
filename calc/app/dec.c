/* dec.c - decimal floating point and expression evaluator for Scribble Calc (see dec.h).
 *
 * Wide intermediates (products up to 36 digits, aligned sums up to 37 digits) are kept in
 * 128-bit integers made of eight 16-bit limbs, least significant first, so the 68000 can use
 * MULU.W (16x16->32) for products and DIVU.W-sized steps (32/16) for division by 10 and 10^4.
 * Every result goes through finish(), which rounds the exact value (an integer times a power
 * of ten, plus a "sticky" flag for nonzero digits already dropped below it) to 18 digits
 * half-even exactly like Python's decimal (_fix), including gradual underflow and overflow.
 */
#include "dec.h"

typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define NL 8                    /* 16-bit limbs in a wide integer */
#define PREC DEC_PREC
#define ETINY DEC_ETINY

static const u64 P10[20] = {
    1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL, 100000ULL, 1000000ULL, 10000000ULL,
    100000000ULL, 1000000000ULL, 10000000000ULL, 100000000000ULL, 1000000000000ULL,
    10000000000000ULL, 100000000000000ULL, 1000000000000000ULL, 10000000000000000ULL,
    100000000000000000ULL, 1000000000000000000ULL, 10000000000000000000ULL
};

static const Dec ONE = { 1, 0, 0, 0 };

/* number of decimal digits (0 for 0) */
static int ndig(u64 v)
{
    int k = 0;
    while (k < 20 && v >= P10[k]) k++;
    return k;
}

static u64 mul10(u64 v)
{
    u64 t2 = v + v, t8 = t2 + t2;
    t8 += t8;
    return t8 + t2;
}

static void setzero(Dec *r)
{
    r->m = 0; r->e = 0; r->neg = 0; r->pad = 0;
}

/* ---- 128-bit integers as 16-bit limbs ---- */

static void from64(u16 *x, u64 v)
{
    u32 lo = (u32)v, hi = (u32)(v >> 32);
    x[0] = (u16)lo; x[1] = (u16)(lo >> 16);
    x[2] = (u16)hi; x[3] = (u16)(hi >> 16);
    x[4] = x[5] = x[6] = x[7] = 0;
}

static u64 to64(const u16 *x)   /* low 64 bits */
{
    return ((u64)(((u32)x[3] << 16) | x[2]) << 32) | (((u32)x[1] << 16) | x[0]);
}

static int high(const u16 *x)    /* nonzero if x >= 2^64 */
{
    return x[4] | x[5] | x[6] | x[7];
}

/* x /= d (1 <= d <= 65535); returns the remainder */
static unsigned divs(u16 *x, unsigned d)
{
    u32 r = 0;
    int i;
    for (i = NL - 1; i >= 0; i--) {
        r = (r << 16) | x[i];               /* r < d << 16, so the quotient fits 16 bits */
        if (r) {
#if defined(__m68k__) && !defined(DEC_NO_ASM)
            /* DIVU.W: low word = quotient, high word = remainder (3x faster than __udivsi3) */
            __asm__("divu.w %1,%0" : "+d"(r) : "d"((u16)d) : "cc");
            x[i] = (u16)r;
            r >>= 16;
#else
            u16 q = (u16)(r / d);
            x[i] = q;
            r -= (u32)q * (u16)d;
#endif
        }
    }
    return (unsigned)r;
}

/* r = a * b (exact, < 2^128) */
static void mul64(u16 *r, u64 a, u64 b)
{
    u16 A[NL], B[NL];
    u32 c;
    int i, j;
    from64(A, a);
    from64(B, b);
    for (i = 0; i < NL; i++) r[i] = 0;
    for (i = 0; i < 4; i++) {
        c = 0;
        for (j = 0; j < 4; j++) {
            c += (u32)A[i] * B[j] + r[i + j];
            r[i + j] = (u16)c;
            c >>= 16;
        }
        r[i + 4] = (u16)c;
    }
}

static void badd(u16 *x, const u16 *y)  /* x += y */
{
    u32 c = 0;
    int i;
    for (i = 0; i < NL; i++) {
        c += (u32)x[i] + y[i];
        x[i] = (u16)c;
        c >>= 16;
    }
}

static void bsub(u16 *x, const u16 *y)  /* x -= y, x >= y */
{
    int i, t, b = 0;
    for (i = 0; i < NL; i++) {
        t = (int)x[i] - y[i] - b;
        x[i] = (u16)t;
        b = t < 0;
    }
}

static int bigcmp(const u16 *x, const u16 *y)
{
    int i;
    for (i = NL - 1; i >= 0; i--)
        if (x[i] != y[i]) return x[i] > y[i] ? 1 : -1;
    return 0;
}

/* Round the exact value  (x + sticky fraction) * 10^e  to 18 digits half-even and store it.
   sticky may only be set when x >= 10^18 (at least one digit is dropped above it). */
static int finish(Dec *r, u16 *x, int e, int st, int neg)
{
    int rd = 0;         /* last digit dropped (the rounding digit) */
    u64 q;
    while (x[7] | x[6] | x[5] | (x[4] >> 10)) {     /* x >= 2^74 > 10^22: drop 4 digits */
        if (divs(x, 10000)) st = 1;
        e += 4;
    }
    for (;;) {
        q = to64(x);
        if (!high(x)) {
            if (q < P10[PREC] && e >= ETINY) break;
            if (!q) {               /* underflow: everything left is zero */
                st |= rd; rd = 0; e = ETINY;
                break;
            }
        }
        st |= rd;
        rd = (int)divs(x, 10);
        e++;
    }
    if (rd > 5 || (rd == 5 && (st || (q & 1)))) {
        if (++q == P10[PREC]) { q = P10[PREC - 1]; e++; }
    }
    if (!q) { setzero(r); return DEC_OK; }
    if (e + ndig(q) - 1 > DEC_EMAX) return DEC_OVERFLOW;
    r->m = q; r->e = (short)e; r->neg = (unsigned char)neg; r->pad = 0;
    return DEC_OK;
}

/* digits d[0..n) (values > 9 are skipped: the point token), pp digits before the point */
static int num(Dec *r, const unsigned char *d, int n, int pp)
{
    u16 X[NL];
    u64 q = 0;
    int i, k = 0, nd = 0, last = 0, st = 0;
    for (i = 0; i < n; i++) {
        unsigned c = d[i];
        if (c > 9) continue;
        nd++;
        if (k < PREC + 1) {
            if (q || c) { q = mul10(q) + c; k++; last = nd; }
        } else if (c) st = 1;
    }
    if (!q) { setzero(r); return DEC_OK; }
    from64(X, q);
    return finish(r, X, pp - last, st, 0);
}

int dec_from_digits(Dec *out, const unsigned char *digits, int n, int point_pos)
{
    int i;
    if (n <= 0) return DEC_SYNTAX;
    for (i = 0; i < n; i++)
        if (digits[i] > 9) return DEC_SYNTAX;
    return num(out, digits, n, point_pos);
}

static int addsub(Dec *r, const Dec *a, const Dec *b, int flip)
{
    u16 X[NL], Y[NL], *p = X, *s = Y;
    const Dec *t;
    int sa = a->neg, sb = b->neg ^ flip, st = 0, d, sh, e;
    if (!b->m) { *r = *a; return DEC_OK; }
    if (!a->m) { *r = *b; r->neg = (unsigned char)sb; return DEC_OK; }
    if (a->e < b->e) { t = a; a = b; b = t; d = sa; sa = sb; sb = d; }
    d = a->e - b->e;
    sh = d < 19 ? d : 19;
    mul64(X, a->m, P10[sh]);            /* a's coefficient aligned (exact, < 10^37) */
    e = a->e - sh;
    from64(Y, b->m);
    if (d > 19) {                       /* b sits below X's last digit: truncate it */
        d -= 19;
        if (d >= PREC) { from64(Y, 0); st = 1; }
        else while (d--) if (divs(Y, 10)) st = 1;
    }
    /* here X >= 10^19 > Y + 1 whenever st is set */
    if (sa == sb) badd(X, Y);
    else {
        if (st) from64(Y, to64(Y) + 1);    /* X - (Y + f) = (X - Y - 1) + (1 - f) */
        d = bigcmp(X, Y);
        if (!d) { setzero(r); return DEC_OK; }
        if (d < 0) { p = Y; s = X; sa = sb; }
        bsub(p, s);
    }
    return finish(r, p, e, st, sa);
}

int dec_add(Dec *r, const Dec *a, const Dec *b) { return addsub(r, a, b, 0); }
int dec_sub(Dec *r, const Dec *a, const Dec *b) { return addsub(r, a, b, 1); }

int dec_mul(Dec *r, const Dec *a, const Dec *b)
{
    u16 X[NL];
    if (!a->m || !b->m) { setzero(r); return DEC_OK; }
    mul64(X, a->m, b->m);
    return finish(r, X, a->e + b->e, 0, a->neg ^ b->neg);
}

int dec_div(Dec *r, const Dec *a, const Dec *b)
{
    u16 X[NL];
    u64 x, y, q = 0;
    int e, i, dg;
    if (!b->m) return DEC_DIV0;
    if (!a->m) { setzero(r); return DEC_OK; }
    x = a->m; y = b->m;
    e = a->e - b->e - PREC;
    while (x < P10[PREC - 1]) { x = mul10(x); e--; }    /* both to exactly 18 digits */
    while (y < P10[PREC - 1]) { y = mul10(y); e++; }
    if (x < y) { x = mul10(x); e--; }                   /* now y <= x < 10y */
    for (i = 0; i <= PREC; i++) {                       /* 19 quotient digits */
        if (i) x = mul10(x);
        dg = 0;
        while (x >= y) { x -= y; dg++; }
        q = mul10(q) + (unsigned)dg;
    }
    from64(X, q);
    return finish(r, X, e, x != 0, a->neg ^ b->neg);
}

void dec_neg(Dec *r, const Dec *a)
{
    *r = *a;
    r->neg = (unsigned char)(r->m ? !a->neg : 0);
}

int dec_is_zero(const Dec *a) { return a->m == 0; }

int dec_pow(Dec *r, const Dec *base, const Dec *ex)
{
    u16 X[NL];
    Dec res = ONE, p = *base;
    u64 m = ex->m;
    int e = ex->e, neg = ex->neg, rc = DEC_OK;
    unsigned n = 0;
    if (m) {
        while (e < 0) {                 /* strip trailing zeros of a fractional exponent */
            from64(X, m);
            if (divs(X, 10)) return DEC_DOMAIN;
            m = to64(X);
            e++;
        }
        while (e > 0 && m <= 9999) { m = mul10(m); e--; }
        if (e > 0 || m > 9999) return DEC_DOMAIN;
        n = (unsigned)m;
    }
    if (!n) { *r = ONE; return DEC_OK; }
    if (!p.m) {
        if (neg) return DEC_DIV0;
        setzero(r);
        return DEC_OK;
    }
    for (;;) {
        if (n & 1) { rc = dec_mul(&res, &res, &p); if (rc) break; }
        n >>= 1;
        if (!n) break;
        rc = dec_mul(&p, &p, &p);
        if (rc) break;
    }
    if (neg) {
        if (rc) { setzero(r); return DEC_OK; }      /* 1/(overflow) underflows to 0 */
        if (!res.m) return DEC_OVERFLOW;            /* 1/(underflow) */
        return dec_div(r, &ONE, &res);
    }
    if (rc) return rc;
    *r = res;
    return DEC_OK;
}

/* ---- formatting ---- */

int dec_format(const Dec *a, int sig, char *buf, int *sci, int *exp10)
{
    unsigned char dg[20];
    char *p = buf;
    u64 v = a->m;
    int nd, i, adj;
    *sci = 0; *exp10 = 0;
    if (sig < 1) sig = 1;
    if (sig > PREC) sig = PREC;
    if (!v) { buf[0] = '0'; buf[1] = 0; return 1; }
    nd = ndig(v);
    for (i = 0; i < nd; i++) {          /* digits, most significant first */
        unsigned char k = 0;
        u64 pw = P10[nd - 1 - i];
        while (v >= pw) { v -= pw; k++; }
        dg[i] = k;
    }
    adj = a->e + nd - 1;
    if (nd > sig) {                     /* round half-up */
        i = sig;
        nd = sig;
        if (dg[i] >= 5) {
            while (--i >= 0 && dg[i] == 9) dg[i] = 0;
            if (i < 0) { dg[0] = 1; adj++; }
            else dg[i]++;
        }
    }
    while (nd > 1 && !dg[nd - 1]) nd--;
    if (a->neg) *p++ = '-';
    if (adj >= -6 && adj <= 11) {
        if (adj < 0) {
            *p++ = '0'; *p++ = '.';
            for (i = -1; i > adj; i--) *p++ = '0';
            for (i = 0; i < nd; i++) *p++ = (char)('0' + dg[i]);
        } else {
            for (i = 0; i <= adj || i < nd; i++) {
                if (i == adj + 1) *p++ = '.';
                *p++ = (char)('0' + (i < nd ? dg[i] : 0));
            }
        }
    } else {
        *sci = 1; *exp10 = adj;
        *p++ = (char)('0' + dg[0]);
        if (nd > 1) {
            *p++ = '.';
            for (i = 1; i < nd; i++) *p++ = (char)('0' + dg[i]);
        }
    }
    *p = 0;
    return (int)(p - buf);
}

int dec_to_text(const Dec *a, char *buf)
{
    int sci, x, n = dec_format(a, 12, buf, &sci, &x), k = 0;
    unsigned u;
    char t[6];
    if (sci) {
        buf[n++] = 'E';
        if (x < 0) { buf[n++] = '-'; x = -x; }
        u = (unsigned)x;
        do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u);
        while (k) buf[n++] = t[--k];
        buf[n] = 0;
    }
    return n;
}

/* ---- expression evaluator (recursive descent, depth-limited) ---- */

typedef struct {
    const unsigned char *t;
    int n, pos, depth, err;     /* err: first evaluation error */
} Parser;

#define PEEK(p) ((p)->pos < (p)->n ? (p)->t[(p)->pos] : 255)

typedef int (*OpFn)(Dec *, const Dec *, const Dec *);

static void arith(Parser *p, OpFn f, Dec *a, const Dec *b)
{
    if (!p->err) p->err = f(a, a, b);
}

static int p_expr(Parser *p, Dec *v);

/* unary := '-' unary | primary ('^' unary)? ; returns DEC_SYNTAX (p->pos = where) or 0 */
static int p_unary(Parser *p, Dec *v)
{
    Dec w;
    int c, s, pp = -1, nd = 0, rc = 0;
    if (++p->depth > DEC_MAXDEPTH) return DEC_SYNTAX;
    c = PEEK(p);
    if (c == T_MINUS) {
        p->pos++;
        rc = p_unary(p, v);
        if (!rc) dec_neg(v, v);
    } else {
        setzero(v);
        if (c == T_LPAR) {
            p->pos++;
            rc = p_expr(p, v);
            if (!rc) {
                if (PEEK(p) != T_RPAR) rc = DEC_SYNTAX;
                else p->pos++;
            }
        } else {
            s = p->pos;
            while ((c = PEEK(p)) <= T_9 || c == T_POINT) {
                if (c == T_POINT) {
                    if (pp >= 0) break;
                    pp = nd;
                } else nd++;
                p->pos++;
            }
            if (!nd) { p->pos = s; rc = DEC_SYNTAX; }
            else if (!p->err) {
                if (pp < 0) pp = nd;
                p->err = num(v, p->t + s, p->pos - s, pp);
            }
        }
        if (!rc && PEEK(p) == T_POW) {
            p->pos++;
            rc = p_unary(p, &w);
            if (!rc) arith(p, dec_pow, v, &w);
        }
    }
    p->depth--;
    return rc;
}

static int p_term(Parser *p, Dec *v)
{
    Dec w;
    int c, rc = p_unary(p, v);
    while (!rc) {
        c = PEEK(p);
        if (c == T_TIMES || c == T_DIV) p->pos++;
        else if (c != T_LPAR) break;            /* implicit multiplication before '(' */
        rc = p_unary(p, &w);
        if (!rc) arith(p, c == T_DIV ? dec_div : dec_mul, v, &w);
    }
    return rc;
}

static int p_expr(Parser *p, Dec *v)
{
    Dec w;
    int c, rc = p_term(p, v);
    while (!rc && ((c = PEEK(p)) == T_PLUS || c == T_MINUS)) {
        p->pos++;
        rc = p_term(p, &w);
        if (!rc) arith(p, c == T_PLUS ? dec_add : dec_sub, v, &w);
    }
    return rc;
}

int dec_eval(const unsigned char *tok, int n, Dec *result, int *err_pos)
{
    Parser p;
    int rc;
    p.t = tok; p.n = n < 0 ? 0 : n; p.pos = 0; p.depth = 0; p.err = DEC_OK;
    rc = p_expr(&p, result);
    if (!rc && p.pos < p.n) rc = DEC_SYNTAX;
    if (err_pos) *err_pos = rc ? p.pos : -1;
    if (!rc) rc = p.err;
    if (rc) setzero(result);
    return rc;
}
