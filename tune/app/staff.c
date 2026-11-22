/*
 * Tune's staff. Events (notes, rests, bar lines) are added as the tune is composed; each bar gets a
 * "slot" (four to a system, plus a narrow one for an opening pickup), and adding to a bar redraws
 * just that bar. Notes are placed by their position in the bar, beamed by beat, stems up or down by
 * the group's height. Six systems with 6 px between staff lines; a longer tune switches to 4 px.
 */
#include <Quickdraw.h>
#include <Fonts.h>
#include "staff.h"
#include "glyphs.h"
#include "tune_consts.h"

#define TOP_PAD(s) ((s)->sp == 6 ? 13 : 9)          /* above the top line, within a system */

static const signed char sharp_pos[7] = {10, 7, 11, 8, 5, 9, 6};   /* F5 C5 G5 D5 A4 E5 B4 */
static const signed char flat_pos[7] = {6, 9, 5, 8, 4, 7, 3};      /* B4 E5 A4 D5 G4 C5 F4 */
static unsigned char slot_flag[ST_MAXSLOT];  /* 1/2: under a first/second ending bracket; +4 first slot */
static short in_ending;       /* 1 or 2 while adding bars under an ending bracket */
static short ending_first;

void st_letter(const Staff *s, short deg, short *letter, short *octave)
{
    short t = s->tonic_li + deg;
    short q = t >= 0 ? t / 7 : -((-t + 6) / 7);
    *letter = (short)(t - 7 * q);
    *octave = (short)(s->tonic_oct + q);
}

short st_note_midi(const Staff *s, short deg, short alt)
{
    short q = deg >= 0 ? deg / 7 : -((-deg + 6) / 7);
    short r = (short)(deg - 7 * q);
    return (short)(s->tonic_midi + 12 * q + tk_mode_steps[s->mode][r] + alt);
}

/* staff step of a note: diatonic index with C4 = 0 (E4, the bottom line, is 2) */
static short note_di(const Staff *s, short deg)
{
    short l, o;
    st_letter(s, deg, &l, &o);
    return (short)(7 * (o - 4) + l);
}

static short sys_top(const Staff *s, short sys)      /* y of the top staff line */
{
    return (short)(s->area.top + sys * s->sys_h + TOP_PAD(s));
}

static short di_y(const Staff *s, short sys, short di)
{
    return (short)(sys_top(s, sys) + 4 * s->sp - (di - 2) * (s->sp / 2));
}

static short header_w(const Staff *s, short sys)
{
    short w = (s->sp == 6 ? 17 : 12) + (s->nsig > 0 ? s->nsig : -s->nsig) * (s->sp == 6 ? 7 : 5);
    if (sys == 0)
        w += s->sp == 6 ? 14 : 10;
    return (short)(w + 2);
}

static void place_slot(Staff *s, short k)
{
    short sys, j, x0, w, avail;
    short left = (short)(s->area.left + 2), right = (short)(s->area.right - 3);
    if (s->pickup) {
        if (k == 0) {
            sys = 0;
            j = -1;
        } else {
            sys = (short)((k - 1) / 4);
            j = (short)((k - 1) % 4);
        }
    } else {
        sys = (short)(k / 4);
        j = (short)(k % 4);
    }
    x0 = (short)(left + header_w(s, sys));
    avail = (short)(right - x0);
    if (sys == 0 && s->pickup) {
        short pw = (short)(avail * s->pickup / (s->blen * 4 + s->pickup));
        if (pw < 20)
            pw = 20;
        if (j < 0) {
            s->slot_x0[k] = x0;
            s->slot_x1[k] = (short)(x0 + pw);
        } else {
            w = (short)((avail - pw) / 4);
            s->slot_x0[k] = (short)(x0 + pw + j * w);
            s->slot_x1[k] = (short)(x0 + pw + (j + 1) * w);
        }
    } else {
        w = (short)(avail / 4);
        s->slot_x0[k] = (short)(x0 + j * w);
        s->slot_x1[k] = (short)(x0 + (j + 1) * w);
    }
    s->slot_sys[k] = sys;
}

