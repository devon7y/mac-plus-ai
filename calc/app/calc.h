/* Scribble Calc: shared declarations of the Mac app (main.c, draw.c, line.c). */
#ifndef CALC_H
#define CALC_H

#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <OSUtils.h>
#include <MacMemory.h>
#include <Resources.h>
#include <ToolUtils.h>
#include <SegLoad.h>
#include <Devices.h>
#include "ink.h"
#include "dec.h"

#define MAXTOK MAXGRP
#define TAPE_N 24
#define TAPE_TOK 64

/* debug block read by the MAME tests (the Lua scripts in tests/); offsets are fixed */
struct DebugBlock {
    unsigned long magic;      /*   0 'SCAL' */
    unsigned long version;    /*   4 */
    unsigned long notMagic;   /*   8 */
    unsigned long state;      /*  12 1 = event loop running */
    Rect paper;               /*  16 the paper's inside, global coordinates */
    Rect strip;               /*  24 the "read as" strip */
    short nStk, nGrp;         /*  32, 34 alive strokes, groups */
    short href, pending;      /*  36, 38 reference height, groups still to classify */
    short nTok, err;          /*  40, 42 decoded symbols; dec error (-1: no answer) */
    signed char tok[48];      /*  44 decoded classes (dec.h token codes; +32 = superscript) */
    char result[32];          /*  92 the answer as text */
    unsigned long recogCount; /* 124 classifications run */
    unsigned long recogTicks; /* 128 ticks spent classifying */
    unsigned long freeMem;    /* 132 */
    unsigned long cmd, arg;   /* 136, 140 mailbox: command and argument */
    unsigned long cmdDone;    /* 144 incremented when a command finished */
    unsigned long cmdResult;  /* 148 */
    unsigned char raster[72]; /* 152 mailbox raster (24 rows x 3 bytes) */
    short scores[18];         /* 224 mailbox scores */
    short tapeN;              /* 260 */
    short tool;               /* 262 */
    unsigned long loopTicks;  /* 264 */
    unsigned long answers;    /* 268 answers computed so far */
    unsigned long stkAddr;    /* 272 gStk (Stroke: first@0, n@2, alive@12; 56 bytes each) */
    unsigned long ptsAddr;    /* 276 gPts (x, y shorts) */
    short nStkAll;            /* 280 gNStk (including dead strokes) */
    unsigned long recogAborts;/* 282 classifications interrupted by the mouse */
    short cx[48];             /* 286 global x of each symbol's character in the strip */
    short popTop, popLeft;    /* 382, 384 the correction popup's box (0 when closed) */
    unsigned char popOrder[18]; /* 386 the classes in its cells (7 per row, cells 26 x 20 from +4,+4) */
};
#define DBG_MAGIC 0x5343414CUL    /* 'SCAL' */
extern struct DebugBlock dbg;

/* main.c */
extern WindowPtr gWin;
extern short gTool;                                 /* 0 pen, 1 eraser */
void SetStatus(const char *msg);

/* draw.c: layout and drawing */
extern Rect gPaperR, gStripR, gTapeR, gStatusR, gBtnR[4];
enum { B_PEN, B_ERASER, B_UNDO, B_CLEAR };
void DrawInit(void);
void Layout(void);
void DrawAll(void);
void DrawButtons(void);
void DrawStrip(void);
void DrawTape(void);
void DrawStatus(void);
void PaperRender(void);                              /* offscreen paper: guides, strokes, answer */
void PaperShow(const Rect *r);                       /* copy (part of) the offscreen paper to the screen */
void PaperStrokeSegment(Pt a, Pt b);                 /* draw one ink segment (offscreen and screen) */
short DrawToken(short tok, short x, short base, Boolean draw);   /* clean font; returns the width */
short DrawTokens(const signed char *tok, short n, short x, short base, Boolean draw);
int ChoosePopup(short gi, Point where);              /* correction popup; returns class, -2 auto, -1 none */
extern char gStatusMsg[64];

/* line.c: the expression being written, recognition, the answer, undo, the tape */
typedef struct {
    signed char tok[TAPE_TOK];
    short nTok;
    char result[DEC_BUFLEN];
    short sci, exp10;
    short err;
} TapeEntry;
extern TapeEntry gTape[TAPE_N];
extern short gTapeN, gTapeCur;
extern signed char gTok[MAXTOK];
extern short gNTok, gEqAt;                           /* gEqAt: index of the '=' group or -1 */
extern short gErr;                                   /* dec error, -1 = no answer */
extern char gAnsText[DEC_BUFLEN];                    /* the answer's mantissa text */
extern short gAnsSci, gAnsExp;
void LineInit(void);
void TrackPen(Point where);
void TrackEraser(Point where);
void LineChanged(void);                              /* strokes changed: regroup, schedule */
void LineIdle(void);                                 /* classify pending symbols, then decode */
void DoUndo(void);
void DoClear(void);
void DoCopy(void);
void CopyTape(short i);
void ClickStrip(Point where);
int ClassifyRaster(const unsigned char *bits, short *scores);
extern unsigned long gLastInkTick;

#endif
