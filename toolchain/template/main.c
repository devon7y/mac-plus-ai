/*
 * Hello, Plus: the shared Retro68 test app (and the template for new apps).
 *
 * Draws a test pattern centred in QuickDraw's screenBits.bounds (Narrow Screen
 * insets the Plus's 512 px screen, so never assume 512): a 320x240 frame, a
 * crosshair through its centre, and the text "Hello, Plus" in a box at the exact
 * centre. Then it idles in an event loop; each mouse click inverts the text box.
 *
 * It also fills a debug block in its globals that a Lua script in MAME finds by
 * scanning RAM for the magic tag 'HPLS' (see check.lua). All fields big-endian.
 */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <OSUtils.h>
#include <MacMemory.h>

#define FRAME_W 320
#define FRAME_H 240

/* Debug block. Layout is fixed (check.lua reads it by offset). */
struct DebugBlock {
    unsigned long magic;      /*  0: 'HPLS' */
    unsigned long version;    /*  4: 1 */
    unsigned long notMagic;   /*  8: ~'HPLS', guards against false matches in code */
    unsigned long state;      /* 12: 0 started, 1 pattern drawn, 2+ = 1 + number of clicks */
    Rect bounds;              /* 16: qd.screenBits.bounds (top, left, bottom, right) */
    unsigned long baseAddr;   /* 24: qd.screenBits.baseAddr */
    short rowBytes;           /* 28: qd.screenBits.rowBytes */
    short pad;                /* 30 */
    Rect frame;               /* 32: the frame we drew, global coordinates */
    short textWidth;          /* 40: StringWidth("Hello, Plus") */
    short ascent;             /* 42: font ascent */
    unsigned long drawTicks;  /* 44: TickCount() when drawing finished */
    unsigned long a5;         /* 48: A5 (app globals) */
    unsigned long freeMem;    /* 52: FreeMem() after setup */
    Point lastClick;          /* 56: last click (v, h), global coordinates */
};

static struct DebugBlock dbg;   /* note: globals are only word-aligned at run time */
static Rect textBox;          /* the box around the text; clicks invert it */

#define MAGIC 0x48504C53UL    /* 'HPLS' */

static void drawPattern(WindowPtr win)
{
    Rect b = qd.screenBits.bounds;
    short cx = (short)((b.left + b.right) / 2);
    short cy = (short)((b.top + b.bottom) / 2);
    Rect frame;
    FontInfo fi;
    short w;
    static const unsigned char msg[] = "\pHello, Plus";

    SetPort(win);
    EraseRect(&win->portRect);
    /* Window is placed at the screen's top-left, so local = global coordinates. */
    SetRect(&frame, cx - FRAME_W / 2, cy - FRAME_H / 2, cx + FRAME_W / 2, cy + FRAME_H / 2);
    PenNormal();
    FrameRect(&frame);
    /* crosshair */
    MoveTo(frame.left, cy);
    LineTo(frame.right - 1, cy);
    MoveTo(cx, frame.top);
    LineTo(cx, frame.bottom - 1);
    /* corner ticks 8 px inside the frame make clipping easy to spot */
    MoveTo(frame.left + 8, frame.top + 8);   LineTo(frame.left + 24, frame.top + 8);
    MoveTo(frame.right - 9, frame.bottom - 9); LineTo(frame.right - 25, frame.bottom - 9);

    TextFont(0);              /* system font (Chicago 12, in ROM) */
    TextSize(0);
    GetFontInfo(&fi);
    w = StringWidth(msg);
    SetRect(&textBox, cx - w / 2 - 6, cy - fi.ascent / 2 - 6,
            cx - w / 2 + w + 6, cy + fi.ascent / 2 + fi.descent + 5);
    EraseRect(&textBox);
    FrameRect(&textBox);
    MoveTo(cx - w / 2, cy + fi.ascent / 2);
    DrawString(msg);

    dbg.frame = frame;
    dbg.textWidth = w;
    dbg.ascent = fi.ascent;
}

int main(void)
{
    WindowPtr win;
    EventRecord ev;
    Rect r;

    dbg.magic = MAGIC;
    dbg.version = 1;
    dbg.notMagic = ~MAGIC;
    dbg.state = 0;

    MaxApplZone();
    MoreMasters();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);

    dbg.bounds = qd.screenBits.bounds;
    dbg.baseAddr = (unsigned long)qd.screenBits.baseAddr;
    dbg.rowBytes = qd.screenBits.rowBytes;
    dbg.a5 = (unsigned long)SetCurrentA5();

    /* A borderless window covering the whole (possibly narrowed) screen. */
    r = qd.screenBits.bounds;
    win = NewWindow(NULL, &r, "\p", true, plainDBox, (WindowPtr)-1, false, 0);
    drawPattern(win);

    dbg.freeMem = FreeMem();
    dbg.drawTicks = TickCount();
    dbg.state = 1;

    for (;;) {
        SystemTask();
        if (!GetNextEvent(everyEvent, &ev))
            continue;
        switch (ev.what) {
        case mouseDown:
            dbg.lastClick = ev.where;
            dbg.state++;
            SetPort(win);
            InvertRect(&textBox);       /* visible feedback */
            break;
        case updateEvt:
            BeginUpdate((WindowPtr)ev.message);
            drawPattern((WindowPtr)ev.message);
            EndUpdate((WindowPtr)ev.message);
            break;
        default:
            break;
        }
    }
    return 0;
}
