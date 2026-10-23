/*
 * Doodle's drawing core: the 576x720 MacPaint page (an offscreen GrafPort), the palettes,
 * every tool, one level of undo, the rectangle selection and the text tool. The visible
 * canvas shows the page's top-left 336x272 pixels; tools draw into the page and Blit()
 * copies the changed part to the screen, then draws the overlays (AI frame, selection,
 * text caret).
 */
#include "doodle.h"
#include "icons.h"

BitMap gDoc, gUndo;
GrafPort gDocPort;
short gTool = T_PENCIL, gPat = 0, gSize = 1;
Pattern gPats[NPATS];
short gPts[MAXPTS * 3];
short gNPts;

static Ptr gMask;                       /* SeedFill mask for the visible canvas */
static Rect docVis;                     /* (0, 0, CANVAS_H, CANVAS_W) */
static Boolean undoValid;
static short undoNPts;
static Boolean recInside;               /* the prompt stroke being recorded is open */
static Boolean selOn;
static Rect selR;
static Boolean textOn;
static Point textOrig;                  /* baseline start (document coordinates) */
static short textBase, caretX;
static unsigned char textBuf[256];
static short curCursor = -1;
static long lastToolClick;
static unsigned long sprayRand = 0x12345678UL;

/* DoubleTime (low memory $2F0), read through asm so GCC doesn't object to the fixed address */
static long DoubleTicks(void)
{
    long v;
    __asm__ volatile("move.l 0x2F0.w,%0" : "=d"(v));
    return v;
}

static const short kLineW[NSIZES] = {1, 2, 3, 4};
static const short kBrushW[NSIZES] = {3, 5, 8, 12};

static const unsigned char *const kToolNames[NTOOLS] = {
    "\pSelect: drag a rectangle, drag inside it to move, Backspace clears it",
    "\pText: click, then type", "\pPaint bucket", "\pSpray can", "\pBrush", "\pPencil",
    "\pLine", "\pEraser (double-click it to clear the canvas)", "\pRectangle", "\pFilled rectangle",
    "\pOval", "\pFilled oval"};

void MarkOp(void)
{
    dbg.ops++;
    dbg.nPoints = (unsigned long)gNPts;
}

static void DocToScreen(Rect *r)
{
    OffsetRect(r, gCanvasR.left, gCanvasR.top);
}

static void ScreenToDoc(Point *p)
{
    p->h -= gCanvasR.left;
    p->v -= gCanvasR.top;
}

static Point MouseDoc(void)
{
    Point m;
    SetPort(gWin);
    GetMouse(&m);
    ScreenToDoc(&m);
    return m;
}

static Boolean DocPixel(short h, short v)
{
    if (h < 0 || v < 0 || h >= PAGE_W || v >= PAGE_H)
        return false;
    return (gDoc.baseAddr[(long)v * PAGE_RB + (h >> 3)] & (0x80 >> (h & 7))) != 0;
}

static void Overlays(const Rect *clip)
{
    Rect r;
    SetPort(gWin);
    ClipRect((Rect *)clip);
    PenNormal();
    PenPat(&qd.gray);
    PenMode(patOr);
    r = gAIFrameR;
    InsetRect(&r, -1, -1);
    FrameRect(&r);
    if (selOn) {
        r = selR;
        DocToScreen(&r);
        InsetRect(&r, -1, -1);
        PenMode(patXor);
        FrameRect(&r);
    }
    PenNormal();
    if (textOn) {
        MoveTo(gCanvasR.left + caretX, gCanvasR.top + textBase - 10);
        LineTo(gCanvasR.left + caretX, gCanvasR.top + textBase + 2);
    }
    ClipRect(&gWin->portRect);
}

void Blit(const Rect *docR)
{
    Rect r, s;
    if (!SectRect(docR, &docVis, &r))
        return;
    s = r;
    DocToScreen(&s);
    SetPort(gWin);
    CopyBits(&gDoc, &gWin->portBits, &r, &s, srcCopy, NULL);
    Overlays(&s);
}

void DrawCanvas(void)
{
    Rect r = gCanvasR;
    SetPort(gWin);
    PenNormal();
    InsetRect(&r, -1, -1);
    FrameRect(&r);
    Blit(&docVis);
}

