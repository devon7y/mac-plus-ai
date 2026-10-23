/* Blockworld column renderer (C reference implementation of engine/render.py, bit-exact).
 *
 * Phases per frame:
 *   r_cast()   one ray per column: front-to-back pieces into per-column run lists
 *              (each run: end row, pattern id, surface key, depth). r_cast_asm() is the 68000
 *              version (cast.s), with the same output.
 *   r_edges()  per column: background for uncovered runs, and the surface keys compared with
 *              the column to the left, giving the compact list (end row, id | R_EDGE)
 *   r_raster() two 4-px columns per byte: merge the two lists, fill the 1-bit buffer
 * Runs keep their depth (8.8 blocks) for depth-correct sprites.
 */
#ifndef BW_RENDER_H
#define BW_RENDER_H

#define R_MAXNC 128
#define R_MAXH 288
#define R_MAXN 120          /* runs per column */
#define R_MAXG 40           /* uncovered gaps per column */
#define R_MAXC 120          /* compact entries per column */
#define R_MAXREC 64         /* cells per ray */
#define R_MAXSB 48          /* segments in one run (at most one per block) */
#define R_MAXP 300          /* pieces of one cell (non-overlapping, each at least one row) */
#define R_EDGE 1024         /* id flag: leftmost pixel of the column is an edge */
#define R_DFAR 0x7FFF       /* depth of the background */
#define R_GAPID (-1)        /* id of an uncovered run */

typedef struct RCam {
    long x, y, z;           /* 8.8 blocks, y = eye height */
    short yaw;              /* 0..1023 */
    short pitch;            /* horizon shift in rows, + = look up */
} RCam;

typedef struct RRun {       /* one run of rows in a column's list (12 bytes) */
    short yend;             /* last row + 1 */
    short id;               /* pattern id; R_GAPID = uncovered (background) */
    short key;              /* surface key (0 = background / far) */
    short d;                /* depth, 8.8 blocks */
    short next;             /* byte offset of the next run from the column's first run */
    short pad;
} RRun;

typedef struct RStats {
    long cells, segs, pieces, projs;
} RStats;

/* Shared with cast.s: field offsets are fixed (checked by static asserts in render.c). */
typedef struct RCtx {
    short H, H0, ng, nn;                 /*  0  rows, horizon, gaps, next free run (byte offset) */
    short gstart, gend, depth, dn;       /*  8  first gap row, last gap end, piece depth, near distance */
    short df, vf, ty1, fogd;             /* 16  far distance, var|fog, temporary, fog distance */
    short dmax, ncache, pch0, pch1;      /* 24  view distance (8.8), previous cell's cache */
    short pcy0, pcy1, nch, ch0;          /* 32 */
    short ch1, cy0, cy1, skey;           /* 40  this cell's cache, side key */
    short sface, pid, pkey, ty0;         /* 48  side id offset (0 in the eye's cell), paint id/key, temporary */
    short dirx, dirz, plx, plz;          /* 56  view direction and camera plane (Q14) */
    short fx, fz, kx0, kz0;              /* 64  eye position in its cell; x/z plane keys of the eye's cell */
    short ncols, col, var0, varc;        /* 72  columns, current column, variant of the eye's cell / this cell */
    long  pwx4;                          /* 80  bytes per x step in the column table */
    struct RRun *runs;                   /* 84  first column's runs */
    const short *t8, *t16, *b8, *b16;    /* 88  height tables: 8.8 and x16 saturated; turf band */
    const short *recip;                  /* 104 */
    const short *camx;                   /* 108 */
    const void *cp0;                     /* 112 column table entry of the eye's cell */
    const short *bnear;                  /* 116 band table at the near distance */
    long recend;                         /* 120 end of this ray's cell records */
    const void *seg0, *segk;             /* 124 this cell's first segment, first one not above the eye */
    short *lastg;                        /* 132 the last gap entry */
    short rkey, mode;                    /* 136 this cell's entry key; mode 1 = DDA only (timing) */
    long savesp, botlim;                 /* 140 fast loop: its stack level, bottom stack limit */
    short *pbuf;                         /* 148 the general path's piece list (R_MAXP x 6 words) */
    struct RRun *topsp, *botsp;          /* 152 fast mode: top stack (up from the column's start),
                                                bottom stack (down from its end) */
    short slow, nnlim;                   /* 160 general gap list in use; node limit there */
    short *rstart;                       /* 164 r_start */
    short *sbp;                          /* 168 end of the side buffer */
    const void *runstart, *runend;       /* 172 this run's segments */
    short topf, sy0, sy1, sy0p;          /* 180 top/bottom face pending; side rows */
    short syb, sline, cstep, cfirst;     /* 188 ...; columns cast: cfirst, cfirst + cstep, ... */
    long nproj, nrun;                    /* 196 counters (timing experiments) */
    short rdx, rdz;                      /* 204 this column's ray direction (Q14) */
    long camxl, camzl;                   /* 208 eye x, z (8.8) */
    const unsigned char *coarse;         /* 216 bw_coarse: highest surface per 4 x 4 blocks */
    short sbuf[R_MAXSB * 4];             /* 220 sides of a run: Seg*, y0, y1 */
    short rec[R_MAXREC * 5];             /* 220 + 8*R_MAXSB: cell records: key, Seg*, far distance, recip */
    short gaps0[3];                      /* front sentinel */
    short gaps[(R_MAXG + 1) * 3];        /* gaps (first row, end row, run offset) + end sentinel */
} RCtx;

