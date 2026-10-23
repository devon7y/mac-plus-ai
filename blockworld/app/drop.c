/* Blockworld dropped items (see drop.h). */
#include <string.h>
#include "drop.h"
#include "obj.h"
#include "inv.h"
#include "world.h"
#include "sound.h"

Drop drops[NDROP];
unsigned short drop_age[NDROP];

#define LIFE 6000                  /* 5 minutes, then gone */
static long labs_(long v) { return v < 0 ? -v : v; }

void drop_spawn(unsigned char item, short count, long x, long y, long z)
{
    short i, oldest = 0;
    if (!item || count <= 0) return;
    for (i = 0; i < NDROP; i++)                        /* onto a stack of the same lying near */
        if (drops[i].count && drops[i].item == item && labs_(drops[i].x - x) < 128 && labs_(drops[i].z - z) < 128
            && labs_(drops[i].y - y) < 256 && drops[i].count + count <= (item_stack[item] ? item_stack[item] : 64)) {
            drops[i].count = (unsigned char)(drops[i].count + count);
            drop_age[i] = 0;
            return;
        }
    for (i = 0; i < NDROP; i++) {
        if (!drops[i].count) break;
        if (drop_age[i] > drop_age[oldest]) oldest = i;
    }
    if (i == NDROP) i = oldest;                        /* full: the oldest goes */
    drops[i].x = x; drops[i].y = y; drops[i].z = z;
    drops[i].vy = 24;                                  /* a little hop */
    drops[i].item = item; drops[i].count = (unsigned char)count;
    drop_age[i] = 0;
}

short drops_tick(const Player *pl, short collect_all)
{
    short i, moved = 0;
    long px = pl->x, py = pl->y + 160, pz = pl->z;
    for (i = 0; i < NDROP; i++) {
        Drop *d = &drops[i];
        long dx, dy, dz, ny;
        if (!d->count) continue;
        if (++drop_age[i] > LIFE) { d->count = 0; continue; }
        dx = px - d->x; dy = py - d->y; dz = pz - d->z;
        if (collect_all || (labs_(dx) < 180 && labs_(dz) < 180 && labs_(dy) < 300)) {   /* picked up */
            short left = (short)inv_add(d->item, d->count);
            if (left < d->count) { snd_play(FX_CRAFT); moved |= 2; }
            d->count = (unsigned char)left;
            if (!left) continue;
        }
        if (labs_(dx) < 512 && labs_(dz) < 512 && labs_(dy) < 512) {  /* within 2 blocks: drifts in */
            d->x += dx / 4; d->z += dz / 4; d->y += dy / 4;
            d->vy = 0;
            moved = 1;
            continue;
        }
        ny = d->y + d->vy;                             /* falls onto the block below */
        if (player_solid((int)(d->x >> 8), (int)(d->z >> 8), (int)(ny >> 8))) {
            if (d->vy < 0) { d->y = ((ny >> 8) + 1) << 8; d->vy = 0; }
            else d->vy = 0;
        } else {
            d->y = ny;
            d->vy -= 8;
            if (d->vy < -160) d->vy = -160;
            if (labs_(dx) < 4096 && labs_(dz) < 4096) moved = 1;
        }
        if (d->y < 0) d->count = 0;
    }
    return moved;
}

/* ---- pictures: the item's 16 x 16 icon, its outside clear (flood fill from the edges) ---- */
#define NCACHE 24
static unsigned char cache_item[NCACHE];
static char cache_rows[NCACHE][16][17];
static const char *cache_pic[NCACHE][16];
static short ncache;

static const char *const *item_pic(unsigned char item)
{
    short i, r, c, changed;
    char (*rows)[17];
    for (i = 0; i < ncache; i++)
        if (cache_item[i] == item) return cache_pic[i];
    if (ncache >= NCACHE) return 0;                    /* (24 kinds lying about: the rest unseen) */
    i = ncache++;
    cache_item[i] = item;
    rows = cache_rows[i];
    for (r = 0; r < 16; r++) {
        for (c = 0; c < 16; c++) rows[r][c] = (item_icon[item][r] >> (15 - c)) & 1 ? '#' : 'o';
        rows[r][16] = 0;
        cache_pic[i][r] = rows[r];
    }
    for (r = 0; r < 16; r++)                           /* white touching the edge: clear */
        for (c = 0; c < 16; c++)
            if ((r == 0 || r == 15 || c == 0 || c == 15) && rows[r][c] == 'o') rows[r][c] = '.';
    do {
        changed = 0;
        for (r = 1; r < 15; r++)
            for (c = 1; c < 15; c++)
                if (rows[r][c] == 'o' && (rows[r - 1][c] == '.' || rows[r + 1][c] == '.' || rows[r][c - 1] == '.'
                                          || rows[r][c + 1] == '.')) { rows[r][c] = '.'; changed = 1; }
    } while (changed);
    return cache_pic[i];
}

void drops_draw(const RCam *cam, unsigned char *buf, short fw, short fh, short moving, short dmax)
{
    long lim = (long)dmax << 8;
    short i;
    for (i = 0; i < NDROP; i++) {
        const Drop *d = &drops[i];
        Sprite sp;
        if (!d->count || labs_(d->x - cam->x) > lim || labs_(d->z - cam->z) > lim) continue;
        sp.x = d->x; sp.y = d->y; sp.z = d->z;
        sp.sw = 96; sp.sh = 96;
        sp.pic = item_pic(d->item); sp.pw = 16; sp.ph = 16;
        if (!sp.pic) continue;
        sp.flip = 0; sp.solid = 0; sp.dim = 0;
        sprite_draw(cam, &sp, buf, fw, fh, moving);
    }
}
