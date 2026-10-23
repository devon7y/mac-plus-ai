/* Host test: the unified cell model (render.c, CELL_MODEL) against the reference caster, run list
 * by run list.   model_test SEED NC H DMAX CAMS.txt */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "world.h"
#include "render.h"

extern int getenv_model;
static void *xalloc(long n) { return calloc(1, (size_t)n); }
void cast_columns(RCtx *c) { (void)c; }
void edges_asm(EdgeCtx *e) { (void)e; }
void raster_asm(RastCtx *r) { (void)r; }
void raster8_asm(Rast8Ctx *q) { (void)q; }
void present_asm(const void *code, const unsigned char *src, unsigned char *dst, long h1, long skip)
{ (void)code; (void)src; (void)dst; (void)h1; (void)skip; }
static RRun saved[R_MAXNC][R_MAXN];

static int cmp_col(int c)
{
    const RRun *a = saved[c], *b = r_runs[c];
    int n = 0;
    for (;;) {
        if (a->yend != b->yend || a->id != b->id || a->key != b->key || a->d != b->d) return n + 1;
        if (a->yend >= r_h || n > R_MAXN) return 0;
        a = (const RRun *)((const char *)saved[c] + a->next);
        b = (const RRun *)((const char *)r_runs[c] + b->next);
        n++;
    }
}

int main(int argc, char **argv)
{
    int nc = atoi(argv[2]), h = atoi(argv[3]), dmax = atoi(argv[4]), i = 0, bad = 0, c;
    long x, y, z;
    int yaw, pitch;
    FILE *cams = fopen(argv[5], "r");
    if (world_init(xalloc)) return 1;
    if (getenv("BW_WORLD")) {
        FILE *wf = fopen(getenv("BW_WORLD"), "rb");
        if (!wf || fread(bw_blocks, 1, (size_t)WX * WX * WY, wf) != (size_t)WX * WX * WY) return 3;
        fclose(wf);
    } else
        world_generate((unsigned short)atoi(argv[1]));
    world_build_segments();
    r_config((short)nc, (short)h, (short)dmax);
    while (fscanf(cams, "%ld %ld %ld %d %d", &x, &y, &z, &yaw, &pitch) == 5) {
        RCam cam = {x, y, z, (short)yaw, (short)pitch};
        int nbad = 0, first = -1, k = 0;
        getenv_model = 0; r_cast(&cam); memcpy(saved, r_runs, sizeof r_runs);
        getenv_model = 1; r_cast(&cam);
        for (c = 0; c < nc; c++) {
            int d = cmp_col(c);
            if (d) { if (first < 0) { first = c; k = d - 1; } nbad++; }
        }
        if (nbad) {
            bad++;
            if (bad <= 5) {
                const RRun *a = saved[first], *b = r_runs[first];
                int j;
                printf("cam %d (%ld %ld %ld %d %d): %d columns differ, first %d run %d\n", i, x, y, z, yaw, pitch, nbad, first, k);
                for (j = 0; j < 40; j++) {
                    printf("   %2d ref (%d %d %d %d)  model (%d %d %d %d)%s\n", j, a->yend, a->id, a->key, a->d,
                           b->yend, b->id, b->key, b->d, j == k ? " <<" : "");
                    if (a->yend >= h && b->yend >= h) break;
                    if (a->yend < h) a = (const RRun *)((const char *)saved[first] + a->next);
                    if (b->yend < h) b = (const RRun *)((const char *)r_runs[first] + b->next);
                }
            }
        }
        i++;
    }
    printf("model: %d/%d frames with identical run lists (%dx%d, view %d)\n", i - bad, i, nc, h, dmax);
    return bad != 0;
}
