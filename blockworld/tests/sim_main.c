/* sim_main.c: the engine on a bare 68000 (tests/sim68k.py runs it in Musashi).
 *
 * No Toolbox: tests/sim68k.py loads the flat binary, writes sim_cam / the arguments below and
 * calls these functions one at a time (each returns to an A-line trap), counting cycles. */
#include <string.h>
#include "world.h"
#include "render.h"
#include "obj.h"
#include "mobs_gen.h"
#include "tables.h"

#define HEAP 0x400000L          /* the world's memory: a fixed place, whatever the code size */
static char *hp;
static void *sim_alloc(long n)
{
    char *p = hp;
    hp += (n + 3) & ~3L;
    memset(p, 0, n);
    return p;
}

RCam sim_cam;
long sim_args[4];
long sim_out[16];
static RRun savedC[R_MAXNC][R_MAXN];
static short compC[R_MAXNC][R_MAXC * 2];
static unsigned char bufC[64 * 288 + 16], bufA[64 * 288 + 16];

/* args: seed, restore (1: the caller writes the blocks instead). Returns bw_blocks. */
long sim_init(void)
{
    hp = (char *)HEAP;
    if (world_init(sim_alloc)) return -1;
    if (!sim_args[1]) world_generate((unsigned short)sim_args[0]);
    return (long)bw_blocks;
}
void sim_segments(void) { world_build_segments(); }
/* make_segs_asm against the C version over every column: returns the columns that differ */
short make_segs_asm(const unsigned char *b, Seg *s);
long sim_segcheck(void)
{
    static Seg a[WY + 2], c[WY + 2];
    long x, z, bad = 0;
    for (x = 0; x < WX; x++)
        for (z = 0; z < WX; z++) {
            const unsigned char *b = &bw_blocks[BIDX(x, z, 0)];
            short na = make_segs_asm(b, a), nc = make_segs_c(b, c);
            if (na != nc || memcmp(a, c, (nc + 1) * sizeof(Seg))) bad++;
        }
    return bad;
}
void sim_gen(void) { world_generate((unsigned short)sim_args[0]); world_build_segments(); }

/* the light: dithers per class (args 0-3); torch light in random columns (args: seed, 1 in n) */
void sim_shade(void)
{
    unsigned char sh[4];
    short i;
    for (i = 0; i < 4; i++) sh[i] = (unsigned char)sim_args[i];
    r_set_shade(sh);
}
void sim_lit(void)
{
    unsigned long r = (unsigned long)sim_args[0] * 2654435761UL + 1;
    long i;
    for (i = 0; i < (long)WX * WX; i++) {
        unsigned char lo, hi;
        r = r * 1103515245UL + 12345;
        if ((r >> 16) % (unsigned long)sim_args[1]) { bw_lit[i][0] = bw_lit[i][1] = 0; continue; }
        r = r * 1103515245UL + 12345;
        lo = (unsigned char)((r >> 16) % (WY - 1));
        r = r * 1103515245UL + 12345;
        hi = (unsigned char)(lo + 1 + (r >> 16) % 8);
        bw_lit[i][0] = lo; bw_lit[i][1] = hi > WY ? WY : hi;
    }
    world_build_segments();
}
void sim_config(void) { r_config((short)sim_args[0], (short)sim_args[1], (short)sim_args[2]); }
void sim_cast_c(void) { r_cast(&sim_cam); memcpy(savedC, r_runs, sizeof r_runs); }
void sim_cast_asm(void) { r_cast_asm(&sim_cam); }
void sim_castm_c(void) { r_cast_cols(&sim_cam, 0, 2); memcpy(savedC, r_runs, sizeof r_runs); }
void sim_castm_asm(void) { r_cast_cols_asm(&sim_cam, 0, 2); }
void sim_raster8_c(void) { r_raster8(bufC, r_nc / 2); }
void sim_raster8_asm(void) { r_raster8_asm(bufA, r_nc / 2); }
void sim_castw_c(void) { r_cast_cols(&sim_cam, 0, 4); memcpy(savedC, r_runs, sizeof r_runs); }
void sim_castw_asm(void) { r_cast_cols_asm(&sim_cam, 0, 4); }
void sim_raster16_c(void) { r_raster16(bufC); }
void sim_castcols_asm(void) { r_cast_cols_asm(&sim_cam, (short)sim_args[0], (short)sim_args[1]); }
void sim_raster16_asm(void) { r_raster16_asm(bufA); }
void sim_edges_c(void) { r_edges(); memcpy(compC, r_comp, sizeof r_comp); }
void sim_edges_asm(void) { r_edges_asm(); }
void sim_raster_c(void) { r_raster(bufC, r_nc / 2); }
void sim_raster_asm(void) { r_raster_asm(bufA, r_nc / 2); }

