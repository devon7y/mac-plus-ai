/* Blockworld world generator and segment lists: a port of engine/world.py (bit-identical). */
#include <stdint.h>
#include <string.h>
#include "world.h"
#include "tables.h"

unsigned char *bw_blocks;
Seg **bw_cols;
unsigned char bw_coarse[CWX * CWX];

static Seg *seg_heap;               /* segment storage */
static unsigned short *col_id;      /* each column's id in the last loaded world (with col_cap 0: still its list) */
static short col_ids_n;             /* how many ids that world had (0: none) */
static long heap_size, heap_used;   /* in Seg units */
static unsigned char *col_cap;      /* capacity (in Segs, including the terminator) per column */
static Seg ocean[2] = {{SEA + 1, 0, WATER, SEG_TOPEXP | (1 << 2)}, {0, 0, SEG_COVERED, 0}};
unsigned char bw_lit[WX * WX][2];
static void *(*walloc)(long);
void (*world_progress)(short permille);
#define PROG(v) do { if (world_progress) world_progress((short)(v)); } while (0)

#define HEAP_SEGS 180000L           /* about 4.1 segments per column average plus slack */

static uint32_t hash3(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = a * 0x9E3779B1UL + b * 0x85EBCA6BUL + c * 0xC2B2AE35UL;
    h ^= h >> 15;
    h *= 0x2C1B3C6DUL;
    h ^= h >> 12;
    h *= 0x297A2D39UL;
    h ^= h >> 15;
    return h;
}

static uint32_t rng_s;
static uint32_t rng_next(void)
{
    uint32_t s = rng_s;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    rng_s = s;
    return s;
}
static int below(int n) { return (int)(((rng_next() >> 16) * (uint32_t)n) >> 16); }

static int smooth(int t) { return (int)(((long)t * t * (768 - 2 * t)) >> 16); }

int world_init(void *(*alloc)(long))
{
    walloc = alloc;
    bw_blocks = (unsigned char *)alloc((long)WX * WX * WY);
    bw_cols = (Seg **)alloc((long)PWX * PWX * sizeof(Seg *));
    col_cap = (unsigned char *)alloc((long)WX * WX);
    heap_size = HEAP_SEGS;
    seg_heap = (Seg *)alloc((heap_size + WY + 2) * sizeof(Seg));      /* + a column's worth of slack */
    col_id = (unsigned short *)alloc((long)WX * WX * 2);
    col_ids_n = 0;
    if (!bw_blocks || !bw_cols || !col_cap || !seg_heap || !col_id)
        return -1;
    return 0;
}

#define AT(x, z, y) bw_blocks[BIDX(x, z, y)]

/* The height map's value noise, with each octave's lattice hashed once (the same values as
 * vnoise; about 50x faster on the 68000, whose 32-bit multiplies are subroutines). */
#define NOCT 5
#define LATN 38                     /* lattice points per side for shift 2: ((WX + 16) >> 2) + 2 */
static const short oct_def[NOCT][4] = {{7, 3, 4, 101}, {0, 0, 3, 202}, {0, 0, 2, 303}, {1, 5, 5, 404}, {3, 9, 6, 808}};
static unsigned char oct_lat[NOCT][LATN * LATN];
static unsigned short oct_smooth[NOCT][64];

static void noise_setup(uint32_t seed)
{
    int o, gx, gz, i;
    for (o = 0; o < NOCT; o++) {
        int sh = oct_def[o][2], n = ((WX + 16) >> sh) + 2;
        for (gx = 0; gx < n; gx++)
            for (gz = 0; gz < n; gz++)
                oct_lat[o][gx * LATN + gz] = (unsigned char)(hash3(gx, gz, seed + oct_def[o][3]) & 255);
        for (i = 0; i < (1 << sh); i++)
            oct_smooth[o][i] = (unsigned short)smooth((i << 8) >> sh);
    }
}

