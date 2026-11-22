/*
 * Broadside host simulator: the Mac's AI (src/ai.c, the same code as the Mac app) against
 * uniformly random fleets. Reports shots to sink the whole fleet.
 *
 *   sim [-l 0|1|2] [-n games] [-t] [-x] [-s samples] [-f flags] [-z noise] [-c exactCap]
 *       [-seed N] [-v] [-salvo]
 *   -l  level (0 Easy, 1 Normal, 2 Hard)    -t  "no touching" rules
 *   -s  Hard: samples per move              -f  Hard flags (ai.h AIF_*; default 5)
 *   -salvo  AI against AI full Salvo games (reports turns and who won as first player)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "../src/game.h"
#include "../src/ai.h"

static int level = 2, games = 200, rules = 0, flags = AIF_DEFAULT, verbose = 0, cap = -1;
static int samples = AI_HARD_SAMPLES, noise = -1;
static uint32_t seed = 12345;

static void setup(AI *a, int lvl, uint32_t s)
{
    ai_init(a, lvl, s);
    a->flags = (uint8_t)flags;
    a->minSamples = (uint16_t)samples;
    if (noise >= 0)
        a->normalNoise = (uint16_t)noise;
    if (cap >= 0)
        a->exactCap = (uint16_t)cap;
}

/* one game: the AI shoots at a random fleet until it is sunk; returns shots */
static int play(uint32_t g, double *work, long *exacts, long *moves)
{
    Fleet f;
    Know k;
    AI a;
    Place ships[NSHIP];
    uint32_t rng = 0x9E3779B9UL ^ (g * 2654435761UL) ^ seed;
    if (!rng)
        rng = 1;
    fleet_random(ships, (uint8_t)rules, &rng);
    fleet_build(&f, ships, (uint8_t)rules);
    know_init(&k, (uint8_t)rules);
    setup(&a, level, rng ^ 0xA5A5A5A5UL);
    while (f.afloat) {
        uint8_t cell;
        int sunk, r;
        ai_new_state(&a, &k);
        while (!ai_work(&a, &k, 50))
            ;
        if (ai_choose(&a, &k, &cell, 1) != 1) {
            fprintf(stderr, "no target!\n");
            exit(1);
        }
        *work += a.work;
        *exacts += a.exact;
        (*moves)++;
        r = fire(&f, &k, cell, &sunk);
        if (r == SHOT_BAD) {
            fprintf(stderr, "bad shot %d\n", cell);
            exit(1);
        }
        if (verbose > 1)
            printf("  shot %c%d -> %d (exact %d, samples %d good %d)\n", 'A' + cell % 10, cell / 10 + 1, r,
                   a.exact, a.samples, a.good);
    }
    return k.shots;
}

/* Salvo, AI (first, level) against AI (second, level2): returns 1 if the first wins; *turns */
static int salvo_game(uint32_t g, int l1, int l2, int *turns)
{
    Fleet f[2];
    Know k[2];
    AI a[2];
    Place ships[NSHIP];
    int t = 0, side;
    uint32_t rng = 0x9E3779B9UL ^ (g * 2654435761UL) ^ seed;
    for (side = 0; side < 2; side++) {
        fleet_random(ships, (uint8_t)rules, &rng);
        fleet_build(&f[side], ships, (uint8_t)(rules | RULE_SALVO));
        know_init(&k[side], (uint8_t)(rules | RULE_SALVO));
        setup(&a[side], side ? l2 : l1, rng ^ (side ? 0x5A5A5A5AUL : 0xA5A5A5A5UL));
    }
    for (;;) {
        t++;
        for (side = 0; side < 2; side++) {
            uint8_t cells[5];
            int n = f[side].afloat, i, got, sunk;      /* shots = my ships afloat */
            Fleet *target = &f[1 - side];
            ai_new_state(&a[side], &k[side]);
            while (!ai_work(&a[side], &k[side], 50))
                ;
            got = ai_choose(&a[side], &k[side], cells, n);
            for (i = 0; i < got; i++)
                fire(target, &k[side], cells[i], &sunk);
            if (!target->afloat) {
                *turns = t;
                return side == 0;
            }
        }
    }
}

int main(int argc, char **argv)
{
    int i, salvo = 0, l2 = -1;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) level = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-l2")) l2 = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n")) games = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t")) rules |= RULE_NOTOUCH;
        else if (!strcmp(argv[i], "-s")) samples = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f")) flags = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-z")) noise = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c")) cap = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-seed")) seed = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-v")) verbose++;
        else if (!strcmp(argv[i], "-p")) { extern int ai_parity_shift; ai_parity_shift = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-salvo")) salvo = 1;
        else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (salvo) {
        int wins = 0, tsum = 0;
        if (l2 < 0)
            l2 = level;
        for (i = 0; i < games; i++) {
            int t;
            wins += salvo_game((uint32_t)i, level, l2, &t);
            tsum += t;
        }
        printf("salvo%s level %d vs %d: first player won %d/%d (%.1f%%), mean turns %.2f\n",
               (rules & RULE_NOTOUCH) ? " notouch" : "", level, l2, wins, games, 100.0 * wins / games,
               (double)tsum / games);
        return 0;
    }
    {
        int *shots = malloc(sizeof(int) * games), mn = 999, mx = 0;
        double sum = 0, sq = 0, work = 0;
        long exacts = 0, moves = 0;
        clock_t t0 = clock();
        for (i = 0; i < games; i++) {
            shots[i] = play((uint32_t)i, &work, &exacts, &moves);
            sum += shots[i];
            sq += (double)shots[i] * shots[i];
            if (shots[i] < mn) mn = shots[i];
            if (shots[i] > mx) mx = shots[i];
            if (verbose)
                printf("game %d: %d shots\n", i, shots[i]);
        }
        {
            double mean = sum / games, sd = sqrt(sq / games - mean * mean);
            int j, k2, tmp;
            for (j = 1; j < games; j++)       /* insertion sort for the median */
                for (k2 = j; k2 > 0 && shots[k2 - 1] > shots[k2]; k2--) {
                    tmp = shots[k2]; shots[k2] = shots[k2 - 1]; shots[k2 - 1] = tmp;
                }
            printf("level %d%s flags %d samples %d: %d games, mean %.2f (+-%.2f se), sd %.2f, median %d, "
                   "min %d, max %d | exact moves %.1f%%, work/move %.0f, %.2f ms/move\n",
                   level, (rules & RULE_NOTOUCH) ? " notouch" : "", flags, samples, games, mean,
                   sd / sqrt(games), sd, shots[games / 2], mn, mx, 100.0 * exacts / moves, work / moves,
                   1000.0 * (clock() - t0) / CLOCKS_PER_SEC / moves);
        }
        free(shots);
    }
    return 0;
}
