/* Doodle: a MacPaint-style paint program with "Finish it" for the Mac Plus. Shared declarations. */
#ifndef DOODLE_H
#define DOODLE_H

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
#include <Files.h>
#include <StandardFile.h>
#include <ToolUtils.h>
#include <SegLoad.h>

/* MacPaint page */
#define PAGE_W 576
#define PAGE_H 720
#define PAGE_RB 72
#define PAGE_BYTES ((long)PAGE_RB * PAGE_H)

/* Layout, relative to the app area (384 px wide, centred in screenBits.bounds, below the menu bar) */
#define APP_W 384
#define CANVAS_X 48
#define CANVAS_Y 4
#define CANVAS_W 336
#define CANVAS_H 272
#define AIF_X 40          /* AI frame inside the canvas (document coordinates) */
#define AIF_Y 8           /* (canvas 336x272: 8 px above and below) */
#define AIF_N 256

enum { T_SELECT, T_TEXT, T_BUCKET, T_SPRAY, T_BRUSH, T_PENCIL, T_LINE, T_ERASER,
       T_RECT, T_FRECT, T_OVAL, T_FOVAL, NTOOLS };
#define NPATS 38
#define NSIZES 4

/* Debug block read by the MAME test scripts (fixed offsets; big-endian; word-aligned). */
struct DebugBlock {
    unsigned long magic;      /*   0 'DOOD' */
    unsigned long version;    /*   4 */
    unsigned long notMagic;   /*   8 ~'DOOD' */
    unsigned long state;      /*  12 0 init, 1 ready (event loop running) */
    short tool, pat, size, textOn;   /* 16 18 20 22 */
    unsigned long docBase;    /*  24 document bitmap (576x720, 72 bytes per row) */
    unsigned long undoBase;   /*  28 */
    Rect canvas;              /*  32 canvas on screen */
    Rect aiFrame;             /*  40 AI frame on screen */
    unsigned long ops;        /*  48 committed operations */
    long lastErr;             /*  52 last file error (OSErr) */
    unsigned long freeMem;    /*  56 */
    unsigned long loopTicks;  /*  60 TickCount at the last event-loop pass */
    short selOn;              /*  64 */
    Rect sel;                 /*  66 selection (document coordinates) */
    short pad74;              /*  74 */
    unsigned long nPoints;    /*  76 AI prompt points recorded */
    unsigned long pointBuf;   /*  80 address of (x, y, lift) words, frame coordinates */
    unsigned long fileOps;    /*  84 successful saves + opens */
    unsigned long fiState;    /*  88 Finish it: 0 idle 1 loading 2 priming 3 drawing 4 done 5 error 6 cancelled */
    unsigned long fiCat;      /*  92 */
    unsigned long fiSeed;     /*  96 */
    unsigned long fiPrimed;   /* 100 rows fed to the model (including the start token) */
    unsigned long fiGen;      /* 104 rows generated */
    unsigned long fiRows;     /* 108 address of generated rows (dx, dy, pen words) */
    unsigned long fiT0, fiT1, fiT2;  /* 112 116 120 ticks: start, priming done, end */
    unsigned long fiModel;    /* 124 H | kind << 16 | cell << 24 when a model is loaded */
    unsigned long cmd;        /* 128 mailbox: written by the test script, cleared by the app */
    unsigned long arg[4];     /* 132 */
    unsigned long cmdDone;    /* 148 incremented after each command */
    unsigned long cmdResult;  /* 152 */
    unsigned long fiPrompt;   /* 156 address of the prompt rows fed (dx, dy, pen words) */
    unsigned long fiNPrompt;  /* 160 */
    unsigned long fiLoadTicks;/* 164 ticks spent loading the model (first Finish it) */
    unsigned long modelBytes; /* 168 RAM used by the model */
    short temp;               /* 172 temperature setting: 0 tidy 0.3, 1 normal 0.5, 2 wild 0.8 */
    short modelWhere;         /* 174 model found in: 1 the app's folder, 2 default folder, 3 volume root */
    unsigned long scrapLen;   /* 176 bytes of 'PICT' put on / read from the scrap by the last cut/copy/paste */
    unsigned long clipOps;    /* 180 cuts + copies + pastes done */
};
extern struct DebugBlock dbg;
#define DBG_MAGIC 0x444F4F44UL

/* main.c */
extern WindowPtr gWin;
extern short gOX, gOY;                /* app area origin on screen */
extern Rect gCanvasR, gToolsR, gFinishR, gSizeR, gPatR, gCurPatR, gAIFrameR;
void SetStatus(const unsigned char *msg);   /* bottom-left status text (Pascal string) */

/* paint.c */
extern BitMap gDoc, gUndo;
extern GrafPort gDocPort;
extern short gTool, gPat, gSize;
extern Pattern gPats[NPATS];
extern short gRecording;
Boolean PaintInit(void);
void DrawPalettes(void);
void DrawCanvas(void);
void Blit(const Rect *docR);          /* copy part of the document to the screen + overlays */
void SaveUndo(void);
void DoUndo(void);
void ClearCanvas(void);
void ClickPalettes(Point p, long when);
void TrackCanvas(Point p, short mods);
void TypeKey(char c, short mods);
void EndText(void);
void Deselect(void);
void ClearSelection(void);
void CopySelection(Boolean cut);      /* Cut / Copy: the selection as a 'PICT' on the system scrap */
void PasteScrap(void);                /* Paste a 'PICT' from the scrap as a new selection */
void SetToolCursor(Point p);
void MarkOp(void);
void PaintIdle(void);
/* AI prompt strokes (frame coordinates) */
#define MAXPTS 2000
extern short gPts[MAXPTS * 3];
extern short gNPts;
void ClearPrompt(void);

/* file.c */
void FileNew(void);
void FileOpen(void);
void FileSave(Boolean saveAs);
OSErr WritePaintFile(ConstStr255Param name, short vRef);
OSErr ReadPaintFile(ConstStr255Param name, short vRef);

/* finish.c */
void FinishInit(void);
void FinishIt(short cat, unsigned long seed, Boolean askCategory);
void SetTemperature(short which);     /* 0 tidy 0.3, 1 normal 0.5, 2 wild 0.8 */
extern short gTemp;

#endif
