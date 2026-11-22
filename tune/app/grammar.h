/* The tune grammar (port of tune/corpus/vocab.py State). */
#ifndef GRAMMAR_H
#define GRAMMAR_H
#include "tune_consts.h"

#define HIST_POS 73                 /* tick positions 0..72 */

typedef struct Gram {
    short blen, pos, trip, tdur, bar, sec, e1pos, e1bar, acc, started, ntok, last, nbars;
    unsigned char hist[TK_MAXSEC][TK_HISTBARS][HIST_POS];   /* token that started there, 0 none */
} Gram;

void gram_init(Gram *g, short blen);
void gram_feats(const Gram *g, short *f);           /* pos, trip, bar, sec */
void gram_lookback(const Gram *g, short *f);        /* la sym, la dur, lb sym, lb dur */
void gram_allowed(const Gram *g, unsigned char *ok, short maxtok, short maxbars);
void gram_step(Gram *g, short t);
void tok_inputs(short t, short *sym, short *dur);

#endif
