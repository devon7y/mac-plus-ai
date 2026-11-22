/* Host check of recog.c + the generated classifier (built with -DNET_HOST): reads test lines from
 * stdin, writes what the Mac would compute. calc/app/tests/test_recog.py drives it.
 * Input per line (little-endian): int16 nstrokes; per stroke int16 npts, npts x (int16 x, int16 y);
 *   int16 nforced; nforced x (int16 group, int16 class).
 * Output per line: int16 href, int16 ngroups; per group int16 n, n x int16 stroke, 18 x int16 score,
 *   int16 token (+ 32 when read as a superscript). */
#include <stdio.h>
#include "ink.h"

static int rd(short *v) { return fread(v, 2, 1, stdin) == 1; }
static void wr(short v) { fwrite(&v, 2, 1, stdout); }

int main(void)
{
    short ns, i, j, k, nf;
    signed char tok[MAXGRP];
    while (rd(&ns)) {
        gNStk = 0;
        gNPts = 0;
        for (i = 0; i < ns; i++) {
            short np;
            Stroke *s = &gStk[gNStk++];
            rd(&np);
            s->first = gNPts;
            s->n = np;
            s->alive = 1;
            s->force = NOFORCE;
            s->cached = 0;
            for (j = 0; j < np; j++) {
                Pt *p = &gPts[gNPts++];
                rd(&p->x);
                rd(&p->y);
                if (j == 0 || p->x < s->x0) s->x0 = p->x;
                if (j == 0 || p->x > s->x1) s->x1 = p->x;
                if (j == 0 || p->y < s->y0) s->y0 = p->y;
                if (j == 0 || p->y > s->y1) s->y1 = p->y;
            }
        }
        rec_segment();
        while ((k = (short)rec_next_pending()) >= 0)
            rec_classify(k);
        rd(&nf);
        for (i = 0; i < nf; i++) {
            short g, c;
            rd(&g);
            rd(&c);
            gStk[gGrp[g].key].force = (signed char)c;
            gStk[gGrp[g].key].forceN = (unsigned char)gGrp[g].n;
        }
        rec_segment();             /* picks up the corrections (scores stay cached) */
        rec_decode(tok);
        wr(gHref);
        wr(gNGrp);
        for (i = 0; i < gNGrp; i++) {
            wr(gGrp[i].n);
            for (j = 0; j < gGrp[i].n; j++)
                wr(gGrp[i].stk[j]);
            for (j = 0; j < NCLS; j++)
                wr(gGrp[i].sc[j]);
            wr((short)(tok[i] + 32 * gGrp[i].raised));
        }
        fflush(stdout);
    }
    return 0;
}