/* ------------------------------------------------------------------ palettes */
static void ToolCell(short i, Rect *r)
{
    SetRect(r, gToolsR.left + (i & 1) * 22, gToolsR.top + (i >> 1) * 20,
            gToolsR.left + (i & 1) * 22 + 22, gToolsR.top + (i >> 1) * 20 + 20);
}

static void DrawTools(void)
{
    short i;
    Rect r, d;
    BitMap ic;
    SetPort(gWin);
    PenNormal();
    for (i = 0; i < NTOOLS; i++) {
        ToolCell(i, &r);
        EraseRect(&r);
        FrameRect(&r);
        ic.baseAddr = (Ptr)kIcons[i];
        ic.rowBytes = 2;
        SetRect(&ic.bounds, 0, 0, 16, 16);
        SetRect(&d, r.left + 3, r.top + 2, r.left + 19, r.top + 18);
        CopyBits(&ic, &gWin->portBits, &ic.bounds, &d, srcCopy, NULL);
        if (i == gTool) {
            InsetRect(&r, 1, 1);
            InvertRect(&r);
        }
    }
}

static void DrawFinishButton(void)
{
    Rect r = gFinishR;
    SetPort(gWin);
    PenNormal();
    EraseRect(&r);
    PenSize(2, 2);
    FrameRoundRect(&r, 8, 8);
    PenNormal();
    TextFont(3);
    TextSize(9);
    MoveTo(r.left + (r.right - r.left - StringWidth("\pFinish it")) / 2, r.top + 13);
    DrawString("\pFinish it");
    TextFont(0);
    TextSize(0);
}

static void DrawSizes(void)
{
    short i;
    Rect r, l;
    SetPort(gWin);
    PenNormal();
    for (i = 0; i < NSIZES; i++) {
        SetRect(&r, gSizeR.left, gSizeR.top + i * 12, gSizeR.right, gSizeR.top + i * 12 + 12);
        EraseRect(&r);
        SetRect(&l, r.left + 6, r.top + 6 - kLineW[i] / 2, r.right - 6, r.top + 6 - kLineW[i] / 2 + kLineW[i]);
        PaintRect(&l);
        if (i == gSize)
            InvertRect(&r);
    }
    r = gSizeR;
    InsetRect(&r, -1, -1);
    FrameRect(&r);
}

static void PatCell(short i, Rect *r)
{
    SetRect(r, gPatR.left + (i % 19) * 17, gPatR.top + (i / 19) * 13,
            gPatR.left + (i % 19) * 17 + 16, gPatR.top + (i / 19) * 13 + 12);
}

static void DrawPatterns(void)
{
    short i;
    Rect r;
    SetPort(gWin);
    PenNormal();
    for (i = 0; i < NPATS; i++) {
        PatCell(i, &r);
        FillRect(&r, &gPats[i]);
        FrameRect(&r);
    }
    r = gCurPatR;
    FillRect(&r, &gPats[gPat]);
    FrameRect(&r);
    InsetRect(&r, -2, -2);
    FrameRect(&r);
}

void DrawPalettes(void)
{
    DrawTools();
    DrawFinishButton();
    DrawSizes();
    DrawPatterns();
}

static void SelectTool(short t)
{
    EndText();
    if (t != T_SELECT)
        Deselect();
    gTool = t;
    dbg.tool = t;
    DrawTools();
    SetStatus(kToolNames[t]);
}

void ClickPalettes(Point p, long when)
{
    short i;
    Rect r;
    if (PtInRect(p, &gToolsR)) {
        for (i = 0; i < NTOOLS; i++) {
            ToolCell(i, &r);
            if (PtInRect(p, &r)) {
                if (i == T_ERASER && gTool == T_ERASER && when - lastToolClick <= DoubleTicks())
                    ClearCanvas();          /* MacPaint: double-click the eraser */
                lastToolClick = when;
                SelectTool(i);
                return;
            }
        }
    } else if (PtInRect(p, &gFinishR)) {
        Rect b = gFinishR;
        InsetRect(&b, 2, 2);
        InvertRect(&b);
        while (StillDown())
            ;
        InvertRect(&b);
        EndText();
        Deselect();
        FinishIt(-1, 0, true);
    } else if (PtInRect(p, &gSizeR)) {
        gSize = (p.v - gSizeR.top) / 12;
        if (gSize >= NSIZES)
            gSize = NSIZES - 1;
        dbg.size = gSize;
        DrawSizes();
    } else if (PtInRect(p, &gPatR)) {
        for (i = 0; i < NPATS; i++) {
            PatCell(i, &r);
            if (PtInRect(p, &r)) {
                gPat = i;
                dbg.pat = i;
                DrawPatterns();
                return;
            }
        }
    }
}

