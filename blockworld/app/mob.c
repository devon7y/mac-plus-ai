/* Blockworld mobs: bodies, AI, combat, arrows, spawning and drawing (see mob.h). */
#include <string.h>
#include "mob.h"
#include "world.h"
#include "obj.h"
#include "tables.h"
#include "sound.h"

Mob mobs[NMOB];
Arrow arrows[NARROW];

#ifdef __m68k__
_Static_assert(sizeof(Mob) == 36, "Mob");
#endif

#define BL 256                          /* a block (8.8) */
#define GRAV 20
#define DESPAWN (40L * BL)              /* farther than this: gone */

static unsigned long rs = 0x6C078965UL;
static unsigned short mrnd(void)        /* xorshift */
{
    rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
    return (unsigned short)(rs >> 8);
}
static long labs_(long v) { return v < 0 ? -v : v; }
static long dist2d(long dx, long dz)    /* about the distance (max + min / 2: within 12%) */
{
    long a = labs_(dx), b = labs_(dz);
    return a > b ? a + b / 2 : b + a / 2;
}

short yaw_to(long dx, long dz)
{
    long ax = labs_(dx), az = labs_(dz);
    short a;
    if (!ax && !az) return 0;
    if (ax >= az) a = mob_atan[(az * 64) / ax];
    else a = (short)(256 - mob_atan[(ax * 64) / az]);
    if (dx < 0) a = (short)(512 - a);
    if (dz < 0) a = (short)-a;
    return (short)(a & 1023);
}

/* ---- bodies -------------------------------------------------------------------------------- */

static int box_hits(const MobKind *k, long x, long y, long z)
{
    int x0 = (int)((x - k->hw) >> 8), x1 = (int)((x + k->hw - 1) >> 8);
    int z0 = (int)((z - k->hw) >> 8), z1 = (int)((z + k->hw - 1) >> 8);
    int y0 = (int)(y >> 8), y1 = (int)((y + k->ht - 1) >> 8);
    int bx, bz, by;
    for (bx = x0; bx <= x1; bx++)
        for (bz = z0; bz <= z1; bz++)
            for (by = y0; by <= y1; by++)
                if (player_solid(bx, bz, by)) return 1;
    return 0;
}

static short block(long x, long y, long z)
{
    long bx = x >> 8, by = y >> 8, bz = z >> 8;
    if (bx < 0 || bx >= WX || bz < 0 || bz >= WX || by < 0 || by >= WY) return AIR;
    return bw_blocks[BIDX(bx, bz, by)];
}

/* a drop of 3 or more blocks (or water) one step ahead along yaw (backwards if sp < 0) */
static int drop_ahead(const Mob *m, short sp)
{
    long c = bw_sin[(m->yaw + 256) & 1023], s = bw_sin[m->yaw & 1023];
    long ax = m->x + ((c * (sp < 0 ? -200 : 200)) >> 14), az = m->z + ((s * (sp < 0 ? -200 : 200)) >> 14);
    int bx = (int)(ax >> 8), bz = (int)(az >> 8), by = (int)(m->y >> 8);
    if (player_solid(bx, bz, by) || player_solid(bx, bz, by - 1)) return 0;
    if (block(ax, m->y - BL, az) == WATER) return 1;
    return !player_solid(bx, bz, by - 2) && !player_solid(bx, bz, by - 3);
}

/* one step: walk along the yaw at sp, knockback, floating, gravity; steps up one block (crawlers
   climb walls). Returns 1 if a wall stopped it. */
