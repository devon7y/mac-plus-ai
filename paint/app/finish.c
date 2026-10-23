/*
 * "Finish it": turn the pencil/brush strokes drawn inside the AI frame into the model's
 * prompt (per stroke: drop repeated points, drop single points, Ramer-Douglas-Peucker with
 * epsilon 2 in exact integers), ask what it is (all categories, type to search), prime the
 * decoder with the prompt, then sample and draw the completion move by move.
 * paint/bench/ref.py (prompt_rows, finish_it) is the bit-exact reference.
 */
#include "doodle.h"
#include "model.h"

short gTemp = 1;

#define MAX_ROWS 250
#define MAX_GEN 200
static short gRows[MAX_ROWS * 3];     /* prompt rows fed: dx, dy, pen (0 down, 1 lifted) */
static short gGen[MAX_GEN * 3];       /* generated rows */
static short gStartX, gStartY;

void FinishInit(void)
{
    dbg.fiRows = (unsigned long)gGen;
    dbg.fiPrompt = (unsigned long)gRows;
}

void SetTemperature(short which)
{
    if (which < 0 || which > 2)
        which = 1;
    gTemp = which;
}

/* ------------------------------------------------------------------ prompt */
static char keepF[MAXPTS];
static short stackS[MAXPTS], stackE[MAXPTS];

static void RDP(const short *xs, const short *ys, short n, char *keep)
{
    short sp = 0, s, e, i, bi;
    long dx, dy, L2, v, best, t1, t2;
    long long cr, bestc;
    for (i = 0; i < n; i++)
        keep[i] = n < 3;
    if (n < 3)
        return;
    keep[0] = keep[n - 1] = 1;
    stackS[sp] = 0;
    stackE[sp++] = n - 1;
    while (sp > 0) {
        sp--;
        s = stackS[sp];
        e = stackE[sp];
        if (e <= s + 1)
            continue;
        dx = xs[e] - xs[s];
        dy = ys[e] - ys[s];
        L2 = dx * dx + dy * dy;
        bi = -1;
        if (L2 == 0) {
            best = -1;
            for (i = s + 1; i < e; i++) {
                t1 = xs[i] - xs[s];
                t2 = ys[i] - ys[s];
                v = t1 * t1 + t2 * t2;
                if (v > best) {
                    best = v;
                    bi = i;
                }
            }
            if (best <= 4)
                continue;
        } else {
            bestc = -1;
            for (i = s + 1; i < e; i++) {
                cr = (long long)(dx * (ys[i] - ys[s]) - dy * (xs[i] - xs[s]));
                cr *= cr;
                if (cr > bestc) {
                    bestc = cr;
                    bi = i;
                }
            }
            if (bestc <= 4LL * L2)
                continue;
        }
        keep[bi] = 1;
        stackS[sp] = s;
        stackE[sp++] = bi;
        stackS[sp] = bi;
        stackE[sp++] = e;
    }
}

/* Points of one stroke (after dropping repeats), then RDP, appended to px/py/lift. */
static short sx[MAXPTS], sy[MAXPTS];
static short px[MAXPTS], py[MAXPTS], pl[MAXPTS];

static short BuildPrompt(void)
{
    short i, n = 0, ns = 0, k, nrows = 0;
    for (i = 0; i < gNPts; i++) {
        short x = gPts[i * 3], y = gPts[i * 3 + 1], lift = gPts[i * 3 + 2];
        if (ns == 0 || x != sx[ns - 1] || y != sy[ns - 1]) {
            sx[ns] = x;
            sy[ns] = y;
            ns++;
        }
        if (lift || i == gNPts - 1) {
            if (ns >= 2) {
                RDP(sx, sy, ns, keepF);
                for (k = 0; k < ns; k++)
                    if (keepF[k]) {
                        px[n] = sx[k];
                        py[n] = sy[k];
                        pl[n] = 0;
                        n++;
                    }
                pl[n - 1] = 1;
            }
            ns = 0;
        }
    }
    if (n < 2)
        return 0;
    gStartX = px[0];
    gStartY = py[0];
    for (i = 1; i < n && nrows < MAX_ROWS - 1; i++, nrows++) {
        gRows[nrows * 3] = px[i] - px[i - 1];
        gRows[nrows * 3 + 1] = py[i] - py[i - 1];
        gRows[nrows * 3 + 2] = pl[i];
    }
    return nrows;
}

