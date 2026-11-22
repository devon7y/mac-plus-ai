/* Broadside: the Mac's targeting and fleet placement (see ai.h). */
#include "ai.h"
#include "ai_tables.h"
#include <string.h>

#define LMASK(L) ((uint16_t)((1u << (L)) - 1))

typedef struct {
    uint8_t len, dir, r, c, mult;
} Opt;

/* bit i set: cells i .. i+L-1 of a 10-cell line are all free (f = free cells) */
static uint16_t startmask(uint16_t f, int L)
{
    uint16_t s = f & (f >> 1);
    if (L > 2) {
        s &= f >> 2;
        if (L > 3) {
            s &= f >> 3;
            if (L > 4)
                s &= f >> 4;
        }
    }
    return s;
}

static int lowbit(uint16_t m)
{
    int i = 0;
    while (!(m & 1)) {
        m >>= 1;
        i++;
    }
    return i;
}

void ai_init(AI *a, int level, uint32_t seed)
{
    memset(a, 0, sizeof *a);
    a->level = (uint8_t)level;
    a->flags = AIF_DEFAULT;
    a->rng = seed ? seed : 0x2545F491UL;
    a->minSamples = AI_HARD_SAMPLES;
    a->exactCap = 64;
    a->normalNoise = 2048;
}

void ai_new_state(AI *a, const Know *k)
{
    AISample *b = &a->base;
    int i, hits = 0;
    memset(b, 0, sizeof *b);
    memset(a->H, 0, sizeof a->H);
    memset(a->Hc, 0, sizeof a->Hc);
    for (i = 0; i < NCELL; i++) {
        int v = k->cell[i], r = kRow[i], c = kCol[i];
        if (v == K_HIT) {
            a->H[r] |= (uint16_t)(1 << c);
            a->Hc[c] |= (uint16_t)(1 << r);
            b->U[r] |= (uint16_t)(1 << c);
            hits = 1;
        } else if (v != K_UNKNOWN) {
            b->Br[r] |= (uint16_t)(1 << c);
            b->Bc[c] |= (uint16_t)(1 << r);
        }
    }
    for (i = 0; i < NSHIP; i++)
        if (!k->sunk[i]) {
            b->cnt[kShipLen[i]]++;
            b->nrem++;
        }
    memset(a->acc, 0, sizeof a->acc);
    a->total = 0;
    a->ref = 0;
    a->samples = a->good = 0;
    a->exact = a->done = a->tried = 0;
    a->hunting = (uint8_t)!hits;
    a->notouch = (uint8_t)((k->rules & RULE_NOTOUCH) ? 1 : 0);
    a->maxPrefix = -32768;
    a->lastCount = 0;
    a->work = 0;
}

/* ---- placements ----------------------------------------------------------------------------- */

/* put a ship on the sample: block its cells (and, with no touching, the water around it) and
   mark the hits it covers */
static void apply(AISample *s, int L, int dir, int r, int c, int notouch)
{
    int r0, r1, c0, c1, i;
    uint16_t rowbits, colbits;
    if (notouch) {
        r0 = r - 1;
        c0 = c - 1;
        r1 = dir ? r + L : r + 1;
        c1 = dir ? c + 1 : c + L;
        if (r0 < 0) r0 = 0;
        if (c0 < 0) c0 = 0;
        if (r1 > 9) r1 = 9;
        if (c1 > 9) c1 = 9;
    } else {
        r0 = r;
        c0 = c;
        r1 = dir ? r + L - 1 : r;
        c1 = dir ? c : c + L - 1;
    }
    rowbits = (uint16_t)(LMASK(c1 - c0 + 1) << c0);
    colbits = (uint16_t)(LMASK(r1 - r0 + 1) << r0);
    for (i = r0; i <= r1; i++)
        s->Br[i] |= rowbits;
    for (i = c0; i <= c1; i++)
        s->Bc[i] |= colbits;
    if (!dir)
        s->U[r] &= (uint16_t)~(LMASK(L) << c);
    else
        for (i = r; i < r + L; i++)
            s->U[i] &= (uint16_t)~(1 << c);
    s->cnt[L]--;
    s->nrem--;
    s->path[s->npath][0] = (uint8_t)L;
    s->path[s->npath][1] = (uint8_t)dir;
    s->path[s->npath][2] = (uint8_t)r;
    s->path[s->npath][3] = (uint8_t)c;
    s->npath++;
}