static int body_tick(Mob *m, short sp)
{
    const MobKind *k = &mob_kind[m->kind];
    long c = bw_sin[(m->yaw + 256) & 1023], s = bw_sin[m->yaw & 1023];
    long dx = ((c * sp) >> 14) + m->vx, dz = ((s * sp) >> 14) + m->vz, n;
    int blocked = 0;
    if (dx) {
        n = m->x + dx;
        if (box_hits(k, n, m->y, m->z)) blocked = 1;
        else m->x = n;
    }
    if (dz) {
        n = m->z + dz;
        if (box_hits(k, m->x, m->y, n)) blocked = 1;
        else m->z = n;
    }
    m->vx = (short)(m->vx * 3 / 4);
    m->vz = (short)(m->vz * 3 / 4);
    if (blocked && sp) {
        if (m->kind == MK_CRAWLER) m->vy = 50;                          /* up the wall */
        else if (m->ground && !box_hits(k, m->x + dx, m->y + BL + 8, m->z + dz)) m->vy = 120;   /* a step */
    }
    if (block(m->x, m->y + 128, m->z) == WATER && m->vy < 16) m->vy += 24;         /* floats */
    m->vy -= GRAV;
    if (m->vy < -200) m->vy = -200;
    n = m->y + m->vy;
    if (box_hits(k, m->x, n, m->z)) {
        if (m->vy < 0) {
            short i = 0;
            n = (m->y >> 8) << 8;
            while (box_hits(k, m->x, n, m->z) && i++ < 4) n += BL;
            m->ground = 1;
        } else
            n = m->y;
        m->vy = 0;
    } else
        m->ground = 0;
    m->y = n;
    return blocked;
}

/* nothing solid on the line between two points (4 samples): melee and arrows need it */
static int clear_line(long x0, long y0, long z0, long x1, long y1, long z1)
{
    short t;
    for (t = 1; t < 5; t++) {
        long x = x0 + (x1 - x0) * t / 5, y = y0 + (y1 - y0) * t / 5, z = z0 + (z1 - z0) * t / 5;
        if (player_solid((int)(x >> 8), (int)(z >> 8), (int)(y >> 8))) return 0;
    }
    return 1;
}

/* open to the sky above its head; inside the light of a torch */
static int sky_open(const Mob *m)
{
    long bx = m->x >> 8, bz = m->z >> 8;
    if (bx < 0 || bx >= WX || bz < 0 || bz >= WX) return 1;
    return bw_cols[(bx + BORDER) * PWX + (bz + BORDER)]->top <= ((m->y + 128) >> 8);
}
static int torch_lit(long x, long y, long z)
{
    long bx = x >> 8, bz = z >> 8, by = y >> 8;
    const unsigned char *l;
    if (bx < 0 || bx >= WX || bz < 0 || bz >= WX) return 0;
    l = bw_lit[bx * WX + bz];
    return l[1] && by >= l[0] && by < l[1];
}

/* ---- life and death ------------------------------------------------------------------------ */

short mob_spawn(short kind, long x, long y, long z)
{
    short i;
    for (i = 0; i < NMOB; i++)
        if (!mobs[i].alive) {
            Mob *m = &mobs[i];
            memset(m, 0, sizeof *m);
            m->x = x; m->y = y; m->z = z;
            m->kind = (unsigned char)kind; m->alive = 1;
            m->hp = mob_kind[kind].hp;
            m->yaw = (short)(mrnd() & 1023);
            m->timer = (short)(mrnd() % 40);
            return i;
        }
    return -1;
}

void mob_hurt(short i, short dmg, short yaw, MobWorld *w)
{
    Mob *m = &mobs[i];
    const MobKind *k = &mob_kind[m->kind];
    if (i < 0 || i >= NMOB || !m->alive) return;
    m->hp = (short)(m->hp - dmg);
    m->hurt = 6;
    m->vx = (short)(((long)bw_sin[(yaw + 256) & 1023] * 64) >> 14);
    m->vz = (short)(((long)bw_sin[yaw & 1023] * 64) >> 14);
    if (m->ground) m->vy = 70;
    if (!k->hostile) { m->mode = MO_FLEE; m->timer = 70; }
    snd_play(m->hp <= 0 ? FX_DIE : FX_HIT);
    if (m->hp <= 0) {
        short j;
        m->alive = 0;
        w->kills++;
        for (j = 0; j < 2; j++)
            if (k->drop[j][0]) {
                short n = (short)(k->drop[j][1] + mrnd() % (k->drop[j][2] - k->drop[j][1] + 1));
                if (n > 0) w->drop(k->drop[j][0], n, m->x, m->y + 128, m->z);
            }
    }
}