static int onoise(int o, int x, int z)
{
    int sh = oct_def[o][2], m = (1 << sh) - 1;
    const unsigned char *L;
    short a, b, c, d, ab, cd, tx, tz;
    x += oct_def[o][0];
    z += oct_def[o][1];
    L = &oct_lat[o][(x >> sh) * LATN + (z >> sh)];
    tx = (short)oct_smooth[o][x & m];
    tz = (short)oct_smooth[o][z & m];
    a = L[0]; b = L[LATN]; c = L[1]; d = L[LATN + 1];
    ab = (short)(a + (((long)(short)(b - a) * tx) >> 8));
    cd = (short)(c + (((long)(short)(d - c) * tx) >> 8));
    return ab + (int)(((long)(short)(cd - ab) * tz) >> 8);
}

static unsigned char height_at(int x, int z, uint32_t seed)
{
    int n1 = onoise(0, x, z);
    int n2 = onoise(1, x, z);
    int n3 = onoise(2, x, z);
    int rough = onoise(3, x, z);
    int big = onoise(4, x, z);
    int hill = ((n1 - 128) * (6 + (rough >> 4))) >> 7;
    int h = 31 + hill + (((n2 - 128) * 3) >> 7) + ((n3 - 128) >> 7) + (((big - 128) * 6) >> 7);
    int dx = 2 * x - (WX - 1), dz = 2 * z - (WX - 1);
    long r2 = (long)dx * dx + (long)dz * dz;
    if (r2 > 11600)
        h -= (int)((r2 - 11600) >> 8);
    if (h < 5) h = 5;
    if (h > WY - 8) h = WY - 8;
    return (unsigned char)h;
}

/* progress (per mille) at the end of each part of world_generate (measured in MAME: the height map
   41 %, the columns 27 %, the caves 26 %, the rest 6 % of the time; tests/world.lua prints it) */
#define P_HM 415
#define P_COLS 688
#define P_CAVES 944
#define P_ORES 958
#define P_RAVINES 970
#define P_DEPTH 985

static const unsigned char ores[4][4] = {{COAL, 120, 40, 7}, {IRON, 64, 30, 5}, {GOLD, 20, 16, 4}, {GEM, 10, 10, 3}};