/* no touching: would a ship here touch a hit that it doesn't cover? */
static int touches_hit(const AISample *s, int L, int dir, int r, int c)
{
    int r0 = r - 1, r1 = dir ? r + L : r + 1, c0 = c - 1, c1 = dir ? c + 1 : c + L, i;
    uint16_t rect, ship = dir ? (uint16_t)(1 << c) : (uint16_t)(LMASK(L) << c);
    if (r0 < 0) r0 = 0;
    if (c0 < 0) c0 = 0;
    if (r1 > 9) r1 = 9;
    if (c1 > 9) c1 = 9;
    rect = (uint16_t)(LMASK(c1 - c0 + 1) << c0);
    for (i = r0; i <= r1; i++) {
        uint16_t m = s->U[i] & rect;
        if (dir ? (i >= r && i < r + L) : i == r)
            m &= (uint16_t)~ship;
        if (m)
            return 1;
    }
    return 0;
}

/* would a ship here leave some uncovered hit uncovered? (for the last ship) */
static int leaves_hits(const AISample *s, int L, int dir, int r, int c)
{
    int i;
    for (i = 0; i < 10; i++) {
        uint16_t m = s->U[i];
        if (!m)
            continue;
        if (dir) {
            if (i >= r && i < r + L)
                m &= (uint16_t)~(1 << c);
        } else if (i == r) {
            m &= (uint16_t)~(LMASK(L) << c);
        }
        if (m)
            return 1;
    }
    return 0;
}

static int any_uncovered(const AISample *s)
{
    int i;
    for (i = 0; i < 10; i++)
        if (s->U[i])
            return 1;
    return 0;
}

/* placements through the first uncovered hit, for every remaining ship length (mult = ships of
   that length left). last: the last ship, which must cover every uncovered hit. Returns the
   number of options; *nw = the sum of their mults. */
static int hit_options(const AI *a, const AISample *s, Opt *o, int last, int *nw)
{
    int r0 = 0, c0, L, n = 0, w = 0, i, lo, hi, notouch = a->notouch;
    while (!s->U[r0])
        r0++;
    c0 = lowbit(s->U[r0]);
    for (L = 5; L >= 2; L--) {
        int m = s->cnt[L];
        if (!m)
            continue;
        lo = c0 - L + 1 < 0 ? 0 : c0 - L + 1;
        hi = c0 > 10 - L ? 10 - L : c0;
        for (i = lo; i <= hi; i++) {
            uint16_t msk = (uint16_t)(LMASK(L) << i);
            if ((s->Br[r0] & msk) || !(msk & ~a->H[r0]))
                continue;
            if (notouch && touches_hit(s, L, 0, r0, i))
                continue;
            if (last && leaves_hits(s, L, 0, r0, i))
                continue;
            o[n].len = (uint8_t)L; o[n].dir = 0; o[n].r = (uint8_t)r0; o[n].c = (uint8_t)i; o[n].mult = (uint8_t)m;
            n++;
            w += m;
        }
        lo = r0 - L + 1 < 0 ? 0 : r0 - L + 1;
        hi = r0 > 10 - L ? 10 - L : r0;
        for (i = lo; i <= hi; i++) {
            uint16_t msk = (uint16_t)(LMASK(L) << i);
            if ((s->Bc[c0] & msk) || !(msk & ~a->Hc[c0]))
                continue;
            if (notouch && touches_hit(s, L, 1, i, c0))
                continue;
            if (last && leaves_hits(s, L, 1, i, c0))
                continue;
            o[n].len = (uint8_t)L; o[n].dir = 1; o[n].r = (uint8_t)i; o[n].c = (uint8_t)c0; o[n].mult = (uint8_t)m;
            n++;
            w += m;
        }
    }
    *nw = w;
    return n;
}

/* start masks of a ship of length L on every row (sm[0..9]) and column (sm[10..19]); returns
   the number of placements */