/* ------------------------------------------------------------------ category picker */
#define PK_ROWS 12
#define PK_W 268
#define PK_H (PK_ROWS * 12 + 76)
static short pkMatch[400], pkN, pkSel, pkTop, pkQLen;
static unsigned char pkQuery[24];
static Rect pkQueryR, pkListR, pkUpR, pkDownR, pkOkR, pkCancelR;

static Boolean Contains(const unsigned char *name, const unsigned char *q, short ql)
{
    short i, j, n = name[0];
    for (i = 0; i + ql <= n; i++) {
        for (j = 0; j < ql; j++) {
            unsigned char c = name[1 + i + j];
            if (c >= 'A' && c <= 'Z')
                c += 32;
            if (c != q[j])
                break;
        }
        if (j == ql)
            return true;
    }
    return false;
}

static void PkFilter(void)
{
    short i;
    pkN = 0;
    for (i = 0; i < ModelNCat(); i++)
        if (Contains(ModelName(i), pkQuery, pkQLen))
            pkMatch[pkN++] = i;
    pkSel = 0;
    pkTop = 0;
}

static void PkDrawList(void)
{
    short r;
    Rect row;
    EraseRect(&pkListR);
    TextFont(0);
    TextSize(0);
    for (r = 0; r < PK_ROWS && pkTop + r < pkN; r++) {
        SetRect(&row, pkListR.left + 1, pkListR.top + 1 + r * 12, pkListR.right - 1, pkListR.top + 13 + r * 12);
        TextFont(3);
        TextSize(9);
        MoveTo(row.left + 4, row.bottom - 3);
        DrawString(ModelName(pkMatch[pkTop + r]));
        if (pkTop + r == pkSel)
            InvertRect(&row);
    }
    if (pkN == 0) {
        TextFont(3);
        TextSize(9);
        MoveTo(pkListR.left + 6, pkListR.top + 12);
        DrawString("\p(nothing matches)");
    }
    FrameRect(&pkListR);
    TextFont(0);
    TextSize(0);
}

static void PkDrawQuery(void)
{
    Rect r = pkQueryR;
    unsigned char s[26];
    short i;
    EraseRect(&r);
    FrameRect(&r);
    s[0] = (unsigned char)pkQLen;
    for (i = 0; i < pkQLen; i++)
        s[1 + i] = pkQuery[i];
    MoveTo(r.left + 4, r.bottom - 4);
    DrawString(s);
    MoveTo(r.left + 4 + StringWidth(s), r.top + 3);
    Line(0, r.bottom - r.top - 7);
}

static void PkButton(const Rect *r, const unsigned char *label, Boolean dflt)
{
    Rect b = *r;
    EraseRect(&b);
    FrameRoundRect(&b, 10, 10);
    if (dflt) {
        InsetRect(&b, -3, -3);
        PenSize(2, 2);
        FrameRoundRect(&b, 14, 14);
        PenNormal();
    }
    MoveTo(r->left + (r->right - r->left - StringWidth(label)) / 2, r->bottom - 5);
    DrawString(label);
}

static void PkDrawAll(void)
{
    EraseRect(&qd.thePort->portRect);
    MoveTo(10, 16);
    DrawString("\pWhat is it? Type to search:");
    PkDrawQuery();
    PkDrawList();
    FrameRect(&pkUpR);
    FrameRect(&pkDownR);
    {   /* small triangles */
        short x = (pkUpR.left + pkUpR.right) / 2, y = pkUpR.top + 5, k;
        for (k = 0; k < 5; k++) { MoveTo(x - k, y + k); LineTo(x + k, y + k); }
        y = pkDownR.bottom - 6;
        for (k = 0; k < 5; k++) { MoveTo(x - k, y - k); LineTo(x + k, y - k); }
    }
    PkButton(&pkCancelR, "\pCancel", false);
    PkButton(&pkOkR, "\pFinish it", true);
}

