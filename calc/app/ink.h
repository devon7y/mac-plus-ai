/* Scribble Calc: strokes on the paper and the recogniser's state (no Toolbox types here, so the
 * recogniser also builds on the host for the bit-exact tests). */
#ifndef INK_H
#define INK_H

#define NCLS 18
#define MAXPTS 8000
#define MAXSTK 400
#define MAXGRP 64
#define GRPSTK 12              /* strokes per group at most */
#define NOFORCE (-1)

typedef struct { short x, y; } Pt;           /* paper coordinates (0,0 = the paper's top left) */

typedef struct {
    short first, n;                           /* its points: gPts[first .. first+n-1] */
    short x0, y0, x1, y1;                     /* bounding box */
    unsigned char alive;
    signed char force;                        /* a user correction for the group this stroke keys */
    unsigned char forceN;                     /* ... valid while the group still has this many strokes */
    unsigned char cached;                     /* sc[] holds the scores of the group with key ckey */
    unsigned long ckey;
    short sc[NCLS];
} Stroke;

typedef struct {
    short n;                                  /* strokes */
    short stk[GRPSTK];                        /* indices into gStk, ascending */
    short x0, y0, x1, y1;
    short bar0, bar1, bary0, bary1;           /* box of its flat strokes (hasbar) */
    unsigned char tiny, hastall, hasbar, ready;
    unsigned char allowR, raised;             /* may be / was read as a superscript */
    short key;                                /* the first (oldest) stroke: holds cache and correction */
    unsigned long hash;
    signed char cls;                          /* decoded class */
    signed char forced;                       /* NOFORCE or the user's choice */
    short *sc;                                /* scores (gStk[key].sc) when ready */
} Group;

extern Pt gPts[MAXPTS];
extern Stroke gStk[MAXSTK];
extern short gNPts, gNStk;
extern Group gGrp[MAXGRP];
extern short gNGrp, gHref;

/* recog.c */
int rec_segment(void);                        /* groups from the alive strokes; sets gHref */
void rec_raster(const Group *g, short minD, unsigned char *bits);   /* 24 x 24, 3 bytes per row */
int rec_next_pending(void);                   /* a group without scores, or -1 */
int rec_classify(int gi);                     /* runs the classifier, caches the scores; 0 if gRecAbort stopped it */
extern int (*gRecAbort)(void);                /* checked between rows of the classifier (may be NULL) */
int rec_decode(signed char *tokens);          /* Viterbi over the groups; returns 1 if every group was ready */

#endif