void world_generate(unsigned short seed16)
{
    uint32_t seed = seed16;
    static unsigned char hm[WX * WX];
    int x, z, y, i;
    PROG(0);
    memset(bw_blocks, 0, (long)WX * WX * WY);
    noise_setup(seed);
    for (x = 0; x < WX; x++) {
        if (!(x & 7)) PROG((long)P_HM * x / WX);
        for (z = 0; z < WX; z++)
            hm[x * WX + z] = height_at(x, z, seed);
    }
    PROG(P_HM);
    for (x = 0; x < WX; x++) {
        if (!(x & 15)) PROG(P_HM + (long)(P_COLS - P_HM) * x / WX);
        for (z = 0; z < WX; z++) {
            int h = hm[x * WX + z], beach = h <= SEA + 1;
            unsigned char *b = &AT(x, z, 0);
            b[0] = BEDROCK;
            if (hash3(x, z, seed + 505) & 1)
                b[1] = BEDROCK;
            for (y = 1; y <= h; y++) {
                if (b[y]) continue;
                if (y == h) b[y] = beach ? SAND : TURF;
                else if (y >= h - 3) b[y] = beach ? SAND : SOIL;
                else b[y] = ROCK;
            }
            for (y = h + 1; y <= SEA; y++)
                b[y] = WATER;
        }
    }
    PROG(P_COLS);
    rng_s = hash3(seed, 77, 99);
    if (!rng_s) rng_s = 0x12345678UL;
    /* caves */
    for (i = 0; i < 28; i++) {
        PROG(P_COLS + (long)(P_CAVES - P_COLS) * i / 28);
        long cx8 = (long)(8 + below(WX - 16)) << 8, cz8 = (long)(8 + below(WX - 16)) << 8;
        long cy8 = (long)(3 + below(26)) << 8;
        int dirn = below(256), dy = 0, steps = 60 + below(60), s;
        for (s = 0; s < steps; s++) {
            int r, cx, cz, cy, xx, zz, yy;
            dirn = (dirn + below(41) - 20) & 255;
            dy = dy + below(17) - 8;
            if (dy > 24) dy = 24;
            if (dy < -24) dy = -24;
            cx8 += bw_cos256(dirn) >> 2;
            cz8 += bw_sin256[dirn] >> 2;
            cy8 += dy;
            if (cy8 < (3 << 8)) { cy8 = 3 << 8; dy = 0; }
            if (cy8 > ((long)(WY - 8) << 8)) { cy8 = (long)(WY - 8) << 8; dy = 0; }
            r = 1 + (below(4) == 0);
            cx = (int)(cx8 >> 8); cz = (int)(cz8 >> 8); cy = (int)(cy8 >> 8);
            for (xx = cx - r; xx <= cx + r; xx++)
                for (zz = cz - r; zz <= cz + r; zz++)
                    for (yy = cy - r; yy <= cy + r; yy++) {
                        int t;
                        if (!(xx >= 1 && xx < WX - 1 && zz >= 1 && zz < WX - 1 && yy >= 2 && yy < WY - 1))
                            continue;
                        if ((xx - cx) * (xx - cx) + (zz - cz) * (zz - cz) + (yy - cy) * (yy - cy) > r * r + 1)
                            continue;
                        t = AT(xx, zz, yy);
                        if (t == AIR || t == WATER || t == BEDROCK)
                            continue;
                        if (AT(xx, zz, yy + 1) == WATER || AT(xx + 1, zz, yy) == WATER || AT(xx - 1, zz, yy) == WATER ||
                            AT(xx, zz + 1, yy) == WATER || AT(xx, zz - 1, yy) == WATER)
                            continue;
                        AT(xx, zz, yy) = AIR;
                    }
        }
    }
    PROG(P_CAVES);
    /* ore veins */
    for (i = 0; i < 4; i++) {
        int ore = ores[i][0], count = ores[i][1], ymax = ores[i][2], size = ores[i][3], v, k, t;
        for (v = 0; v < count; v++) {
            x = z = y = 0;
            for (t = 0; t < 8; t++) {
                x = 1 + below(WX - 2);
                z = 1 + below(WX - 2);
                y = 2 + below(ymax - 2);
                if (AT(x, z, y) == ROCK) break;
            }
            for (k = 0; k < size; k++) {
                int m;
                if (AT(x, z, y) == ROCK) AT(x, z, y) = (unsigned char)ore;
                m = below(6);
                if (m == 0 && x < WX - 2) x++;
                else if (m == 1 && x > 1) x--;
                else if (m == 2 && z < WX - 2) z++;
                else if (m == 3 && z > 1) z--;
                else if (m == 4 && y < ymax) y++;
                else if (y > 2) y--;
            }
        }
    }
    PROG(P_ORES);
    /* world depth (its own random stream): ravines, lava in the deepest layers, gravel, clay */
    rng_s = hash3(seed, 88, 11);
    if (!rng_s) rng_s = 0x12345678UL;
    for (i = 0; i < 4; i++) {                       /* ravines: deep narrow cuts, half a block a step */
        long x8 = (long)(16 + below(WX - 32)) << 8, z8 = (long)(16 + below(WX - 32)) << 8;
        int dirn = below(256), length = 30 + below(30), floor = 3 + below(5), step;
        for (step = 0; step < length; step++) {
            int cx, cz, r, xx, zz, yy;
            dirn = (dirn + below(9) - 4) & 255;
            x8 += bw_cos256(dirn) >> 1;
            z8 += bw_sin256[dirn] >> 1;
            cx = (int)(x8 >> 8); cz = (int)(z8 >> 8);
            r = (step < 5 || step >= length - 5) ? 0 : 1;
            for (xx = cx - r; xx <= cx + r; xx++)
                for (zz = cz - r; zz <= cz + r; zz++) {
                    int h;
                    if ((xx > cx ? xx - cx : cx - xx) + (zz > cz ? zz - cz : cz - zz) > r) continue;
                    if (!(xx >= 1 && xx < WX - 1 && zz >= 1 && zz < WX - 1)) continue;
                    h = hm[xx * WX + zz];
                    if (h <= SEA + 1) continue;
                    for (yy = floor + (step & 1); yy <= h; yy++) {
                        int t = AT(xx, zz, yy);
                        if (t == AIR || t == WATER || t == BEDROCK) continue;
                        if (AT(xx, zz, yy + 1) == WATER || AT(xx + 1, zz, yy) == WATER || AT(xx - 1, zz, yy) == WATER ||
                            AT(xx, zz + 1, yy) == WATER || AT(xx, zz - 1, yy) == WATER)
                            continue;
                        AT(xx, zz, yy) = AIR;
                    }
                }
        }
    }
    PROG(P_RAVINES);
    for (x = 1; x < WX - 1; x++)                    /* lava: air at y 2..4 (deep cave and ravine floors) */
        for (z = 1; z < WX - 1; z++)
            for (y = 2; y < 5; y++)
                if (AT(x, z, y) == AIR) AT(x, z, y) = LAVA;
    for (i = 0; i < 40; i++) {                      /* gravel pockets in rock */
        int k;
        x = 1 + below(WX - 2); z = 1 + below(WX - 2); y = 3 + below(36);
        for (k = 0; k < 10; k++) {
            int m;
            if (AT(x, z, y) == ROCK) AT(x, z, y) = GRAVEL;
            m = below(6);
            if (m == 0 && x < WX - 2) x++;
            else if (m == 1 && x > 1) x--;
            else if (m == 2 && z < WX - 2) z++;
            else if (m == 3 && z > 1) z--;
            else if (m == 4 && y < 40) y++;
            else if (y > 3) y--;
        }
    }
    for (i = 0; i < 16; i++) {                      /* clay patches on the sea floor */
        int cx = 2 + below(WX - 4), cz = 2 + below(WX - 4), xx, zz;
        for (xx = cx - 2; xx <= cx + 2; xx++)
            for (zz = cz - 2; zz <= cz + 2; zz++) {
                int h = hm[xx * WX + zz];
                if ((xx - cx) * (xx - cx) + (zz - cz) * (zz - cz) <= 5 && h < SEA && AT(xx, zz, h) == SAND)
                    AT(xx, zz, h) = CLAY;
            }
    }
    PROG(P_DEPTH);
    /* trees */
    {
        static unsigned char tx[1024], tz[1024];
        int nt = 0, j;
        for (x = 3; x < WX - 3; x++) {
            if (!(x & 7)) PROG(P_DEPTH + (long)(1000 - P_DEPTH) * x / WX);
            for (z = 3; z < WX - 3; z++) {
                int h = hm[x * WX + z], th, top, yy, xx, zz, ok = 1;
                if (AT(x, z, h) != TURF || h > WY - 10) continue;
                if ((hash3(x, z, seed + 606) & 127) >= 3) continue;
                for (j = 0; j < nt; j++)
                    if (tx[j] - x < 6 && x - tx[j] < 6 && tz[j] - z < 6 && z - tz[j] < 6) { ok = 0; break; }
                if (!ok || nt >= 1024) continue;
                tx[nt] = (unsigned char)x; tz[nt] = (unsigned char)z; nt++;
                th = 4 + (int)(hash3(x, z, seed + 707) & 1);
                top = h + th;
                for (yy = top - 2; yy < top + 2; yy++) {
                    int rad = yy < top ? 2 : 1;
                    for (xx = x - rad; xx <= x + rad; xx++)
                        for (zz = z - rad; zz <= z + rad; zz++) {
                            int corner = (xx - x == rad || x - xx == rad) && (zz - z == rad || z - zz == rad);
                            if (corner && (rad == 1 || (hash3(xx, zz, seed + yy) & 1))) continue;
                            if (AT(xx, zz, yy) == AIR) AT(xx, zz, yy) = LEAVES;
                        }
                }
                for (yy = h + 1; yy <= top; yy++) AT(x, z, yy) = LOG;
                AT(x, z, h) = SOIL;
            }
        }
    }
    PROG(1000);
}