void SetToolCursor(Point p)
{
    short want = 0;     /* 0 arrow; else CURS id: 1 I-beam, 2 cross, 3 plus */
    if (PtInRect(p, &gCanvasR)) {
        if (gTool == T_TEXT)
            want = 1;
        else if (gTool == T_SELECT || gTool >= T_LINE)
            want = gTool == T_ERASER ? 3 : 2;
        else
            want = 3;
    }
    if (want == curCursor)
        return;
    curCursor = want;
    if (want == 0) {
        SetCursor(&qd.arrow);
    } else {
        CursHandle h = GetCursor(want);
        if (h)
            SetCursor(*h);
    }
}

/* ------------------------------------------------------------------ undo */
void SaveUndo(void)
{
    BlockMove(gDoc.baseAddr, gUndo.baseAddr, PAGE_BYTES);
    undoNPts = gNPts;
    undoValid = true;
}

void DoUndo(void)
{
    Ptr t;
    short n;
    if (!undoValid)
        return;
    selOn = false;
    t = gDoc.baseAddr;
    gDoc.baseAddr = gUndo.baseAddr;
    gUndo.baseAddr = t;
    SetPort(&gDocPort);
    SetPortBits(&gDoc);
    n = gNPts;
    gNPts = undoNPts;
    undoNPts = n;
    recInside = false;
    dbg.docBase = (unsigned long)gDoc.baseAddr;
    dbg.undoBase = (unsigned long)gUndo.baseAddr;
    DrawCanvas();
    MarkOp();
}

void ClearPrompt(void)
{
    gNPts = 0;
    recInside = false;
}

void ClearCanvas(void)
{
    long *p, n;
    SaveUndo();
    selOn = false;
    p = (long *)gDoc.baseAddr;
    for (n = PAGE_BYTES / 4; n > 0; n--)
        *p++ = 0;
    ClearPrompt();
    DrawCanvas();
    MarkOp();
}

/* ------------------------------------------------------------------ prompt recording */
static void RecPoint(Point p)
{
    short fx = p.h - AIF_X, fy = p.v - AIF_Y;
    if (!gRecording)
        return;
    if (fx >= 0 && fx < AIF_N && fy >= 0 && fy < AIF_N) {
        if (recInside && gPts[(gNPts - 1) * 3] == fx && gPts[(gNPts - 1) * 3 + 1] == fy)
            return;
        if (gNPts >= MAXPTS)
            return;
        gPts[gNPts * 3] = fx;
        gPts[gNPts * 3 + 1] = fy;
        gPts[gNPts * 3 + 2] = 0;
        gNPts++;
        recInside = true;
    } else if (recInside) {
        gPts[(gNPts - 1) * 3 + 2] = 1;
        recInside = false;
    }
}

static void RecEnd(void)
{
    if (recInside) {
        gPts[(gNPts - 1) * 3 + 2] = 1;
        recInside = false;
    }
    dbg.nPoints = (unsigned long)gNPts;
}

short gRecording;

/* ------------------------------------------------------------------ tools */
static void SegRect(Point a, Point b, short sz, Rect *r)
{
    SetRect(r, (a.h < b.h ? a.h : b.h) - sz - 1, (a.v < b.v ? a.v : b.v) - sz - 1,
            (a.h > b.h ? a.h : b.h) + sz + 2, (a.v > b.v ? a.v : b.v) + sz + 2);
}