static void set_scale(Staff *s, short sp)
{
    short k;
    s->sp = sp;
    s->sys_h = sp == 6 ? 46 : 30;
    s->nsys_max = (short)((s->area.bottom - s->area.top) / s->sys_h);
    for (k = 0; k < s->nslot; k++)
        place_slot(s, k);
}

void st_reset(Staff *s, const Rect *area, short meter_num, short meter_den,
              short tonic_li, short tonic_oct, short tonic_midi, short mode, short nsig, const signed char *sig)
{
    short i;
    s->area = *area;
    s->nev = 0;
    s->nslot = 0;
    s->pickup = 0;
    s->meter_num = meter_num;
    s->meter_den = meter_den;
    s->blen = (short)(meter_num * 48 / meter_den);
    s->tonic_li = tonic_li;
    s->tonic_oct = tonic_oct;
    s->tonic_midi = tonic_midi;
    s->mode = mode;
    s->nsig = nsig;
    for (i = 0; i < 7; i++)
        s->sig[i] = sig[i];
    s->highlight = -1;
    s->open_g0 = -1;
    for (i = 0; i < ST_MAXSLOT; i++)
        slot_flag[i] = 0;
    in_ending = 0;
    ending_first = 0;
    set_scale(s, 6);
}

/* ---- drawing primitives ---------------------------------------------------------------------- */

static void glyph(const Staff *s, short g, short x, short y)
{
    const Glyph *G = &glyphs[s->sp == 6 ? 0 : 1][g];
    BitMap bm;
    Rect dr;
    GrafPtr port;
    bm.baseAddr = (Ptr)G->bits;
    bm.rowBytes = G->rowBytes;
    SetRect(&bm.bounds, 0, 0, G->w, G->h);
    SetRect(&dr, x, y, x + G->w, y + G->h);
    GetPort(&port);
    CopyBits(&bm, &port->portBits, &bm.bounds, &dr, srcOr, NULL);
}

static short glyph_w(const Staff *s, short g) { return glyphs[s->sp == 6 ? 0 : 1][g].w; }

static void hline(short x0, short x1, short y)
{
    MoveTo(x0, y);
    LineTo(x1, y);
}

static void vline(short x, short y0, short y1)
{
    MoveTo(x, y0);
    LineTo(x, y1);
}

static void staff_lines(const Staff *s, short sys, short x0, short x1)
{
    short i, t = sys_top(s, sys);
    for (i = 0; i < 5; i++)
        hline(x0, x1, (short)(t + i * s->sp));
}

static void draw_header(const Staff *s, short sys)
{
    short x = (short)(s->area.left + 2), t = sys_top(s, sys), i, n;
    staff_lines(s, sys, x, (short)(x + header_w(s, sys)));
    /* the clef's G line is 27/18 px below its top (gen_glyphs.py) */
    glyph(s, GL_CLEF, (short)(x + 1), (short)(t + 3 * s->sp - (s->sp == 6 ? 27 : 18)));
    x += s->sp == 6 ? 17 : 12;
    n = s->nsig > 0 ? s->nsig : -s->nsig;
    for (i = 0; i < n; i++) {
        short di = s->nsig > 0 ? sharp_pos[i] : flat_pos[i];
        short y = di_y(s, sys, di);
        if (s->nsig > 0)
            glyph(s, GL_SHARP, x, (short)(y - (s->sp == 6 ? 7 : 5)));
        else
            glyph(s, GL_FLAT, x, (short)(y - (s->sp == 6 ? 9 : 6)));
        x += s->sp == 6 ? 7 : 5;
    }
    if (sys == 0) {
        Str15 num;
        unsigned char c;
        TextFont(s->sp == 6 ? 0 : 3);
        TextSize(s->sp == 6 ? 12 : 9);
        TextFace(s->sp == 6 ? 0 : bold);
        x += 2;
        NumToString(s->meter_num, num);
        MoveTo(x, (short)(t + 2 * s->sp - 1));
        DrawString(num);
        NumToString(s->meter_den, num);
        MoveTo(x, (short)(t + 4 * s->sp - 1));
        DrawString(num);
        TextFace(0);
        (void)c;
    }
}