/* ---- segment lists ------------------------------------------------------------------------- */

/* the segments of a column (its WY blocks at b) into s, top first, ending with top = 0 (and
   class 2, for the bottom face above it); returns how many */
short make_segs_c(const unsigned char *b, Seg *s)
{
    Seg *s0 = s, *e, *t;
    const unsigned char *p = b + WY - 1;
    while (p >= b) {
        unsigned char ty = *p;
        if (!ty) { p--; continue; }
        s->top = (unsigned char)(p - b + 1);
        do p--; while (p >= b && *p == ty);
        s->bot = (unsigned char)(p - b + 1);
        s->type = ty;
        if (s > s0) {                                  /* light class: under a tree, or covered */
            unsigned char u = s[-1].type & SEG_TYPE;
            s->type |= (u == LEAVES || u == LOG) ? SEG_TREE : SEG_COVERED;
        }
        s->flags = (unsigned char)(((s->top == WY || b[s->top] == AIR) ? SEG_TOPEXP : 0) |
                                   ((s->bot > 0 && *p == AIR) ? SEG_BOTEXP : 0));
        s++;
    }
    s->top = 0; s->bot = 0; s->type = SEG_COVERED; s->flags = 0;
    e = s;                     /* skip counts: segments from each one to the next run top (or the end) */
    for (t = s - 1; t >= s0; t--) {
        if (t[1].top == 0 || (t[1].flags & SEG_TOPEXP)) e = t + 1;
        t->flags |= (unsigned char)((e - t) << 2);
    }
    return (short)(s - s0);
}