static void PkShowSel(void)
{
    if (pkSel < pkTop)
        pkTop = pkSel;
    if (pkSel >= pkTop + PK_ROWS)
        pkTop = pkSel - PK_ROWS + 1;
    PkDrawList();
}

static short PickCategory(void)
{
    WindowPtr w;
    Rect r;
    EventRecord ev;
    short result = -2, lastRow = -1;
    long lastClick = 0;
    SetRect(&r, gOX + (APP_W - PK_W) / 2, 50, gOX + (APP_W - PK_W) / 2 + PK_W, 50 + PK_H);
    w = NewWindow(NULL, &r, "\p", true, dBoxProc, (WindowPtr)-1, false, 0);
    SetPort(w);
    SetRect(&pkQueryR, 10, 22, PK_W - 10, 40);
    SetRect(&pkListR, 10, 46, PK_W - 30, 48 + PK_ROWS * 12);
    SetRect(&pkUpR, PK_W - 26, 46, PK_W - 10, 46 + 20);
    SetRect(&pkDownR, PK_W - 26, pkListR.bottom - 20, PK_W - 10, pkListR.bottom);
    SetRect(&pkCancelR, 30, PK_H - 24, 110, PK_H - 6);
    SetRect(&pkOkR, PK_W - 110, PK_H - 24, PK_W - 30, PK_H - 6);
    pkQLen = 0;
    PkFilter();
    PkDrawAll();
    SetCursor(&qd.arrow);
    while (result == -2) {
        SystemTask();
        if (!GetNextEvent(everyEvent, &ev))
            continue;
        SetPort(w);
        if (ev.what == keyDown || ev.what == autoKey) {
            unsigned char c = (unsigned char)(ev.message & charCodeMask);
            if (c == 13 || c == 3) {
                if (pkN > 0)
                    result = pkMatch[pkSel];
                else
                    SysBeep(5);
            } else if (c == 27 || ((ev.modifiers & cmdKey) && c == '.')) {
                result = -1;
            } else if (c == 8) {
                if (pkQLen > 0) {
                    pkQLen--;
                    PkFilter();
                    PkDrawQuery();
                    PkDrawList();
                }
            } else if (c == 0x1E) {
                if (pkSel > 0) { pkSel--; PkShowSel(); }
            } else if (c == 0x1F) {
                if (pkSel < pkN - 1) { pkSel++; PkShowSel(); }
            } else if (c >= 32 && c < 127 && pkQLen < 23 && !(ev.modifiers & cmdKey)) {
                if (c >= 'A' && c <= 'Z')
                    c += 32;
                pkQuery[pkQLen++] = c;
                PkFilter();
                PkDrawQuery();
                PkDrawList();
            }
        } else if (ev.what == mouseDown) {
            Point p = ev.where;
            GlobalToLocal(&p);
            if (PtInRect(p, &pkListR)) {
                short row = (p.v - pkListR.top - 1) / 12;
                if (pkTop + row < pkN && row >= 0 && row < PK_ROWS) {
                    if (pkTop + row == lastRow && ev.when - lastClick <= 30) {
                        result = pkMatch[pkTop + row];
                    } else {
                        pkSel = pkTop + row;
                        PkDrawList();
                    }
                    lastRow = pkTop + row;
                    lastClick = ev.when;
                }
            } else if (PtInRect(p, &pkUpR)) {
                pkTop -= PK_ROWS - 1;
                if (pkTop < 0) pkTop = 0;
                if (pkSel >= pkTop + PK_ROWS) pkSel = pkTop + PK_ROWS - 1;
                PkDrawList();
            } else if (PtInRect(p, &pkDownR)) {
                if (pkTop + PK_ROWS < pkN) pkTop += PK_ROWS - 1;
                if (pkSel < pkTop) pkSel = pkTop;
                PkDrawList();
            } else if (PtInRect(p, &pkOkR)) {
                if (pkN > 0) result = pkMatch[pkSel];
            } else if (PtInRect(p, &pkCancelR)) {
                result = -1;
            }
        } else if (ev.what == updateEvt) {
            WindowPtr uw = (WindowPtr)ev.message;
            BeginUpdate(uw);
            if (uw == w) {
                PkDrawAll();
            } else if (uw == gWin) {
                DrawPalettes();
                DrawCanvas();
                SetPort(w);
            }
            EndUpdate(uw);
        }
    }
    DisposeWindow(w);
    SetPort(gWin);
    DrawPalettes();          /* repaint what the picker covered now: Finish it runs before the */
    DrawCanvas();            /* event loop sees the update event */
    ValidRect(&gWin->portRect);
    return result;
}

