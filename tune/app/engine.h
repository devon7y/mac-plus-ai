/* Tune's integer GRU (the numbers of tune/ref/tuneref.py, bit for bit). */
#ifndef ENGINE_H
#define ENGINE_H
#include <stdint.h>
#include "grammar.h"

#define TM_NFEAT 10        /* sym dur pos trip bar sec lasym ladur lbsym lbdur */
#define TM_NTAB 13         /* + typ met mode */
#define TM_MAXL 2

typedef struct TMLayer {
    short H, G, k_hh, Gi, k_ih;
    const short *Mb;        /* 4 words per unit: Mr Mz Mn bhn */
    const short *codes_hh;  /* 3H rows x G (rows interleaved r z n per unit) */
    const short *b_ih, *M_ih, *codes_ih;   /* layer > 0: input projection (3H, interleaved) */
} TMLayer;

typedef struct TuneModel {
    short L, V;
    short rows[TM_NTAB];
    const short *tab[TM_NTAB];    /* rows x 3H0, columns interleaved */
    TMLayer l[TM_MAXL];
    short Go, k_out;
    const short *B_out, *M_out, *codes_out;
} TuneModel;

typedef struct TuneGen {
    const TuneModel *m;
    short *cond, *x, *acc;
    short *h[TM_MAXL], *hq[TM_MAXL], *lut[TM_MAXL];
    short *allow;                 /* allowed token ids */
    short *lg;                    /* their logits, then weights */
    short *zero;                  /* 3H0 zero words */
    short nallow;
    unsigned char ok[TK_VOCAB];
    Gram g;
    uint32_t rng;
    short invT, cut, maxtok, maxbars;
} TuneGen;

/* blob: the bytes of model.bin (big-endian; on a little-endian host call tm_swap first). */
short tm_load(TuneModel *m, const void *blob, long len);
void tm_swap(void *blob, long len);
long tg_bytes(const TuneModel *m);           /* work memory for a generator, word aligned */
void tg_init(TuneGen *s, const TuneModel *m, void *work);
void tg_start(TuneGen *s, short type, short meter, short mode, uint32_t seed);
void tg_feed(TuneGen *s, short tok);
short tg_sample(TuneGen *s);

/* kernels: 68000 assembly on the Mac (kernels68k.s, gen_kernels.py), C on the host (kernels_c.c) */
void k_tab5(const short *x, long groups, short *lut);
void k_mv(const short *codes, const short *lutmid, long nrows, short *out, long G);
void k_gru(const short *x, const short *acc, const short *Mb, long k, short *h, long H,
           const short *sigmid, const short *tanhmid);
void k_scale(const short *acc, const short *M, long k, const short *b, short *out, long n);
void k_sum5(short *out, long n, const short *p0, const short *p1, const short *p2,
            const short *p3, const short *p4, long accumulate);

extern const short tk_sig[8192], tk_tanh[8192];
extern const unsigned short tk_exp[256];

#endif