static Seg tmp_segs[WY + 2];

#ifdef __m68k__
short make_segs_asm(const unsigned char *b, Seg *s);   /* segs.s: the same, about 2x faster */
#define make_segs make_segs_asm
#else
#define make_segs make_segs_c
#endif

/* torch light: segments of the column overlapping its lit blocks [lo, hi) get class 3, unless
   their top face shows above the light (a tall segment from the surface down to a lit cave) */
static void light_col(int x, int z, Seg *s)
{
    const unsigned char *l = bw_lit[x * WX + z];
    if (!l[1]) return;
    for (; s->top; s++)
        if (s->bot < l[1] && s->top > l[0] && (s->top <= l[1] || !(s->flags & SEG_TOPEXP)))
            s->type |= SEG_LIT;
}

/* highest surface of the 4 x 4 block region holding column (x, z) */
static void coarse_update(int x, int z)
{
    int x0 = x & ~3, z0 = z & ~3, i, j, m = 0;
    for (i = x0; i < x0 + 4; i++)
        for (j = z0; j < z0 + 4; j++) {
            const unsigned char *b = &AT(i, j, 0);
            int y = WY - 1;
            while (y > 0 && b[y] == AIR) y--;
            if (y + 1 > m) m = y + 1;
        }
    bw_coarse[(x >> 2) * CWX + (z >> 2)] = (unsigned char)m;
}

void world_build_segments(void)
{
    long i;
    short x, z;
    col_ids_n = 0;                          /* (every list its own from here) */
    for (i = 0; i < (long)PWX * PWX; i++)
        bw_cols[i] = ocean;
    memset(bw_coarse, 0, sizeof bw_coarse);
    heap_used = 0;
    for (x = 0; x < WX; x++) {
        if (!(x & 7)) PROG((long)x * 1000 / WX);
        for (z = 0; z < WX; z++) {
            Seg *s = seg_heap + heap_used;          /* (room for a full column: see world_init) */
            short n = make_segs(&AT(x, z, 0), s), cap = (short)(n + 3);         /* room to grow by 2 runs */
            unsigned char *c = &bw_coarse[(x >> 2) * CWX + (z >> 2)];
            unsigned char top = n ? s[0].top : 1;
            if (heap_used + cap > heap_size) {      /* out of room (never seen): */
                cap = (short)(n + 1);
                if (heap_used + cap > heap_size) { col_cap[x * WX + z] = 0; continue; }  /* the column shows as sea */
            }
            light_col(x, z, s);
            heap_used += cap;
            col_cap[x * WX + z] = (unsigned char)cap;
            bw_cols[(long)(x + BORDER) * PWX + (z + BORDER)] = s;
            if (top > *c) *c = top;
        }
    }
    PROG(1000);
}

