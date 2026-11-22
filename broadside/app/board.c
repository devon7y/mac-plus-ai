/*
 * Broadside: layout and drawing. One window covers the screen (local = global coordinates).
 * The layout adapts to screenBits.bounds: 20-pixel cells on a 512-pixel screen (website, SE,
 * a Plus without Narrow Screen), 18-pixel cells on Narrow Screen's 448 pixels.
 */
#include "broadside.h"
#include "art.h"

short CS;
Rect gGridR[2], gMsgR, gBarR, gThinkR;
static Rect gStatR[2], gTitleR[2];
Rect gBtnR[5];
short gBtnShown[5];
uint8_t gHeat[NCELL];
short gHeatTarget = -1;
short gFlame;

static char gMsg[96];
static short gMood;
static Pattern heatPat[17];
static Pattern deckPat;

static const char *const kShipName[NSHIP] = {"Carrier", "Dreadnought", "Cruiser", "Submarine", "Destroyer"};

static void make_patterns(void)
{
    static const unsigned char bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    short L, y, x;
    for (L = 0; L <= 16; L++)
        for (y = 0; y < 8; y++) {
            unsigned char b = 0;
            for (x = 0; x < 8; x++)
                if (bayer[y & 3][x & 3] < L)
                    b |= (unsigned char)(0x80 >> x);
            heatPat[L].pat[y] = b;
        }
    /* ship deck: light lines along the hull */
    for (y = 0; y < 8; y++)
        deckPat.pat[y] = (y & 1) ? 0x00 : (y & 2 ? 0x11 : 0x44);
}

void layout(void)
{
    Rect b = qd.screenBits.bounds;
    short W = (short)(b.right - b.left), lbl = 12, gap, total, x, top, gw;
    CS = W >= 470 ? 20 : 18;
    gap = W >= 470 ? 30 : 22;
    gw = (short)(10 * CS + 1);
    total = (short)(2 * (lbl + gw) + gap);
    x = (short)(b.left + (W - total) / 2);
    top = 50;
    SetRect(&gGridR[0], x + lbl, top, x + lbl + gw, top + gw);
    SetRect(&gGridR[1], x + 2 * lbl + gw + gap, top, x + 2 * lbl + 2 * gw + gap, top + gw);
    SetRect(&gTitleR[0], gGridR[0].left - lbl, 22, gGridR[0].right, 38);
    SetRect(&gTitleR[1], gGridR[1].left - lbl, 22, gGridR[1].right, 38);
    SetRect(&gStatR[0], gGridR[0].left, gGridR[0].bottom + 4, gGridR[0].right, gGridR[0].bottom + 14);
    SetRect(&gStatR[1], gGridR[1].left, gGridR[1].bottom + 4, gGridR[1].right, gGridR[1].bottom + 14);
    SetRect(&gMsgR, gGridR[0].left - lbl, gStatR[0].bottom + 4, gGridR[1].right, gStatR[0].bottom + 34);
    SetRect(&gBarR, gMsgR.left, gMsgR.bottom + 4, gMsgR.right, b.bottom - 3);
    SetRect(&gThinkR, gBarR.left, gBarR.bottom - 12, gGridR[0].right + 40, gBarR.bottom);
    {
        short bw = 92, bh = 20, by = (short)(gBarR.top + 2);
        SetRect(&gBtnR[BTN_RANDOM], gBarR.right - 2 * bw - 12, by, gBarR.right - bw - 12, by + bh);
        SetRect(&gBtnR[BTN_START], gBarR.right - bw, by, gBarR.right, by + bh);
        SetRect(&gBtnR[BTN_NEW], gBarR.right - bw, by, gBarR.right, by + bh);
        SetRect(&gBtnR[BTN_ROTATE], 0, 0, 0, 0);
    }
    make_patterns();
    art_init();
}

void cell_rect(int grid, int cell, Rect *r)
{
    short l = (short)(gGridR[grid].left + kCol[cell] * CS + 1), t = (short)(gGridR[grid].top + kRow[cell] * CS + 1);
    SetRect(r, l, t, l + CS - 1, t + CS - 1);
}

int cell_at(int grid, Point p)
{
    short c, r;
    if (!PtInRect(p, &gGridR[grid]))
        return -1;
    c = (short)((p.h - gGridR[grid].left - 1) / CS);
    r = (short)((p.v - gGridR[grid].top - 1) / CS);
    if (c < 0 || c > 9 || r < 0 || r > 9)
        return -1;
    return r * 10 + c;
}

