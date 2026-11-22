/*
 * Scribble Calc: layout and drawing. The paper lives in an offscreen bitmap (guides, ink and the
 * Mac's handwritten answer) copied to the screen; the "read as" strip, buttons, status line, history
 * tape and the correction popup are drawn straight into the window with QuickDraw.
 */
#include "calc.h"
#include "hand_gen.h"

Rect gPaperR, gStripR, gTapeR, gStatusR, gBtnR[4];
char gStatusMsg[64];

static BitMap gPaperBM;
static GrafPort gPaperPort;
static short gPaperW, gPaperH;
#define GUIDE_TOP 40
#define GUIDE_BASE 104
#define TAPE_LINE 16

static const char *const kBtnName[4] = { "Pen", "Eraser", "Undo", "Clear" };

void Layout(void)
{
    Rect b = qd.screenBits.bounds;
    short L = (short)(b.left + 8), R = (short)(b.right - 8), x, i;
    static const short bw[4] = { 46, 60, 52, 52 };
    SetRect(&gPaperR, L, 26, R, 166);
    SetRect(&gStripR, L, 169, R, 189);
    x = L;
    for (i = 0; i < 4; i++) {
        SetRect(&gBtnR[i], x, 195, (short)(x + bw[i]), 215);
        x = (short)(x + bw[i] + 6);
    }
    SetRect(&gStatusR, (short)(x + 6), 195, R, 215);
    SetRect(&gTapeR, L, 222, R, (short)(b.bottom - 6));
    gPaperW = (short)(gPaperR.right - gPaperR.left);
    gPaperH = (short)(gPaperR.bottom - gPaperR.top);
    dbg.paper = gPaperR;
    dbg.strip = gStripR;
}

void DrawInit(void)
{
    short rb = (short)(((gPaperW + 15) / 16) * 2);
    gPaperBM.baseAddr = NewPtrClear((long)rb * gPaperH);
    gPaperBM.rowBytes = rb;
    SetRect(&gPaperBM.bounds, 0, 0, gPaperW, gPaperH);
    OpenPort(&gPaperPort);
    SetPortBits(&gPaperBM);
    gPaperPort.portRect = gPaperBM.bounds;
    RectRgn(gPaperPort.visRgn, &gPaperBM.bounds);
    ClipRect(&gPaperBM.bounds);
    SetPort(gWin);
}

/* ------------------------------------------------------------------ text helpers */
static void CStr(const char *s, short x, short y)
{
    MoveTo(x, y);
    while (*s)
        DrawChar(*s++);
}

static short CStrWidth(const char *s)
{
    short w = 0;
    while (*s)
        w = (short)(w + CharWidth(*s++));
    return w;
}

void SetStatus(const char *msg)
{
    short i;
    for (i = 0; i < 63 && msg[i]; i++)
        gStatusMsg[i] = msg[i];
    gStatusMsg[i] = 0;
    DrawStatus();
}

void DrawStatus(void)
{
    GrafPtr old;
    GetPort(&old);
    SetPort(gWin);
    EraseRect(&gStatusR);
    TextFont(3);                  /* Geneva 9 (ROM) */
    TextSize(9);
    CStr(gStatusMsg, gStatusR.left, (short)(gStatusR.bottom - 6));
    TextFont(0);
    TextSize(0);
    SetPort(old);
}