static int free_masks(const AISample *s, int L, uint16_t *sm)
{
    int i, n = 0;
    for (i = 0; i < 10; i++) {
        sm[i] = startmask((uint16_t)(~s->Br[i] & 0x3FF), L);
        n += pc10[sm[i]];
        sm[10 + i] = startmask((uint16_t)(~s->Bc[i] & 0x3FF), L);
        n += pc10[sm[10 + i]];
    }
    return n;
}

static int largest(const AISample *s)
{
    int L = 5;
    while (!s->cnt[L])
        L--;
    return L;
}

/* add w to the cells of a ship (unrolled: this is the 68000's hottest loop) */
static void add_ship(uint32_t *acc, int L, int dir, int r, int c, uint32_t w)
{
    uint32_t *p = acc + (short)(r * 10 + c);
    if (dir) {
        switch (L) {
        case 5: p[40] += w; /* fall through */
        case 4: p[30] += w; /* fall through */
        case 3: p[20] += w; /* fall through */
        default: p[10] += w; p[0] += w;
        }
    } else {
        switch (L) {
        case 5: p[4] += w; /* fall through */
        case 4: p[3] += w; /* fall through */
        case 3: p[2] += w; /* fall through */
        default: p[1] += w; p[0] += w;
        }
    }
}

/* add w to every cell of every placement in the start masks sm (rows 0..9, columns 10..19) of a
   ship of length L. One loop per length, the adds unrolled: the hottest code on the 68000. */
#define COVER_ROWS(ADD)                                                                      \
    for (i = 0; i < 10; i++) {                                                               \
        uint16_t m = sm[i];                                                                  \
        uint32_t *p = acc + rowoff[i];                                                       \
        for (; m; m >>= 1, p++)                                                              \
            if (m & 1) { ADD(1) }                                                            \
    }                                                                                        \
    for (i = 0; i < 10; i++) {                                                               \
        uint16_t m = sm[10 + i];                                                             \
        uint32_t *p = acc + i;                                                               \
        for (; m; m >>= 1, p += 10)                                                          \
            if (m & 1) { ADD(10) }                                                           \
    }
#define ADD2(S) p[0] += w; p[S] += w;
#define ADD3(S) p[0] += w; p[S] += w; p[2 * S] += w;
#define ADD4(S) p[0] += w; p[S] += w; p[2 * S] += w; p[3 * S] += w;
#define ADD5(S) p[0] += w; p[S] += w; p[2 * S] += w; p[3 * S] += w; p[4 * S] += w;
static const uint8_t rowoff[10] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90};

static void cover(uint32_t *acc, const uint16_t *sm, int L, uint32_t w)
{
    short i;
    switch (L) {
    case 2: COVER_ROWS(ADD2) break;
    case 3: COVER_ROWS(ADD3) break;
    case 4: COVER_ROWS(ADD4) break;
    default: COVER_ROWS(ADD5) break;
    }
}

static uint32_t weight(AI *a, int16_t lw);

/* the last ship of a sample: every placement it can take. Returns how many (0 = none). Adds each
   with weight w0, the ships placed before it with w0 * n, and that to the total; w0 = wexact, or
   (wexact == 0) the importance weight of log2 weight lw. */
static int last_ship(AI *a, const AISample *s, int16_t lw, uint32_t wexact)
{
    int n, i;
    uint32_t w0;
    Opt o[40];
    uint16_t sm[20];
    int L = largest(s), hit = any_uncovered(s);
    if (hit) {
        int no = hit_options(a, s, o, 1, &n);
        if (!n)
            return 0;
        w0 = wexact ? wexact : weight(a, lw);
        if (!wexact)
            a->good++;
        if (w0)
            for (i = 0; i < no; i++)
                add_ship(a->acc, o[i].len, o[i].dir, o[i].r, o[i].c, w0);
    } else {
        n = free_masks(s, L, sm);
        if (!n)
            return 0;
        w0 = wexact ? wexact : weight(a, lw);
        if (!wexact)
            a->good++;
        if (w0)
            cover(a->acc, sm, L, w0);
    }
    if (w0) {
        uint32_t w1 = w0 * (uint32_t)n;
        for (i = 0; i < s->npath; i++)
            add_ship(a->acc, s->path[i][0], s->path[i][1], s->path[i][2], s->path[i][3], w1);
        a->total += w1;
    }
    return n;
}

/* ---- Hard: importance sampling -------------------------------------------------------------- */

