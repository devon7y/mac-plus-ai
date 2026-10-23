/*
 * Doodle: a MacPaint-style paint program for the Mac Plus (1 MB, System 6) with one AI
 * feature, "Finish it". This file: Toolbox setup, layout, menus, the event loop, the debug
 * block that the MAME tests read, and the test mailbox.
 */
#include "doodle.h"

struct DebugBlock dbg;
WindowPtr gWin;
short gOX, gOY;
Rect gCanvasR, gToolsR, gFinishR, gSizeR, gPatR, gCurPatR, gAIFrameR;
static Rect gStatusR;
static MenuHandle mApple, mFile, mEdit, mAI;

enum { M_APPLE = 128, M_FILE, M_EDIT, M_AI };
enum { I_NEW = 1, I_OPEN, I_SAVE, I_SAVEAS, I_SEP1, I_QUIT };
enum { I_UNDO = 1, I_SEP2, I_CUT, I_COPY, I_PASTE, I_CLEARSEL, I_SEP4, I_CLEARALL };
enum { I_FINISH = 1, I_SEP3, I_TIDY, I_NORMAL, I_WILD };

void SetStatus(const unsigned char *msg)
{
    GrafPtr old;
    GetPort(&old);
    SetPort(gWin);
    EraseRect(&gStatusR);
    TextFont(3);          /* Geneva 9 (ROM) */
    TextSize(9);
    MoveTo(gStatusR.left + 2, gStatusR.bottom - 2);
    DrawString(msg);
    TextFont(0);
    TextSize(0);
    SetPort(old);
}

static void Layout(void)
{
    Rect b = qd.screenBits.bounds;
    gOX = (short)((b.left + b.right - APP_W) / 2);
    gOY = 20;
    SetRect(&gToolsR, gOX + 2, gOY + 4, gOX + 46, gOY + 4 + 6 * 20);
    SetRect(&gFinishR, gOX + 2, gToolsR.bottom + 6, gOX + 46, gToolsR.bottom + 26);
    SetRect(&gSizeR, gOX + 2, gFinishR.bottom + 8, gOX + 46, gFinishR.bottom + 8 + NSIZES * 12);
    SetRect(&gCanvasR, gOX + CANVAS_X, gOY + CANVAS_Y, gOX + CANVAS_X + CANVAS_W, gOY + CANVAS_Y + CANVAS_H);
    SetRect(&gCurPatR, gOX + 6, gCanvasR.bottom + 6, gOX + 42, gCanvasR.bottom + 32);
    SetRect(&gPatR, gCanvasR.left, gCanvasR.bottom + 6, gCanvasR.left + 19 * 17, gCanvasR.bottom + 6 + 26);
    SetRect(&gAIFrameR, gCanvasR.left + AIF_X, gCanvasR.top + AIF_Y,
            gCanvasR.left + AIF_X + AIF_N, gCanvasR.top + AIF_Y + AIF_N);
    SetRect(&gStatusR, gOX, gPatR.bottom + 1, gOX + APP_W, b.bottom);
    dbg.canvas = gCanvasR;
    dbg.aiFrame = gAIFrameR;
}

static void SetupMenus(void)
{
    mApple = NewMenu(M_APPLE, "\p\x14");
    AppendMenu(mApple, "\pAbout Doodle\xC9;(-");
    AppendResMenu(mApple, 'DRVR');      /* desk accessories (System 7: Apple Menu Items, Scrapbook...) */
    InsertMenu(mApple, 0);
    mFile = NewMenu(M_FILE, "\pFile");
    AppendMenu(mFile, "\pNew/N;Open\xC9/O;Save/S;Save As\xC9;(-;Quit/Q");
    InsertMenu(mFile, 0);
    mEdit = NewMenu(M_EDIT, "\pEdit");
    AppendMenu(mEdit, "\pUndo/Z;(-;Cut/X;Copy/C;Paste/V;Clear;(-;Clear Canvas");
    InsertMenu(mEdit, 0);
    mAI = NewMenu(M_AI, "\pFinish");
    AppendMenu(mAI, "\pFinish It\xC9/F;(-;Tidy;Normal;Wild");
    SetMenuItemText(mAI, I_TIDY, "\pTidy (0.3)");        /* "(" in AppendMenu would disable the item */
    SetMenuItemText(mAI, I_NORMAL, "\pNormal (0.5)");
    SetMenuItemText(mAI, I_WILD, "\pWild (0.8)");
    InsertMenu(mAI, 0);
    DrawMenuBar();
}

void SetTemperatureMenu(short which)
{
    short i;
    for (i = 0; i < 3; i++)
        CheckItem(mAI, I_TIDY + i, i == which);
}