/* ------------------------------------------------------------------ the clean font (Chicago + drawn operators) */
short DrawToken(short tok, short x, short base, Boolean draw)
{
    char c = 0;
    short w;
    if (tok >= 0 && tok <= 9)
        c = (char)('0' + tok);
    else if (tok == T_PLUS) c = '+';
    else if (tok == T_EQ) c = '=';
    else if (tok == T_LPAR) c = '(';
    else if (tok == T_RPAR) c = ')';
    else if (tok == T_POINT) c = '.';
    if (c) {
        w = CharWidth(c);
        if (draw) {
            MoveTo(x, base);
            DrawChar(c);
        }
        return w;
    }
    w = 10;
    if (!draw)
        return w;
    PenSize(1, 1);
    switch (tok) {
    case T_MINUS:
        MoveTo((short)(x + 1), (short)(base - 4));
        LineTo((short)(x + 7), (short)(base - 4));
        break;
    case T_TIMES:
        MoveTo((short)(x + 1), (short)(base - 7));
        LineTo((short)(x + 7), (short)(base - 1));
        MoveTo((short)(x + 7), (short)(base - 7));
        LineTo((short)(x + 1), (short)(base - 1));
        break;
    case T_DIV:
        MoveTo((short)(x + 1), (short)(base - 4));
        LineTo((short)(x + 7), (short)(base - 4));
        MoveTo((short)(x + 4), (short)(base - 7));
        LineTo((short)(x + 4), (short)(base - 7));
        MoveTo((short)(x + 4), (short)(base - 1));
        LineTo((short)(x + 4), (short)(base - 1));
        break;
    default:                                     /* not read yet */
        MoveTo((short)(x + 2), (short)(base - 1));
        LineTo((short)(x + 2), (short)(base - 1));
        MoveTo((short)(x + 5), (short)(base - 1));
        LineTo((short)(x + 5), (short)(base - 1));
        MoveTo((short)(x + 8), (short)(base - 1));
        LineTo((short)(x + 8), (short)(base - 1));
        break;
    }
    return w;
}

static void Small(Boolean on)
{
    TextFont(on ? 3 : 0);                        /* Geneva 9 for exponents, else Chicago 12 */
    TextSize(on ? 9 : 12);
}

/* draws a token list; T_POW ( ... ) is drawn as a raised, smaller exponent */
short DrawTokens(const signed char *tok, short n, short x, short base, Boolean draw)
{
    short i, x0 = x, up = 0;
    for (i = 0; i < n; i++) {
        short t = tok[i];
        if (t == T_POW && i + 1 < n && tok[i + 1] == T_LPAR) {
            up = 1;
            i++;
            Small(true);
            continue;
        }
        if (up && t == T_RPAR) {
            up = 0;
            Small(false);
            continue;
        }
        if (!up && i && ((t >= T_PLUS && t <= T_EQ) || (tok[i - 1] >= T_PLUS && tok[i - 1] <= T_EQ)))
            x = (short)(x + 3);
        x = (short)(x + DrawToken(t, x, (short)(up ? base - 6 : base), draw));
    }
    if (up)
        Small(false);
    return (short)(x - x0);
}

static short DrawAnswerText(const char *t, short sci, short exp10, short x, short base, Boolean draw)
{
    short x0 = x, i;
    char e[8];
    for (i = 0; t[i]; i++) {
        if (t[i] == '-') {
            x = (short)(x + DrawToken(T_MINUS, x, base, draw));
        } else {
            if (draw) {
                MoveTo(x, base);
                DrawChar(t[i]);
            }
            x = (short)(x + CharWidth(t[i]));
        }
    }
    if (sci) {
        short v = exp10, n = 0, k;
        x = (short)(x + 2 + DrawToken(T_TIMES, (short)(x + 2), base, draw));
        if (draw) {
            MoveTo(x, base);
            DrawChar('1');
            DrawChar('0');
        }
        x = (short)(x + CharWidth('1') + CharWidth('0'));
        if (v < 0) {
            e[n++] = '-';
            v = (short)-v;
        }
        {
            char tmp[6];
            short m = 0;
            do {
                tmp[m++] = (char)('0' + v % 10);
                v = (short)(v / 10);
            } while (v);
            while (m)
                e[n++] = tmp[--m];
        }
        TextSize(9);
        TextFont(3);
        for (k = 0; k < n; k++) {
            if (draw) {
                MoveTo(x, (short)(base - 6));
                DrawChar(e[k]);
            }
            x = (short)(x + CharWidth(e[k]));
        }
        TextFont(0);
        TextSize(12);
    }
    return (short)(x - x0);
}