void place_rect(int grid, const Place *p, int len, Rect *r)
{
    short l = (short)(gGridR[grid].left + p->c * CS + 1), t = (short)(gGridR[grid].top + p->r * CS + 1);
    SetRect(r, l, t, l + (p->dir ? 1 : len) * CS - 1, t + (p->dir ? len : 1) * CS - 1);
    InsetRect(r, 2, 2);
}

/* style: 0 afloat, 1 sunk, 2 outline (heat map underneath), 3 the Mac's survivors at the end */
static void draw_ship(int grid, const Place *p, int len, int style)
{
    Rect r;
    short rad = (short)(CS - 4), i;
    place_rect(grid, p, len, &r);
    PenNormal();
    if (style == 0) {
        FillRoundRect(&r, rad, rad, &deckPat);
        FrameRoundRect(&r, rad, rad);
        for (i = 1; i < len; i++) {           /* turrets */
            Rect t;
            short cx = (short)(p->dir ? (r.left + r.right) / 2 : r.left + i * CS + CS / 2 - 2);
            short cy = (short)(p->dir ? r.top + i * CS + CS / 2 - 2 : (r.top + r.bottom) / 2);
            if (i == len - 1 && len > 2)
                continue;
            SetRect(&t, cx - 3, cy - 3, cx + 3, cy + 3);
            PaintRect(&t);
        }
    } else if (style == 1) {
        FillRoundRect(&r, rad, rad, &qd.dkGray);
        FrameRoundRect(&r, rad, rad);
    } else if (style == 2) {
        /* outline with a white halo, over the heat map */
        PenSize(3, 3);
        PenMode(patBic);
        FrameRoundRect(&r, rad, rad);
        PenNormal();
        InsetRect(&r, 1, 1);
        PenSize(1, 1);
        FrameRoundRect(&r, rad - 2, rad - 2);
    } else {
        PenPat(&qd.gray);
        PenSize(2, 2);
        FrameRoundRect(&r, rad, rad);
        PenNormal();
    }
}

static void clip_full(void)
{
    ClipRect(&gWin->portRect);
}

/* marker in a cell: miss, hit (flames), known water */
static void draw_marker(int grid, int cell, const Rect *r)
{
    int v = g.know[grid].cell[cell];
    short cx = (short)((r->left + r->right) / 2), cy = (short)((r->top + r->bottom) / 2);
    Rect m;
    if (v == K_MISS) {
        SetRect(&m, cx - 3, cy - 3, cx + 3, cy + 3);
        EraseOval(&m);
        FrameOval(&m);
        SetRect(&m, cx - 1, cy - 1, cx + 1, cy + 1);
        PaintRect(&m);
    } else if (v == K_EMPTY) {
        SetRect(&m, cx - 1, cy - 1, cx + 1, cy + 1);
        PaintRect(&m);
    } else if (v == K_HIT) {
        art_draw(gFlame ? ART_FLAME_B : ART_FLAME_A, cx, cy);
    } else if (v == K_SUNK) {
        art_draw(ART_WRECK, cx, cy);
    }
}

static void draw_selection(const Rect *r)
{
    short cx = (short)((r->left + r->right) / 2), cy = (short)((r->top + r->bottom) / 2);
    Rect o;
    SetRect(&o, cx - 6, cy - 6, cx + 7, cy + 7);
    PenNormal();
    PenSize(2, 2);
    FrameOval(&o);
    PenNormal();
    MoveTo(cx, r->top + 1);
    LineTo(cx, r->bottom - 2);
    MoveTo(r->left + 1, cy);
    LineTo(r->right - 2, cy);
}

void draw_reticle(int grid, int cell, int on)
{
    Rect r, o;
    short cx, cy;
    if (cell < 0)
        return;
    if (!on) {
        draw_cell(grid, cell);
        return;
    }
    cell_rect(grid, cell, &r);
    cx = (short)((r.left + r.right) / 2);
    cy = (short)((r.top + r.bottom) / 2);
    ClipRect(&r);
    PenNormal();
    /* a white disc with a black ring and cross: visible on any shade */
    SetRect(&o, cx - 7, cy - 7, cx + 8, cy + 8);
    EraseOval(&o);
    PenSize(2, 2);
    FrameOval(&o);
    PenNormal();
    MoveTo(cx, cy - 4);
    LineTo(cx, cy + 4);
    MoveTo(cx - 4, cy);
    LineTo(cx + 4, cy);
    clip_full();
}