/* walk both run lists of column c; 0 when equal, else 1 + the index of the first difference */
static long cmp_col(short c)
{
    const RRun *ra = savedC[c], *rb = r_runs[c], *a = ra;
    const RRun *b = (const RRun *)((const char *)rb + r_start[c]);
    short n = 0;
    for (;;) {
        if (a->yend != b->yend || a->id != b->id || a->key != b->key || a->d != b->d) return n + 1;
        if (a->yend >= r_h) return 0;
        if (n > R_MAXN) return n + 1;
        a = (const RRun *)((const char *)ra + a->next);
        b = (const RRun *)((const char *)rb + b->next);
        n++;
    }
}

/* out[0] columns whose runs differ, out[1] first such column, out[2] its first differing run,
 * out[3] compact lists differ, out[4] raster buffers differ */
void sim_compare(void)
{
    short c;
    long nb = (long)(r_nc / 2) * r_h, j;
    short step = sim_args[1] ? (short)sim_args[1] : 1;
    sim_out[0] = 0; sim_out[1] = -1; sim_out[2] = -1;
    for (c = 0; c < r_nc; c += step) {
        long k = cmp_col(c);
        if (k) {
            if (!sim_out[0]) { sim_out[1] = c; sim_out[2] = k - 1; }
            sim_out[0]++;
        }
    }
    sim_out[3] = step == 1 && memcmp(compC, r_comp, sizeof r_comp) != 0;
    sim_out[4] = 0;
    for (j = 0; j < nb; j++)
        if (bufC[j] != bufA[j]) { sim_out[4] = 1; break; }
}

/* the C caster's column c runs and the asm's, as (yend, id, key, d) lists for debugging */
long sim_dump[2][R_MAXN][4];
void sim_dumpcol(void)
{
    short c = (short)sim_args[0], i, w;
    for (w = 0; w < 2; w++) {
        const RRun *r0 = w ? r_runs[c] : savedC[c];
        const RRun *p = (const RRun *)((const char *)r0 + (w ? r_start[c] : 0));
        for (i = 0; i < R_MAXN; i++) {
            sim_dump[w][i][0] = p->yend; sim_dump[w][i][1] = p->id;
            sim_dump[w][i][2] = p->key; sim_dump[w][i][3] = p->d;
            if (p->yend >= r_h || p->yend < 0 || i == R_MAXN - 1) { sim_dump[w][i + (i < R_MAXN - 1)][0] = -9999; break; }
            p = (const RRun *)((const char *)r0 + p->next);
        }
    }
}

/* the present: the asm frame onto a 64-byte-row screen at byte 4, against r_rows */
static unsigned char screen[80 * 288], rowsA[64 * 288];
long sim_present(void)
{
    short y, b, rb = r_nc / 2;
    memset(screen, 0xAA, sizeof screen);
    r_present(screen + 4, 80, bufA);
    r_rows(bufA, rowsA);
    for (y = 0; y < r_h; y++)
        for (b = 0; b < rb; b++)
            if (screen[y * 80 + 4 + b] != rowsA[y * rb + b]) return 1 + y * 100 + b;
    for (y = 0; y < r_h; y++)
        if (screen[y * 80 + 3] != 0xAA || screen[y * 80 + 4 + rb] != 0xAA) return -1;
    return 0;
}

void sim_present_only(void) { r_present(screen + 4, 80, bufA); }

/* the frame as the Mac shows it (asm), for pictures */
unsigned char *sim_frame(void) { return bufA; }

/* a mob's side picture straight ahead at distance args[1] (8.8), standing on the eye's level - 1.62
   (args[0]: kind), drawn into a moving frame (after sim_castm_asm) */
