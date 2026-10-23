/*
 * Blockworld renderer benchmark (Mac Plus / SE, 4 MB), M1 optimisation round.
 *
 * Generates the seed-2 world on the Mac, self-tests the 68000 code against the C reference, then
 * for each window (columns, rows, view distance): a moving check frame (the even columns, 8 px
 * wide) and the still frame refined from it, each dumped by the Lua (ack); then times over the
 * BENCH_NCAM cameras: moving frames (cast even columns + raster8 + present), the refinement (odd
 * columns + edges + raster + present), and the parts. Results: the 'BWBN' block, read by
 * tests/m1_bench.lua.
 */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Events.h>
#include <OSUtils.h>
#include <MacMemory.h>
#include <string.h>
#include "plus.h"
#include "world.h"
#include "render.h"
#include "bench_cams.h"

#define MAGIC 0x4257424EUL     /* 'BWBN' */
#define NCFG 6
#define REPS 3

/* 4-px columns, rows, view distance. Moving frames: the even columns, 8 px wide (raster8);
   still frames: all columns, 4 px wide (the odd ones cast after a moving frame). */
static const short cfgs[NCFG][3] = {
    {80, 240, 12}, {72, 216, 12}, {64, 192, 12}, {80, 240, 10}, {72, 216, 10}, {64, 192, 10}};

typedef struct Result {        /* ticks for (frames) operations each */
    unsigned long moving, castm, raster8, present, frames;          /*  0 */
    unsigned long refine, casto, edges, raster4;                     /* 20 */
    unsigned long still, cast;                                       /* 36: a full still frame; its cast */
    unsigned long pad[5];                                            /* 44 */
} Result;

struct Block {
    unsigned long magic, notMagic;   /*  0,  4 */
    volatile unsigned long state;    /*  8: 1 generating, 2 benching, 3 check frame ready, 4 done, 5 self-test, 0xDEAD no memory */
    unsigned long cfg;               /* 12: configuration being measured */
    unsigned long buf;               /* 16: buffer address */
    short nc, h, dmax, rowbytes;     /* 20..27 */
    volatile unsigned long ack;      /* 28: the Lua sets it after dumping the check frame */
    unsigned long genTicks;          /* 32: world generation + segments */
    unsigned long freeMem;           /* 36 */
    unsigned long nCfg, nCam, reps;  /* 40, 44, 48 */
    Result res[NCFG];                /* 52: 64 bytes each */
    unsigned long mainAddr;          /* 436: runtime address of main (for the PC profiler) */
    unsigned long tested, mismatch;  /* 440, 444: self-test frames, frames where the 68000 code differs from C */
    unsigned long which;             /* 448: the check frame: 0 moving, 1 still */
    unsigned long selftestTicks;     /* 452 */
    volatile unsigned long profile;  /* 456: set by the Lua: loop the 68000 caster forever (PC profiling) */
    unsigned long nbytes;            /* 460: buffer bytes (column pairs: see render.h) */
    unsigned long segTicks;          /* 464: world_build_segments alone */
};
static struct Block blk;

static void *mac_alloc(long n) { return NewPtrClear(n); }



static void fill_black(void)
{
    Rect r = qd.screenBits.bounds;
    long n = (long)(r.bottom - r.top) * qd.screenBits.rowBytes / 4, i;
    unsigned long *p = (unsigned long *)qd.screenBits.baseAddr;
    for (i = 0; i < n; i++) p[i] = 0xFFFFFFFFUL;
}

static RCam cam_of(short k)
{
    RCam c;
    c.x = bench_cams[k][0]; c.y = bench_cams[k][1]; c.z = bench_cams[k][2];
    c.yaw = (short)bench_cams[k][3]; c.pitch = (short)bench_cams[k][4];
    return c;
}

/* Self-test: the 68000 code against the C reference (casts, edges, both rasterisers). */
static RRun (*saved)[R_MAXN];
static unsigned char *bufA, *bufB;