static void draw_aim(const Rect *r)
{
    /* keyboard aim: corner brackets */
    short l = r->left, t = r->top, rr = (short)(r->right - 1), b = (short)(r->bottom - 1), k = 4;
    PenNormal();
    PenSize(2, 2);
    MoveTo(l, t + k); LineTo(l, t); LineTo(l + k, t);
    MoveTo(rr - k - 1, t); LineTo(rr - 1, t); LineTo(rr - 1, t + k);
    MoveTo(l, b - k - 1); LineTo(l, b - 1); LineTo(l + k, b - 1);
    MoveTo(rr - k - 1, b - 1); LineTo(rr - 1, b - 1); LineTo(rr - 1, b - k - 1);
    PenNormal();
}

static int show_heat(int grid, int cell)
{
    return grid == ME && gPrefs.think && g.state != ST_PLACE && g.state != ST_OVER
           && g.know[ME].cell[cell] == K_UNKNOWN;
}

/* which style a ship is drawn in on this grid now (-1 = hidden) */
static int ship_style(int grid, int s)
{
    if (grid == ME)
        return g.know[ME].sunk[s] ? 1 : (gPrefs.think && g.state != ST_PLACE ? 2 : 0);
    if (g.know[MAC].sunk[s])
        return 1;
    return g.state == ST_OVER ? 3 : -1;
}

/* redraw the cells of rows r0..r1, columns c0..c1 (backgrounds, the ships over them, markers);
   ships are clipped to the block, so inside it they cover the grid lines between their cells */
static void draw_cells(int grid, int r0, int c0, int r1, int c1)
{
    Rect clip, a, b;
    int r, c, sh;
    if (r0 < 0) r0 = 0;
    if (c0 < 0) c0 = 0;
    if (r1 > 9) r1 = 9;
    if (c1 > 9) c1 = 9;
    for (r = r0; r <= r1; r++)
        for (c = c0; c <= c1; c++) {
            Rect cr;
            int cell = r * 10 + c;
            cell_rect(grid, cell, &cr);
            if (show_heat(grid, cell) && gHeat[cell])
                FillRect(&cr, &heatPat[1 + (gHeat[cell] * 11 + 127) / 255]);
            else
                EraseRect(&cr);
        }
    cell_rect(grid, r0 * 10 + c0, &a);
    cell_rect(grid, r1 * 10 + c1, &b);
    SetRect(&clip, a.left, a.top, b.right, b.bottom);
    ClipRect(&clip);
    for (sh = 0; sh < NSHIP; sh++) {
        const Place *p = &g.fleet[grid].ship[sh];
        int L = kShipLen[sh], st, pr1, pc1;
        if (grid == MAC && !g.fleet[MAC].owner[p->r * 10 + p->c])
            continue;                          /* (the Mac's fleet isn't placed yet) */
        pr1 = p->dir ? p->r + L - 1 : p->r;
        pc1 = p->dir ? p->c : p->c + L - 1;
        if (pr1 < r0 || p->r > r1 || pc1 < c0 || p->c > c1)
            continue;
        st = ship_style(grid, sh);
        if (st >= 0)
            draw_ship(grid, p, L, st);
    }
    clip_full();
    for (r = r0; r <= r1; r++)
        for (c = c0; c <= c1; c++) {
            Rect cr;
            int cell = r * 10 + c;
            cell_rect(grid, cell, &cr);
            draw_marker(grid, cell, &cr);
            if (grid == MAC && g.sel[cell])
                draw_selection(&cr);
            if (grid == MAC && g.aim == cell && g.state == ST_PLAYER)
                draw_aim(&cr);
            if (grid == ME && gPrefs.think && cell == gHeatTarget && g.state == ST_PLAYER
                && g.know[ME].cell[cell] == K_UNKNOWN)
                draw_reticle(ME, cell, 1);
        }
}

void draw_cell(int grid, int cell)
{
    draw_cells(grid, kRow[cell], kCol[cell], kRow[cell], kCol[cell]);
}

void draw_cells_around(int grid, int cell)
{
    draw_cells(grid, kRow[cell] - 1, kCol[cell] - 1, kRow[cell] + 1, kCol[cell] + 1);
}