/* ------------------------------------------------------------------ the paper */
static void Guides(void)
{
    short r, i, rb = gPaperBM.rowBytes;
    static const short rows[2] = { GUIDE_TOP, GUIDE_BASE };
    for (r = 0; r < 2; r++) {
        unsigned char *p = (unsigned char *)gPaperBM.baseAddr + (long)rows[r] * rb;
        for (i = 0; i < rb; i++)
            p[i] = 0x88;
    }
}

static short HandIndex(char c)
{
    short i;
    for (i = 0; i < HAND_N; i++)
        if (handChars[i] == c)
            return i;
    return -1;
}

/* width of a glyph string at height h (paper pixels) */
static short HandWidth(const char *t, short h)
{
    long w = 0;
    short i, k;
    for (i = 0; t[i]; i++)
        if ((k = HandIndex(t[i])) >= 0)
            w += handAdv[k];
    return (short)(w * h / 100);
}

static short HandDraw(const char *t, short x, short base, short h, unsigned short seed)
{
    short i, k;
    for (i = 0; t[i]; i++) {
        const unsigned char *d;
        short ns, s;
        short dy, hh;
        if ((k = HandIndex(t[i])) < 0)
            continue;
        seed = (unsigned short)(seed * 25173u + 13849u);
        dy = (short)((short)((seed >> 8) % 5) - 2) * h / 64;      /* a little wobble per glyph */
        hh = (short)(h + ((short)((seed >> 4) % 7) - 3) * h / 60);
        d = handData + handOff[k];
        ns = *d++;
        for (s = 0; s < ns; s++) {
            short np = *d++, j;
            for (j = 0; j < np; j++) {
                short px = (short)(x + (long)d[2 * j] * hh / 100);
                short py = (short)(base + dy + ((long)d[2 * j + 1] - 100) * hh / 100);
                if (j == 0)
                    MoveTo(px, py);
                LineTo(px, py);
            }
            d += 2 * np;
        }
        x = (short)(x + (long)handAdv[k] * h / 100);
    }
    return x;
}

static void DrawHandAnswer(void)
{
    const Group *g;
    short h, x, base, w, sh = 0, room;
    char t[DEC_BUFLEN + 4], e[8];
    short i, n = 0;
    unsigned short seed = 1;
    if (gEqAt < 0 || gErr != DEC_OK)
        return;
    g = &gGrp[gEqAt];
    for (i = 0; gAnsText[i]; i++) {
        t[n++] = gAnsText[i];
        seed = (unsigned short)(seed * 31 + gAnsText[i]);
    }
    if (gAnsSci) {
        short v = gAnsExp, m = 0;
        char tmp[6];
        t[n++] = 'x';
        t[n++] = '1';
        t[n++] = '0';
        if (v < 0) {
            e[m++] = '-';
            v = (short)-v;
        }
        do {
            tmp[sh++] = (char)('0' + v % 10);
            v = (short)(v / 10);
        } while (v);
        while (sh)
            e[m++] = tmp[--sh];
        e[m] = 0;
        sh = 1;
    }
    t[n] = 0;
    {
        /* size and baseline from the digits the Mac read (medians), else from the '=' */
        short hs[MAXGRP], bs[MAXGRP], nd = 0, a, b2, v;
        for (i = 0; i < gEqAt; i++)
            if (gTok[i] >= 0 && gTok[i] <= 9) {
                hs[nd] = (short)(gGrp[i].y1 - gGrp[i].y0);
                bs[nd] = gGrp[i].y1;
                nd++;
            }
        for (a = 1; a < nd; a++) {                       /* insertion sorts */
            v = hs[a];
            for (b2 = a; b2 > 0 && hs[b2 - 1] > v; b2--) hs[b2] = hs[b2 - 1];
            hs[b2] = v;
            v = bs[a];
            for (b2 = a; b2 > 0 && bs[b2 - 1] > v; b2--) bs[b2] = bs[b2 - 1];
            bs[b2] = v;
        }
        h = nd ? hs[nd / 2] : (short)(gHref * 9 / 10);
        if (h < 14) h = 14;
        if (h > 90) h = 90;
        base = nd ? bs[nd / 2] : (short)((g->y0 + g->y1) / 2 + h / 2);
    }
    x = (short)(g->x1 + (h / 4 > 6 ? h / 4 : 6));
    room = (short)(gPaperW - 6 - x);
    w = (short)(HandWidth(t, h) + (sh ? HandWidth(e, (short)(h / 2)) : 0));
    if (w > room && w > 0) {
        h = (short)((long)h * room / w);
        if (h < 12)
            h = 12;
    }
    if (base > gPaperH - 4)
        base = (short)(gPaperH - 4);
    if (base - h < 2)
        base = (short)(h + 2);
    PenSize(h >= 28 ? 2 : 1, h >= 28 ? 2 : 1);
    x = HandDraw(t, x, base, h, seed);
    if (sh)
        HandDraw(e, (short)(x + 2), (short)(base - h / 2), (short)(h / 2), seed);
}