void world_column_changed(int x, int z)
{
    short n = make_segs(&AT(x, z, 0), tmp_segs);
    long ci = (long)(x + BORDER) * PWX + (z + BORDER);
    if (n + 1 > col_cap[x * WX + z]) {
        short cap = (short)(n + 3);
        if (heap_used + cap > heap_size) {   /* out of room: rebuild everything compactly */
            world_build_segments();
            return;
        }
        bw_cols[ci] = seg_heap + heap_used;
        heap_used += cap;
        col_cap[x * WX + z] = (unsigned char)cap;
    }
    light_col(x, z, tmp_segs);
    memcpy(bw_cols[ci], tmp_segs, (n + 1) * sizeof(Seg));
    coarse_update(x, z);
}

/* ---- the world in a save file: a dictionary of distinct columns (see world.h) ------------- */

#define DCOARSE 0
#define DN (CWX * CWX)
#define DINDEX (DN + 2)
#define DREC (DINDEX + 2 * WX * WX)
#define LAST 0x80                    /* a run's type byte: it reaches y 0 (no length byte) */
#define DHASH 4096

/* world_pack's work (WORLD_WORK bytes): the hash heads, each entry's next and its list (its first
   column's), each column's id */
typedef struct Dict {
    short head[DHASH];
    short next[WX * WX];
    const Seg *list[WX * WX];
    short id[WX * WX];
    short map[WX * WX];              /* the last loaded world's ids -> these (-1: not seen yet) */
    short n;
} Dict;

/* a column is known by its segment list: top, bottom and type of each segment (equal lists, equal
   blocks); the light class and the flags left out */
#ifdef __m68k__
#define SEGKEY(s) (*(const uint32_t *)(s) & 0xFFFF1F00UL)
#else
#define SEGKEY(s) (((uint32_t)(s)->top << 24) | ((uint32_t)(s)->bot << 16) | ((uint32_t)((s)->type & SEG_TYPE) << 8))
#endif

static unsigned short list_hash(const Seg *s)
{
    uint32_t h = 0;
    for (;; s++) {
        h = ((h << 1) | (h >> 31)) + SEGKEY(s);
        if (!s->top) break;
    }
    return (unsigned short)((h ^ (h >> 16)) & (DHASH - 1));
}

static short list_eq(const Seg *a, const Seg *b)
{
    for (;; a++, b++) {
        if (SEGKEY(a) != SEGKEY(b)) return 0;
        if (!a->top) return 1;
    }
}

/* one column's record from its list: its top, then the runs down to y 0 (the last one: type | LAST) */
static short list_record(const Seg *s, unsigned char *o)
{
    unsigned char *o0 = o;
    short y = s->top;
    *o++ = s->top;
    for (; s->top; s++) {
        unsigned char t = s->type & SEG_TYPE;
        if (s->top < y) { *o++ = AIR; *o++ = (unsigned char)(y - s->top); }   /* air above it */
        if (!s->bot) { *o++ = (unsigned char)(t | LAST); return (short)(o - o0); }
        *o++ = t; *o++ = (unsigned char)(s->top - s->bot);
        y = s->bot;
    }
    if (y > 0) *o++ = AIR | LAST;                      /* air down to y 0 */
    return (short)(o - o0);
}

long world_pack(unsigned char *out, void *work)
{
    Dict *d = (Dict *)work;
    long c;
    short x, z;
    if (!out) {                                        /* the dictionary, and the most it needs */
        Seg **cp = bw_cols + (long)BORDER * PWX + BORDER;
        short i;
        for (i = 0; i < DHASH; i++) d->head[i] = -1;
        for (i = 0; i < col_ids_n; i++) d->map[i] = -1;
        d->n = 0;
        for (x = 0, c = 0; x < WX; x++, cp += 2 * BORDER)
            for (z = 0; z < WX; z++, c++) {
                const Seg *l = *cp++;
                unsigned short h;
                short e, o = -1;
                if (l == ocean) return 0;              /* (a column without a list: never seen) */
                if (!col_cap[c] && col_ids_n) {        /* still the loaded world's shared list */
                    o = (short)col_id[c];
                    if (d->map[o] >= 0) { d->id[c] = d->map[o]; continue; }
                }
                h = list_hash(l);
                for (e = d->head[h]; e >= 0 && !list_eq(d->list[e], l); e = d->next[e]) ;
                if (e < 0) {
                    e = d->n++;
                    d->list[e] = l;
                    d->next[e] = d->head[h];
                    d->head[h] = e;
                }
                d->id[c] = e;
                if (o >= 0) d->map[o] = e;
            }
        return DREC + (long)d->n * (2 * WY + 2);
    }
    memcpy(out + DCOARSE, bw_coarse, DN);
    out[DN] = (unsigned char)(d->n >> 8);
    out[DN + 1] = (unsigned char)d->n;
    {
        unsigned char *o = out + DINDEX;
        const short *id = d->id;
        for (c = 0; c < (long)WX * WX; c++, id++) {
            *o++ = (unsigned char)(*id >> 8);
            *o++ = (unsigned char)*id;
        }
    }
    {
        unsigned char *o = out + DREC;
        short e;
        for (e = 0; e < d->n; e++) o += list_record(d->list[e], o);
        return o - out;
    }
}

