/* Posterior check: brute force over all fleets of the remaining ships vs. ai.c (exact and sampled).
   check [-t] [-n states] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../src/game.h"
#include "../src/ai.h"

static int rules = 0;
static const Know *K;
static int rem[NSHIP], nrem;
static double cnt[NCELL], total;
static uint8_t occ[NCELL];

static int ok_place(int len, int dir, int r, int c, int id)
{
    int i, at = r * 10 + c, step = dir ? 10 : 1, unknown = 0;
    if (dir ? r + len > 10 : c + len > 10) return 0;
    for (i = 0; i < len; i++, at += step) {
        int v = K->cell[at];
        if (occ[at] || (v != K_UNKNOWN && v != K_HIT)) return 0;
        if (v == K_UNKNOWN) unknown = 1;
        if (rules & RULE_NOTOUCH) {
            int rr = at / 10, cc = at % 10, dr, dc;
            for (dr = -1; dr <= 1; dr++) for (dc = -1; dc <= 1; dc++) {
                int r2 = rr + dr, c2 = cc + dc;
                if (r2 < 0 || r2 > 9 || c2 < 0 || c2 > 9) continue;
                if (occ[r2 * 10 + c2] && occ[r2 * 10 + c2] != id) return 0;
            }
        }
    }
    return unknown;
}

static void mark(int len, int dir, int r, int c, int v)
{
    int i, at = r * 10 + c, step = dir ? 10 : 1;
    for (i = 0; i < len; i++, at += step) occ[at] = (uint8_t)v;
}

static void rec(int d)
{
    int dir, r, c, i;
    if (d == nrem) {
        /* all hits covered; under notouch, no ship next to a hit it doesn't own */
        for (i = 0; i < NCELL; i++) if (K->cell[i] == K_HIT && !occ[i]) return;
        if (rules & RULE_NOTOUCH)
            for (i = 0; i < NCELL; i++) {
                int rr = i / 10, cc = i % 10, dr, dc;
                if (K->cell[i] != K_HIT) continue;
                for (dr = -1; dr <= 1; dr++) for (dc = -1; dc <= 1; dc++) {
                    int r2 = rr + dr, c2 = cc + dc;
                    if (r2 < 0 || r2 > 9 || c2 < 0 || c2 > 9) continue;
                    if (occ[r2 * 10 + c2] && occ[r2 * 10 + c2] != occ[i]) return;
                }
            }
        for (i = 0; i < NCELL; i++) if (occ[i]) cnt[i] += 1;
        total += 1;
        return;
    }
    for (dir = 0; dir < 2; dir++) for (r = 0; r < 10; r++) for (c = 0; c < 10; c++) {
        int len = kShipLen[rem[d]];
        if (!ok_place(len, dir, r, c, d + 1)) continue;
        mark(len, dir, r, c, d + 1);
        rec(d + 1);
        mark(len, dir, r, c, 0);
    }
}

int main(int argc, char **argv)
{
    int i, n = 30, s, attempt = 0;
    double worstEx = 0, worstIs = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t")) rules |= RULE_NOTOUCH;
        else if (!strcmp(argv[i], "-n")) n = atoi(argv[++i]);
    }
    for (s = 0; s < n; s++) {
        Fleet f; Know k; Place ships[NSHIP]; AI a, b;
        uint32_t rng = 1234567u + (uint32_t)(attempt++) * 7919u;
        int shots = 0, j;
        fleet_random(ships, (uint8_t)rules, &rng);
        fleet_build(&f, ships, (uint8_t)rules);
        know_init(&k, (uint8_t)rules);
        /* sink 2 or 3 ships outright, then 10..40 random shots (stop after a hit sometimes) */
        {
            int sinkN = 2 + (s & 1), q, extra = 10 + bs_below(&rng, 31);
            for (q = 0; q < sinkN; q++) {
                uint8_t cl[5]; int sk, m, sh = bs_below(&rng, NSHIP);
                if (k.sunk[sh]) continue;
                ship_cells(&f.ship[sh], kShipLen[sh], cl);
                for (m = 0; m < kShipLen[sh]; m++) if (k.cell[cl[m]] == K_UNKNOWN) { fire(&f, &k, cl[m], &sk); shots++; }
            }
            while (extra-- > 0 && f.afloat) {
                int cell = bs_below(&rng, 100), sk, res;
                if (k.cell[cell] != K_UNKNOWN) { extra++; continue; }
                res = fire(&f, &k, cell, &sk);
                shots++;
                if (res == SHOT_HIT && bs_below(&rng, 3) == 0) break;
            }
        }
        if (f.afloat > 3) { s--; continue; }
        if (!f.afloat) { s--; continue; }
        K = &k; nrem = 0;
        for (j = 0; j < NSHIP; j++) if (!k.sunk[j]) rem[nrem++] = j;
        memset(cnt, 0, sizeof cnt); total = 0; memset(occ, 0, sizeof occ);
        rec(0);
        /* labelled brute force; ai.c counts labelled fleets too */
        ai_init(&a, AI_HARD, 99); a.exactCap = 60000;
        ai_new_state(&a, &k);
        ai_work(&a, &k, 8);
        ai_work(&a, &k, 1);
        ai_init(&b, AI_HARD, 77); b.flags = 0; b.minSamples = 20000;
        ai_new_state(&b, &k);
        while (!ai_work(&b, &k, 1000))
            continue;
        {
            double dEx = 0, dIs = 0;
            for (j = 0; j < NCELL; j++) {
                double p = total ? cnt[j] / total : 0;
                double pe = a.total ? (double)a.acc[j] / a.total : 0;
                double pi = b.total ? (double)b.acc[j] / b.total : 0;
                if (k.cell[j] != K_UNKNOWN) continue;
                if (fabs(p - pe) > dEx) dEx = fabs(p - pe);
                if (fabs(p - pi) > dIs) dIs = fabs(p - pi);
            }
            printf("state %2d: %d ships left, %d shots, brute %.0f fleets, ai exact=%d count %lu (work %lu) | "
                   "max|p-exact| %.5f  max|p-sampled| %.4f (good %d/%d)\n",
                   s, nrem, shots, total, a.exact, (unsigned long)a.total, (unsigned long)a.work, dEx, dIs,
                   b.good, b.samples);
            if (a.exact && dEx > worstEx) worstEx = dEx;
            if (dIs > worstIs) worstIs = dIs;
        }
    }
    printf("worst: exact %.6f, sampled (20000) %.4f\n", worstEx, worstIs);
    return 0;
}