/* x of a bar position inside slot k */
static short pos_x(const Staff *s, short k, short pos)
{
    short x0 = s->slot_x0[k], x1 = s->slot_x1[k], m = s->sp == 6 ? 9 : 6, p0 = 0, span = s->blen;
    if (k == 0 && s->pickup) {
        p0 = (short)(s->blen - s->pickup);
        span = s->pickup;
    }
    if (pos < p0)
        pos = p0;
    return (short)(x0 + m + (long)(pos - p0) * (x1 - x0 - m - (s->sp == 6 ? 8 : 6)) / span);
}

static void draw_bar(const Staff *s, short sys, short x, short tok)
{
    short t = sys_top(s, sys), b = (short)(t + 4 * s->sp), dy = s->sp / 2;
    Rect r;
    switch (tok) {
    case TK_BAR:
    case TK_E1:
        vline(x, t, b);
        break;
    case TK_DBL:
        vline((short)(x - 3), t, b);
        vline(x, t, b);
        break;
    case TK_EOS:
        vline((short)(x - 4), t, b);
        SetRect(&r, x - 1, t, x + 2, b + 1);
        PaintRect(&r);
        break;
    case TK_RS:
        SetRect(&r, x - 1, t, x + 2, b + 1);
        PaintRect(&r);
        vline((short)(x + 4), t, b);
        SetRect(&r, x + 6, t + s->sp + dy - 1, x + 8, t + s->sp + dy + 1);
        PaintRect(&r);
        OffsetRect(&r, 0, s->sp);
        PaintRect(&r);
        break;
    case TK_RE:
    case TK_E2:
        SetRect(&r, x - 7, t + s->sp + dy - 1, x - 5, t + s->sp + dy + 1);
        PaintRect(&r);
        OffsetRect(&r, 0, s->sp);
        PaintRect(&r);
        vline((short)(x - 3), t, b);
        SetRect(&r, x - 1, t, x + 2, b + 1);
        PaintRect(&r);
        break;
    case TK_RER:
        SetRect(&r, x - 7, t + s->sp + dy - 1, x - 5, t + s->sp + dy + 1);
        PaintRect(&r);
        OffsetRect(&r, 0, s->sp);
        PaintRect(&r);
        SetRect(&r, x - 2, t, x + 1, b + 1);
        PaintRect(&r);
        SetRect(&r, x + 3, t + s->sp + dy - 1, x + 5, t + s->sp + dy + 1);
        PaintRect(&r);
        OffsetRect(&r, 0, s->sp);
        PaintRect(&r);
        break;
    }
}

static void notehead(const Staff *s, short x, short y, short hollow)
{
    Rect r;
    if (s->sp == 6)
        SetRect(&r, x, y - 2, x + 7, y + 3);
    else
        SetRect(&r, x, y - 2, x + 5, y + 2);
    if (hollow)
        FrameOval(&r);
    else
        PaintOval(&r);
}

static short head_w(const Staff *s) { return s->sp == 6 ? 7 : 5; }

static void ledgers(const Staff *s, short sys, short x, short di)
{
    short d, w = head_w(s);
    for (d = 12; d <= di; d += 2)
        hline((short)(x - 2), (short)(x + w + 1), di_y(s, sys, d));
    for (d = 0; d >= di; d -= 2)
        hline((short)(x - 2), (short)(x + w + 1), di_y(s, sys, d));
}

static void draw_rest(const Staff *s, short sys, short x, short dur)
{
    short t = sys_top(s, sys);
    Rect r;
    short base = dur == 9 || dur == 18 || dur == 36 ? dur * 2 / 3 : dur;
    if (base <= 3)
        glyph(s, GL_SREST, x, (short)(t + s->sp));
    else if (base <= 6)
        glyph(s, GL_EREST, x, (short)(t + s->sp + 1));
    else if (base <= 12)
        glyph(s, GL_QREST, x, (short)(t + s->sp / 2));
    else {
        short w = s->sp == 6 ? 7 : 5;
        if (base >= 48)
            SetRect(&r, x, t + s->sp, x + w, t + s->sp + s->sp / 2);
        else
            SetRect(&r, x, t + 2 * s->sp - s->sp / 2, x + w, t + 2 * s->sp);
        PaintRect(&r);
    }
    if (dur == 9 || dur == 18 || dur == 36) {
        SetRect(&r, x + 8, t + 2 * s->sp - 3, x + 10, t + 2 * s->sp - 1);
        PaintRect(&r);
    }
}