/* ------------------------------------------------------------------ progress, drawing */
static void Progress(short done, short total)
{
    Rect r;
    short w;
    SetPort(gWin);
    SetRect(&r, gOX + 250, gPatR.bottom + 3, gOX + APP_W - 2, gPatR.bottom + 11);
    FrameRect(&r);
    InsetRect(&r, 1, 1);
    w = total > 0 ? (short)((long)(r.right - r.left) * done / total) : 0;
    r.right = r.left + w;
    PaintRect(&r);
}

static void DrawMove(short x0, short y0, short x1, short y1)
{
    Rect clip, r;
    short sz = 1, off;
    SetPort(&gDocPort);
    SetRect(&clip, AIF_X, AIF_Y, AIF_X + AIF_N, AIF_Y + AIF_N);
    ClipRect(&clip);
    PenNormal();
    if (gTool == T_BRUSH) {
        static const short bw[NSIZES] = {3, 5, 8, 12};
        sz = bw[gSize];
        PenPat(&gPats[gPat]);
    }
    PenSize(sz, sz);
    off = sz / 2;
    MoveTo(AIF_X + x0 - off, AIF_Y + y0 - off);
    LineTo(AIF_X + x1 - off, AIF_Y + y1 - off);
    SetRect(&clip, 0, 0, CANVAS_W, CANVAS_H);
    ClipRect(&clip);
    SetRect(&r, AIF_X + (x0 < x1 ? x0 : x1) - sz - 1, AIF_Y + (y0 < y1 ? y0 : y1) - sz - 1,
            AIF_X + (x0 > x1 ? x0 : x1) + sz + 2, AIF_Y + (y0 > y1 ? y0 : y1) + sz + 2);
    Blit(&r);
}

static void AddPoint(short x, short y, short lift)
{
    if (gNPts >= MAXPTS)
        return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > AIF_N - 1) x = AIF_N - 1;
    if (y > AIF_N - 1) y = AIF_N - 1;
    gPts[gNPts * 3] = x;
    gPts[gNPts * 3 + 1] = y;
    gPts[gNPts * 3 + 2] = lift;
    gNPts++;
}