/* the segments of one run (type t, from top down to bot) onto the column's list being built
   (records hold maximal runs: no two of one type touch) */
#define PUSH(t, tp, bt) do {                                                                    \
        if (!(t)) {                                                                              \
            if (s > s0) s[-1].flags |= SEG_BOTEXP;                                               \
            gap = 1;                                                                             \
        } else {                                                                                 \
            s->top = (unsigned char)(tp); s->bot = (unsigned char)(bt);                          \
            if (s == s0) { s->type = (unsigned char)(t); s->flags = SEG_TOPEXP; }                \
            else {                                                                               \
                unsigned char u_ = s[-1].type & SEG_TYPE;                                        \
                s->type = (unsigned char)((t) | ((u_ == LEAVES || u_ == LOG) ? SEG_TREE : SEG_COVERED)); \
                s->flags = gap ? SEG_TOPEXP : 0;                                                 \
            }                                                                                    \
            s++; gap = 0;                                                                        \
        }                                                                                        \
    } while (0)

/* every column with torch light gets a list of its own (a copy, room to grow), lit */
static void unpack_light(void)
{
    const uint32_t *l = (const uint32_t *)bw_lit;
    long i;
    short x, z;
    for (i = 0; i < (long)WX * WX / 2 && !l[i]; i++) ;       /* (no torch light: done) */
    if (i == (long)WX * WX / 2) return;
    for (x = 0; x < WX; x++)
        for (z = 0; z < WX; z++)
            if (bw_lit[x * WX + z][1]) {
                Seg **cp = &bw_cols[(long)(x + BORDER) * PWX + (z + BORDER)], *s = *cp;
                short n = 0, cap;
                if (s == ocean) continue;
                while (s[n].top) n++;
                cap = (short)(n + 3);
                if (heap_used + cap > heap_size) continue;
                memcpy(seg_heap + heap_used, s, (n + 1) * sizeof(Seg));
                *cp = seg_heap + heap_used;
                heap_used += cap;
                col_cap[x * WX + z] = (unsigned char)cap;
                light_col(x, z, *cp);
            }
}

/* the common part of both versions: the number of entries (or -1), the per-id table at the
   heap's end (each id: its first column's blocks, its list), the border, coarse map, caps */
static short unpack_begin(const unsigned char *in, long len, Seg ***tab)
{
    short n;
    if (len < DREC) return -1;
    n = (short)((in[DN] << 8) | in[DN + 1]);
    if (n < 1 || n > WX * WX) return -1;
    *tab = (Seg **)(seg_heap + heap_size) - 2L * n;       /* (free heap: the lists stay below it) */
    {                                                     /* the border: open sea */
        Seg **p = bw_cols;
        short x, k;
        for (x = 0; x < PWX; x++, p += PWX)
            if (x >= BORDER && x < BORDER + WX)
                for (k = 0; k < BORDER; k++) { p[k] = ocean; p[BORDER + WX + k] = ocean; }
            else
                for (k = 0; k < PWX; k++) p[k] = ocean;
    }
    memcpy(bw_coarse, in + DCOARSE, DN);
    memset(col_cap, 0, (long)WX * WX);                   /* (shared lists: a change copies first) */
    heap_used = 0;
    col_ids_n = 0;
    return n;
}