extern short r_nc, r_h, r_dmax, r_h0;         /* columns, rows, view distance (blocks), horizon */
extern RRun r_runs[R_MAXNC][R_MAXN];          /* per column run lists */
extern short r_start[R_MAXNC];                /* byte offset of each column's first run */
extern short r_comp[R_MAXNC][R_MAXC * 2];     /* per column compact (yend, id) pairs */
extern RStats r_stats;
extern RCtx r_ctx;

void r_config(short nc, short h, short dmax_blocks);
void r_cast(const RCam *cam);
void r_cast_asm(const RCam *cam);
/* only columns first, first + step, ... (moving: the even ones, drawn 8 px wide; still: the odd
   ones are added later) */
void r_cast_cols(const RCam *cam, short first, short step);
void r_cast_cols_asm(const RCam *cam, short first, short step);
/* moving frames: the even columns, 8 px wide, edges against the previous even column */
void r_raster8(unsigned char *buf, short rowbytes);
void r_raster8_asm(unsigned char *buf, short rowbytes);
/* Full size's moving frames: columns 0, 4, 8, ... drawn 16 px wide, edges against column c - 4 */
void r_raster16(unsigned char *buf);
void r_raster16_asm(unsigned char *buf);
void r_edges(void);
void r_raster(unsigned char *buf, short rowbytes);
void r_frame(const RCam *cam, unsigned char *buf, short rowbytes);

/* cast.s */
void cast_columns(RCtx *ctx);

/* raster.s: the edge pass and the rasteriser (same output as r_edges / r_raster) */
typedef struct EdgeCtx {
    RRun *runs; short *start; short *comp;       /* 0 */
    short nc, h, hz, h0;                         /* 12 */
} EdgeCtx;
typedef struct RastCtx {
    short *comp; unsigned char *buf;             /* 0 */
    const unsigned char *pl, *pr;                /* 8 */
    const void *code;                            /* 16 */
    short nb, rb, h, k, save, savee, savep;      /* 20 */
} RastCtx;
typedef struct Rast8Ctx {
    RRun *runs; short *start; unsigned char *buf;  /* 0 */
    const void *code;                            /* 12 (unused) */
    short nc, h, hz, h0;                         /* 16 */
    short j, y, ps, ni;                          /* 24 column, row, pending start, saved idx */
    unsigned char *col;                          /* 32 this byte column */
    short sv, pad[5];                            /* 36 raster16: a saved register */
    unsigned char p16[2 * 1024][16];             /* 48 pattern bytes (twice) | edge pixel, by id | R_EDGE */
    unsigned char p2[2 * 1024][32];              /* 32816 the same, every other byte (for MOVEP); the odd
                                                    bytes: without the edge pixel (raster16's words) */
} Rast8Ctx;
void present_asm(const void *code, const unsigned char *src, unsigned char *dst, long h1, long skip);
/* The frame buffer holds column pairs: byte b of row y is at (b >> 1) * 2 * H + 2 * y + (b & 1)
   (rb = nc / 2 bytes per row). r_present copies it to the screen, r_rows to plain rows. */
void r_present(unsigned char *dst, short dstRowBytes, const unsigned char *buf);
/* the background's patterns (sky, haze, fog: ids 0-2) for the 68000 rasterisers (night) */
void r_set_sky(const unsigned char sky[8], const unsigned char haze[8], const unsigned char fog[8]);
/* one background pattern (the sky's, the distant hills' BW_FARHILL, + 1, BW_FARLINE) */
void r_set_pat(short id, const unsigned char pat[8]);
/* light: the dither (0 none, 2 dim, 3 dark) for each light class of segments (0 open sky, 1 under
   a tree, 2 covered, 3 torch-lit; the class is in bits 5-6 of a segment's type byte) */
extern unsigned char r_shade[4];
void r_set_shade(const unsigned char sh[4]);
void r_rows(const unsigned char *buf, unsigned char *rows);
void raster8_asm(Rast8Ctx *q);
void raster16_asm(Rast8Ctx *q);
void edges_asm(EdgeCtx *e);
void raster_asm(RastCtx *r);
void r_edges_asm(void);
void r_raster_asm(unsigned char *buf, short rowbytes);

#endif
