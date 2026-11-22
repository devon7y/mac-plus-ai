/*
 * Host simulator: the AI plays many balls on the real table (physics only, no rules) to find
 * dead spots, escapes and what the shots reach. Uses the generated table data.
 *
 *   cc -O2 -Icore -Ibuild/gen tests/sim.c core/phys.c core/ai.c build/gen/table_data.c -o build/sim
 *   build/sim [balls] [--dump N]   (--dump N: path of ball N for tools/plot_path.py)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "phys.h"
#include "ai.h"
#include "table_data.h"

static World W;
static Ai A;

static const char *idname(int id)
{
    static const char *n[] = {"?", "LANE0", "LANE1", "LANE2", "LANE3", "LANE4", "BUMPER0", "BUMPER1", "BUMPER2",
        "SLING_L", "SLING_R", "DROP0", "DROP1", "DROP2", "SPINNER", "ORBIT", "RAMP_MADE", "SINK",
        "INLANE_L", "INLANE_R", "OUTLANE_L", "OUTLANE_R", "KICKBACK", "SHOOTER_EXIT", "POST"};
    return id >= 0 && id < (int)(sizeof n / sizeof n[0]) ? n[id] : "?";
}

int main(int argc, char **argv)
{
    int balls = argc > 1 ? atoi(argv[1]) : 200, dump = -1, b, i;
    long counts[64] = {0}, flips = 0, escapes = 0, stuck = 0, timeouts = 0, held_total = 0;
    double total_t = 0;
    long kick_counts[16] = {0};
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--dump") && i + 1 < argc)
            dump = atoi(argv[++i]);
    for (b = 0; b < balls; b++) {
        long t, still_since = -1;
        int done = 0;
        if (phys_init(&W, table_shapes, NSHAPES, table_mats, TABLE_W * 64, TABLE_H * 64)) {
            printf("phys_init failed\n");
            return 2;
        }
        phys_add_flipper(&W, &table_flippers[0]);
        phys_add_flipper(&W, &table_flippers[1]);
        phys_set_ramp_box(&W, RAMP_BOX);
        phys_set_plunger(&W, PLUNGER_X0, PLUNGER_X1, PLUNGER_Y, PLUNGER_PULL, PLUNGER_RATE, PLUNGER_SPEED);
        phys_arm(&W, phys_find(&W, S_LINE, ID_KICKBACK), 1);
        ai_init(&A, 1000 + b);
        phys_add_ball(&W, BALL_START_X, BALL_START_Y, 0, 0);
        if (b == dump) {
            for (i = 0; i < W.nshapes; i++) {
                const Shape *s = &W.shapes[i];
                printf("S %d %.2f %.2f %.2f %.2f %.2f 0 0\n", s->kind, s->ax / 64.0, s->ay / 64.0, s->bx / 64.0, s->by / 64.0, s->len / 64.0);
            }
            for (i = 0; i < W.nflip; i++)
                printf("FD %d %.2f %.2f %.2f\n", i, W.flip[i].d.px / 64.0, W.flip[i].d.py / 64.0, W.flip[i].d.len / 64.0);
        }
        for (t = 0; t < 60L * 180 && !done; t++) {      /* 3 minutes max per ball */
            uint16_t in = ai_tick(&A, &W);
            int s;
            if ((in & 1) && !W.flip[0].held) flips++;
            phys_flipper_button(&W, 0, in & 1);
            phys_flipper_button(&W, 1, (in >> 1) & 1);
            phys_plunger_button(&W, (in >> 2) & 1);
            for (s = 0; s < STEPS_PER_TICK; s++) {
                int e;
                W.nev = 0;
                phys_step(&W);
                for (e = 0; e < W.nev; e++) {
                    Event *ev = &W.ev[e];
                    if (ev->type == EV_DRAIN) done = 1;
                    else if (ev->type == EV_ESCAPE) {
                        done = 1;
                        escapes++;
                        printf("ESCAPE ball %d at t=%.1f s\n", b, t / 60.0);
                    } else if (ev->type == EV_SINK) {
                        counts[ID_SINK]++;
                    } else if (ev->type == EV_KICKBACK) {
                        kick_counts[0]++;
                    } else if (ev->type == EV_DROP) {
                        counts[ev->id]++;
                    } else if (ev->type == EV_SENSOR || ev->type == EV_SPIN || ev->type == EV_BUMPER || ev->type == EV_SLING) {
                        if (ev->type != EV_SENSOR || ev->val < 0 || ev->id == ID_RAMP_MADE)
                            counts[ev->id]++;
                    }
                }
            }
            /* sink: eject after a second, towards the left flipper */
            for (i = 0; i < MAX_BALLS; i++)
                if (W.ball[i].state == BALL_HELD) {
                    if (++held_total % 60 == 0)
                        phys_eject(&W, i, -152000, 248000); /* about 520 px/s, down-left */
                }
            /* drop targets: reset when all three are down */
            {
                int d0 = phys_find(&W, S_SEG, ID_DROP0), k, down = 0;
                for (k = 0; k < 3; k++)
                    if (W.shapes[d0 + k].flags & SF_OFF) down++;
                if (down == 3)
                    for (k = 0; k < 3; k++) phys_shape_on(&W, d0 + k, 1);
            }
            /* stuck: no motion for 4 s, not held, not on a raised flipper or the plunger */
            if (W.ball[0].state == BALL_FREE && W.ball[0].still > STEPS_PER_TICK * 60 * 4) {
                double x = W.ball[0].x / 65536.0, y = W.ball[0].y / 65536.0;
                int on_flipper = y > 255 && x > 50 && x < 200;
                int on_plunger = x > 238;
                if (!on_flipper && !on_plunger) {
                    if (still_since < 0) {
                        printf("STUCK ball %d at (%.1f, %.1f) t=%.1f s layer %d\n", b, x, y, t / 60.0, W.ball[0].layer);
                        stuck++;
                    }
                    still_since = t;
                    W.ball[0].vx += 80000;        /* "ball search" kick */
                    W.ball[0].vy -= 120000;
                    W.ball[0].sleep = 0;
                    W.ball[0].still = 0;
                }
            }
            if (b == dump) {
                printf("0 %.3f %.3f F0 %d F1 %d\n", W.ball[0].x / 65536.0, W.ball[0].y / 65536.0,
                       (int)(W.flip[0].ang >> 4), (int)(W.flip[1].ang >> 4));
            }
        }
        if (!done) {
            timeouts++;
            if (timeouts <= 3)
                printf("TIMEOUT ball %d: last pos (%.1f, %.1f), still %u\n", b, W.ball[0].x / 65536.0, W.ball[0].y / 65536.0, W.ball[0].still);
        }
        total_t += t / 60.0;
    }
    if (dump >= 0)
        return 0;
    printf("%d balls: mean %.1f s per ball, %ld timeouts (3 min), %ld escapes, %ld stuck, %ld kickbacks\n",
           balls, total_t / balls, timeouts, escapes, stuck, kick_counts[0]);
    for (i = 1; i < ID_COUNT; i++)
        if (counts[i])
            printf("  %-14s %6.2f per ball\n", idname(i), counts[i] / (double)balls);
    (void)flips;
    return 0;
}