short mob_count(short hostile)
{
    short i, n = 0;
    for (i = 0; i < NMOB; i++)
        if (mobs[i].alive && mob_kind[mobs[i].kind].hostile == hostile) n++;
    return n;
}

short mobs_hostile_near(long x, long z, long r)
{
    short i, n = 0;
    for (i = 0; i < NMOB; i++)
        if (mobs[i].alive && mob_kind[mobs[i].kind].hostile && dist2d(mobs[i].x - x, mobs[i].z - z) < r) n++;
    return n;
}

void arrow_shoot(long x, long y, long z, short vx, short vy, short vz, unsigned char mob)
{
    short i;
    for (i = 0; i < NARROW; i++)
        if (!arrows[i].alive) {
            Arrow *a = &arrows[i];
            a->x = x; a->y = y; a->z = z; a->vx = vx; a->vy = vy; a->vz = vz;
            a->life = 100; a->alive = 1; a->mob = mob;
            return;
        }
}

/* ---- spawning ------------------------------------------------------------------------------ */

/* one try at a spot 10-24 blocks away (14-28 for animals): animals on grass under the sky,
   monsters in the dark (under the sky at night, or under cover near the player's height) */
static void try_spawn(const Player *pl, MobWorld *w, short hostile)
{
    short ang = (short)(mrnd() & 1023), r = (short)(hostile ? 10 + mrnd() % 15 : 14 + mrnd() % 15);
    long x = pl->x + (((long)bw_sin[(ang + 256) & 1023] * r) >> 6);
    long z = pl->z + (((long)bw_sin[ang & 1023] * r) >> 6);
    short bx = (short)(x >> 8), bz = (short)(z >> 8), top, y, kind;
    if (bx < 1 || bx >= WX - 1 || bz < 1 || bz >= WX - 1) return;
    top = bw_cols[(long)(bx + BORDER) * PWX + (bz + BORDER)]->top;
    if (!hostile) {
        if (top < 2 || top >= WY - 2 || bw_blocks[BIDX(bx, bz, top - 1)] != TURF) return;
        y = top;
        kind = (short)(mrnd() % 4);
    } else {
        kind = (short)(4 + mrnd() % 4);
        if (w->night && (mrnd() & 1)) {
            if (top < 2 || top >= WY - 2 || bw_blocks[BIDX(bx, bz, top - 1)] == WATER) return;
            y = top;
        } else {                                       /* a cave floor near the player's height */
            short py = (short)(pl->y >> 8);
            y = (short)(py - 10 + mrnd() % 21);
            if (y < 1 || y >= top) return;
            if (player_solid(bx, bz, y)) return;
            while (y > 1 && !player_solid(bx, bz, y - 1)) y--;
            if (y >= top && !w->night) return;
            if (bw_blocks[BIDX(bx, bz, y - 1)] == WATER || bw_blocks[BIDX(bx, bz, y)] == WATER) return;
        }
        if (torch_lit(x, (long)y << 8, z)) return;
    }
    x = ((long)bx << 8) + 128; z = ((long)bz << 8) + 128;
    if (box_hits(&mob_kind[kind], x, (long)y << 8, z)) return;
    if (dist2d(x - pl->x, z - pl->z) < 8L * BL) return;
    mob_spawn(kind, x, (long)y << 8, z);
}

/* ---- the step ------------------------------------------------------------------------------ */