static void Freehand(Point p)
{
    Point m, last = p;
    short sz, off;
    Rect r;
    SaveUndo();
    SetPort(&gDocPort);
    PenNormal();
    gRecording = false;
    if (gTool == T_PENCIL) {
        sz = 1;
        if (DocPixel(p.h, p.v)) {
            PenPat(&qd.white);      /* MacPaint: the pencil erases when it starts on black */
        } else {
            PenPat(&qd.black);
            gRecording = true;
        }
    } else if (gTool == T_BRUSH) {
        sz = kBrushW[gSize];
        PenPat(&gPats[gPat]);
        gRecording = gPat != 19;    /* not when painting white */
    } else {
        sz = 16;
        PenPat(&qd.white);
    }
    PenSize(sz, sz);
    off = sz / 2;
    MoveTo(p.h - off, p.v - off);
    LineTo(p.h - off, p.v - off);
    SegRect(p, p, sz, &r);
    Blit(&r);
    RecPoint(p);
    while (StillDown()) {
        m = MouseDoc();
        if (m.h == last.h && m.v == last.v)
            continue;
        SetPort(&gDocPort);
        MoveTo(last.h - off, last.v - off);
        LineTo(m.h - off, m.v - off);
        SegRect(last, m, sz, &r);
        Blit(&r);
        RecPoint(m);
        last = m;
    }
    RecEnd();
    gRecording = false;
    MarkOp();
}

static void Spray(Point p)
{
    Point m = p;
    long t, lastT = -1;
    short k, dx, dy, x, y;
    Rect r;
    SaveUndo();
    do {
        t = TickCount();
        if (t != lastT) {
            lastT = t;
            for (k = 0; k < 6; k++) {
                sprayRand ^= sprayRand << 13;
                sprayRand ^= sprayRand >> 17;
                sprayRand ^= sprayRand << 5;
                dx = (short)(sprayRand & 31) - 16;
                dy = (short)((sprayRand >> 8) & 31) - 16;
                dx /= 2;
                dy /= 2;
                if (dx * dx + dy * dy > 64)
                    continue;
                x = m.h + dx;
                y = m.v + dy;
                if (x < 0 || y < 0 || x >= CANVAS_W || y >= CANVAS_H)
                    continue;
                if ((gPats[gPat].pat[y & 7] >> (7 - (x & 7))) & 1)
                    gDoc.baseAddr[(long)y * PAGE_RB + (x >> 3)] |= (char)(0x80 >> (x & 7));
            }
            SetRect(&r, m.h - 9, m.v - 9, m.h + 10, m.v + 10);
            Blit(&r);
        }
        m = MouseDoc();
    } while (StillDown());
    MarkOp();
}

static void ShapeRect(Point a, Point b, Rect *r)
{
    Pt2Rect(a, b, r);
    r->right++;
    r->bottom++;
}

static void DrawShape(Point a, Point b)
{
    Rect r;
    short w = kLineW[gSize];
    PenNormal();
    PenSize(w, w);
    if (gTool == T_LINE) {
        MoveTo(a.h, a.v);
        LineTo(b.h, b.v);
        return;
    }
    ShapeRect(a, b, &r);
    if (gTool == T_RECT || gTool == T_FRECT) {
        if (gTool == T_FRECT)
            FillRect(&r, &gPats[gPat]);
        FrameRect(&r);
    } else {
        if (gTool == T_FOVAL)
            FillOval(&r, &gPats[gPat]);
        FrameOval(&r);
    }
}

static void Shape(Point a)
{
    Point b = a, m, sa, sb;
    Rect prev, r;
    Boolean have = false;
    short w = kLineW[gSize];
    SaveUndo();
    while (StillDown()) {
        m = MouseDoc();
        if (m.h == b.h && m.v == b.v && have)
            continue;
        b = m;
        if (have)
            Blit(&prev);
        SegRect(a, b, w, &prev);
        have = true;
        SetPort(gWin);
        ClipRect(&gCanvasR);
        sa = a;
        sb = b;
        sa.h += gCanvasR.left; sa.v += gCanvasR.top;
        sb.h += gCanvasR.left; sb.v += gCanvasR.top;
        DrawShape(sa, sb);
        ClipRect(&gWin->portRect);
    }
    SetPort(&gDocPort);
    DrawShape(a, b);
    SegRect(a, b, w, &r);
    if (have)
        UnionRect(&r, &prev, &r);
    Blit(&r);
    MarkOp();
}

