/*
 * The tune grammar: bar positions, triplets, sections, and which tokens may come next. A line-by-line
 * port of vocab.State (tune/corpus/vocab.py); the two must stay identical (test_host.py checks).
 */
#include "grammar.h"

static short tok_dur(short t)
{
    if (t >= TK_NOTE0)
        return tk_durs[(t - TK_NOTE0) % TK_NDURS];
    if (t >= TK_REST0)
        return tk_durs[t - TK_REST0];
    return 0;
}

void gram_init(Gram *g, short blen)
{
    short i, j, k;
    for (i = 0; i < TK_MAXSEC; i++)
        for (j = 0; j < TK_HISTBARS; j++)
            for (k = 0; k < HIST_POS; k++)
                g->hist[i][j][k] = 0;
    g->blen = blen;
    g->pos = g->trip = g->tdur = g->bar = g->sec = 0;
    g->e1pos = -1;
    g->e1bar = 0;
    g->acc = 0;
    g->started = 0;
    g->ntok = 0;
    g->last = TK_BOS;
    g->nbars = 0;
}

void gram_feats(const Gram *g, short *f)
{
    f[0] = g->pos < TK_MAXPOS - 1 ? g->pos : TK_MAXPOS - 1;
    f[1] = g->trip;
    f[2] = g->bar < 0 ? TK_MAXBAR - 1 : g->bar < TK_MAXBAR - 2 ? g->bar : TK_MAXBAR - 2;
    f[3] = g->sec < TK_MAXSEC - 1 ? g->sec : TK_MAXSEC - 1;
}

/* history slot of (sec, bar), bar >= -1 */
static const unsigned char *slot(const Gram *g, short sec, short bar)
{
    if (sec > TK_MAXSEC - 1)
        sec = TK_MAXSEC - 1;
    if (bar > TK_HISTBARS - 2)
        bar = TK_HISTBARS - 2;
    return g->hist[sec][bar + 1];
}

void gram_lookback(const Gram *g, short *f)
{
    short k, sec, bar, t;
    for (k = 0; k < 2; k++) {
        sec = k ? g->sec - 1 : g->sec;
        bar = k ? g->bar : g->bar - 4;
        t = 0;
        if (sec >= 0 && bar >= -1 && g->pos < HIST_POS)
            t = slot(g, sec, bar)[g->pos];
        if (t)
            tok_inputs(t, &f[2 * k], &f[2 * k + 1]);
        else {
            f[2 * k] = TK_NSYM;
            f[2 * k + 1] = 0;
        }
    }
}

static void notes(const Gram *g, unsigned char *ok, short room, short notes_only)
{
    short di, k;
    for (di = 0; di < TK_NDURS; di++) {
        short d = tk_durs[di];
        if (g->trip == 3) {
            if ((d != 3 && d != 6 && d != 12) || 2 * d > room)
                continue;
        } else if (g->trip) {
            if (d != g->tdur)
                continue;
        } else if (d > room)
            continue;
        for (k = 0; k < TK_NDEG; k++)
            ok[TK_NOTE0 + k * TK_NDURS + di] = 1;
        if (!notes_only)
            ok[TK_REST0 + di] = 1;
    }
}

void gram_allowed(const Gram *g, unsigned char *ok, short maxtok, short maxbars)
{
    short i, room;
    for (i = 0; i < TK_VOCAB; i++)
        ok[i] = 0;
    if (!g->started) {
        for (i = 0; i < TK_NPICKS; i++)
            if (tk_picks[i] < g->blen)
                ok[TK_PICK0 + i] = 1;
        return;
    }
    room = g->blen - g->pos;
    if (g->acc) {
        notes(g, ok, room, 1);
        return;
    }
    if (g->trip) {
        notes(g, ok, room, 0);
        ok[TK_UP] = ok[TK_DOWN] = 1;
        return;
    }
    if (g->ntok >= maxtok || g->nbars >= maxbars) {
        if (g->pos == 0) {
            ok[TK_EOS] = 1;
            return;
        }
        if (room > 0)
            notes(g, ok, room, 0);
        else
            ok[TK_EOS] = ok[TK_RE] = 1;
        return;
    }
    if (room > 0) {
        notes(g, ok, room, 0);
        ok[TK_UP] = ok[TK_DOWN] = 1;
        if (6 <= room)              /* 2*d <= room for d in 3, 6, 12 */
            ok[TK_T3] = 1;
    }
    if (g->last == TK_RE || g->last == TK_DBL || g->last == TK_RER)
        ok[TK_EOS] = 1;
    if (g->pos > 0) {
        ok[TK_EOS] = 1;
        ok[TK_DBL] = ok[TK_RS] = ok[TK_RE] = ok[TK_RER] = ok[TK_E2] = 1;
        if (g->e1pos < 0)
            ok[TK_E2] = 0;
        if (room == 0) {
            ok[TK_BAR] = 1;
            if (g->e1pos < 0)
                ok[TK_E1] = 1;
        }
    }
}

void gram_step(Gram *g, short t)
{
    g->ntok++;
    g->last = t;
    if (t >= TK_PICK0 && t < TK_REST0) {
        g->started = 1;
        g->pos = (g->blen - tk_picks[t - TK_PICK0]) % g->blen;
        g->bar = tk_picks[t - TK_PICK0] ? -1 : 0;
        return;
    }
    if (t == TK_UP || t == TK_DOWN) {
        g->acc = 1;
        return;
    }
    if (t == TK_T3) {
        g->trip = 3;
        g->tdur = 0;
        return;
    }
    if (t >= TK_REST0) {
        short d = tok_dur(t);
        g->acc = 0;
        if (g->pos < HIST_POS)
            ((unsigned char *)slot(g, g->sec, g->bar))[g->pos] = (unsigned char)t;
        if (g->trip) {
            if (g->tdur == 0)
                g->tdur = d;
            g->pos += d * 2 / 3;
            g->trip--;
            if (g->trip == 0)
                g->tdur = 0;
        } else
            g->pos += d;
        return;
    }
    if (t >= TK_BAR && t <= TK_E2) {
        short full = g->pos >= g->blen;
        if (full || t == TK_BAR || t == TK_E1)
            g->nbars++;
        if (t == TK_BAR) {
            g->pos = 0;
            g->bar++;
        } else if (t == TK_E1) {
            g->pos = 0;
            g->bar++;
            g->e1pos = 0;
            g->e1bar = g->bar;
        } else if (t == TK_E2) {
            g->pos = g->e1pos;
            g->bar = g->e1bar;
            g->e1pos = -1;
        } else {
            if (full)
                g->pos = 0;
            if (t != TK_RS || g->bar > 0 || g->pos > 0)
                g->sec++;
            g->bar = g->pos == 0 ? 0 : -1;
            g->e1pos = -1;
        }
    }
}

/* (sym, dur) model inputs of a token: vocab.tok_inputs */
void tok_inputs(short t, short *sym, short *dur)
{
    if (t >= TK_NOTE0) {
        *sym = (t - TK_NOTE0) / TK_NDURS;
        *dur = 1 + (t - TK_NOTE0) % TK_NDURS;
    } else if (t >= TK_REST0) {
        *sym = TK_NSYM - 1;
        *dur = 1 + t - TK_REST0;
    } else if (t >= TK_PICK0) {
        *sym = TK_NDEG + TK_NSPECIALS + (t - TK_PICK0);
        *dur = 0;
    } else {
        *sym = TK_NDEG + (t - TK_BOS);      /* specials are BOS..T3 in order */
        *dur = 0;
    }
}