static void arrows_tick(const Player *pl, MobWorld *w)
{
    short i, j, k;
    for (i = 0; i < NARROW; i++) {
        Arrow *a = &arrows[i];
        if (!a->alive) continue;
        if (--a->life <= 0) { a->alive = 0; continue; }
        for (k = 0; k < 2 && a->alive; k++) {           /* two half steps: nothing is jumped over */
            a->x += a->vx / 2; a->y += a->vy / 2; a->z += a->vz / 2;
            if (player_solid((int)(a->x >> 8), (int)(a->z >> 8), (int)(a->y >> 8))) { a->alive = 0; break; }
            if (a->mob) {
                if (labs_(a->x - pl->x) < 110 && labs_(a->z - pl->z) < 110 && a->y >= pl->y && a->y < pl->y + PL_HT) {
                    w->hurt_player(3, a->x - a->vx, a->z - a->vz);
                    a->alive = 0;
                }
            } else
                for (j = 0; j < NMOB; j++) {
                    Mob *m = &mobs[j];
                    const MobKind *mk = &mob_kind[m->kind];
                    if (m->alive && labs_(a->x - m->x) < mk->hw + 32 && labs_(a->z - m->z) < mk->hw + 32
                        && a->y >= m->y && a->y < m->y + mk->ht) {
                        mob_hurt(j, 5, yaw_to(a->vx, a->vz), w);
                        a->alive = 0;
                        break;
                    }
                }
        }
        a->vy -= 3;
    }
}