static void Bucket(Point p)
{
    short y, x;
    unsigned short pw, mw, *d, *mk;
    if (DocPixel(p.h, p.v))
        return;
    SaveUndo();
    {
        long t0 = TickCount();
        SeedFill(gDoc.baseAddr, gMask, PAGE_RB, CANVAS_W / 8, CANVAS_H, CANVAS_W / 16, p.h, p.v);
        dbg.pad74 = (short)(TickCount() - t0);      /* fill time in ticks (diagnostic) */
    }
    for (y = 0; y < CANVAS_H; y++) {
        pw = (unsigned short)(((unsigned char)gPats[gPat].pat[y & 7] << 8) | (unsigned char)gPats[gPat].pat[y & 7]);
        d = (unsigned short *)(gDoc.baseAddr + (long)y * PAGE_RB);
        mk = (unsigned short *)(gMask + (long)y * (CANVAS_W / 8));
        for (x = 0; x < CANVAS_W / 16; x++) {
            mw = mk[x];
            d[x] = (unsigned short)((d[x] & ~mw) | (pw & mw));
        }
    }
    Blit(&docVis);
    MarkOp();
}

/* ------------------------------------------------------------------ text */
static void TextLineRect(Rect *r)
{
    SetRect(r, textOrig.h - 1, textBase - 11, CANVAS_W, textBase + 5);
}

static void RedrawText(void)
{
    Rect r;
    TextLineRect(&r);
    SetPort(&gDocPort);
    CopyBits(&gUndo, &gDoc, &r, &r, srcCopy, NULL);   /* the line as it was before typing */
    PenNormal();
    MoveTo(textOrig.h, textBase);
    DrawString(textBuf);
    caretX = textOrig.h + StringWidth(textBuf);
    Blit(&r);
}

static void StartText(Point p)
{
    EndText();
    SaveUndo();
    textOn = true;
    dbg.textOn = 1;
    textOrig = p;
    textBase = p.v + 4;
    textBuf[0] = 0;
    RedrawText();
}

void EndText(void)
{
    Rect r;
    if (!textOn)
        return;
    textOn = false;
    dbg.textOn = 0;
    TextLineRect(&r);
    Blit(&r);
    MarkOp();
}

void TypeKey(char c, short mods)
{
    (void)mods;
    if (!textOn) {
        if ((c == 8 || c == 127) && selOn)
            ClearSelection();
        return;
    }
    if (c == 8) {
        if (textBuf[0] > 0)
            textBuf[0]--;
    } else if (c == 13) {
        Rect r;
        TextLineRect(&r);
        textOn = false;
        Blit(&r);           /* remove the caret from the finished line */
        textOn = true;
        textBase += 15;
        textBuf[0] = 0;
        if (textBase > CANVAS_H) {
            EndText();
            return;
        }
        BlockMove(gDoc.baseAddr, gUndo.baseAddr, PAGE_BYTES);   /* keep the line just typed */
    } else if ((unsigned char)c >= 32 && textBuf[0] < 250) {
        textBuf[++textBuf[0]] = (unsigned char)c;
    } else {
        return;
    }
    RedrawText();
}

/* ------------------------------------------------------------------ selection */
void Deselect(void)
{
    Rect r;
    if (!selOn)
        return;
    selOn = false;
    dbg.selOn = 0;
    r = selR;
    InsetRect(&r, -2, -2);
    Blit(&r);
}

void ClearSelection(void)
{
    if (!selOn)
        return;
    SaveUndo();
    SetPort(&gDocPort);
    EraseRect(&selR);
    Deselect();
    MarkOp();
}