short world_unpack_c(const unsigned char *in, long len)
{
    const unsigned char *r = in + DREC, *re = in + len;
    Seg **tab;
    long c;
    short ndict = unpack_begin(in, len, &tab), seen = 0;
    if (ndict < 0) return -1;
    for (c = 0; c < (long)WX * WX; c++) {
        short id = (short)((in[DINDEX + 2 * c] << 8) | in[DINDEX + 2 * c + 1]);
        unsigned char *b = bw_blocks + c * WY;
        Seg **cp = &bw_cols[(long)((c >> 7) + BORDER) * PWX + ((c & (WX - 1)) + BORDER)];
        if (id < 0 || id > seen || id >= ndict) return -1;
        if (id < seen) {                                     /* a column seen before: the same */
            memcpy(b, (unsigned char *)tab[2 * id], WY);
            *cp = tab[2 * id + 1];
        } else {                                             /* a new one: its record */
            Seg *s0 = seg_heap + heap_used, *s = s0, *t, *e;
            short top, y, gap = 0;
            if (r >= re) return -1;
            top = *r++;
            if (top > WY) return -1;
            memset(b + top, 0, WY - top);
            for (y = top; y > 0;) {
                unsigned char ty;
                short k;
                if (r >= re) return -1;
                ty = *r++;
                if (ty & LAST) { ty &= ~LAST; k = 0; }
                else {
                    if (r >= re) return -1;
                    k = (short)(y - *r++);
                    if (k <= 0 || k >= y) return -1;
                }
                if (ty >= NBLOCKS || (y == top && !ty)) return -1;
                memset(b + k, ty, y - k);
                PUSH(ty, y, k);
                y = k;
            }
            s->top = 0; s->bot = 0; s->type = SEG_COVERED; s->flags = 0;
            e = s;                                          /* skip counts (as make_segs_c) */
            for (t = s - 1; t >= s0; t--) {
                if (t[1].top == 0 || (t[1].flags & SEG_TOPEXP)) e = t + 1;
                t->flags |= (unsigned char)((e - t) << 2);
            }
            if ((Seg **)(s + 1) > tab) return -1;           /* (the heap is full: never seen) */
            heap_used += (s - s0) + 1;
            tab[2 * id] = (Seg *)b;
            tab[2 * id + 1] = s0;
            *cp = s0;
            seen++;
        }
    }
    if (seen != ndict || r != re) return -1;
    for (c = 0; c < (long)WX * WX; c++)
        col_id[c] = (unsigned short)((in[DINDEX + 2 * c] << 8) | in[DINDEX + 2 * c + 1]);
    col_ids_n = ndict;
    unpack_light();
    return 0;
}

#ifdef __m68k__
typedef struct UnpackCtx {                  /* unpack.s's U_ offsets */
    const unsigned short *index;            /* the columns' ids */
    const unsigned char *rec, *rec_end;     /* the records */
    unsigned char *blocks;
    Seg *heap, **tab, **cols;               /* the next free segment; the per-id table (the heap's end) */
    short ndict;
} UnpackCtx;
short unpack_asm(UnpackCtx *u);
short world_unpack_ref;                     /* tests: 1 = the C version */

short world_unpack(const unsigned char *in, long len)
{
    UnpackCtx u;
    if (world_unpack_ref) return world_unpack_c(in, len);
    if ((long)in & 1) return -1;
    if ((u.ndict = unpack_begin(in, len, &u.tab)) < 0) return -1;
    u.index = (const unsigned short *)(in + DINDEX);
    u.rec = in + DREC; u.rec_end = in + len;
    u.blocks = bw_blocks;
    u.heap = seg_heap;
    u.cols = bw_cols + (long)BORDER * PWX + BORDER;
    if (unpack_asm(&u) || u.rec != u.rec_end) return -1;
    heap_used = u.heap - seg_heap;
    memcpy(col_id, in + DINDEX, 2L * WX * WX);           /* (big-endian: as the 68000 reads it) */
    col_ids_n = u.ndict;
    unpack_light();
    return 0;
}
#else
short world_unpack(const unsigned char *in, long len) { return world_unpack_c(in, len); }
#endif
