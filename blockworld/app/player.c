/* Blockworld player movement: a port of engine/player.py (bit-identical). */
#include "player.h"
#include "world.h"
#include "tables.h"

void player_init(Player *p, long x, long y, long z, short yaw)
{
    p->x = x; p->y = y; p->z = z;
    p->vy = 0;
    p->yaw = (short)(yaw & 1023);
    p->pitch = 0;
    p->ground = 0;
    p->autojumps = 0;
}

int player_solid(int x, int z, int y)
{
    int t;
    if (y < 0) return 1;
    if (y >= WY) return 0;
    if (x < 0 || x >= WX || z < 0 || z >= WX) return 1;
    t = bw_blocks[BIDX(x, z, y)];
    if (t != AIR && t != WATER && t != LAVA && t != LADDER) return 1;     /* (world.py SOLID) */
    return y > 0 && bw_blocks[BIDX(x, z, y - 1)] == FENCE;                /* fences are 1.5 high */
}

static int box_hits(long x, long y, long z)
{
    int x0 = (int)((x - PL_HW) >> 8), x1 = (int)((x + PL_HW - 1) >> 8);
    int z0 = (int)((z - PL_HW) >> 8), z1 = (int)((z + PL_HW - 1) >> 8);
    int y0 = (int)(y >> 8), y1 = (int)((y + PL_HT - 1) >> 8);
    int bx, bz, by;
    for (bx = x0; bx <= x1; bx++)
        for (bz = z0; bz <= z1; bz++)
            for (by = y0; by <= y1; by++)
                if (player_solid(bx, bz, by)) return 1;
    return 0;
}

void player_push(Player *p, long dx, long dz)
{
    if (dx && !box_hits(p->x + dx, p->y, p->z)) p->x += dx;
    if (dz && !box_hits(p->x, p->y, p->z + dz)) p->z += dz;
}

void player_look(Player *p, short dyaw, short dpitch)
{
    short pt = (short)(p->pitch + dpitch);
    p->yaw = (short)((p->yaw + dyaw) & 1023);
    if (pt > PL_PITCH) pt = PL_PITCH;
    if (pt < -PL_PITCH) pt = -PL_PITCH;
    p->pitch = pt;
}

void player_tick(Player *p, short fwd, short strafe, short jump)
{
    long c = bw_sin[(p->yaw + 256) & 1023], s = bw_sin[p->yaw & 1023];
    long mx = fwd * c - strafe * s, mz = fwd * s + strafe * c;
    long dx, dz, nx, nz, ny;
    int blocked = 0;
    if (fwd && strafe) {                         /* diagonal: 1/sqrt(2) */
        mx = (mx * 181) >> 8;
        mz = (mz * 181) >> 8;
    }
    dx = (mx * PL_WALK) >> 14;
    dz = (mz * PL_WALK) >> 14;
    if (dx) {
        nx = p->x + dx;
        if (box_hits(nx, p->y, p->z)) {
            blocked = 1;
            nx = dx > 0 ? (((nx + PL_HW - 1) >> 8) << 8) - PL_HW : ((((nx - PL_HW) >> 8) + 1) << 8) + PL_HW;
            if (box_hits(nx, p->y, p->z)) nx = p->x;
        }
        p->x = nx;
    }
    if (dz) {
        nz = p->z + dz;
        if (box_hits(p->x, p->y, nz)) {
            blocked = 1;
            nz = dz > 0 ? (((nz + PL_HW - 1) >> 8) << 8) - PL_HW : ((((nz - PL_HW) >> 8) + 1) << 8) + PL_HW;
            if (box_hits(p->x, p->y, nz)) nz = p->z;
        }
        p->z = nz;
    }
    /* auto-jump: walking into a one-block step with room above it */
    if (blocked && p->ground && (dx || dz))
        if (!box_hits(p->x + dx, p->y + 256 + 8, p->z + dz)) {
            jump = 1;
            p->autojumps++;
        }
    if (jump && p->ground) {
        p->vy = PL_JUMP;
        p->ground = 0;
    }
    p->vy -= PL_GRAV;
    if (p->vy < -PL_VMAX) p->vy = -PL_VMAX;
    ny = p->y + p->vy;
    if (box_hits(p->x, ny, p->z)) {
        if (p->vy < 0) {
            ny = (p->y >> 8) << 8;               /* land on the block below */
            while (box_hits(p->x, ny, p->z)) ny += 256;
            p->ground = 1;
        } else
            ny = p->y;
        p->vy = 0;
    } else
        p->ground = 0;
    p->y = ny;
}