static void renorm(AI *a, int k)
{
    int i;
    if (k > 31)
        k = 31;
    for (i = 0; i < NCELL; i++)
        a->acc[i] >>= k;
    a->total >>= k;
    a->ref = (int16_t)(a->ref + 256 * k);
}

/* 2^(10 + (lw - ref)/256), renormalising the sums first if it would exceed 2^19 */
static uint32_t weight(AI *a, int16_t lw)
{
    int16_t d;
    int e;
    uint32_t w;
    if (!a->good)
        a->ref = lw;
    d = (int16_t)(lw - a->ref);
    e = 10 + (d >= 0 ? d >> 8 : -((255 - d) >> 8));
    if (e > 18) {
        renorm(a, e - 18);
        d = (int16_t)(lw - a->ref);
        e = 18;
    }
    if (e < 0)
        return 0;
    w = exp2tab[d & 255];
    return e >= 15 ? w << (e - 15) : w >> (15 - e);
}

/* one sample: a whole fleet consistent with the knowledge, built ship by ship (the hits first,
   each through the first hit not yet covered, then the other ships largest first), weighted by
   the product of the number of choices at each step (so the weighted samples are uniform over
   all consistent fleets). The last ship is not drawn: all its placements are added. */
static void is_sample(AI *a)
{
    AISample s = a->base;
    int16_t lw = 0;
    int notouch = a->notouch;
    a->samples++;
    a->work++;
    while (s.nrem > 1) {
        if (any_uncovered(&s)) {
            Opt o[40];
            int nw, n = hit_options(a, &s, o, 0, &nw), t, i;
            if (!nw)
                return;
            t = bs_below(&a->rng, (uint16_t)nw);
            for (i = 0; t >= o[i].mult; i++)
                t -= o[i].mult;
            lw = (int16_t)(lw + log2tab[nw]);
            apply(&s, o[i].len, o[i].dir, o[i].r, o[i].c, notouch);
            (void)n;
        } else {
            uint16_t sm[20];
            int L = largest(&s), n = free_masks(&s, L, sm), t, i, b;
            if (!n)
                return;
            t = bs_below(&a->rng, (uint16_t)n);
            for (i = 0; t >= pc10[sm[i]]; i++)
                t -= pc10[sm[i]];
            for (b = 0;; b++)
                if ((sm[i] >> b) & 1) {
                    if (!t)
                        break;
                    t--;
                }
            lw = (int16_t)(lw + log2tab[n]);
            if (i < 10)
                apply(&s, L, 0, i, b, notouch);
            else
                apply(&s, L, 1, b, i - 10, notouch);
        }
    }
    if (s.nrem == 1) {
        last_ship(a, &s, lw, 0);
        if (a->total > 0x40000000UL)
            renorm(a, 1);
    }
    if (lw > a->maxPrefix)
        a->maxPrefix = lw;
}


/* ---- Hard: exact enumeration ---------------------------------------------------------------- */

static int ex_rec(AI *a, const AISample *s, uint32_t w)
{
    int notouch = a->notouch, i;
    if (s->nrem == 1) {
        if (++a->work > a->exactCap)
            return 0;
        a->lastCount += (uint32_t)last_ship(a, s, 0, w) * w;
        return 1;
    }
    if (any_uncovered(s)) {
        Opt o[40];
        int nw, n = hit_options(a, s, o, 0, &nw);
        for (i = 0; i < n; i++) {
            AISample t = *s;
            apply(&t, o[i].len, o[i].dir, o[i].r, o[i].c, notouch);
            if (!ex_rec(a, &t, w * o[i].mult))
                return 0;
        }
    } else {
        uint16_t sm[20];
        int L = largest(s), b;
        free_masks(s, L, sm);
        for (i = 0; i < 20; i++) {
            uint16_t m = sm[i];
            for (b = 0; m; b++, m >>= 1) {
                AISample t;
                if (!(m & 1))
                    continue;
                t = *s;
                if (i < 10)
                    apply(&t, L, 0, i, b, notouch);
                else
                    apply(&t, L, 1, b, i - 10, notouch);
                if (!ex_rec(a, &t, w))
                    return 0;
            }
        }
    }
    return 1;
}

