/*
 * Aim sweep: cradle a ball on a flipper, release it, flip when it has rolled t px along the
 * flipper, and record the first target the shot reaches. Prints a table per flipper and
 * writes the best aim points per target (used by the AI) with --c.
 *
 *   cc -O2 -Icore -Ibuild/gen tests/aim.c core/phys.c build/gen/table_data.c -o build/aim
 */
#include <stdio.h>
#include <string.h>
#include "phys.h"
#include "table_data.h"

static World W;

static const char *tname(int id)
{
    switch (id) {
    case ID_RAMP_MADE: return "RAMP";
    case ID_SPINNER: return "SPINNER";
    case ID_SINK: return "SINK";
    case ID_DROP0: case ID_DROP1: case ID_DROP2: return "DROP";
    case ID_LANE0: case ID_LANE1: case ID_LANE2: case ID_LANE3: case ID_LANE4: return "LANES";
    case ID_BUMPER0: case ID_BUMPER1: case ID_BUMPER2: return "BUMPER";
    case ID_SLING_L: case ID_SLING_R: return "SLING";
    case ID_ORBIT: return "ORBIT";
    case -1: return "DRAIN";
    case 0: return "-";
    }
    return "other";
}

static void setup(void)
{
    phys_init(&W, table_shapes, NSHAPES, table_mats, TABLE_W * 64, TABLE_H * 64);
    phys_add_flipper(&W, &table_flippers[0]);
    phys_add_flipper(&W, &table_flippers[1]);
    phys_set_ramp_box(&W, RAMP_BOX);
    phys_set_plunger(&W, PLUNGER_X0, PLUNGER_X1, PLUNGER_Y, PLUNGER_PULL, PLUNGER_RATE, PLUNGER_SPEED);
}

static void ticks(int n, int f, int held)
{
    while (n-- > 0) {
        int s;
        phys_flipper_button(&W, f, held);
        for (s = 0; s < STEPS_PER_TICK; s++) {
            W.nev = 0;
            phys_step(&W);
        }
    }
}

int main(int argc, char **argv)
{
    int f, k, feed;
    int c_out = argc > 1 && !strcmp(argv[1], "--c");
    for (feed = 0; feed < 2; feed++)
    for (f = 0; f < 2; f++) {
        const FlipperDef *fd = &table_flippers[f];
        double px = fd->px / 64.0, py = fd->py / 64.0;
        int ux = fcos(fd->rest), uy = fsin(fd->rest);
        if (!c_out)
            printf("%s flipper, %s:\n", f == 0 ? "left" : "right", feed ? "ball from the inlane" : "from the cradle");
        for (k = 0; k < 144; k++) {
            int t, first = 0, flipped = 0;
            double at = 2 + k * 0.25;
            setup();
            if (!feed) {
                /* drop the ball onto the raised flipper, let it settle in the cradle */
                ticks(10, f, 1);
                phys_add_ball(&W, (fx)((px + (f == 0 ? 20 : -20)) * 65536), (fx)((py - 40) * 65536), 0, 0);
                ticks(180, f, 1);
            } else {
                /* drop the ball into the top of the inlane: it rolls down onto the flipper */
                phys_add_ball(&W, (fx)((f == 0 ? 27.0 : 242.0 - 27.0) * 65536), (fx)(222.0 * 65536), 0, 0);
            }
            /* release; flip (deciding every 2 physics steps, as the Mac app polls the keys)
               when the ball is `at` px along; hold for 14 ticks */
            for (t = 0; t < STEPS_PER_TICK * 60 * 4 && !first; t++) {
                double dx = W.ball[0].x / 65536.0 - px, dy = W.ball[0].y / 65536.0 - py;
                double along = (dx * ux + dy * uy) / 16384.0;
                int e;
                if (!flipped && along >= at)                  /* (decided every step, as the Mac app) */
                    flipped = t + 1;
                if (!flipped && t > STEPS_PER_TICK * 150)
                    break;
                phys_flipper_button(&W, f, flipped && t - flipped < 14 * STEPS_PER_TICK);
                W.nev = 0;
                phys_step(&W);
                for (e = 0; e < W.nev && !first; e++) {
                    Event *ev = &W.ev[e];
                    if (ev->type == EV_DRAIN) first = -1;
                    else if (ev->type == EV_SINK) first = ID_SINK;
                    else if (ev->type == EV_SPIN) first = ID_SPINNER;
                    else if ((ev->type == EV_SENSOR && ev->id == ID_RAMP_MADE) || ev->type == EV_DROP ||
                             ev->type == EV_BUMPER || ev->type == EV_SLING ||
                             (ev->type == EV_SENSOR && ev->id >= ID_LANE0 && ev->id <= ID_LANE4))
                        first = ev->id;
                }
            }
            if (!c_out)
                printf("  flip at %4.1f px: %-8s%s\n", at, flipped ? tname(first) : "(no flip)",
                       first == 0 && flipped ? "  (ball still in play)" : "");
            else if (flipped && first)
                printf("%d %d %.2f %s\n", feed, f, at, tname(first));
        }
    }
    return 0;
}
