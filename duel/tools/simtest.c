/* Host check of the simulation: computer pilot against computer pilot, many matches.
 *   cc -O2 -I app -o build/simtest tools/simtest.c app/sim.c && build/simtest [matches] [lvl0] [lvl1]
 * Prints rounds, how ships died, match winners, and checks that a re-run with the same seed and
 * inputs gives the same checksums (determinism). */
#include <stdio.h>
#include <stdlib.h>
#include "sim.h"

static uint32_t run(int matches, int l0, int l1, uint32_t seed, int verbose, long *steps_out)
{
    static Sim s;
    Ai a0, a1;
    long steps = 0, causes[5] = {0}, wins[2] = {0}, rounds = 0, draws = 0, fires = 0, hypers = 0;
    uint32_t h = 0;
    int m = 0;
    sim_init(&s, seed, 5);
    ai_init(&a0, seed * 3 + 1, l0, 1);
    ai_init(&a1, seed * 7 + 5, l1, 1);
    while (m < matches && steps < 5000000) {
        uint8_t i0 = ai_input(&a0, &s, 0), i1 = ai_input(&a1, &s, 1);
        sim_step(&s, i0, i1);
        steps++;
        if (s.events & (EV_FIRE0 | EV_FIRE1)) fires++;
        if (s.events & EV_HYPER) hypers++;
        if (s.events & EV_BOOM)
            for (int k = 0; k < 2; k++)
                if (!s.ship[k].alive && s.ship[k].dead_t == 0) causes[s.ship[k].cause]++;
        if (s.events & EV_ROUND) {
            rounds++;
            if (s.winner == 2) draws++;
        }
        if (s.events & EV_MATCH) {
            wins[s.winner]++;
            m++;
        }
        if (steps % 30 == 0) h = h * 31 + sim_checksum(&s);
    }
    if (verbose) {
        printf("levels %d vs %d: %d matches in %ld steps (%.1f min of play), %ld rounds (%ld draws), "
               "avg round %.1f s\n", l0, l1, m, steps, steps / 30.0 / 60, rounds, draws, steps / 30.0 / (rounds ? rounds : 1));
        printf("  wins %ld : %ld; deaths: torpedo %ld, star %ld, collision %ld, hyperspace %ld; "
               "shots %ld, jumps %ld\n", wins[0], wins[1], causes[1], causes[2], causes[3], causes[4], fires, hypers);
    }
    *steps_out = steps;
    return h;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 20;
    long st1, st2;
    uint32_t h1, h2;
    run(n, 1, 1, 12345, 1, &st1);
    run(n, 2, 0, 777, 1, &st1);
    run(n, 2, 1, 999, 1, &st1);
    run(n, 2, 2, 4242, 1, &st1);
    h1 = run(5, 1, 2, 31337, 0, &st1);
    h2 = run(5, 1, 2, 31337, 0, &st2);
    printf("determinism: %08x/%ld vs %08x/%ld -> %s\n", (unsigned)h1, st1, (unsigned)h2, st2, h1 == h2 && st1 == st2 ? "same" : "DIFFERENT");
    return 0;
}