short mobs_tick(const Player *pl, MobWorld *w)
{
    static unsigned short tick;
    short i, moved = 0;
    tick++;
    for (i = 0; i < NMOB; i++) {
        Mob *m = &mobs[i];
        const MobKind *k = &mob_kind[m->kind];
        long dx, dz, dy, dist, ox, oy, oz;
        short sp = 0, oh, chase = 0;
        if (!m->alive) continue;
        dx = pl->x - m->x; dz = pl->z - m->z; dy = pl->y - m->y;
        dist = dist2d(dx, dz);
        ox = m->x; oy = m->y; oz = m->z; oh = m->hurt;
        if (m->hurt) m->hurt--;
        if (m->cool) m->cool--;
        if (dist > DESPAWN || m->y < BL) { m->alive = 0; continue; }
        if (k->hostile) {
            if (!w->night && (m->kind == MK_SHAMBLER || m->kind == MK_ARCHER) && sky_open(m) && !(tick & 15)) {
                m->hp = (short)(m->hp - 2);                               /* sunlight burns */
                m->hurt = 6;
                if (m->hp <= 0) { m->alive = 0; continue; }
            }
            if (dist > 20L * BL && !(mrnd() % 200)) { m->alive = 0; continue; }   /* far: wanders off */
            chase = dist < 16L * BL && labs_(dy) < 6L * BL;
        }
        if (chase) {
            m->mode = MO_CHASE;
            m->yaw = yaw_to(dx, dz);
            sp = k->speed;
            switch (m->kind) {
            case MK_SHAMBLER:
            case MK_CRAWLER:
                if (dist < 410 && labs_(dy) < 384) {                      /* within reach (1.6) */
                    sp = 0;
                    if (!m->cool && clear_line(m->x, m->y + 300, m->z, pl->x, pl->y + 300, pl->z)) { w->hurt_player(m->kind == MK_SHAMBLER ? 3 : 2, m->x, m->z); m->cool = 20; }
                } else if (m->kind == MK_CRAWLER && dist < 3L * BL && m->ground && !m->fuse) {
                    m->vy = 80;                                           /* a leap */
                    m->fuse = 30;
                }
                if (m->kind == MK_CRAWLER && m->fuse) m->fuse--;
                break;
            case MK_ARCHER:
                if (dist < 5L * BL) sp = (short)-k->speed;                /* keeps its distance */
                else if (dist < 10L * BL) sp = 0;
                if (!m->cool && dist < 14L * BL
                    && clear_line(m->x, m->y + k->ht * 3 / 4, m->z, pl->x, pl->y + PL_EYE, pl->z)) {   /* shoots at the eye */
                    long t = dist / 120 + 1, aim = dy + PL_EYE - 64 - (k->ht * 3 / 4);
                    long c = bw_sin[(m->yaw + 256) & 1023], s = bw_sin[m->yaw & 1023];
                    arrow_shoot(m->x + ((c * 100) >> 14), m->y + k->ht * 3 / 4, m->z + ((s * 100) >> 14),
                                (short)((c * 120) >> 14), (short)(aim / t + 3 * t / 2), (short)((s * 120) >> 14), 1);
                    m->cool = (short)(50 + mrnd() % 25);
                }
                break;
            case MK_SIZZLER:
                if (dist < 640 && labs_(dy) < 512) {                      /* hisses */
                    sp = 0;
                    if (!m->fuse++) snd_play(FX_HISS);
                }
                else if (dist > 1024 && m->fuse > 0) m->fuse = (short)(m->fuse > 2 ? m->fuse - 2 : 0);
                if (m->fuse >= 30) {
                    m->alive = 0;
                    w->explode(m->x, m->y + 128, m->z);
                    moved = 1;
                    continue;
                }
                break;
            }
        } else if (m->mode == MO_FLEE) {
            m->yaw = (short)((yaw_to(-dx, -dz) + (mrnd() & 63) - 32) & 1023);
            sp = (short)(k->speed * 3 / 2);
            if (--m->timer <= 0) m->mode = MO_IDLE;
        } else {
            if (m->mode == MO_CHASE) m->mode = MO_IDLE;
            if (--m->timer <= 0) {                                        /* wander or stand */
                if (mrnd() & 1) { m->mode = MO_WALK; m->yaw = (short)(mrnd() & 1023); }
                else m->mode = MO_IDLE;
                m->timer = (short)(20 + mrnd() % 60);
            }
            if (m->mode == MO_WALK) sp = k->speed;
            if (m->kind == MK_SIZZLER && m->fuse) m->fuse--;
        }
        if (sp && m->ground && m->kind != MK_CRAWLER && drop_ahead(m, sp)) {   /* no walking off cliffs */
            sp = 0;
            if (m->mode == MO_WALK) m->timer = 0;
        }
        if (sp || m->vx || m->vz || m->vy || !m->ground || ((tick + i) & 7) == 0)
            body_tick(m, sp);                                     /* (standing still: now and then) */
        if (sp) m->walk++;
        if (m->x != ox || m->y != oy || m->z != oz || m->hurt != oh || (m->kind == MK_SIZZLER && m->fuse))
            if (dist < 16L * BL) moved = 1;
    }
    arrows_tick(pl, w);
    for (i = 0; i < NARROW; i++)
        if (arrows[i].alive) moved = 1;
    if (w->spawn && !(tick % 20)) {                                       /* spawning, once a second */
        if (mob_count(0) < NPASSIVE) try_spawn(pl, w, 0);
        if (mob_count(1) < NHOSTILE) try_spawn(pl, w, 1);
    }
    return moved;
}

/* ---- drawing ------------------------------------------------------------------------------- */

static const unsigned char DIM[8] = {0xAA, 0x44, 0xAA, 0x10, 0xAA, 0x44, 0xAA, 0x01};
static const unsigned char DARK[8] = {0xFF, 0x55, 0xFB, 0x55, 0xFF, 0x55, 0xBF, 0x55};
static const char *const arrow_pic[4] = {"##..", ".##.", "..##", "...#"};

static long depth_of(const RCam *cam, long x, long z)
{
    long c = bw_sin[(cam->yaw + 256) & 1023], s = bw_sin[cam->yaw & 1023];
    return ((x - cam->x) * c + (z - cam->z) * s) >> 14;
}