void draw_ship_cells(int grid, int ship)
{
    const Place *p = &g.fleet[grid].ship[ship];
    int L = kShipLen[ship];
    draw_cells(grid, p->r - 1, p->c - 1, (p->dir ? p->r + L - 1 : p->r) + 1, (p->dir ? p->c : p->c + L - 1) + 1);
}

void draw_grid_cells(int grid)
{
    draw_cells(grid, 0, 0, 9, 9);
}

static void draw_text_c(const char *s, short x, short y)
{
    MoveTo(x, y);
    DrawText((char *)s, 0, (short)strlen(s));
}

static void draw_text_centered(const char *s, const Rect *r, short y)
{
    short w = TextWidth((char *)s, 0, (short)strlen(s));
    draw_text_c(s, (short)((r->left + r->right - w) / 2), y);
}

static void draw_grid(int grid)
{
    Rect R = gGridR[grid];
    short i, x, y;
    char lab[3];
    PenNormal();
    EraseRect(&R);
    FrameRect(&R);
    PenPat(&qd.gray);
    for (i = 1; i < 10; i++) {
        x = (short)(R.left + i * CS);
        y = (short)(R.top + i * CS);
        MoveTo(x, R.top + 1);
        LineTo(x, R.bottom - 2);
        MoveTo(R.left + 1, y);
        LineTo(R.right - 2, y);
    }
    PenNormal();
    TextFont(3);
    TextSize(9);
    for (i = 0; i < 10; i++) {
        lab[0] = (char)('A' + i);
        lab[1] = 0;
        x = (short)(R.left + i * CS + CS / 2 - CharWidth(lab[0]) / 2);
        draw_text_c(lab, x, R.top - 3);
        if (i == 9) {
            lab[0] = '1';
            lab[1] = '0';
            lab[2] = 0;
        } else {
            lab[0] = (char)('1' + i);
            lab[1] = 0;
        }
        draw_text_c(lab, (short)(R.left - 2 - TextWidth(lab, 0, (short)strlen(lab))), (short)(R.top + i * CS + CS / 2 + 4));
    }
    draw_cells(grid, 0, 0, 9, 9);
}

static void draw_titles(void)
{
    static const char *const t[2] = {"Your fleet", "Enemy waters"};
    int i;
    TextFont(0);
    TextSize(0);
    for (i = 0; i < 2; i++) {
        Rect r = gTitleR[i];
        EraseRect(&r);
        draw_text_c(t[i], (short)(r.left + 12), r.bottom - 4);
        if ((g.state == ST_PLAYER && i == MAC) || (g.state == ST_BUSY && i == ME && g.winner < 0)) {
            /* whose turn: a pointer next to the title */
            Rect a;
            short x = (short)(r.left + 2), y = (short)(r.bottom - 9);
            SetRect(&a, x, y - 3, x + 7, y + 4);
            PaintOval(&a);
        }
    }
}

void draw_status(void)
{
    int grid, i;
    for (grid = 0; grid < 2; grid++) {
        Rect R = gStatR[grid];
        short x = R.left;
        char buf[16];
        int n = g.state == ST_PLACE ? NSHIP : g.fleet[grid].afloat;
        EraseRect(&R);
        for (i = 0; i < NSHIP; i++) {
            Rect s;
            short w = (short)(kShipLen[i] * 5);
            SetRect(&s, x, R.top + 2, x + w, R.top + 7);
            if (g.know[grid].sunk[i])
                PaintRoundRect(&s, 4, 4);
            else
                FrameRoundRect(&s, 4, 4);
            x = (short)(x + w + 4);
        }
        TextFont(3);
        TextSize(9);
        buf[0] = (char)('0' + n);
        strcpy(buf + 1, " afloat");
        draw_text_c(buf, (short)(R.right - TextWidth(buf, 0, (short)strlen(buf))), R.top + 8);
    }
}

/* ---- the Mac's message ---------------------------------------------------------------------- */

