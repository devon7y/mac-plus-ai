/*
 * Scribble Calc: a handwriting calculator for the Mac Plus (1 MB, System 6) and the SE.
 * Write a sum with the mouse, e.g. 12 x (3.5 + 4) =, and the Mac reads every symbol (a small
 * ternary CNN plus the expression grammar), shows what it read and writes the exact answer.
 * This file: Toolbox setup, menus, the About box, the event loop and the test mailbox.
 */
#include "calc.h"

struct DebugBlock dbg;
WindowPtr gWin;
short gTool;

static MenuHandle mApple, mFile, mEdit;
enum { M_APPLE = 128, M_FILE, M_EDIT };
enum { I_NEWLINE = 1, I_SEP1, I_QUIT };
enum { I_UNDO = 1, I_SEP2, I_CUT, I_COPY, I_PASTE, I_CLEAR, I_SEP3, I_PEN, I_ERASER };

static Cursor gEraserCurs, gCrossCurs;

static void SetupMenus(void)
{
    mApple = NewMenu(M_APPLE, "\p\x14");
    AppendMenu(mApple, "\pAbout Scribble Calc\xC9;(-");
    AppendResMenu(mApple, 'DRVR');
    InsertMenu(mApple, 0);
    mFile = NewMenu(M_FILE, "\pFile");
    AppendMenu(mFile, "\pNew Line/N;(-;Quit/Q");
    InsertMenu(mFile, 0);
    mEdit = NewMenu(M_EDIT, "\pEdit");
    AppendMenu(mEdit, "\pUndo/Z;(-;Cut/X;Copy/C;Paste/V;Clear;(-;Pen/P;Eraser/E");
    InsertMenu(mEdit, 0);
    DrawMenuBar();
}

static void SetTool(short t)
{
    gTool = t;
    dbg.tool = t;
    CheckItem(mEdit, I_PEN, t == 0);
    CheckItem(mEdit, I_ERASER, t == 1);
    DrawButtons();
}

static void About(void)
{
    WindowPtr w;
    Rect r, b = qd.screenBits.bounds;
    EventRecord ev;
    static const char *const lines[] = {
        "Scribble Calc",
        "Write a sum with the mouse and end it with =.",
        "The Mac reads each symbol with a small neural network",
        "and works out the answer exactly, to 12 digits.",
        "",
        "Learned from handwriting in the UJI Pen Characters v2",
        "set (Prat, Castro, Llorens, Marzal, Vilar) and the",
        "Pen-Based Handwritten Digits set (Alpaydin, Alimoglu),",
        "UCI Machine Learning Repository, licensed CC BY 4.0",
        "(creativecommons.org/licenses/by/4.0), plus synthetic",
        "strokes. Both sets were modified (resampled, slanted,",
        "made to look mouse-drawn).",
        "",
        "Click a symbol under the paper to fix a misreading.",
        "Click or press a key to continue.",
    };
    short i, n = (short)(sizeof lines / sizeof lines[0]);
    short wd = 330;
    SetRect(&r, (short)((b.left + b.right - wd) / 2), 50, (short)((b.left + b.right + wd) / 2), (short)(50 + 22 + n * 13));
    w = NewWindow(NULL, &r, "\p", true, dBoxProc, (WindowPtr)-1, false, 0);
    SetPort(w);
    for (i = 0; i < n; i++) {
        const char *s = lines[i];
        TextFont(i == 0 ? 0 : 3);
        TextSize(i == 0 ? 12 : 9);
        MoveTo(14, (short)(20 + i * 13));
        while (*s)
            DrawChar(*s++);
    }
    for (;;) {
        SystemTask();
        if (GetNextEvent(mDownMask | keyDownMask, &ev))
            break;
    }
    DisposeWindow(w);
    SetPort(gWin);
    TextFont(0);
    TextSize(0);
}

static void Quit(void)
{
    ExitToShell();
}

static void DoMenu(long mr)
{
    short menu = (short)(mr >> 16), item = (short)mr;
    switch (menu) {
    case M_APPLE:
        if (item == 1) {
            About();
        } else {
            Str255 name;
            GrafPtr old;
            GetPort(&old);
            GetMenuItemText(mApple, item, name);
            OpenDeskAcc(name);
            SetPort(old);
        }
        break;
    case M_FILE:
        if (item == I_NEWLINE) DoClear();
        else if (item == I_QUIT) Quit();
        break;
    case M_EDIT:
        if (item <= I_CLEAR && SystemEdit((short)(item - 1)))   /* a desk accessory in front */
            break;
        if (item == I_UNDO) DoUndo();
        else if (item == I_COPY) DoCopy();
        else if (item == I_CLEAR) DoClear();
        else if (item == I_PEN) SetTool(0);
        else if (item == I_ERASER) SetTool(1);
        break;
    }
    HiliteMenu(0);
}

static void Mailbox(void)
{
    unsigned long c = dbg.cmd, t0;
    long k;
    if (!c)
        return;
    dbg.cmd = 0;
    dbg.cmdResult = 0;
    switch (c) {
    case 1:     /* classify dbg.raster (arg times, for timing); scores into dbg.scores */
        t0 = TickCount();
        for (k = 0; k < (long)(dbg.arg ? dbg.arg : 1); k++)
            ClassifyRaster(dbg.raster, dbg.scores);
        dbg.cmdResult = TickCount() - t0;
        break;
    case 2: DoClear(); break;
    case 3: DoUndo(); break;
    case 4: DoCopy(); break;
    case 5:     /* length of the TEXT on the scrap */
    {
        long off, len = GetScrap(NULL, 'TEXT', &off);
        dbg.cmdResult = (unsigned long)(len > 0 ? len : 0);
        break;
    }
    }
    dbg.cmdDone++;
}

