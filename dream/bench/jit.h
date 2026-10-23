#ifndef DREAM_JIT_H
#define DREAM_JIT_H

typedef struct {
    unsigned char kind, mode, gather, scatter, center, tab, wq, pad;
    short src, dst, cin, cout, grid;
} LDesc;

enum { K_PW = 0, K_DW = 1 };
enum { M_RELU = 0, M_LIN = 1, M_SETH = 2, M_ADDH = 3, M_RAW = 4 };

unsigned short *jit_pw(unsigned short *p, const LDesc *l, const signed char *w, const signed char *k,
                       short Ws, short Cs, short Wd, short Cd);
unsigned short *jit_dw(unsigned short *p, const LDesc *l, const signed char *w, const signed char *k,
                       short Ws, short C);
unsigned short *jit_dw_clamp(unsigned short *p, const LDesc *l, const signed char *w, const signed char *k,
                             short Ws, short C, short lo, short hi);     /* table-free (1 MB build) */
unsigned short *jit_pw_ex(unsigned short *p, const LDesc *l, const signed char *w, const signed char *k,
                          short Ws, short Cs, short Wd, short Cd, int clamp, short lo, short hi);
void jit_set_two(int two);                                /* two-term gain codes (k1, e2) per channel */
int jit_templates(void *(*alloc)(long));                 /* once, before jit_pw_codes */
void jit_free_templates(void (*fr)(void *));
unsigned short *jit_pw_codes(unsigned short *p, const LDesc *l, const unsigned short *codes,
                             const signed char *k, short Ws, short Cs, short Wd, short Cd);
void k_run(const void *code, void *a0, void *a1, const short *bias, const short *tabmid,
           long nx, long ny, long in_xstep, long in_rowstep, long out_xstep, long out_rowstep);
#endif