static void clear_sums(AI *a)
{
    memset(a->acc, 0, sizeof a->acc);
    a->total = 0;
    a->ref = 0;
    a->samples = a->good = 0;
    a->lastCount = 0;
    a->maxPrefix = -32768;
}

static int try_exact(AI *a)
{
    a->tried = 1;
    clear_sums(a);
    a->work = 0;
    if (ex_rec(a, &a->base, 1) && a->total) {
        a->exact = 1;
        return 1;
    }
    clear_sums(a);
    return 0;
}

/* ---- Normal: the density map ---------------------------------------------------------------- */

static void density_map(AI *a)
{
    const AISample *b = &a->base;
    int L, i, hits = 0;
    for (i = 0; i < 10; i++)
        hits |= a->H[i];
    memset(a->acc, 0, sizeof a->acc);
    for (L = 2; L <= 5; L++) {
        int m = b->cnt[L];
        if (!m)
            continue;
        for (i = 0; i < 20; i++) {
            uint16_t f = (uint16_t)(~(i < 10 ? b->Br[i] : b->Bc[i - 10]) & 0x3FF);
            uint16_t hl = i < 10 ? a->H[i] : a->Hc[i - 10];
            uint16_t sm = startmask(f, L);
            int s;
            for (s = 0; sm; s++, sm >>= 1) {
                uint16_t msk;
                int nh;
                if (!(sm & 1))
                    continue;
                msk = (uint16_t)(LMASK(L) << s);
                if (!(msk & ~hl))
                    continue;             /* all hits: that ship would be sunk */
                nh = pc10[msk & hl];
                if (hits && !nh)
                    continue;             /* while finishing a ship, only placements through hits */
                if (i < 10)
                    add_ship(a->acc, L, 0, i, s, (uint32_t)m << (3 * nh));
                else
                    add_ship(a->acc, L, 1, s, i - 10, (uint32_t)m << (3 * nh));
            }
        }
    }
    a->total = 1;
}

/* ---- Easy: hunt and finish ------------------------------------------------------------------ */

static int is_hit(const Know *k, int r, int c)
{
    return r >= 0 && r < 10 && c >= 0 && c < 10 && k->cell[r * 10 + c] == K_HIT;
}

static int is_open(const Know *k, int r, int c)
{
    return r >= 0 && r < 10 && c >= 0 && c < 10 && k->cell[r * 10 + c] == K_UNKNOWN;
}

static void easy_map(AI *a, const Know *k)
{
    int i, best = 0;
    static const signed char dr[4] = {0, 1, 0, -1}, dc[4] = {1, 0, -1, 0};
    memset(a->acc, 0, sizeof a->acc);
    for (i = 0; i < NCELL; i++) {
        int r = kRow[i], c = kCol[i], d;
        if (k->cell[i] != K_HIT)
            continue;
        for (d = 0; d < 4; d++) {
            /* a line of hits: the open cell at its end */
            if (is_hit(k, r - dr[d], c - dc[d])) {
                int rr = r, cc = c;
                while (is_hit(k, rr, cc)) {
                    rr += dr[d];
                    cc += dc[d];
                }
                if (is_open(k, rr, cc)) {
                    a->acc[rr * 10 + cc] = 2;
                    best = 2;
                }
            } else if (is_open(k, r + dr[d], c + dc[d]) && !a->acc[(r + dr[d]) * 10 + c + dc[d]]) {
                a->acc[(r + dr[d]) * 10 + c + dc[d]] = 1;
                if (!best)
                    best = 1;
            }
        }
    }
    if (best == 2)
        for (i = 0; i < NCELL; i++)
            if (a->acc[i] == 1)
                a->acc[i] = 0;
    if (!best)
        for (i = 0; i < NCELL; i++)
            a->acc[i] = k->cell[i] == K_UNKNOWN;
    a->total = 1;
}

/* ---- the interface -------------------------------------------------------------------------- */

