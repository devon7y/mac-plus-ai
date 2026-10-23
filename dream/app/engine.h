/* Dream engine: load a model file (dream/engine/FORMAT.md), compile its layers to 68000 code
 * (jit.c; the host build runs plain C kernels instead), and make pictures step by step.
 * Bit-exact with dream/engine/picture.py. */
#ifndef DREAM_ENGINE_H
#define DREAM_ENGINE_H

#include "jit.h"

#define DM_MAXT 32
#define DM_MAXL 96
#define DM_MAXTAB 8
#define DM_MAXSTEP 16
#define DM_MAXSUBJ 400

typedef long (*dm_read_fn)(void *ctx, void *buf, long n);    /* returns bytes read */

typedef struct {
    short nt, nl, ntab, nsub, D, N, img, patch, flags, two, in_shift;
    long ctot;
    short TH[DM_MAXT], TW[DM_MAXT], TC[DM_MAXT];
    short *tens[DM_MAXT];               /* padded (H+2) x (W+2) x C int16 */
    long tbytes[DM_MAXT];
    short tlo[DM_MAXTAB], thi[DM_MAXTAB];
    short *tabmid[DM_MAXTAB];           /* middle of a 32768-entry clamp table */
    LDesc L[DM_MAXL];
    long boff[DM_MAXL];                 /* first bias channel of each layer */
    signed char *shift;                 /* ctot */
    unsigned short *code[DM_MAXL];      /* Mac: generated code per layer */
    signed char *w[DM_MAXL];            /* host, and depthwise layers in the LUT build: unpacked weights */
    short *codes[DM_MAXL];              /* LUT build: W4 codes (cout x cin/4) of the 1x1 convs */
    short *lut, *gin, *sums;            /* LUT build: group tables, gathered inputs, row sums */
    unsigned short *codebase;
    long codebytes;
    short *B0, *Mc, *BT;                /* ctot, ctot, N x ctot */
    signed char *E, *U;                 /* nsub x D, D x ctot */
    short C1[DM_MAXSTEP], C2[DM_MAXSTEP], C3[DM_MAXSTEP];
    unsigned short tq[DM_MAXSTEP];
    unsigned char *names;               /* nsub Pascal strings, back to back */
    unsigned char *name[DM_MAXSUBJ];
    /* picture state */
    short *SB, *B;                      /* ctot each */
    short *x, *x0, *z;                  /* G x G x P*P (unpadded, storage order); z: fresh noise */
    unsigned long rng;                  /* noise stream state after the initial noise */
    long npix;
    short subject;
    unsigned long seed;
    short step;
    long ram;                           /* bytes allocated */
    long err;
} DreamModel;

/* Load and compile. progress(done, total) is called during compilation (may be NULL).
 * Returns 0 or an error code (1 read, 2 format, 3 memory). */
long dm_load(DreamModel *m, dm_read_fn rd, void *ctx, void (*progress)(long, long));
void dm_begin(DreamModel *m, short subject, unsigned long seed);   /* subject biases + noise */
void dm_step(DreamModel *m);              /* one network pass + sampler update; x0 = current estimate */
void dm_run_layers(DreamModel *m);        /* network pass on tens[0] with biases B (for tests) */

extern void (*dm_layer_hook)(long done, long total);   /* called after each layer (may be NULL) */

/* allocation hooks (Mac: NewPtr; host: calloc) */
void *dm_alloc(long n);
void dm_free(void *p);
#endif