void PaperRender(void)
{
    short i, j;
    GrafPtr old;
    GetPort(&old);
    SetPort(&gPaperPort);
    EraseRect(&gPaperBM.bounds);
    Guides();
    PenSize(2, 2);
    for (i = 0; i < gNStk; i++) {
        const Stroke *s = &gStk[i];
        const Pt *p = gPts + s->first;
        if (!s->alive)
            continue;
        MoveTo(p[0].x, p[0].y);
        LineTo(p[0].x, p[0].y);
        for (j = 1; j < s->n; j++)
            LineTo(p[j].x, p[j].y);
    }
    DrawHandAnswer();
    PenSize(1, 1);
    SetPort(old);
}

void PaperShow(const Rect *r)
{
    Rect src = gPaperBM.bounds, dst = gPaperR;
    GrafPtr old;
    if (r) {
        src = *r;
        dst = *r;
        OffsetRect(&dst, gPaperR.left, gPaperR.top);
    }
    GetPort(&old);
    SetPort(gWin);
    CopyBits(&gPaperBM, &gWin->portBits, &src, &dst, srcCopy, NULL);
    SetPort(old);
}

void PaperStrokeSegment(Pt a, Pt b)
{
    GrafPtr old;
    GetPort(&old);
    SetPort(&gPaperPort);
    PenSize(2, 2);
    MoveTo(a.x, a.y);
    LineTo(b.x, b.y);
    PenSize(1, 1);
    SetPort(gWin);
    PenSize(2, 2);
    MoveTo((short)(a.x + gPaperR.left), (short)(a.y + gPaperR.top));
    LineTo((short)(b.x + gPaperR.left), (short)(b.y + gPaperR.top));
    PenSize(1, 1);
    SetPort(old);
}