void mobs_draw(const RCam *cam, unsigned char *buf, short fw, short fh, short moving, short dmax)
{
    short order[NMOB], n = 0, i, j;
    long dep[NMOB], lim = (long)dmax << 8;
    for (i = 0; i < NMOB; i++) {                         /* the ones in view, far first */
        const Mob *m = &mobs[i];
        long d;
        if (!m->alive || labs_(m->x - cam->x) > lim || labs_(m->z - cam->z) > lim) continue;
        d = depth_of(cam, m->x, m->z);
        if (d < 40) continue;
        for (j = n; j > 0 && dep[j - 1] < d; j--) { dep[j] = dep[j - 1]; order[j] = order[j - 1]; }
        dep[j] = d; order[j] = i; n++;
    }
    for (j = 0; j < n; j++) {
        const Mob *m = &mobs[order[j]];
        const MobKind *k = &mob_kind[m->kind];
        Sprite sp;
        short rel = (short)((m->yaw - yaw_to(m->x - cam->x, m->z - cam->z)) & 1023), cls;
        sp.x = m->x; sp.y = m->y; sp.z = m->z;
        sp.sh = k->ht;
        if (rel >= 128 && rel < 384) sp.flip = 0;            /* facing right on the screen */
        else if (rel >= 640 && rel < 896) sp.flip = 1;       /* left */
        else sp.flip = -1;                                    /* towards or away from the eye */
        if (sp.flip < 0) { sp.flip = 0; sp.pic = k->front; sp.pw = k->fpw; sp.ph = k->fph; sp.sw = k->fw; }
        else { sp.pic = k->side[(m->walk >> 2) & 1]; sp.pw = k->pw; sp.ph = k->ph; sp.sw = k->sw; }
        sp.solid = m->hurt > 0 || (m->kind == MK_SIZZLER && (m->fuse & 4));
        cls = torch_lit(m->x, m->y + 128, m->z) ? 3 : sky_open(m) ? 0 : 2;
        sp.dim = r_shade[cls] == 3 ? DARK : r_shade[cls] == 2 ? DIM : 0;
        sprite_draw(cam, &sp, buf, fw, fh, moving);
    }
    for (i = 0; i < NARROW; i++) {
        const Arrow *a = &arrows[i];
        Sprite sp;
        if (!a->alive || labs_(a->x - cam->x) > lim || labs_(a->z - cam->z) > lim) continue;
        sp.x = a->x; sp.y = a->y - 48; sp.z = a->z;
        sp.sw = 96; sp.sh = 96; sp.pic = arrow_pic; sp.pw = 4; sp.ph = 4;
        sp.flip = (a->vx * (long)bw_sin[cam->yaw & 1023] - a->vz * (long)bw_sin[(cam->yaw + 256) & 1023]) > 0;
        sp.solid = 1; sp.dim = 0;
        sprite_draw(cam, &sp, buf, fw, fh, moving);
    }
}

short mob_pick(const RCam *cam, short fw, short fh, long maxd, long *depth)
{
    long c = bw_sin[(cam->yaw + 256) & 1023], s = bw_sin[cam->yaw & 1023];
    short i, best = -1;
    long h0 = fh / 2 + cam->pitch, bestd = maxd;
    (void)fw;
    for (i = 0; i < NMOB; i++) {
        const Mob *m = &mobs[i];
        const MobKind *k = &mob_kind[m->kind];
        long rx, rz, d, l, ytop, ybot, hw;
        if (!m->alive) continue;
        rx = m->x - cam->x; rz = m->z - cam->z;
        d = (rx * c + rz * s) >> 14;
        if (d < 24 || d >= bestd) continue;
        l = (rz * c - rx * s) >> 14;
        hw = k->hw < 80 ? 80 : k->hw;
        if (labs_(l) > hw) continue;
        ytop = h0 - (m->y + k->ht - cam->y) * 256 / d;
        ybot = h0 - (m->y - cam->y) * 256 / d;
        if (fh / 2 < ytop || fh / 2 >= ybot) continue;
        best = i; bestd = d;
    }
    if (best >= 0 && depth) *depth = bestd;
    return best;
}
