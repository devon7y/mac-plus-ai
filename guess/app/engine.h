/* Guess My Doodle: the classifier engine (model file -> 68000 code -> logits).
 * The arithmetic is specified by guess/engine/ref.py; the file format by guess/engine/model_file.py. */
#ifndef GUESS_ENGINE_H
#define GUESS_ENGINE_H

#define GK_T 1
#define GK_P 2
#define GK_D 3
#define GK_C 4
#define GK_G 5
#define GK_L 6
#define G_MAXT 24
#define G_MAXL 24
#define G_MAXBLK 16
#define G_BLOCK_WORDS 30000L    /* a fully connected layer's code is split into blocks of about this size */
#define G_STEP_CYCLES 200000L   /* g_step's work, roughly (about 26 ms in MAME, 38 ms on a real Plus) */

typedef struct {
    short H, W, C, pad;
    short *base;            /* storage, with a zero border when pad */
    short *org;             /* element (0, 0), channel 0 */
    long bytes;
} GTensor;

typedef struct {
    short kind, src, dst, k, stride, cin, cout, A;
    const unsigned char *data;
    long len;
    unsigned short *code;   /* generated code (D, C, L) */
    unsigned short *code2;  /* C fused with a pool: the requantisation pass */
    long codebytes, code1bytes;
    short fused;            /* T: the next layer (a P) is done together with it; P: skipped */
    short rows;             /* output rows (L: code blocks) per g_step */
    short nblk;             /* L: code blocks, each for outputs blko[b].. */
    unsigned short *blk[G_MAXBLK];
    short blko[G_MAXBLK];
} GLayer;

typedef struct {
    short G, ncls, ntens, nlay;
    GTensor t[G_MAXT];
    GLayer l[G_MAXL];
    const unsigned char *names;     /* ncls Pascal strings */
    unsigned short *codebuf;
    long codebytes, tensorbytes;
    short cl, crow;                 /* g_step: the next layer and row */
    unsigned char bits[128];        /* g_begin's drawing */
} GModel;

typedef void *(*GAlloc)(long bytes);       /* must return zeroed memory */
typedef void (*GFree)(void *p);

/* Parse the file (kept in memory by the caller), allocate the tensors and compile the layers.
 * Returns 0 on success, else an error code (1 bad file, 2 out of memory, 3 too many layers). */
int g_init(GModel *m, const unsigned char *file, long len, GAlloc alloc, GFree fr);
/* Run the network on a G x G bitmap (rows of G/8 bytes, MSB = left). */
void g_run(GModel *m, const unsigned char *bits);
/* The same in small steps (about G_STEP_CYCLES each): g_begin copies the bitmap, then call g_step
 * until it returns 1. */
void g_begin(GModel *m, const unsigned char *bits);
int g_step(GModel *m);
/* Run one layer (the benchmark times layers one by one); T with fused P does both. */
void g_run_layer(GModel *m, int i, const unsigned char *bits);
void g_run_rows(GModel *m, int i, const unsigned char *bits, short r0, short r1);
const short *g_logits(const GModel *m);
/* The i-th category name (Pascal string). */
const unsigned char *g_name(const GModel *m, int i);

/* krun.s */
void g_krun(const void *code, void *a0, void *a1, long nx, long ny, long ixs, long irs, long oxs, long ors);
void g_kpool(const void *code, const void *rq, void *a0, void *a1, long nx, long ny, long ixs, long irs,
             long oxs, long ors, long sx, long sy, long nfill);
void g_max4(short *o, const short *r0, const short *r1, const short *r2, const short *r3, long C);
void g_trow(short *o, const unsigned long *rows4, const short *const *trow, long C);

#endif