void FinishIt(short cat, unsigned long seed, Boolean askCategory)
{
    short n, i, dx, dy, pen, prevPen, ngen;
    short x, y;
    OSErr err;
    CursHandle watch = GetCursor(4);
    unsigned char num[16];

    dbg.fiState = 1;
    if (!ModelLoaded()) {
        SetStatus("\pLoading the drawing model\xC9");
        if (watch) SetCursor(*watch);
        {
            long t0 = TickCount();
            err = ModelLoad();
            dbg.fiLoadTicks = (unsigned long)(TickCount() - t0);
        }
        dbg.freeMem = FreeMem();
        dbg.modelBytes = (unsigned long)gModelBytes;
        SetCursor(&qd.arrow);
        if (err) {
            dbg.fiState = 5;
            dbg.lastErr = err;
            SysBeep(10);
            SetStatus(err == fnfErr ? "\pThe file \"Doodle Model\" is missing."
                                    : "\pCould not load the drawing model (not enough memory?).");
            return;
        }
    }
    n = BuildPrompt();
    dbg.fiNPrompt = (unsigned long)n;
    if (n < 1) {
        dbg.fiState = 5;
        SysBeep(5);
        SetStatus("\pFirst draw something inside the dotted frame.");
        return;
    }
    if (askCategory) {
        cat = PickCategory();
        if (cat < 0) {
            dbg.fiState = 6;
            SetStatus("\p");
            return;
        }
    }
    if (cat < 0 || cat >= ModelNCat())
        cat = 0;
    if (!seed)
        seed = dbg.fiSeed ? dbg.fiSeed : ((unsigned long)TickCount() * 2654435761UL) | 1;
    dbg.fiCat = (unsigned long)cat;
    dbg.fiSeed = seed;
    dbg.fiGen = 0;
    dbg.fiPrimed = 0;
    dbg.fiT0 = (unsigned long)TickCount();
    dbg.fiState = 2;
    SaveUndo();
    {
        unsigned char msg[64] = "\pStudying your ";
        const unsigned char *nm = ModelName(cat);
        short k;
        for (k = 1; k <= nm[0] && msg[0] < 50; k++)
            msg[++msg[0]] = nm[k];
        msg[++msg[0]] = 0xC9;
        SetStatus(msg);
    }
    if (watch) SetCursor(*watch);
    ModelBegin(cat, seed, gTemp);
    /* inputs: the start token, then the prompt rows; the last one is fed with a full step */
    for (i = 0; i < n; i++) {
        if (i == 0)
            ModelPrime(0, 0, 0);
        else
            ModelPrime(gRows[(i - 1) * 3], gRows[(i - 1) * 3 + 1], gRows[(i - 1) * 3 + 2]);
        dbg.fiPrimed = (unsigned long)(i + 1);
        Progress(i + 1, n + 1);
    }
    ModelFull(gRows[(n - 1) * 3], gRows[(n - 1) * 3 + 1], gRows[(n - 1) * 3 + 2]);
    dbg.fiPrimed = (unsigned long)(n + 1);
    dbg.fiT1 = (unsigned long)TickCount();
    dbg.fiState = 3;
    SetCursor(&qd.arrow);
    SetStatus("\pDrawing\xC9 (click to stop)");
    /* position after the prompt */
    x = gStartX;
    y = gStartY;
    for (i = 0; i < n; i++) {
        x += gRows[i * 3];
        y += gRows[i * 3 + 1];
    }
    prevPen = gRows[(n - 1) * 3 + 2];
    for (ngen = 0;;) {
        ModelSample(&dx, &dy, &pen);
        gGen[ngen * 3] = dx;
        gGen[ngen * 3 + 1] = dy;
        gGen[ngen * 3 + 2] = pen;
        ngen++;
        dbg.fiGen = (unsigned long)ngen;
        if (prevPen == 0)
            DrawMove(x, y, x + dx, y + dy);
        x += dx;
        y += dy;
        AddPoint(x, y, pen != 0);
        prevPen = pen;
        if (pen == 2 || ngen >= MAX_GEN || n + ngen >= MAX_ROWS)
            break;
        if (Button() && !dbg.arg[3]) {
            dbg.fiState = 6;
            break;
        }
        ModelNext();
    }
    dbg.fiT2 = (unsigned long)TickCount();
    if (dbg.fiState == 3)
        dbg.fiState = 4;
    MarkOp();
    NumToString((long)ngen, num);
    {
        unsigned char msg[80] = "\pDone: ";
        short k;
        for (k = 1; k <= num[0]; k++)
            msg[++msg[0]] = num[k];
        BlockMove(" moves. Undo removes them.", msg + msg[0] + 1, 26);
        msg[0] += 26;
        SetStatus(msg);
    }
    while (Button())
        ;
    FlushEvents(mDownMask | mUpMask, 0);
}