static short beat_len(const Staff *s)
{
    return s->meter_den == 8 ? 18 : s->blen == 48 ? 24 : 12;
}

/* does note e continue the beam group that starts with note f? */
static short continues(const Staff *s, const Ev *f, const Ev *e)
{
    short beat = beat_len(s);
    return e->kind == EV_NOTE && e->dur <= 9 &&
           ((f->trip && e->trip > 1) || (!f->trip && !e->trip && e->pos / beat == f->pos / beat));
}

/* stems, beams or flags (and a triplet's 3) of the beam group g0..last */
static void draw_group(const Staff *s, short g0, short last)
{
    short j, n = 0, sum = 0, up, ext, lo = 32767, hi = -32768, stem = (short)(3 * s->sp), hw = head_w(s);
    const Ev *f = &s->ev[g0];
    for (j = g0; j <= last; j++) {
        sum += note_di(s, s->ev[j].deg);
        n++;
        if (s->ev[j].y < lo) lo = s->ev[j].y;
        if (s->ev[j].y > hi) hi = s->ev[j].y;
    }
    up = sum < 6 * n;
    ext = up ? (short)(lo - stem) : (short)(hi + stem);
    for (j = g0; j <= last; j++) {
        const Ev *q = &s->ev[j];
        vline(up ? (short)(q->x + hw - 1) : q->x, q->y, ext);
    }
    if (n == 1) {            /* a flag (two for a sixteenth) */
        short sx = up ? (short)(f->x + hw - 1) : f->x, d = up ? 1 : -1, fl;
        for (fl = 0; fl < (f->dur <= 3 ? 2 : 1); fl++) {
            short fy = (short)(ext + d * fl * 3);
            MoveTo(sx, fy);
            LineTo((short)(sx + 3), (short)(fy + d * 4));
        }
    } else {
        const Ev *l = &s->ev[last];
        short x0 = up ? (short)(f->x + hw - 1) : f->x, x1 = up ? (short)(l->x + hw - 1) : l->x;
        Rect r;
        SetRect(&r, x0, up ? ext : ext - 1, x1 + 1, up ? ext + 2 : ext + 1);
        PaintRect(&r);
        for (j = g0; j < last; j++) {      /* second beam between sixteenths */
            const Ev *a = &s->ev[j], *b = &s->ev[j + 1];
            short ax = up ? (short)(a->x + hw - 1) : a->x, bx = up ? (short)(b->x + hw - 1) : b->x;
            short by = up ? (short)(ext + 3) : (short)(ext - 3);
            if (a->dur <= 3 && b->dur <= 3)
                hline(ax, bx, by);
            else if (b->dur <= 3 && a->dur > 3)
                hline((short)(bx - 4), bx, by);
            else if (a->dur <= 3 && b->dur > 3 && j == g0)
                hline(ax, (short)(ax + 4), by);
        }
        if (f->trip) {            /* the triplet's 3 */
            TextFont(3);
            TextSize(9);
            MoveTo((short)((x0 + x1) / 2 - 2), up ? (short)(ext - 2) : (short)(ext + 10));
            DrawString("\p3");
        }
    }
}

/* event i's own marks: notehead (+ ledger lines, accidental, dot, and the stem of a quarter or
   longer), rest, or bar line */