static int same_runs(short c)
{
    const RRun *ra = saved[c], *rb = r_runs[c], *a = ra;
    const RRun *b = (const RRun *)((const char *)rb + r_start[c]);
    short n = 0;
    for (;;) {
        if (a->yend != b->yend || a->id != b->id || a->key != b->key || a->d != b->d) return 0;
        if (a->yend >= r_h || n > R_MAXN) return a->yend >= r_h;
        a = (const RRun *)((const char *)ra + a->next);
        b = (const RRun *)((const char *)rb + b->next);
        n++;
    }
}

static void selftest(void)
{
    short i, c, ci, mv;
    unsigned long t0 = pl_ticks();
    saved = (RRun (*)[R_MAXN])NewPtr(sizeof r_runs);
    bufA = (unsigned char *)NewPtr(48L * 288 + 16);
    bufB = (unsigned char *)NewPtr(48L * 288 + 16);
    if (!saved || !bufA || !bufB) return;
    blk.state = 5;
    for (ci = 0; ci < NCFG; ci += 2) {
        r_config(cfgs[ci][0], cfgs[ci][1], cfgs[ci][2]);
        for (i = 0; i < 16; i++) {
            RCam cam = cam_of(i & 7);
            long nb = (long)(r_nc / 2) * r_h, j;
            int bad = 0;
            cam.yaw = (short)((cam.yaw + i * 37) & 1023);
            cam.pitch = (short)(((i * 29) % 200) - 100);
            if (i & 8) cam.y += (long)((i * 53) % 1600);
            mv = i & 1;
            if (mv) {
                r_cast_cols(&cam, 0, 2);
                BlockMoveData(r_runs, saved, sizeof r_runs);
                r_raster8(bufA, r_nc / 2);
                r_cast_cols_asm(&cam, 0, 2);
                r_raster8_asm(bufB, r_nc / 2);
                for (c = 0; c < r_nc; c += 2) bad |= !same_runs(c);
            } else {
                r_cast(&cam);
                BlockMoveData(r_runs, saved, sizeof r_runs);
                r_edges(); r_raster(bufA, r_nc / 2);
                r_cast_asm(&cam);
                r_edges_asm(); r_raster_asm(bufB, r_nc / 2);
                for (c = 0; c < r_nc; c++) bad |= !same_runs(c);
            }
            for (j = 0; j < nb; j++)
                if (bufA[j] != bufB[j]) { bad = 1; break; }
            blk.mismatch += bad;
            blk.tested++;
        }
    }
    blk.selftestTicks = pl_ticks() - t0;
}

static void check_frame(short ci, short which)
{
    unsigned long t0;
    blk.which = which;
    blk.ack = 0;
    blk.state = 3;
    t0 = pl_ticks();
    while (!blk.ack && pl_ticks() - t0 < 1800) ;
    blk.state = 2;
    (void)ci;
}