int ai_work(AI *a, const Know *k, uint16_t samples)
{
    if (a->level == AI_EASY) {
        if (!a->done)
            easy_map(a, k);
        return a->done = 1;
    }
    if (a->level == AI_NORMAL) {
        /* hunts with the density map (plus noise), finishes ships like Easy */
        if (!a->done) {
            if (a->hunting)
                density_map(a);
            else
                easy_map(a, k);
        }
        return a->done = 1;
    }
    if (a->exact)
        return a->done = 1;
    if (!a->samples)
        a->maxPrefix = -32768;
    if (!a->tried && (a->flags & AIF_EXACT)) {
        /* exact when the number of fleets before the last ship looks small: always try with two
           ships or fewer left, else after a few samples have estimated it */
        if (a->base.nrem <= 2) {
            /* the number of fleets before the last ship = the first ship's choices */
            int n1;
            if (a->base.nrem == 1) {
                n1 = 1;
            } else if (any_uncovered(&a->base)) {
                Opt o[40];
                hit_options(a, &a->base, o, 0, &n1);
            } else {
                uint16_t sm[20];
                n1 = free_masks(&a->base, largest(&a->base), sm);
            }
            if (n1 <= a->exactCap && try_exact(a))
                return a->done = 1;
            a->tried = 1;
        } else if (a->samples >= 8) {
            /* the samples' weights before the last ship estimate how many there are (try when the
               largest is under a quarter of the cap, so attempts rarely run out) */
            if (a->maxPrefix < (int16_t)(log2tab[a->exactCap > 255 ? 255 : a->exactCap] - 512) && try_exact(a))
                return a->done = 1;
            a->tried = 1;
        }
    }
    while (samples--)
        is_sample(a);
    if (a->samples >= a->minSamples && a->good)
        a->done = 1;
    else if (a->samples >= 4 * a->minSamples) {      /* (never seen: no consistent fleet found) */
        density_map(a);
        a->done = 1;
    }
    return a->done;
}

/* (r + c) mod L without a division (the 68000 has no fast one) */
static int diag_mod(int d, int L)
{
    while (d >= L)
        d -= L;
    return d;
}

static int lattice_phase(const AI *a, const Know *k, int *step)
{
    int L = 2, ph, best = 0, i;
    uint32_t bestMass = 0;
    while (L <= 5 && !a->base.cnt[L])
        L++;
    *step = L;
    for (ph = 0; ph < L; ph++) {
        uint32_t mass = 0;
        for (i = 0; i < NCELL; i++)
            if (k->cell[i] == K_UNKNOWN && diag_mod(kRow[i] + kCol[i], L) == ph)
                mass += a->acc[i] >> 8;
        if (mass > bestMass) {
            bestMass = mass;
            best = ph;
        }
    }
    return best;
}

int ai_parity_shift = 4;   /* off-lattice cells lose 1/16 of their weight */
int ai_choose(AI *a, const Know *k, uint8_t *cells, int n)
{
    uint8_t taken[NCELL];
    uint32_t score[NCELL];
    int got = 0, step = 1, phase = -1, i;
    if (!a->done)
        ai_work(a, k, a->minSamples);
    memset(taken, 0, sizeof taken);
    if (a->level == AI_HARD && (a->flags & AIF_PARITY) && a->hunting)
        phase = lattice_phase(a, k, &step);
    if (a->level == AI_NORMAL && a->hunting && a->normalNoise) {
        /* Normal: noise of up to normalNoise/256 of the best score (the map itself, which "Show
           the Mac's Thinking" displays, stays clean) */
        uint32_t mx = 0, amp;
        int sh = 0;
        for (i = 0; i < NCELL; i++)
            if (a->acc[i] > mx)
                mx = a->acc[i];
        amp = (mx * a->normalNoise) >> 8;
        while ((amp >> sh) > 0xFFFF)
            sh++;
        for (i = 0; i < NCELL; i++)
            score[i] = (a->acc[i] >> sh) + bs_below(&a->rng, (uint16_t)((amp >> sh) + 1));
    } else {
        for (i = 0; i < NCELL; i++) {
            uint32_t v = a->acc[i];
            if (phase >= 0 && diag_mod(kRow[i] + kCol[i], step) != phase)
                v -= v >> ai_parity_shift;     /* off the lattice: a little less attractive */
            score[i] = v;
        }
    }
    while (got < n) {
        int best = -1, start = bs_below(&a->rng, NCELL);
        uint32_t bv = 0;
        for (i = 0; i < NCELL; i++) {
            int c = start + i;
            if (c >= NCELL)
                c -= NCELL;
            if (k->cell[c] != K_UNKNOWN || taken[c])
                continue;
            if (best < 0 || score[c] > bv) {
                best = c;
                bv = score[c];
            }
        }
        if (best < 0)
            break;
        taken[best] = 1;
        cells[got++] = (uint8_t)best;
    }
    return got;
}