static void UpdateCursor(void)
{
    static short cur = -1;
    Point p;
    short want = 0;
    GetMouse(&p);
    if (PtInRect(p, &gPaperR))
        want = gTool == 0 ? 1 : 2;
    if (want == cur)
        return;
    cur = want;
    if (want == 0) {
        SetCursor(&qd.arrow);
    } else if (want == 1) {
        SetCursor(&gCrossCurs);
    } else {
        SetCursor(&gEraserCurs);
    }
}

static void MakeEraserCursor(void)
{
    short i;
    CursHandle h = GetCursor(crossCursor);         /* load it now, not on the first stroke */
    gCrossCurs = h ? **h : qd.arrow;
    for (i = 0; i < 16; i++) {
        gEraserCurs.data[i] = 0;
        gEraserCurs.mask[i] = 0;
    }
    for (i = 3; i <= 13; i++) {
        gEraserCurs.data[i] = (short)(i == 3 || i == 13 ? 0x1FFC : 0x1004);
        gEraserCurs.mask[i] = 0x1FFC;
    }
    gEraserCurs.hotSpot.v = 8;
    gEraserCurs.hotSpot.h = 8;
}

static void Click(EventRecord *ev)
{
    WindowPtr w;
    short part = FindWindow(ev->where, &w), i;
    if (part == inMenuBar) {
        DoMenu(MenuSelect(ev->where));
        return;
    }
    if (part == inSysWindow) {
        SystemClick(ev, w);
        return;
    }
    if (w != gWin)
        return;
    SetPort(gWin);
    if (PtInRect(ev->where, &gPaperR)) {
        if (gTool == 0)
            TrackPen(ev->where);
        else
            TrackEraser(ev->where);
        return;
    }
    if (PtInRect(ev->where, &gStripR)) {
        ClickStrip(ev->where);
        return;
    }
    for (i = 0; i < 4; i++)
        if (PtInRect(ev->where, &gBtnR[i])) {
            Rect r = gBtnR[i];
            InsetRect(&r, 1, 1);
            InvertRoundRect(&r, 8, 8);
            while (StillDown())
                ;
            InvertRoundRect(&r, 8, 8);
            if (i == B_PEN) SetTool(0);
            else if (i == B_ERASER) SetTool(1);
            else if (i == B_UNDO) DoUndo();
            else DoClear();
            return;
        }
    if (PtInRect(ev->where, &gTapeR)) {
        short row = (short)((ev->where.v - gTapeR.top - 16) / 16);
        if (ev->where.v > gTapeR.top + 16)
            CopyTape(row);
    }
}

int main(void)
{
    EventRecord ev;
    Rect r;

    dbg.magic = DBG_MAGIC;
    dbg.version = 1;
    dbg.notMagic = ~DBG_MAGIC;
    dbg.err = -1;
    dbg.stkAddr = (unsigned long)gStk;
    dbg.ptsAddr = (unsigned long)gPts;

    MaxApplZone();
    MoreMasters();
    MoreMasters();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);

    Layout();
    SetupMenus();
    r = qd.screenBits.bounds;                 /* covers the screen: local = global coordinates */
    gWin = NewWindow(NULL, &r, "\p", true, plainDBox, (WindowPtr)-1, false, 0);
    SetPort(gWin);
    TextFont(0);
    TextSize(12);
    MakeEraserCursor();
    DrawInit();
    LineInit();
    SetTool(0);
    SetStatus("Write a sum on the paper, then =.");
    PaperRender();
    DrawAll();
    dbg.freeMem = (unsigned long)FreeMem();
    dbg.state = 1;

    for (;;) {
        dbg.loopTicks = TickCount();
        SystemTask();
        Mailbox();
        if (!GetNextEvent(everyEvent, &ev)) {
            SetPort(gWin);
            UpdateCursor();
            LineIdle();
            continue;
        }
        switch (ev.what) {
        case mouseDown:
            Click(&ev);
            break;
        case keyDown:
        case autoKey:
        {
            char c = (char)(ev.message & charCodeMask);
            short code = (short)((ev.message & keyCodeMask) >> 8);
            if (ev.modifiers & cmdKey) {
                if (ev.what == keyDown)
                    DoMenu(MenuKey(c));
            } else if (code == 0x32 || code == 0x35 || c == 0x1B) {   /* Esc (the Plus's ` key) */
                Quit();
            } else if (c == '\r' || c == 3) {
                DoClear();
            } else if (c == 8) {
                DoUndo();
            } else if (c == 'e' || c == 'E') {
                SetTool(1);
            } else if (c == 'p' || c == 'P') {
                SetTool(0);
            }
            break;
        }
        case updateEvt:
        {
            WindowPtr w = (WindowPtr)ev.message;
            BeginUpdate(w);
            if (w == gWin)
                DrawAll();
            EndUpdate(w);
            break;
        }
        default:
            break;
        }
    }
    return 0;
}
