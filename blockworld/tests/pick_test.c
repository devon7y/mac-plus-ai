/* Host test: pick_ray against a brute-force march (1/256-block steps) on random rays. */
#include <stdio.h>
#include <stdlib.h>
#include "world.h"
#include "player.h"
#include "pick.h"
#include "tables.h"

static void *xalloc(long n) { return calloc(1, (size_t)n); }
static int solid(int x, int y, int z)
{
    if (x < 0 || x >= WX || z < 0 || z >= WX || y >= WY) return 0;
    if (y < 0) return 1;
    return bw_blocks[BIDX(x, z, y)] != AIR && bw_blocks[BIDX(x, z, y)] != WATER;
}

int main(void)
{
    int i, same = 0, both = 0, miss = 0, n = 20000, hits = 0;
    srand(5);
    world_init(xalloc);
    world_generate(2);
    for (i = 0; i < n; i++) {
        RCam c;
        Pick p;
        long t, hx = -99, hy = -99, hz = -99;
        int found = 0;
        double dx, dy, dz;
        int gx = 8 + rand() % (WX - 16), gz = 8 + rand() % (WX - 16), gy = WY - 1;
        while (gy > 0 && !solid(gx, gy, gz)) gy--;
        c.x = (gx << 8) + rand() % 256; c.z = (gz << 8) + rand() % 256;
        c.y = ((gy + 1) << 8) + 415 + (rand() % 3 == 0 ? rand() % 512 : 0);
        {
            short aim = (short)(rand() % 481 - 240);         /* up to 84 degrees up or down */
            double ca = bw_sin[(aim + 256) & 1023] / 16384.0;
            c.yaw = (short)(rand() % 1024); c.pitch = 0;
            pick_ray(&c, aim, &p, solid);
            dx = bw_sin[(c.yaw + 256) & 1023] / 16384.0 * ca; dz = bw_sin[c.yaw & 1023] / 16384.0 * ca;
            dy = bw_sin[aim & 1023] / 16384.0;
        }
        if (solid(c.x >> 8, c.y >> 8, c.z >> 8)) continue;
        for (t = 1; t <= PICK_REACH * 4; t++) {           /* t in 1/1024 block along the ray */
            double fx = c.x / 256.0 + dx * t / 1024.0, fy = c.y / 256.0 + dy * t / 1024.0, fz = c.z / 256.0 + dz * t / 1024.0;
            int x = (int)(fx < 0 ? fx - 1 : fx), y = (int)(fy < 0 ? fy - 1 : fy), z = (int)(fz < 0 ? fz - 1 : fz);
            if (solid(x, y, z)) { found = 1; hx = x; hy = y; hz = z; break; }
        }
        if (found) hits++;
        if (found && p.hit && p.x == hx && p.y == hy && p.z == hz) same++;
        else if (found != p.hit) miss++;
        else if (found && p.hit) both++;
        else same++;
    }
    printf("%d rays: %d agree (%d hits), %d differ in the block, %d hit/miss differ\n", n, same, hits, both, miss);
    return 0;
}