int ai_best(const AI *a, const Know *k)
{
    int step = 1, phase = -1, best = -1, c;
    uint32_t bv = 0;
    if (a->level == AI_HARD && (a->flags & AIF_PARITY) && a->hunting)
        phase = lattice_phase(a, k, &step);
    for (c = 0; c < NCELL; c++) {
        uint32_t v = a->acc[c];
        if (k->cell[c] != K_UNKNOWN)
            continue;
        if (phase >= 0 && diag_mod(kRow[c] + kCol[c], step) != phase)
            v -= v >> ai_parity_shift;
        if (best < 0 || v > bv) {
            best = c;
            bv = v;
        }
    }
    return best;
}

void ai_heat(const AI *a, const Know *k, uint8_t *heat)
{
    /* 1..255 from the least to the most likely possible cell (a contrast stretch: in a hunt the
       chances differ by a few tens of percent), 0 for impossible or known cells */
    uint32_t mx = 0, mn = 0xFFFFFFFFUL, span;
    int i, sh = 0;
    for (i = 0; i < NCELL; i++)
        if (k->cell[i] == K_UNKNOWN && a->acc[i]) {
            if (a->acc[i] > mx)
                mx = a->acc[i];
            if (a->acc[i] < mn)
                mn = a->acc[i];
        }
    if (!mx) {
        memset(heat, 0, NCELL);
        return;
    }
    if (mn > mx / 2)
        mn = mx / 2;               /* (never stretch more than 2:1) */
    span = mx - mn;
    while ((span >> sh) > 0x7FFF)
        sh++;
    for (i = 0; i < NCELL; i++) {
        if (k->cell[i] != K_UNKNOWN || !a->acc[i])
            heat[i] = 0;
        else if (a->acc[i] <= mn)
            heat[i] = 1;
        else
            heat[i] = (uint8_t)(1 + (((a->acc[i] - mn) >> sh) * 254) / ((span >> sh) ? (span >> sh) : 1));
    }
}

/* ---- the Mac's own fleet -------------------------------------------------------------------- */

static int fleet_score(const Place *ships, const uint16_t *early)
{
    uint8_t occ[NCELL], cells[5];
    int i, j, score = 0, edge = 0, touch = 0;
    memset(occ, 0, sizeof occ);
    for (i = 0; i < NSHIP; i++) {
        int e = 0;
        ship_cells(&ships[i], kShipLen[i], cells);
        for (j = 0; j < kShipLen[i]; j++) {
            occ[cells[j]] = (uint8_t)(i + 1);
            if (early)
                score += early[cells[j]] >> 4;
            if (kRow[cells[j]] == 0 || kRow[cells[j]] == 9 || kCol[cells[j]] == 0 || kCol[cells[j]] == 9)
                e = 1;
        }
        edge += e;
    }
    /* ships touching each other: finding one finds the other */
    for (i = 0; i < NCELL; i++) {
        int r = kRow[i], c = kCol[i];
        if (!occ[i])
            continue;
        if (c < 9 && occ[i + 1] && occ[i + 1] != occ[i])
            touch++;
        if (r < 9 && occ[i + 10] && occ[i + 10] != occ[i])
            touch++;
        if (r < 9 && c < 9 && occ[i + 11] && occ[i + 11] != occ[i])
            touch++;
        if (r < 9 && c > 0 && occ[i + 9] && occ[i + 9] != occ[i])
            touch++;
    }
    score += 40 * touch;
    if (edge > 2)
        score += 30 * (edge - 2);       /* people look along the edges */
    return score;
}

void ai_place_fleet(Place *ships, uint8_t rules, uint32_t *rng, const uint16_t *early)
{
    Place cand[NSHIP];
    int best = 0x7FFF, i;
    for (i = 0; i < 12; i++) {
        int s;
        fleet_random(cand, rules, rng);
        s = fleet_score(cand, early) + bs_below(rng, 24);
        if (s < best) {
            best = s;
            memcpy(ships, cand, sizeof cand);
        }
    }
}