int main(void)
{
    static unsigned char *buf;
    PlView view;
    short ci, k, rep;
    unsigned long t0;

    blk.magic = MAGIC;
    blk.notMagic = ~MAGIC;
    blk.nCfg = NCFG; blk.nCam = BENCH_NCAM; blk.reps = REPS;
    blk.mainAddr = (unsigned long)&main;
    MaxApplZone();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitCursor();
    HideCursor();
    FlushEvents(everyEvent, 0);
    fill_black();
    buf = (unsigned char *)NewPtrClear(48L * 288 + 16);
    if (!buf || world_init(mac_alloc)) { blk.state = 0xDEAD; for (;;) ; }
    blk.buf = (unsigned long)buf;
    blk.state = 1;
    t0 = pl_ticks();
    world_generate(BENCH_SEED);
    blk.segTicks = pl_ticks();
    world_build_segments();
    blk.segTicks = pl_ticks() - blk.segTicks;
    blk.genTicks = pl_ticks() - t0;
    blk.freeMem = FreeMem();
    selftest();
    if (blk.profile) {
        r_config(cfgs[0][0], cfgs[0][1], cfgs[0][2]);
        blk.state = 6;
        for (;;)
            for (ci = 0; ci < BENCH_NCAM; ci++) {
                RCam c = cam_of(ci);
                r_cast_cols_asm(&c, 0, 2);
            }
    }
    blk.state = 2;
    for (ci = 0; ci < NCFG; ci++) {
        short nc = cfgs[ci][0], h = cfgs[ci][1], rb = nc / 2;
        Result *res = &blk.res[ci];
        RCam c0 = cam_of(0);
        blk.cfg = ci; blk.nc = nc; blk.h = h; blk.dmax = cfgs[ci][2]; blk.rowbytes = rb;
        blk.nbytes = (unsigned long)rb * h;
        r_config(nc, h, cfgs[ci][2]);
        fill_black();
        pl_view_init(&view, nc * 4, h);
        /* check frames: moving, then the still one refined from it */
        r_cast_cols_asm(&c0, 0, 2); r_raster8_asm(buf, rb);
        r_present(view.base, view.rowBytes, buf);
        check_frame(ci, 0);
        r_cast_cols_asm(&c0, 1, 2); r_edges_asm(); r_raster_asm(buf, rb);
        r_present(view.base, view.rowBytes, buf);
        check_frame(ci, 1);
        /* moving frames: cast the even columns, raster8, present */
        t0 = pl_ticks();
        for (rep = 0; rep < REPS; rep++)
            for (k = 0; k < BENCH_NCAM; k++) {
                RCam c = cam_of(k);
                r_cast_cols_asm(&c, 0, 2); r_raster8_asm(buf, rb);
                r_present(view.base, view.rowBytes, buf);
            }
        res->moving = pl_ticks() - t0;
        res->frames = (unsigned long)REPS * BENCH_NCAM;
        /* the refinement once the view stops: the odd columns, edges, raster, present */
        t0 = pl_ticks();
        for (rep = 0; rep < REPS; rep++)
            for (k = 0; k < BENCH_NCAM; k++) {
                RCam c = cam_of(k);
                unsigned long t1;
                r_cast_cols_asm(&c, 0, 2);                    /* (the moving frame before: untimed) */
                t1 = pl_ticks();
                r_cast_cols_asm(&c, 1, 2); r_edges_asm(); r_raster_asm(buf, rb);
                r_present(view.base, view.rowBytes, buf);
                res->refine += pl_ticks() - t1;
            }
        /* parts */
        t0 = pl_ticks();
        for (rep = 0; rep < REPS; rep++)
            for (k = 0; k < BENCH_NCAM; k++) { RCam c = cam_of(k); r_cast_cols_asm(&c, 0, 2); }
        res->castm = pl_ticks() - t0;
        t0 = pl_ticks();
        for (rep = 0; rep < REPS; rep++)
            for (k = 0; k < BENCH_NCAM; k++) { RCam c = cam_of(k); r_cast_asm(&c); }
        res->cast = pl_ticks() - t0;
        for (k = 0; k < BENCH_NCAM; k++) {
            RCam c = cam_of(k);
            r_cast_asm(&c);
            t0 = pl_ticks();
            for (rep = 0; rep < REPS; rep++) r_raster8_asm(buf, rb);
            res->raster8 += pl_ticks() - t0;
            t0 = pl_ticks();
            for (rep = 0; rep < REPS; rep++) r_edges_asm();
            res->edges += pl_ticks() - t0;
            t0 = pl_ticks();
            for (rep = 0; rep < REPS; rep++) r_raster_asm(buf, rb);
            res->raster4 += pl_ticks() - t0;
            t0 = pl_ticks();
            for (rep = 0; rep < REPS; rep++) r_present(view.base, view.rowBytes, buf);
            res->present += pl_ticks() - t0;
        }
        res->still = res->cast + res->edges + res->raster4 + res->present;
    }
    blk.state = 4;
    for (;;) {
        EventRecord ev;
        SystemTask();
        GetNextEvent(everyEvent, &ev);
    }
    return 0;
}