/* ------------------------------------------------------------------ strip, buttons, tape */
void DrawStrip(void)
{
    short i, base = (short)(gStripR.bottom - 5), lastx = -1;
    GrafPtr old;
    GetPort(&old);
    SetPort(gWin);
    EraseRect(&gStripR);
    TextFont(0);
    TextSize(12);
    for (i = 0; i < gNGrp; i++) {
        const Group *g = &gGrp[i];
        short tok = (short)(g->ready && i < gNTok && gNTok == gNGrp ? gTok[i] : -1);
        short cx = (short)(gPaperR.left + (g->x0 + g->x1) / 2), w, x, up = (short)(tok >= 0 && g->raised);
        Small(up);
        w = DrawToken(tok, 0, 0, false);
        x = (short)(cx - w / 2);
        DrawToken(tok, x, (short)(up ? base - 6 : base), true);
        Small(false);
        if (g->forced != NOFORCE) {               /* a correction: underlined */
            MoveTo(x, (short)(base + 2));
            LineTo((short)(x + w - 1), (short)(base + 2));
        }
        if (i == gEqAt)
            lastx = (short)(x + w + 6);
    }
    if (lastx >= 0 && gNTok == gNGrp) {
        if (gErr == DEC_OK) {
            TextFace(bold);
            DrawAnswerText(gAnsText, gAnsSci, gAnsExp, lastx, base, true);
            TextFace(0);
        } else if (gErr > 0) {
            TextFont(3);
            TextSize(9);
            CStr(gErr == DEC_DIV0 ? "Can't divide by zero." : "?", lastx, base);
            TextFont(0);
            TextSize(12);
        }
    }
    SetPort(old);
}

void DrawButtons(void)
{
    short i;
    GrafPtr old;
    GetPort(&old);
    SetPort(gWin);
    TextFont(0);
    TextSize(12);
    for (i = 0; i < 4; i++) {
        Rect r = gBtnR[i];
        EraseRoundRect(&r, 10, 10);
        FrameRoundRect(&r, 10, 10);
        CStr(kBtnName[i], (short)((r.left + r.right - CStrWidth(kBtnName[i])) / 2), (short)(r.bottom - 6));
        if ((i == B_PEN && gTool == 0) || (i == B_ERASER && gTool == 1)) {
            InsetRect(&r, 1, 1);
            InvertRoundRect(&r, 8, 8);
        }
    }
    SetPort(old);
}

void DrawTape(void)
{
    Rect r = gTapeR, in;
    short i, y;
    GrafPtr old;
    RgnHandle save = NewRgn();
    GetPort(&old);
    SetPort(gWin);
    EraseRect(&r);
    FrameRect(&r);
    MoveTo(r.left, (short)(r.top + 15));
    LineTo((short)(r.right - 1), (short)(r.top + 15));
    TextFont(3);
    TextSize(9);
    CStr("Tape (click a line to copy its answer)", (short)(r.left + 6), (short)(r.top + 11));
    TextFont(0);
    TextSize(12);
    in = r;
    InsetRect(&in, 1, 1);
    in.top = (short)(r.top + 16);
    GetClip(save);
    ClipRect(&in);
    y = (short)(r.top + 16 + 13);
    for (i = 0; i < gTapeN && y < r.bottom + 12; i++, y = (short)(y + TAPE_LINE)) {
        const TapeEntry *e = &gTape[i];
        short x = (short)(r.left + 8);
        x = (short)(x + DrawTokens(e->tok, e->nTok, x, y, true) + 6);
        if (e->err == DEC_OK) {
            DrawAnswerText(e->result, e->sci, e->exp10, x, y, true);
        } else {
            TextFont(3);
            TextSize(9);
            CStr(e->err == DEC_DIV0 ? "Can't divide by zero." : "?", x, y);
            TextFont(0);
            TextSize(12);
        }
    }
    SetClip(save);
    DisposeRgn(save);
    SetPort(old);
}

void DrawAll(void)
{
    Rect r;
    SetPort(gWin);
    r = gWin->portRect;
    r.top = 20;
    EraseRect(&r);
    r = gPaperR;
    InsetRect(&r, -1, -1);
    FrameRect(&r);
    MoveTo((short)(r.left + 2), r.bottom);              /* drop shadow */
    LineTo(r.right, r.bottom);
    LineTo(r.right, (short)(r.top + 2));
    PaperShow(NULL);
    DrawStrip();
    DrawButtons();
    DrawStatus();
    DrawTape();
}

/* ------------------------------------------------------------------ the correction popup */
#define PC 7
#define PW 26
#define PH 20