static void draw_item(Staff *s, short k, short i)
{
    Ev *e = &s->ev[i];
    short sys = s->slot_sys[k], hw = head_w(s), stem = (short)(3 * s->sp);
    if (e->kind == EV_NOTE) {
        short di = note_di(s, e->deg);
        e->x = pos_x(s, k, e->pos);
        e->y = di_y(s, sys, di);
        ledgers(s, sys, e->x, di);
        notehead(s, e->x, e->y, e->dur >= 24);
        if (e->acc) {
            short g = e->acc == 1 ? GL_SHARP : e->acc == 2 ? GL_FLAT : GL_NATURAL;
            short gy = (short)(e->y - (g == GL_FLAT ? (s->sp == 6 ? 9 : 6) : (s->sp == 6 ? 7 : 5)));
            glyph(s, g, (short)(e->x - glyph_w(s, g) - 1), gy);
        }
        if (e->dur == 9 || e->dur == 18 || e->dur == 36) {
            Rect r;
            short dyy = (di & 1) ? 0 : -s->sp / 2;   /* on a line: the dot goes in the space above */
            SetRect(&r, e->x + hw + 1, e->y + dyy - 1, e->x + hw + 3, e->y + dyy + 1);
            PaintRect(&r);
        }
        if (e->dur > 9 && e->dur < 48) {          /* quarter or half: its own stem */
            short up = di < 6;
            vline(up ? (short)(e->x + hw - 1) : e->x, e->y, up ? (short)(e->y - stem) : (short)(e->y + stem));
        }
    } else if (e->kind == EV_REST) {
        e->x = pos_x(s, k, e->pos);
        e->y = sys_top(s, sys);
        draw_rest(s, sys, e->x, e->dur);
    } else if (e->kind == EV_BAR) {
        short x = e->pos >= s->blen ? s->slot_x1[k] : (short)(pos_x(s, k, e->pos) - 3);
        draw_bar(s, sys, x, e->tok);
    }
}

static void draw_bracket(const Staff *s, short k)
{
    if (slot_flag[k] & 3) {
        short t = sys_top(s, s->slot_sys[k]) - (s->sp == 6 ? 11 : 8);
        hline((short)(s->slot_x0[k] + 2), (short)(s->slot_x1[k] - 2), t);
        if (slot_flag[k] & 4) {
            Str15 lab;
            vline((short)(s->slot_x0[k] + 2), t, (short)(t + 5));
            lab[0] = 2;
            lab[1] = (unsigned char)('0' + (slot_flag[k] & 3));
            lab[2] = '.';
            TextFont(3);
            TextSize(9);
            MoveTo((short)(s->slot_x0[k] + 5), (short)(t + 9));
            DrawString(lab);
        }
    }
}

/* draw slot k's events from scratch (beamed by beat) */
static void draw_events(Staff *s, short k)
{
    short e0 = s->slot_ev0[k], e1 = k + 1 < s->nslot ? s->slot_ev0[k + 1] : s->nev, i, g0 = -1;
    for (i = e0; i < e1; i++) {
        Ev *e = &s->ev[i];
        if (g0 >= 0 && !continues(s, &s->ev[g0], e)) {
            draw_group(s, g0, (short)(i - 1));
            g0 = -1;
        }
        draw_item(s, k, i);
        if (e->kind == EV_NOTE && e->dur <= 9 && g0 < 0)
            g0 = i;
    }
    if (g0 >= 0 && !(k + 1 < s->nslot))
        s->open_g0 = g0;          /* the current slot: the group is still open */
    else if (g0 >= 0)
        draw_group(s, g0, (short)(e1 - 1));
    draw_bracket(s, k);
}

static void slot_rect(const Staff *s, short k, Rect *r)
{
    short sys = s->slot_sys[k], top = (short)(s->area.top + sys * s->sys_h);
    SetRect(r, s->slot_x0[k] + 1, top, s->slot_x1[k] + 3, top + s->sys_h);
}

void st_draw_slot(Staff *s, short k)
{
    Rect r;
    PenState ps;
    if (k < 0 || k >= s->nslot)
        return;
    if (s->highlight >= 0 && s->ev[s->highlight].slot == k)
        s->highlight = -1;            /* redrawn without it */
    GetPenState(&ps);
    PenNormal();
    slot_rect(s, k, &r);
    EraseRect(&r);
    staff_lines(s, s->slot_sys[k], s->slot_x0[k], s->slot_x1[k] + 2);
    if (k == 0 || s->slot_sys[k - 1] != s->slot_sys[k])
        vline(s->slot_x0[k], sys_top(s, s->slot_sys[k]), (short)(sys_top(s, s->slot_sys[k]) + 4 * s->sp));
    if (k == s->nslot - 1)
        s->open_g0 = -1;
    draw_events(s, k);
    SetPenState(&ps);
}