void draw_message(void)
{
    Rect b = gMsgR, face;
    PenNormal();
    EraseRect(&b);
    SetRect(&face, b.left, b.top + 1, b.left + 24, b.top + 29);
    art_draw_face(gMood, &face);
    if (!gMsg[0])
        return;
    b.left += 32;
    TextFont(0);
    TextSize(0);
    {
        short w = TextWidth(gMsg, 0, (short)strlen(gMsg));
        Rect bub;
        PolyHandle tail;
        SetRect(&bub, b.left, gMsgR.top + 3, b.left + w + 20, gMsgR.bottom - 3);
        if (bub.right > gMsgR.right)
            bub.right = gMsgR.right;
        FrameRoundRect(&bub, 14, 14);
        tail = OpenPoly();
        MoveTo(bub.left + 1, (short)((bub.top + bub.bottom) / 2 - 4));
        LineTo(bub.left - 7, (short)((bub.top + bub.bottom) / 2));
        LineTo(bub.left + 1, (short)((bub.top + bub.bottom) / 2 + 4));
        ClosePoly();
        ErasePoly(tail);
        MoveTo(bub.left, (short)((bub.top + bub.bottom) / 2 - 4));
        LineTo(bub.left - 7, (short)((bub.top + bub.bottom) / 2));
        LineTo(bub.left, (short)((bub.top + bub.bottom) / 2 + 4));
        KillPoly(tail);
        draw_text_c(gMsg, bub.left + 10, (short)(bub.bottom - 7));
    }
}

void say(const char *text, int mood, int speak)
{
    strncpy(gMsg, text, sizeof gMsg - 1);
    gMsg[sizeof gMsg - 1] = 0;
    gMood = (short)mood;
    draw_message();
    if (speak && gPrefs.speech && speech_available())
        speech_say(text);
}

/* ---- the bottom bar ------------------------------------------------------------------------- */

static void draw_button(int b, int dflt)
{
    Rect r = gBtnR[b];
    static const char *const label[5] = {"", "Random", "Start Battle", "New Game", ""};
    PenNormal();
    EraseRect(&r);
    if (dflt) {
        Rect o = r;
        InsetRect(&o, -3, -3);
        PenSize(2, 2);
        FrameRoundRect(&o, 14, 14);
        PenNormal();
    }
    FrameRoundRect(&r, 10, 10);
    TextFont(0);
    TextSize(0);
    draw_text_centered(label[b], &r, (short)(r.bottom - 6));
}

void draw_think(void)
{
    Rect r = gThinkR;
    char buf[64];
    if (!gPrefs.think || g.state == ST_PLACE || g.state == ST_OVER) {
        EraseRect(&r);
        return;
    }
    /* (the text is made first, so the line is never blank for long) */
    if (!g.ai.done) {
        strcpy(buf, "Mac's view: thinking...");
    } else if (g.level == AI_HARD) {
        const AI *a = &g.ai;
        if (a->exact) {
            /* exact count (labelled fleets; the two 3-long ships can swap) */
            unsigned long n = a->lastCount;
            char num[16];
            short i = 0, j;
            if (a->base.cnt[3] == 2)
                n >>= 1;
            do {
                num[i++] = (char)('0' + n % 10);
                n /= 10;
            } while (n && i < 15);
            strcpy(buf, "Mac's view: exactly ");
            j = (short)strlen(buf);
            while (i)
                buf[j++] = num[--i];
            strcpy(buf + j, " possible fleets");
        } else {
            /* about 10^k fleets: log2 of total * 2^(ref/256 - 10) / samples */
            long l2 = 0;
            unsigned long t = a->total, s = a->samples ? a->samples : 1;
            short k, j;
            while (t > 1) { t >>= 1; l2 += 256; }
            while (s > 1) { s >>= 1; l2 -= 256; }
            l2 += a->ref - 2560;
            if (a->base.cnt[3] == 2)
                l2 -= 256;
            k = (short)((l2 * 77) >> 16);            /* x log10(2) / 256 */
            strcpy(buf, "Mac's view: ");
            j = (short)strlen(buf);
            {
                char sm[8];
                short n = a->samples, i = 0;
                do { sm[i++] = (char)('0' + n % 10); n /= 10; } while (n && i < 7);
                while (i) buf[j++] = sm[--i];
            }
            strcpy(buf + j, " sampled of about 10^");
            j = (short)strlen(buf);
            if (k >= 10) buf[j++] = (char)('0' + k / 10);
            buf[j++] = (char)('0' + (k < 0 ? 0 : k % 10));
            strcpy(buf + j, " fleets");
        }
    } else if (g.level == AI_NORMAL) {
        strcpy(buf, "Mac's view: each ship counted on its own");
    } else {
        strcpy(buf, "Mac's view: random, then near hits");
    }
    EraseRect(&r);
    TextFont(3);
    TextSize(9);
    draw_text_c(buf, r.left, r.bottom - 2);
}

