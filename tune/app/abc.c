/*
 * Tune -> ABC text (the Save command): the same text as tune/ref/abcout.py to_abc, with Mac line
 * endings (CR) so TeachText shows it.
 */
#include "abc.h"
#include "tune_consts.h"

static const char letters[] = "CDEFGAB";
static const short letter_pc[7] = {0, 2, 4, 5, 7, 9, 11};

typedef struct Out { char *p, *end; } Out;

static void put(Out *o, const char *s)
{
    while (*s && o->p < o->end)
        *o->p++ = *s++;
}

static void putc_(Out *o, char c)
{
    if (o->p < o->end)
        *o->p++ = c;
}

static void putn(Out *o, unsigned long v)
{
    char b[12];
    short n = 0;
    do {
        b[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        putc_(o, b[--n]);
}

static const char *dur_abc(short t)
{
    switch (t) {
    case 3: return "/";
    case 6: return "";
    case 9: return "3/2";
    case 12: return "2";
    case 18: return "3";
    case 24: return "4";
    case 30: return "5";
    case 36: return "6";
    }
    return "";
}

long abc_write(char *buf, long cap, const unsigned char *toks, short n, const AbcInfo *k)
{
    Out o;
    short i, nbar = 0, alt = 0, last = -1;
    signed char bar_acc[7][12];
    short a, b;
    o.p = buf;
    o.end = buf + cap;
    put(&o, "X: "); putn(&o, k->seed & 0x7FFFFFFFUL); putc_(&o, '\r');
    put(&o, "T: "); put(&o, k->title); putc_(&o, '\r');
    put(&o, "R: "); put(&o, k->type_name); putc_(&o, '\r');
    put(&o, "C: composed by Tune on a Macintosh Plus (seed "); putn(&o, k->seed); put(&o, ")\r");
    put(&o, "M: "); putn(&o, (unsigned long)k->meter_num); putc_(&o, '/'); putn(&o, (unsigned long)k->meter_den);
    putc_(&o, '\r');
    put(&o, "L: 1/8\r");
    put(&o, "K: "); put(&o, k->key_name); putc_(&o, '\r');
    for (a = 0; a < 7; a++)
        for (b = 0; b < 12; b++)
            bar_acc[a][b] = 99;
    for (i = 0; i < n; i++) {
        short t = toks[i];
        if (t == TK_BOS || (t >= TK_PICK0 && t < TK_REST0))
            continue;
        if (t == TK_UP) { alt = 1; continue; }
        if (t == TK_DOWN) { alt = -1; continue; }
        if (t == TK_T3) { put(&o, "(3"); continue; }
        if (t >= TK_NOTE0) {
            short deg = (t - TK_NOTE0) / TK_NDURS + TK_DEG_MIN, d = tk_durs[(t - TK_NOTE0) % TK_NDURS];
            short q = deg >= 0 ? deg / 7 : -((-deg + 6) / 7), r = deg - 7 * q;
            short midi = k->tonic_midi + 12 * q + tk_mode_steps[k->mode][r] + alt;
            short tl = k->tonic_li + deg, lq = tl >= 0 ? tl / 7 : -((-tl + 6) / 7);
            short l = tl - 7 * lq, octv = k->tonic_oct + lq;
            short nat = 12 * (octv + 1) + letter_pc[l], ac = midi - nat, cur;
            short oi = octv + 2 < 0 ? 0 : octv + 2 > 11 ? 11 : octv + 2;
            alt = 0;
            cur = bar_acc[l][oi] != 99 ? bar_acc[l][oi] : k->sig[l];
            if (ac != cur) {
                put(&o, ac == 1 ? "^" : ac == -1 ? "_" : ac == 0 ? "=" : ac == 2 ? "^^" : "__");
                bar_acc[l][oi] = (signed char)ac;
            }
            if (octv >= 5) {
                short j;
                putc_(&o, (char)(letters[l] + 32));
                for (j = 5; j < octv; j++)
                    putc_(&o, '\'');
            } else {
                short j;
                putc_(&o, letters[l]);
                for (j = octv; j < 4; j++)
                    putc_(&o, ',');
            }
            put(&o, dur_abc(d));
            last = t;
            continue;
        }
        if (t >= TK_REST0) {
            putc_(&o, 'z');
            put(&o, dur_abc(tk_durs[t - TK_REST0]));
            continue;
        }
        if (t == TK_EOS) {
            if (last != TK_RE && last != TK_DBL && last != TK_RER)
                put(&o, "|]");
            break;
        }
        /* bar lines */
        put(&o, t == TK_BAR ? " | " : t == TK_DBL ? " || " : t == TK_RS ? " |: " : t == TK_RE ? " :| " :
                 t == TK_RER ? " :: " : t == TK_E1 ? " |1 " : " :|2 ");
        for (a = 0; a < 7; a++)
            for (b = 0; b < 12; b++)
                bar_acc[a][b] = 99;
        last = t;
        nbar++;
        if (nbar % 4 == 0 && t != TK_E1 && t != TK_E2)
            putc_(&o, '\r');
    }
    putc_(&o, '\r');
    return (long)(o.p - buf);
}