void st_draw_all(Staff *s)
{
    short k, sys, last = -1;
    EraseRect(&s->area);
    s->highlight = -1;
    for (k = 0; k < s->nslot; k++) {
        sys = s->slot_sys[k];
        if (sys != last) {
            draw_header(s, sys);
            last = sys;
        }
        st_draw_slot(s, k);
    }
    if (s->nslot == 0)
        draw_header(s, 0);
}

static void new_slot(Staff *s)
{
    short k = s->nslot;
    if (k >= ST_MAXSLOT)
        return;
    s->slot_ev0[k] = s->nev;
    s->nslot++;
    place_slot(s, k);
    if (s->slot_sys[k] >= s->nsys_max && s->sp == 6) {
        set_scale(s, 4);              /* doesn't fit: smaller staff, everything again */
        st_draw_all(s);
    } else {
        if (k == 0 || s->slot_sys[k - 1] != s->slot_sys[k])
            draw_header(s, s->slot_sys[k]);
        if (s->slot_sys[k] < s->nsys_max) {
            PenState ps;
            GetPenState(&ps);
            PenNormal();
            staff_lines(s, s->slot_sys[k], s->slot_x0[k], s->slot_x1[k] + 2);
            SetPenState(&ps);
        }
    }
    s->open_g0 = -1;
}


short st_add(Staff *s, const Ev *e, short new_slot_after)
{
    short i, k;
    if (s->nev >= ST_MAXEV)
        return -1;
    if (s->nslot == 0)
        new_slot(s);
    i = s->nev++;
    s->ev[i] = *e;
    k = s->nslot - 1;
    s->ev[i].slot = k;
    if (in_ending && !(slot_flag[k] & 3)) {
        slot_flag[k] = (unsigned char)(in_ending | (ending_first ? 4 : 0));
        ending_first = 0;
    }
    if (s->slot_sys[k] < s->nsys_max) {
        Ev *n = &s->ev[i];
        PenState ps;
        short hl = s->highlight;
        st_highlight(s, -1);           /* (drawing over an inverted note would leave gaps) */
        GetPenState(&ps);
        PenNormal();
        if (s->open_g0 >= 0 && !continues(s, &s->ev[s->open_g0], n)) {
            draw_group(s, s->open_g0, (short)(i - 1));
            s->open_g0 = -1;
        }
        draw_item(s, k, i);
        if (n->kind == EV_NOTE && n->dur <= 9 && s->open_g0 < 0)
            s->open_g0 = i;
        if (i == s->slot_ev0[k])
            draw_bracket(s, k);
        SetPenState(&ps);
        st_highlight(s, hl);
    }
    if (e->kind == EV_BAR) {
        if (e->tok == TK_E1) {
            in_ending = 1;
            ending_first = 1;
        } else if (e->tok == TK_E2) {
            in_ending = 2;
            ending_first = 1;
        } else if (e->tok == TK_DBL || e->tok == TK_RS || e->tok == TK_RE || e->tok == TK_RER || e->tok == TK_EOS) {
            in_ending = 0;
        } else if (in_ending == 2 && e->tok == TK_BAR) {
            in_ending = 0;            /* second endings: one bar */
        }
    }
    if (new_slot_after)
        new_slot(s);
    return i;
}

void st_highlight(Staff *s, short ev)
{
    Rect r;
    if (ev == s->highlight)
        return;
    if (s->highlight >= 0) {
        Ev *e = &s->ev[s->highlight];
        SetRect(&r, e->x - 1, e->y - 4, e->x + head_w(s) + 1, e->y + 4);
        InvertRect(&r);
    }
    s->highlight = -1;
    if (ev >= 0 && ev < s->nev && s->ev[ev].kind == EV_NOTE && s->slot_sys[s->ev[ev].slot] < s->nsys_max) {
        Ev *e = &s->ev[ev];
        SetRect(&r, e->x - 1, e->y - 4, e->x + head_w(s) + 1, e->y + 4);
        InvertRect(&r);
        s->highlight = ev;
    }
}