static void About(void)
{
    WindowPtr w;
    Rect r;
    EventRecord ev;
    static const unsigned char *const lines[] = {
        "\pDoodle",
        "\pA paint program that finishes your drawing.",
        "\p",
        "\p\"Finish it\" uses a sketch-rnn style drawing model",
        "\p(Ha & Eck, 2017, \"A Neural Representation of",
        "\pSketch Drawings\"), trained on Google's Quick, Draw!",
        "\pdataset, which is licensed CC BY 4.0.",
        "\p",
        "\pClick or press a key to continue.",
    };
    short i, n = sizeof lines / sizeof lines[0];
    SetRect(&r, gOX + 32, 70, gOX + APP_W - 32, 70 + 24 + n * 13);
    w = NewWindow(NULL, &r, "\p", true, dBoxProc, (WindowPtr)-1, false, 0);
    SetPort(w);
    for (i = 0; i < n; i++) {
        TextFont(i == 0 ? 0 : 3);
        TextSize(i == 0 ? 0 : 9);
        MoveTo(14, 20 + i * 13);
        DrawString(lines[i]);
    }
    dbg.cmdResult = 0xAB;          /* About box shown (for the tests) */
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
        EndText();
        if (item == I_NEW) FileNew();
        else if (item == I_OPEN) FileOpen();
        else if (item == I_SAVE) FileSave(false);
        else if (item == I_SAVEAS) FileSave(true);
        else if (item == I_QUIT) ExitToShell();
        break;
    case M_EDIT:
        if (item <= I_CLEARSEL && SystemEdit(item - 1))   /* a desk accessory in front (System 6) */
            break;
        if (item == I_UNDO) { EndText(); DoUndo(); }
        else if (item == I_CUT) CopySelection(true);
        else if (item == I_COPY) CopySelection(false);
        else if (item == I_PASTE) PasteScrap();
        else if (item == I_CLEARSEL) ClearSelection();
        else if (item == I_CLEARALL) { EndText(); ClearCanvas(); }
        break;
    case M_AI:
        if (item == I_FINISH) { EndText(); Deselect(); FinishIt(-1, 0, true); }
        else if (item >= I_TIDY) { SetTemperature(item - I_TIDY); SetTemperatureMenu(item - I_TIDY); }
        break;
    }
    HiliteMenu(0);
}

static void Mailbox(void)
{
    unsigned long c = dbg.cmd;
    if (!c)
        return;
    dbg.cmd = 0;
    dbg.cmdResult = 0;
    switch (c) {
    case 1:     /* Finish it without the picker: arg0 category, arg1 seed, arg2 temperature (0..2) */
        SetTemperature((short)dbg.arg[2]);
        SetTemperatureMenu((short)dbg.arg[2]);
        FinishIt((short)dbg.arg[0], dbg.arg[1], false);
        break;
    case 2:     /* seed for the next Finish it started from the UI (0 = from the clock) */
        dbg.fiSeed = dbg.arg[0];
        break;
    case 3:     /* save the page as arg-named file "\pTest<arg0>" on the boot volume */
    {
        unsigned char name[8] = "\pTest0";
        name[5] = (unsigned char)('0' + (dbg.arg[0] % 10));
        dbg.cmdResult = (unsigned long)WritePaintFile(name, 0);
        break;
    }
    case 6:     /* empty the system scrap */
        ZeroScrap();
        break;
    case 7:     /* length of the 'PICT' on the system scrap (0 if none) */
    {
        long off, len = GetScrap(NULL, 'PICT', &off);
        dbg.cmdResult = (unsigned long)(len > 0 ? len : 0);
        break;
    }
    case 4:     /* open "\pTest<arg0>" */
    {
        unsigned char name[8] = "\pTest0";
        name[5] = (unsigned char)('0' + (dbg.arg[0] % 10));
        dbg.cmdResult = (unsigned long)ReadPaintFile(name, 0);
        break;
    }
    }
    dbg.cmdDone++;
}

int main(void)
{
    EventRecord ev;
    WindowPtr w;
    Rect r;
    short part;

    dbg.magic = DBG_MAGIC;
    dbg.version = 1;
    dbg.notMagic = ~DBG_MAGIC;

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
    r = qd.screenBits.bounds;           /* covers the screen: local = global coordinates */
    gWin = NewWindow(NULL, &r, "\p", true, plainDBox, (WindowPtr)-1, false, 0);
    SetPort(gWin);
    if (!PaintInit()) {
        SysBeep(10);
        ExitToShell();
    }
    FinishInit();
    SetTemperatureMenu(gTemp);
    DrawPalettes();
    DrawCanvas();
    dbg.freeMem = FreeMem();
    dbg.state = 1;

    for (;;) {
        dbg.loopTicks = TickCount();
        SystemTask();
        Mailbox();
        if (!GetNextEvent(everyEvent, &ev)) {
            Point p;
            SetPort(gWin);
            GetMouse(&p);
            SetToolCursor(p);
            PaintIdle();
            continue;
        }
        switch (ev.what) {
        case mouseDown:
            part = FindWindow(ev.where, &w);
            if (part == inMenuBar) {
                DoMenu(MenuSelect(ev.where));
            } else if (part == inSysWindow) {
                SystemClick(&ev, w);
            } else if (w == gWin) {
                SetPort(gWin);
                if (PtInRect(ev.where, &gCanvasR))
                    TrackCanvas(ev.where, ev.modifiers);
                else
                    ClickPalettes(ev.where, ev.when);
            }
            break;
        case keyDown:
        case autoKey:
        {
            char c = (char)(ev.message & charCodeMask);
            if (ev.modifiers & cmdKey) {
                if (ev.what == keyDown)
                    DoMenu(MenuKey(c));
            } else {
                TypeKey(c, ev.modifiers);
            }
            break;
        }
        case updateEvt:
            w = (WindowPtr)ev.message;
            BeginUpdate(w);
            if (w == gWin) {
                DrawPalettes();
                DrawCanvas();
            }
            EndUpdate(w);
            break;
        default:
            break;
        }
    }
    return 0;
}
