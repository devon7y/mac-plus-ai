/* Host test: the C player (app/player.c) on input scripts; prints the state after each step.
 *   player_test SEED SCRIPT.txt   (lines: x y z yaw | fwd strafe jump dyaw dpitch) */
#include <stdio.h>
#include <stdlib.h>
#include "world.h"
#include "player.h"

static void *xalloc(long n) { return calloc(1, (size_t)n); }

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[2], "r");
    char kind[8];
    Player p;
    if (world_init(xalloc)) return 1;
    world_generate((unsigned short)atoi(argv[1]));
    if (getenv("BW_WORLD")) {                       /* a changed world (check_player.py --props) */
        FILE *wf = fopen(getenv("BW_WORLD"), "rb");
        if (!wf || fread(bw_blocks, 1, (size_t)WX * WX * WY, wf) != (size_t)WX * WX * WY) return 3;
        fclose(wf);
    }
    while (fscanf(f, "%7s", kind) == 1) {
        if (kind[0] == 'P') {
            long x, y, z; int yaw;
            if (fscanf(f, "%ld %ld %ld %d", &x, &y, &z, &yaw) != 4) return 2;
            player_init(&p, x, y, z, (short)yaw);
        } else {
            int fw, st, j, dy, dp;
            if (fscanf(f, "%d %d %d %d %d", &fw, &st, &j, &dy, &dp) != 5) return 2;
            player_look(&p, (short)dy, (short)dp);
            player_tick(&p, (short)fw, (short)st, (short)j);
            printf("%ld %ld %ld %d %d %d %d %ld\n", p.x, p.y, p.z, p.vy, p.yaw, p.pitch, p.ground, p.autojumps);
        }
    }
    return 0;
}
