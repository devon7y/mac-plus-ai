/*
 * Host game simulator: the AI plays full games with all the rules. Reports scores, game
 * lengths and how often each feature happens; --hash prints the state hash after a fixed
 * seeded game (the Mac's debug build must print the same).
 *
 *   cc -O2 -Icore -Ibuild/gen tests/gamesim.c core/game.c core/phys.c core/ai.c build/gen/table_data.c -o build/gamesim
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"

static Game G;

int main(int argc, char **argv)
{
    int games = argc > 1 ? atoi(argv[1]) : 50, n, i;
    long total_ticks = 0;
    double total_score = 0;
    long cnt_multi = 0, cnt_jack = 0, cnt_sing = 0, cnt_tur = 0, cnt_pass = 0, cnt_skill = 0, cnt_lock = 0,
         cnt_epoch = 0, cnt_save = 0, cnt_kick = 0, cnt_tilt = 0;
    int32_t best = 0;
#ifdef PHYS_STATS
    if (argc > 1 && !strcmp(argv[1], "--stats")) {
        extern long pstat[16];
        static const char *nm[16] = {"steps", "submoves", "cell items", "bbox pass", "seg", "circle", "arc",
                                     "line", "disc", "flip bbox", "flip full", "respond", "normalize", "touch"};
        game_init(&G, 7);
        game_start(&G, 1);
        for (i = 0; i < 60 * 600; i++) {
            uint16_t b = game_ai_buttons(&G);
            int s;
            for (s = 0; s < STEPS_PER_TICK; s++)
                game_step(&G, b);
            game_tick(&G, b);
            if (G.state == GS_OVER)
                game_start(&G, 1);
        }
        for (i = 1; i < 14; i++)
            printf("%-11s %6.2f per step\n", nm[i], pstat[i] / (double)pstat[0]);
        return 0;
    }
#endif
    if (argc > 1 && !strcmp(argv[1], "--hash")) {
        /* deterministic check: seed 1, AI plays for 3 minutes of game time */
        game_init(&G, 1);
        game_start(&G, 1);
        for (i = 0; i < 60 * 180; i++) {
            uint16_t b = game_ai_buttons(&G);
            int s;
            for (s = 0; s < STEPS_PER_TICK; s++)
                game_step(&G, b);
            game_tick(&G, b);
            if (G.state == GS_OVER)
                game_start(&G, 1);
        }
        printf("hash %08lx score %ld ball %d ticks %lu\n", (unsigned long)phys_hash(&G.w), (long)G.score, G.ball,
               (unsigned long)G.ticks);
        return 0;
    }
    for (n = 0; n < games; n++) {
        long t = 0;
        game_init(&G, 1000 + n);
        game_start(&G, 1);
        while (G.state == GS_PLAY && t < 60L * 60 * 20) {
            uint16_t b = game_ai_buttons(&G);
            int s;
            for (s = 0; s < STEPS_PER_TICK; s++)
                game_step(&G, b);
            game_tick(&G, b);
            if (G.say == SAY_MULTIBALL) cnt_multi++;
            if (G.say == SAY_JACKPOT) cnt_jack++;
            if (G.say == SAY_SINGULARITY) cnt_sing++;
            if (G.say == SAY_TURING) cnt_tur++;
            if (G.say == SAY_PASSED) cnt_pass++;
            if (G.say == SAY_SKILL) cnt_skill++;
            if (G.say == SAY_TRAINED) cnt_epoch++;
            if (G.say == SAY_SAVED) cnt_save++;
            if (G.say == SAY_TILT) cnt_tilt++;
            if (G.sounds & SND_KICK) cnt_kick++;
            if (G.msg_t > 0 && !strcmp(G.msg[0], "BALL LOCKED") && G.msg_t == 119) cnt_lock++;
            G.say = 0;
            G.sounds = 0;
            t++;
        }
        total_ticks += t;
        total_score += G.score;
        if (G.score > best) best = G.score;
    }
    printf("%d AI games: mean score %.0f (best %ld), mean game %.0f s (%.0f s per ball)\n", games,
           total_score / games, (long)best, total_ticks / 60.0 / games, total_ticks / 60.0 / games / 3);
    printf("per game: multiball %.2f, jackpot %.2f, singularity %.2f, turing started %.2f passed %.2f,\n"
           "          skill shot %.2f, epochs %.2f, locks %.2f, ball saves %.2f, kicks %.2f, tilts %.2f\n",
           cnt_multi / (double)games, cnt_jack / (double)games, cnt_sing / (double)games, cnt_tur / (double)games,
           cnt_pass / (double)games, cnt_skill / (double)games, cnt_epoch / (double)games, cnt_lock / (double)games,
           cnt_save / (double)games, cnt_kick / (double)games, cnt_tilt / (double)games);
    return 0;
}