void sim_sprite(void)
{
    const MobKind *k = &mob_kind[sim_args[0]];
    long c = bw_sin[(sim_cam.yaw + 256) & 1023], s = bw_sin[sim_cam.yaw & 1023];
    Sprite sp;
    sp.x = sim_cam.x + ((c * sim_args[1]) >> 14);
    sp.z = sim_cam.z + ((s * sim_args[1]) >> 14);
    sp.y = sim_cam.y - 415;
    sp.sw = k->sw; sp.sh = k->ht; sp.pic = k->side[0]; sp.pw = k->pw; sp.ph = k->ph;
    sp.flip = 0; sp.solid = 0; sp.dim = 0;
    sprite_draw(&sim_cam, &sp, bufA, (short)(r_nc * 4), r_h, 1);
}

/* the fps test's five animals in a row ahead (yak 5, boar 4, ram 7, fowl 3, yak 9 blocks) */
void sim_sprite5(void)
{
    static const short k[5] = {0, 1, 2, 3, 0}, dist[5] = {9, 7, 5, 4, 3};
    short i;
    for (i = 0; i < 5; i++) {
        sim_args[0] = k[i == 4 ? 3 : i == 3 ? 1 : i == 2 ? 0 : i == 1 ? 2 : 0];
        sim_args[1] = (long)dist[i] << 8;
        sim_sprite();
    }
}

/* the sprite drawer with sprite.s against the C walk: the five animals (and dimmed, and flashing)
   over the same frame in moving (args[0] 1) or still frames; returns the bytes that differ */
extern short sprite_asm;
static unsigned char bufS[64 * 288 + 16];
static const unsigned char DIMP[8] = {0xAA, 0x44, 0xAA, 0x10, 0xAA, 0x44, 0xAA, 0x01};
static void five(unsigned char *buf, short moving, short variant)
{
    static const short k[5] = {0, 2, 0, 1, 3}, dist[5] = {9, 7, 5, 4, 3};
    short i;
    for (i = 0; i < 5; i++) {
        const MobKind *mk = &mob_kind[k[i]];
        long c = bw_sin[(sim_cam.yaw + 256) & 1023], s = bw_sin[sim_cam.yaw & 1023];
        Sprite sp;
        sp.x = sim_cam.x + ((c * ((long)dist[i] << 8)) >> 14) + (i - 2) * 100;
        sp.z = sim_cam.z + ((s * ((long)dist[i] << 8)) >> 14) - (i - 2) * 60;
        sp.y = sim_cam.y - 415;
        sp.sw = mk->sw; sp.sh = mk->ht; sp.pic = mk->side[i & 1]; sp.pw = mk->pw; sp.ph = mk->ph;
        sp.flip = i & 1; sp.solid = variant == 2 && i == 2; sp.dim = variant == 1 ? DIMP : 0;
        sprite_draw(&sim_cam, &sp, buf, (short)(r_nc * 4), r_h, moving);
    }
}
long sim_spritecheck(void)
{
    long n = (long)(r_nc / 2) * r_h, i, bad = 0;
    short moving = (short)sim_args[0], v;
    for (v = 0; v < 3; v++) {
        for (i = 0; i < n; i++) bufS[i] = bufC[i] = (unsigned char)(i * 37 + (i >> 5));
        sprite_asm = 0; five(bufC, moving, v);
        sprite_asm = 1; five(bufS, moving, v);
        for (i = 0; i < n; i++) if (bufS[i] != bufC[i]) bad++;
    }
    return bad;
}

