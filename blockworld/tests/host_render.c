/* Host (macOS clang) harness for the C renderer: compare against the Python reference.
 *   host_render SEED NC H DMAX CAMS.txt OUTDIR
 * Writes OUTDIR/world.bin (the generated blocks) and OUTDIR/frame_<i>.bin (H rows x NC/2 bytes)
 * for each camera line "x y z yaw pitch" (8.8 fixed point), and prints work statistics. */
#include <stdio.h>
#include <stdlib.h>
#include "world.h"
#include "render.h"

static void *xalloc(long n) { return calloc(1, (size_t)n); }
#ifndef CAST_MODEL
void cast_columns(RCtx *c) { (void)c; }   /* the 68000 caster is not available on the host */
#endif
void edges_asm(EdgeCtx *e) { (void)e; }
void raster_asm(RastCtx *r) { (void)r; }
void raster8_asm(Rast8Ctx *q) { (void)q; }
void raster16_asm(Rast8Ctx *q) { (void)q; }
void present_asm(const void *code, const unsigned char *src, unsigned char *dst, long h1, long skip)
{ (void)code; (void)src; (void)dst; (void)h1; (void)skip; }

int main(int argc, char **argv)
{
    int seed, nc, h, dmax, i = 0;
    long x, y, z;
    int yaw, pitch;
    char fn[512];
    FILE *f, *cams;
    unsigned char *buf, *rows;
    if (argc < 7) { fprintf(stderr, "usage: host_render SEED NC H DMAX CAMS OUTDIR\n"); return 2; }
    seed = atoi(argv[1]); nc = atoi(argv[2]); h = atoi(argv[3]); dmax = atoi(argv[4]);
    if (world_init(xalloc)) return 1;
    if (getenv("BW_WORLD")) {                       /* a world file instead of a generated one */
        FILE *wf = fopen(getenv("BW_WORLD"), "rb");
        if (!wf || fread(bw_blocks, 1, (size_t)WX * WX * WY, wf) != (size_t)WX * WX * WY) return 3;
        fclose(wf);
    } else
        world_generate((unsigned short)seed);
    if (getenv("BW_LIT")) {                         /* torch light per column (lo, hi) */
        FILE *lf = fopen(getenv("BW_LIT"), "rb");
        if (!lf || fread(bw_lit, 1, sizeof bw_lit, lf) != sizeof bw_lit) return 4;
        fclose(lf);
    }
    world_build_segments();
    snprintf(fn, sizeof fn, "%s/world.bin", argv[6]);
    f = fopen(fn, "wb"); fwrite(bw_blocks, 1, (size_t)WX * WX * WY, f); fclose(f);
    r_config((short)nc, (short)h, (short)dmax);
    if (getenv("BW_SHADE")) {                       /* the light classes' dithers: "a,b,c,d" */
        unsigned sh[4] = {0, 0, 0, 0};
        unsigned char s8[4];
        sscanf(getenv("BW_SHADE"), "%u,%u,%u,%u", &sh[0], &sh[1], &sh[2], &sh[3]);
        for (i = 0; i < 4; i++) s8[i] = (unsigned char)sh[i];
        r_set_shade(s8);
        i = 0;
    }
    buf = calloc(1, (size_t)h * nc / 2);
    rows = calloc(1, (size_t)h * nc / 2);
    cams = fopen(argv[5], "r");
    while (fscanf(cams, "%ld %ld %ld %d %d", &x, &y, &z, &yaw, &pitch) == 5) {
        RCam cam = {x, y, z, (short)yaw, (short)pitch};
        if (getenv("BW_WIDE")) { r_cast_cols(&cam, 0, 4); r_raster16(buf); }
        else if (getenv("BW_MOVING")) { r_cast_cols(&cam, 0, 2); r_raster8(buf, (short)(nc / 2)); }
        else if (getenv("BW_ASM")) { r_cast_asm(&cam); r_edges(); r_raster(buf, (short)(nc / 2)); }
        else r_frame(&cam, buf, (short)(nc / 2));
        snprintf(fn, sizeof fn, "%s/frame_%d.bin", argv[6], i);
        r_rows(buf, rows);
        f = fopen(fn, "wb"); fwrite(rows, 1, (size_t)h * nc / 2, f); fclose(f);
        printf("%d cells %ld segs %ld pieces %ld projs %ld\n", i, r_stats.cells, r_stats.segs, r_stats.pieces, r_stats.projs);
        i++;
    }
    return 0;
}