/* The selection as a picture, recorded in a private offscreen port so nothing else is drawn. */
static PicHandle SelectionPicture(void)
{
    BitMap sb;
    GrafPort pp;
    GrafPtr old;
    PicHandle pic;
    short w = selR.right - selR.left, h = selR.bottom - selR.top;
    sb.rowBytes = (short)(((w + 15) / 16) * 2);
    SetRect(&sb.bounds, 0, 0, w, h);
    sb.baseAddr = NewPtr((long)sb.rowBytes * h);
    if (!sb.baseAddr)
        return NULL;
    CopyBits(&gDoc, &sb, &selR, &sb.bounds, srcCopy, NULL);
    GetPort(&old);
    OpenPort(&pp);
    SetPortBits(&sb);
    pp.portRect = sb.bounds;
    RectRgn(pp.visRgn, &sb.bounds);
    ClipRect(&sb.bounds);
    pic = OpenPicture(&sb.bounds);
    CopyBits(&sb, &pp.portBits, &sb.bounds, &sb.bounds, srcCopy, NULL);
    ClosePicture();
    ClosePort(&pp);
    SetPort(old);
    DisposePtr(sb.baseAddr);
    return pic;
}

void CopySelection(Boolean cut)
{
    PicHandle pic;
    long len;
    if (!selOn) {
        SysBeep(5);
        return;
    }
    pic = SelectionPicture();
    if (!pic) {
        SysBeep(5);
        return;
    }
    len = GetHandleSize((Handle)pic);
    ZeroScrap();
    HLock((Handle)pic);
    PutScrap(len, 'PICT', (Ptr)*pic);
    HUnlock((Handle)pic);
    KillPicture(pic);
    dbg.scrapLen = (unsigned long)len;
    dbg.clipOps++;
    if (cut)
        ClearSelection();
}

void PasteScrap(void)
{
    Handle h = NewHandle(0);
    long off, len;
    Rect fr, dst, r;
    short w, ht;
    if (!h)
        return;
    len = GetScrap(h, 'PICT', &off);
    if (len <= 0) {
        DisposeHandle(h);
        SysBeep(5);
        return;
    }
    EndText();
    fr = (**(PicHandle)h).picFrame;
    w = fr.right - fr.left;
    ht = fr.bottom - fr.top;
    if (selOn)
        SetRect(&dst, selR.left, selR.top, selR.left + w, selR.top + ht);
    else
        SetRect(&dst, (CANVAS_W - w) / 2, (CANVAS_H - ht) / 2, (CANVAS_W - w) / 2 + w, (CANVAS_H - ht) / 2 + ht);
    if (dst.left < 0) OffsetRect(&dst, -dst.left, 0);
    if (dst.top < 0) OffsetRect(&dst, 0, -dst.top);
    Deselect();
    SaveUndo();
    SetPort(&gDocPort);
    EraseRect(&dst);
    HLock(h);
    DrawPicture((PicHandle)h, &dst);
    HUnlock(h);
    DisposeHandle(h);
    if (SectRect(&dst, &docVis, &selR)) {
        selOn = true;
        dbg.selOn = 1;
        dbg.sel = selR;
    }
    r = dst;
    InsetRect(&r, -2, -2);
    Blit(&r);
    dbg.scrapLen = (unsigned long)len;
    dbg.clipOps++;
    MarkOp();
}

static void MoveSelection(Point p)
{
    BitMap sb;
    Rect cur = selR, nr, r, s;
    Point m;
    short w = selR.right - selR.left, h = selR.bottom - selR.top;
    sb.rowBytes = (short)(((w + 15) / 16) * 2);
    SetRect(&sb.bounds, 0, 0, w, h);
    sb.baseAddr = NewPtr((long)sb.rowBytes * h);
    if (!sb.baseAddr) {
        SysBeep(5);
        return;
    }
    SaveUndo();
    CopyBits(&gDoc, &sb, &selR, &sb.bounds, srcCopy, NULL);
    SetPort(&gDocPort);
    EraseRect(&selR);
    while (StillDown()) {
        m = MouseDoc();
        nr = selR;
        OffsetRect(&nr, m.h - p.h, m.v - p.v);
        if (nr.left == cur.left && nr.top == cur.top)
            continue;
        r = cur;
        InsetRect(&r, -2, -2);
        selOn = false;
        Blit(&r);
        cur = nr;
        s = cur;
        DocToScreen(&s);
        SetPort(gWin);
        ClipRect(&gCanvasR);
        CopyBits(&sb, &gWin->portBits, &sb.bounds, &s, srcCopy, NULL);
        ClipRect(&gWin->portRect);
    }
    SetPort(&gDocPort);
    CopyBits(&sb, &gDoc, &sb.bounds, &cur, srcCopy, NULL);
    DisposePtr(sb.baseAddr);
    r = selR;
    InsetRect(&r, -2, -2);
    selOn = false;
    Blit(&r);
    if (!SectRect(&cur, &docVis, &selR)) {
        dbg.selOn = 0;
    } else {
        selOn = true;
        dbg.sel = selR;
    }
    r = cur;
    InsetRect(&r, -2, -2);
    Blit(&r);
    MarkOp();
}

