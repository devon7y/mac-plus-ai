/*
 * TuneBench: composes a few tunes with the model in resource 'TMDL' 128 and records the tokens and
 * the Ticks they took in a debug block ('TUNB') that tune/app/bench/bench.lua reads in MAME.
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
#include <Resources.h>
#include "engine.h"

#define NT 4
#define MAXT 400
struct Bench {
    unsigned long magic, version, notMagic;
    long state;                  /* 0 loading, 1 composing, 2 done, < 0 error */
    long ntunes, freeMem, kernTicks, kernReps;
    long prof[8];                /* ticks for 200 reps: feed, sample, k_gru, gram_allowed, lookback, k_tab5, k_scale, k_mv out */
    struct { unsigned long ticks; short ntok, pad; unsigned short tok[MAXT]; } t[NT];
};
struct Bench dbg;   /* not static: the stores must not be optimised away */
#define TICKS (*(volatile unsigned long *)0x16A)

static const short cfg[NT][4] = {   /* type, meter, mode, seed */
    {0, 0, 0, 1000}, {1, 1, 1, 8919}, {2, 0, 2, 16838}, {5, 2, 3, 24757}
};

static TuneModel m;
static TuneGen g;      /* ~10 KB (the grammar's history): keep it off the 8 KB stack */

int main(void)
{
    Handle h;
    void *work;
    short i, k;
    unsigned long t0;
    dbg.magic = 0x54554E42UL;
    dbg.version = 1;
    dbg.notMagic = ~0x54554E42UL;
    MaxApplZone();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    h = GetResource('TMDL', 128);
    if (!h) { dbg.state = -1; goto idle; }
    HLock(h);
    if (tm_load(&m, *h, GetHandleSize(h))) { dbg.state = -2; goto idle; }
    work = NewPtr(tg_bytes(&m));
    if (!work) { dbg.state = -3; goto idle; }
    dbg.freeMem = FreeMem();
    tg_init(&g, &m, work);
    dbg.state = 1;
    for (i = 0; i < NT; i++) {
        short t;
        t0 = TICKS;
        tg_start(&g, cfg[i][0], cfg[i][1], cfg[i][2], (uint32_t)cfg[i][3]);
        dbg.t[i].tok[0] = TK_BOS;
        k = 1;
        for (;;) {
            t = tg_sample(&g);
            if (k < MAXT) dbg.t[i].tok[k++] = t;
            if (t == TK_EOS) break;
            tg_feed(&g, t);
        }
        dbg.t[i].ticks = TICKS - t0;
        dbg.t[i].ntok = k;
        dbg.ntunes = i + 1;
    }
    /* the recurrent kernel alone */
    t0 = TICKS;
    for (k = 0; k < 50; k++)
        k_mv(m.l[0].codes_hh, (const short *)((const char *)g.lut[0] + 32768L), 3L * m.l[0].H, g.acc, m.l[0].G);
    dbg.kernTicks = TICKS - t0;
    dbg.kernReps = 50;
#define PROF(i, stmt) t0 = TICKS; for (k = 0; k < 200; k++) { stmt; } dbg.prof[i] = TICKS - t0;
    tg_start(&g, 0, 0, 0, 5);
    tg_feed(&g, TK_PICK0);
    tg_feed(&g, TK_NOTE0 + 10 * TK_NDURS + 1);
    PROF(0, tg_feed(&g, TK_NOTE0 + 10 * TK_NDURS + 1); g.g.ntok--; g.g.pos -= 6; if (g.g.pos < 0) g.g.pos = 0)
    PROF(1, tg_sample(&g))
    PROF(2, k_gru(g.x, g.acc, m.l[0].Mb, m.l[0].k_hh, g.h[0], m.l[0].H, tk_sig + 4096, tk_tanh + 4096))
    PROF(3, gram_allowed(&g.g, g.ok, 600, 40))
    { short ff[4]; PROF(4, gram_lookback(&g.g, ff)) }
    PROF(5, k_tab5(g.hq[0], m.l[0].G, g.lut[0]))
    PROF(6, k_scale(g.acc, m.l[1].M_ih, m.l[1].k_ih, m.l[1].b_ih, g.x, 3L * m.l[1].H))
    PROF(7, k_mv(m.codes_out, (const short *)((const char *)g.lut[1] + 32768L), m.V, g.acc, m.Go))
    dbg.state = 2;
idle:
    for (;;) {
        EventRecord ev;
        SystemTask();
        GetNextEvent(everyEvent, &ev);
    }
    return 0;
}