int ChoosePopup(short gi, Point where)
{
    const Group *g = &gGrp[gi];
    short order[NCLS], i, j, n = NCLS, cur = gTok[gi];
    Rect box, cell, autoR;
    EventRecord ev;
    Rect sb = qd.screenBits.bounds;
    for (i = 0; i < n; i++)
        order[i] = i;
    for (i = 1; i < n; i++) {                     /* by score, best first */
        short v = order[i];
        for (j = i; j > 0 && g->sc[order[j - 1]] < g->sc[v]; j--)
            order[j] = order[j - 1];
        order[j] = v;
    }
    box.top = (short)(gStripR.bottom + 2);
    box.left = (short)(where.h - PC * PW / 2);
    if (box.left < sb.left + 4) box.left = (short)(sb.left + 4);
    if (box.left + PC * PW + 8 > sb.right - 4) box.left = (short)(sb.right - 4 - PC * PW - 8);
    box.right = (short)(box.left + PC * PW + 8);
    box.bottom = (short)(box.top + 3 * PH + 8 + 16);
    SetPort(gWin);
    while (StillDown())
        ;
    EraseRect(&box);
    FrameRect(&box);
    MoveTo((short)(box.left + 2), box.bottom);
    LineTo(box.right, box.bottom);
    LineTo(box.right, (short)(box.top + 2));
    TextFont(0);
    TextSize(12);
    for (i = 0; i < NCLS; i++) {
        short c = order[i], w = DrawToken(c, 0, 0, false);
        SetRect(&cell, (short)(box.left + 4 + (i % PC) * PW), (short)(box.top + 4 + (i / PC) * PH),
                (short)(box.left + 4 + (i % PC + 1) * PW), (short)(box.top + 4 + (i / PC + 1) * PH));
        DrawToken(c, (short)((cell.left + cell.right - w) / 2), (short)(cell.bottom - 5), true);
        if (c == cur)
            InvertRect(&cell);
    }
    dbg.popTop = box.top;
    dbg.popLeft = box.left;
    for (i = 0; i < NCLS; i++)
        dbg.popOrder[i] = (unsigned char)order[i];
    SetRect(&autoR, (short)(box.left + 4 + 4 * PW), (short)(box.top + 4 + 2 * PH),
            (short)(box.left + 4 + 7 * PW), (short)(box.top + 4 + 3 * PH));
    FrameRect(&autoR);
    TextFont(3);
    TextSize(9);
    CStr("Automatic", (short)(autoR.left + 9), (short)(autoR.bottom - 6));
    CStr("Best guesses first. Pick one, or type it.", (short)(box.left + 6), (short)(box.bottom - 5));
    TextFont(0);
    TextSize(12);
    for (;;) {
        SystemTask();
        if (!GetNextEvent(mDownMask | keyDownMask, &ev))
            continue;
        dbg.popTop = dbg.popLeft = 0;
        if (ev.what == keyDown) {
            char c = (char)(ev.message & charCodeMask);
            if (c >= '0' && c <= '9') return c - '0';
            switch (c) {
            case '+': return T_PLUS;
            case '-': return T_MINUS;
            case '*': case 'x': case 'X': return T_TIMES;
            case '/': case ':': return T_DIV;
            case '=': return T_EQ;
            case '(': return T_LPAR;
            case ')': return T_RPAR;
            case '.': case ',': return T_POINT;
            }
            return -1;
        }
        GlobalToLocal(&ev.where);
        if (PtInRect(ev.where, &autoR))
            return -2;
        for (i = 0; i < NCLS; i++) {
            SetRect(&cell, (short)(box.left + 4 + (i % PC) * PW), (short)(box.top + 4 + (i / PC) * PH),
                    (short)(box.left + 4 + (i % PC + 1) * PW), (short)(box.top + 4 + (i / PC + 1) * PH));
            if (PtInRect(ev.where, &cell))
                return order[i];
        }
        return -1;
    }
}
