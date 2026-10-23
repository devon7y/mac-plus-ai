/* Pixel inference engine: the integer forward pass of llm/ref/SPEC.md, bit-exact with
 * llm/ref/model.py. Portable C (host tests) + 68000 table kernels (kernels68k.s) on the Mac. */
#ifndef PX_ENGINE_H
#define PX_ENGINE_H
#include <stdint.h>

enum { PX_D = 160, PX_L = 6, PX_H = 4, PX_KVH = 2, PX_HD = 40, PX_FF = 512, PX_NE = 4,
       PX_V = 2048, PX_W = 64, PX_QKVR = 320, PX_TOPK = 16 };
enum { PX_W4 = 0, PX_B5 = 1 };
enum { PX_PAD = 0, PX_USER = 1, PX_PIXEL = 2, PX_END = 3, PX_NBASE = 99 };

typedef struct { int16_t m, e; } SF;

typedef struct {
    SF gs_attn, gs_ffn, a_q, a_k, a_v, a_o, s_r, a_g[PX_NE], a_u[PX_NE], a_d[PX_NE];
    int16_t g_attn[PX_D], g_ffn[PX_D];
    int8_t router[PX_NE * PX_D];
    void *qkv, *wo, *wg[PX_NE], *wu[PX_NE], *wd[PX_NE], *wd2[PX_NE];  /* W4 (gate/up: W5) codes or B5 bytes */
    int16_t *kpc;               /* key codes d*32 + (c+8)*2 - 32768 [2][64][40] */
    uint8_t *vb;                /* value bytes 2*(code+8) [2][64][40] */
    SF sk[PX_KVH][PX_W], sv[PX_KVH][PX_W];
} PxLayer;

typedef void *(*px_alloc_fn)(int32_t bytes);
typedef void (*px_free_fn)(void *p);
typedef int32_t (*px_read_fn)(void *ctx, void *buf, int32_t n);   /* returns n, or < n on error */

typedef struct {
    int variant;
    int16_t eE, n_merges;
    int16_t alibi[PX_H][PX_W];
    SF gs_final;
    int16_t g_final[PX_D];
    int16_t emb_m[PX_V];
    int16_t *head;              /* pair codes [2048][80] */
    uint16_t *merges;           /* [n][2] */
    uint16_t *mkeys;            /* sorted (a << 12 | b) keys ... */
    uint16_t *mrank;            /* ... and their ranks */
    PxLayer L[PX_L];
    int16_t *lut, *ptabs;       /* ternary tables; pair tables (80 x 256 words) */
    int16_t *tmp;               /* 4096 words */
    int32_t *K;                 /* logits (scaled ints) [2048] */
    SF sigma;                   /* real logit = K * sigma */
    int32_t r[PX_D];
    int32_t pos;
    uint32_t rng;
    int experts[PX_L];
} PxModel;

int32_t px_bytes_needed(int variant);
/* Load a model file (format v2). Returns 0 or an error code. progress(0..100) may be NULL. */
int px_load(PxModel *m, int variant, px_read_fn rd, void *ctx, px_alloc_fn alloc, px_free_fn freefn,
            void (*progress)(int));
void px_reset(PxModel *m);
void px_seed(PxModel *m, uint32_t seed);
void px_step(PxModel *m, int tok, int want_logits);
int px_sample(PxModel *m, int temp256, int topk);
int px_encode(PxModel *m, const char *s, int n, int16_t *out, int max);
int px_tokstr(PxModel *m, int tok, char *buf, int max);   /* decoded text of one token */

/* table kernels (68000 assembly on the Mac, kernels_c.c on the host) */
void k_tab4(const int16_t *x, long groups, int16_t *lut);
void k_mvw(const int16_t *codes, const int16_t *lutmid, long nout, long n, int16_t *out);
void k_mvw40(const int16_t *codes, const int16_t *lutmid, long nout, int16_t *out);   /* n = 40 */
void k_mvw32(const int16_t *codes, const int16_t *lutmid, long nout, int16_t *out);   /* n = 32 */
void k_exp5(const uint8_t *src, int16_t *out, long rows, uint32_t *f);
unsigned long k_shrmax(const int32_t *a, int16_t *b, long n, long s);              /* b = a >> s, max|b| */
void k_headk(const int16_t *t, const int16_t *m, int32_t *K, long n);
void k_tab5(const int16_t *x, long groups, int16_t *lut);
void k_mvb5(const uint8_t *codes, const int16_t *lut, long nout, long n, int16_t *out);
void k_pairs(const int16_t *h, long npairs, int16_t *lut);
void k_ptab(const int16_t *u, long n, int16_t *ptab);
void k_wsum(const uint8_t *vb, const int16_t *ptab, long ctx, int16_t *out);
/* scalar loops (68000 assembly on the Mac, C on the host); k is a right shift in 1..30 */
unsigned long k_maxabs16(const int16_t *x, long n);
unsigned long k_maxabs32(const int32_t *x, long n);
void k_apply16(const int16_t *x, int16_t *y, long n, long m, long k);          /* y = (x*m + h) >> k */
void k_addres(int32_t *r, const int16_t *x, long n, long m, long k);          /* r = sat(r + (x*m+h)>>k) */
void k_shr32to16(const int32_t *a, int16_t *b, long n, long s);              /* b = a >> s, s 0..16 */
unsigned long k_normpass(const int32_t *r, const int16_t *g, int16_t *t, int32_t *a, long n, long sh,
                         unsigned long *maxa);                                  /* returns sum (t*t)>>8 */
unsigned long k_ffnact(const int16_t *accg, const int16_t *accu, int32_t *hh, long n, long m, long k,
                       const int16_t *silumid);                                 /* returns max |hh| */
void k_apply16c(const int16_t *x, int16_t *y, long n, long m, long k, long q);
long k_dot8(const int8_t *w, const int16_t *y, long n);
long k_scores(const int16_t *S, const int16_t *sk, long n, long cqm, long cqe, const int16_t *bias, long pos,
              int16_t *z);
long k_softp(const int16_t *z, long n, long zmax, const int16_t *exp12, int16_t *p);
unsigned long k_softw(const int16_t *p, const int16_t *sv, long n, long emax, uint32_t *w);
void k_rshr(const uint32_t *w, int16_t *u, long n, long sh);
long k_huff2(uint32_t *st, const uint8_t *lentab, const uint8_t *symtab, uint8_t *out, long n);
void k_fletcher(const uint8_t *p, long n, uint32_t *f);
void k_exp40(const uint8_t *src, int16_t *out, long rows, const int16_t *wt, uint32_t *f);
void k_expg(const uint8_t *src, int16_t *out, long blocks, long G, const int16_t *wt, uint32_t *f);

/* optional per-phase tick profile (test build: -DPX_PROFILE, px_clock = TickCount) */
enum { PP_NORM, PP_TERN, PP_ATT, PP_ATTK, PP_FFN, PP_HEAD, PP_SAMPLE,
       PP_AQ, PP_AZ, PP_AS, PP_AR, PP_FR, PP_FS, PP_FQ, PP_N };
extern uint32_t (*px_clock)(void);
extern uint32_t px_prof[PP_N];
enum { PL_EMB, PL_TDEC, PL_EXPAND, PL_SORT, PL_N };   /* load phases */
extern uint32_t px_lprof[PL_N];

extern const int16_t px_exp12[512];
extern const int16_t px_silu[1024];
#endif