void draw_bar(void)
{
    Rect r = gBarR;
    char buf[64];
    int i;
    PenNormal();
    EraseRect(&r);
    for (i = 0; i < 5; i++)
        gBtnShown[i] = 0;
    TextFont(0);
    TextSize(0);
    if (g.state == ST_PLACE) {
        gBtnShown[BTN_RANDOM] = gBtnShown[BTN_START] = 1;
        draw_button(BTN_RANDOM, 0);
        draw_button(BTN_START, 1);
        TextFont(3);
        TextSize(9);
        draw_text_c("Drag a ship to move it; click it", r.left, r.top + 10);
        draw_text_c("(or press R) to turn it.", r.left, r.top + 21);
    } else if (g.state == ST_OVER) {
        gBtnShown[BTN_NEW] = 1;
        draw_button(BTN_NEW, 1);
        strcpy(buf, g.winner == ME ? "You won in " : "The Mac won. Your shots: ");
        {
            short n = g.myShots, j = (short)strlen(buf);
            if (n >= 100) buf[j++] = (char)('0' + n / 100);
            if (n >= 10) buf[j++] = (char)('0' + (n / 10) % 10);
            buf[j++] = (char)('0' + n % 10);
            buf[j] = 0;
            if (g.winner == ME)
                strcpy(buf + j, " shots!");
        }
        draw_text_c(buf, r.left, r.top + 16);
    } else {
        if (g.state == ST_PLAYER) {
            if (g.rules & RULE_SALVO) {
                short left = (short)(g.fleet[ME].afloat - g.nsel);
                strcpy(buf, "Salvo: choose ");
                buf[14] = (char)('0' + left);
                strcpy(buf + 15, left == 1 ? " more target" : " more targets");
            } else {
                strcpy(buf, "Your turn: fire at Enemy waters");
            }
        } else {
            strcpy(buf, g.winner >= 0 ? "" : "The Mac is firing...");
        }
        draw_text_c(buf, r.left, r.top + 16);
        {
            short n = g.myShots, j;
            strcpy(buf, "Shots: ");
            j = 7;
            if (n >= 100) buf[j++] = (char)('0' + n / 100);
            if (n >= 10) buf[j++] = (char)('0' + (n / 10) % 10);
            buf[j++] = (char)('0' + n % 10);
            buf[j] = 0;
            draw_text_c(buf, (short)(r.right - TextWidth(buf, 0, (short)strlen(buf))), r.top + 16);
        }
    }
    draw_think();
}

int button_at(Point p)
{
    int i;
    for (i = 1; i < 5; i++)
        if (gBtnShown[i] && PtInRect(p, &gBtnR[i]))
            return i;
    return BTN_NONE;
}

void track_button(int b)
{
    Rect r = gBtnR[b];
    short in = 1;
    InsetRect(&r, 1, 1);
    InvertRoundRect(&r, 10, 10);
    while (StillDown()) {
        Point p;
        short now;
        GetMouse(&p);
        now = PtInRect(p, &gBtnR[b]);
        if (now != in) {
            InvertRoundRect(&r, 10, 10);
            in = now;
        }
    }
    if (in)
        InvertRoundRect(&r, 10, 10);
}

void draw_drag(Rect *r)
{
    PenNormal();
    PenMode(patXor);
    PenSize(2, 2);
    FrameRoundRect(r, CS - 4, CS - 4);
    PenNormal();
}

void draw_flames(void)
{
    int grid, i;
    for (grid = 0; grid < 2; grid++)
        for (i = 0; i < NCELL; i++)
            if (g.know[grid].cell[i] == K_HIT) {
                Rect r;
                cell_rect(grid, i, &r);
                draw_cell(grid, i);
            }
}

void draw_all(void)
{
    dbg.bench[29]++;
    Rect r = gWin->portRect;
    SetPort(gWin);
    clip_full();
    r.top = 20;
    EraseRect(&r);
    draw_titles();
    draw_grid(ME);
    draw_grid(MAC);
    draw_status();
    draw_message();
    draw_bar();
}

const char *ship_name(int i)
{
    return kShipName[i];
}

void draw_titles_now(void)
{
    draw_titles();
}