/* ---- M7: the save file's run stream and LZ (sim68k.py save) ------------------------------- */
#include "lz.h"
#define PKCAP 600000L
static unsigned char *pk_raw, *pk_lz, *pk_blocks, *pk_snap, *pk_work;
static void *pk_ht;
long sim_pklen[4];                       /* stream length, LZ length, snapshot length */
static void pk_init(void)
{
    if (!pk_raw) {
        pk_raw = sim_alloc(PKCAP); pk_lz = sim_alloc(PKCAP); pk_blocks = sim_alloc((long)WX * WX * WY);
        pk_snap = sim_alloc(1200000L); pk_ht = sim_alloc(lz_work(PKCAP)); pk_work = sim_alloc(WORLD_WORK);
    }
}
/* every column's segment list (to the terminator) and the coarse map, serialized */
static long seg_serial(unsigned char *o)
{
    unsigned char *p = o;
    long i;
    for (i = 0; i < (long)PWX * PWX; i++) {
        const Seg *s = bw_cols[i];
        do { memcpy(p, s, 4); p += 4; } while ((s++)->top);
    }
    memcpy(p, bw_coarse, sizeof bw_coarse);
    p += sizeof bw_coarse;
    return p - o;
}
long sim_snap(void) { pk_init(); memcpy(pk_blocks, bw_blocks, (long)WX * WX * WY); return sim_pklen[2] = seg_serial(pk_snap); }
/* compare with the snapshot: 1 blocks differ, 2 segments differ */
long sim_snapcmp(void)
{
    static unsigned char *now;
    long n, r = 0;
    if (!now) now = sim_alloc(1200000L);
    n = seg_serial(now);
    if (memcmp(pk_blocks, bw_blocks, (long)WX * WX * WY)) r |= 1;
    if (n != sim_pklen[2] || memcmp(now, pk_snap, n)) r |= 2;
    return r;
}
long sim_packcount(void) { pk_init(); return world_pack(0, pk_work); }   /* (now: room) */
long sim_pack(void) { pk_init(); world_pack(0, pk_work); return sim_pklen[0] = world_pack(pk_raw, pk_work); }
long sim_lzpack(void) { return sim_pklen[1] = lz_pack_asm(pk_raw, sim_pklen[0], pk_lz, pk_ht); }
long sim_lzpack_c(void) { return sim_pklen[1] = lz_pack_c(pk_raw, sim_pklen[0], pk_lz, pk_ht); }
/* unpack the LZ into pk_raw (cleared first); args[0]: 1 = the 68000 version */
long sim_lzunpack(void)
{
    memset(pk_raw, 0xA5, sim_pklen[0]);
    return sim_args[0] ? lz_unpack_asm(pk_lz, sim_pklen[1], pk_raw, sim_pklen[0])
                       : lz_unpack_c(pk_lz, sim_pklen[1], pk_raw, sim_pklen[0]);
}
void sim_trash(void) { memset(bw_blocks, 0x5A, (long)WX * WX * WY); }
long sim_unpack(void) { return world_unpack(pk_raw, sim_pklen[0]); }
long sim_unpack_c(void) { return world_unpack_c(pk_raw, sim_pklen[0]); }
unsigned char *sim_pkraw(void) { return pk_raw; }
unsigned char *sim_pklz(void) { return pk_lz; }

/* ---- M7: sound synthesis (synth.s against synth.c): args kind, ms, f0, f1, amp; 0 if equal */
#include "synth.h"
unsigned char *sim_sa, *sim_sb;
long sim_synth(void)
{
    static unsigned char *a, *b;
    long len = 22257L * sim_args[1] / 1000, df, da, i;
    unsigned short la = 0xACE1, lb = 0xACE1;
    short kind = (short)(sim_args[0] & 15), f0 = (short)(sim_args[0] >> 16), f1 = (short)sim_args[2], amp = (short)sim_args[3];
    if (!a) { a = sim_alloc(40000); b = sim_alloc(40000); sim_sa = a; sim_sb = b; }
    df = ((long)(f1 - f0) << 16) / len; da = -(((long)amp * 2) << 16) / (3 * len);
    synth_part_c(a, len, (long)f0 << 16, df, (long)amp << 16, da, kind, &la);
    synth_part_asm(b, len, (long)f0 << 16, df, (long)amp << 16, da, kind, &lb);
    for (i = 0; i < len; i++) if (a[i] != b[i]) return i + 1;
    return la != lb ? -1 : 0;
}
long sim_synth_asm(void)
{
    static unsigned char *b;
    long len = 22257L * sim_args[1] / 1000, df, da;
    unsigned short lb = 0xACE1;
    short kind = (short)(sim_args[0] & 15), f0 = (short)(sim_args[0] >> 16), f1 = (short)sim_args[2], amp = (short)sim_args[3];
    if (!b) b = sim_alloc(40000);
    df = ((long)(f1 - f0) << 16) / len; da = -(((long)amp * 2) << 16) / (3 * len);
    synth_part_asm(b, len, (long)f0 << 16, df, (long)amp << 16, da, kind, &lb);
    return len;
}