static void SelectTrack(Point p)
{
    Point m, last = p;
    Rect r, prev;
    if (selOn && PtInRect(p, &selR)) {
        MoveSelection(p);
        return;
    }
    Deselect();
    SetRect(&prev, p.h, p.v, p.h, p.v);
    while (StillDown()) {
        m = MouseDoc();
        if (m.h == last.h && m.v == last.v)
            continue;
        last = m;
        Pt2Rect(p, m, &r);
        r.right++;
        r.bottom++;
        SectRect(&r, &docVis, &r);
        selOn = false;
        InsetRect(&prev, -2, -2);
        Blit(&prev);
        selR = r;
        prev = r;
        selOn = true;
        InsetRect(&r, -2, -2);
        Blit(&r);
    }
    if (!selOn || selR.right - selR.left < 2 || selR.bottom - selR.top < 2) {
        Deselect();
        return;
    }
    dbg.selOn = 1;
    dbg.sel = selR;
}

void TrackCanvas(Point where, short mods)
{
    Point p = where;
    (void)mods;
    ScreenToDoc(&p);
    if (gTool != T_TEXT)
        EndText();
    if (gTool != T_SELECT)
        Deselect();
    switch (gTool) {
    case T_PENCIL: case T_BRUSH: case T_ERASER:
        Freehand(p);
        break;
    case T_SPRAY:
        Spray(p);
        break;
    case T_LINE: case T_RECT: case T_FRECT: case T_OVAL: case T_FOVAL:
        Shape(p);
        break;
    case T_BUCKET:
        Bucket(p);
        break;
    case T_TEXT:
        StartText(p);
        break;
    case T_SELECT:
        SelectTrack(p);
        break;
    }
}

void PaintIdle(void)
{
    dbg.temp = gTemp;
    dbg.tool = gTool;
    dbg.pat = gPat;
    dbg.size = gSize;
}

Boolean PaintInit(void)
{
    Handle h;
    short i;
    long *p, n;
    SetRect(&docVis, 0, 0, CANVAS_W, CANVAS_H);
    gDoc.baseAddr = NewPtr(PAGE_BYTES);
    gUndo.baseAddr = NewPtr(PAGE_BYTES);
    gMask = NewPtr((long)(CANVAS_W / 8) * CANVAS_H);
    if (!gDoc.baseAddr || !gUndo.baseAddr || !gMask)
        return false;
    for (p = (long *)gDoc.baseAddr, n = PAGE_BYTES / 4; n > 0; n--)
        *p++ = 0;
    gDoc.rowBytes = gUndo.rowBytes = PAGE_RB;
    SetRect(&gDoc.bounds, 0, 0, PAGE_W, PAGE_H);
    gUndo.bounds = gDoc.bounds;
    OpenPort(&gDocPort);
    SetPortBits(&gDoc);
    gDocPort.portRect = gDoc.bounds;
    RectRgn(gDocPort.visRgn, &gDoc.bounds);
    ClipRect(&docVis);
    TextFont(3);            /* Geneva 12 (ROM) */
    TextSize(12);
    h = GetResource('PAT#', 0);
    if (h && *(short *)*h >= NPATS) {
        for (i = 0; i < NPATS; i++)
            BlockMove(*h + 2 + 8 * i, &gPats[i], 8);
    } else {
        for (i = 0; i < NPATS; i++)
            gPats[i] = (i & 1) ? qd.white : qd.black;
    }
    SetPort(gWin);
    dbg.docBase = (unsigned long)gDoc.baseAddr;
    dbg.undoBase = (unsigned long)gUndo.baseAddr;
    dbg.pointBuf = (unsigned long)gPts;
    dbg.tool = gTool;
    return true;
}
