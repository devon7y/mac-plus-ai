/*
 * Broadside's animations: the incoming shell (a sight closing on the cell), the splash or the
 * explosion, the sinking, and the fires that burn on hit cells.
 */
#include "broadside.h"
#include "art.h"

static void frame_at(int grid, int cell, int art, short ticks)
{
    Rect r;
    cell_rect(grid, cell, &r);
    ClipRect(&r);
    art_draw(art, (short)((r.left + r.right) / 2), (short)((r.top + r.bottom) / 2));
    ClipRect(&gWin->portRect);
    pump(ticks);
}

/* a sight closing on the cell: squares from 3 cells wide down to one, drawn in XOR */
static void incoming(int grid, int cell)
{
    Rect r;
    short i;
    cell_rect(grid, cell, &r);
    PenNormal();
    PenMode(patXor);
    for (i = 4; i >= 0; i--) {
        Rect s = r;
        InsetRect(&s, (short)(-i * CS / 4 - 1), (short)(-i * CS / 4 - 1));
        FrameRect(&s);
        pump(2);
        FrameRect(&s);
    }
    PenNormal();
}

void fx_shot(int grid, int cell, int result)
{
    snd_play(SND_FIRE);
    incoming(grid, cell);
    if (result == SHOT_MISS) {
        snd_play(SND_SPLASH);
        frame_at(grid, cell, ART_SPL1, 3);
        draw_cell(grid, cell);
        frame_at(grid, cell, ART_SPL2, 4);
        draw_cell(grid, cell);
        frame_at(grid, cell, ART_SPL3, 4);
    } else {
        snd_play(SND_EXPLOSION);
        frame_at(grid, cell, ART_EXP1, 3);
        frame_at(grid, cell, ART_EXP2, 4);
        draw_cell(grid, cell);
        frame_at(grid, cell, ART_EXP3, 5);
    }
    draw_cell(grid, cell);
}

void fx_sink(int grid, int ship)
{
    uint8_t cells[5];
    int i, k, n = ship_cells(&g.fleet[grid].ship[ship], kShipLen[ship], cells);
    snd_play(SND_HORN);
    for (k = 0; k < 3; k++) {
        for (i = 0; i < n; i++) {
            Rect r;
            cell_rect(grid, cells[i], &r);
            InvertRect(&r);
        }
        pump(5);
        for (i = 0; i < n; i++) {
            Rect r;
            cell_rect(grid, cells[i], &r);
            InvertRect(&r);
        }
        pump(5);
    }
    draw_ship_cells(grid, ship);             /* (no touching: the water around it is now known) */
    draw_status();
}

void fx_idle(void)
{
    static unsigned long next;
    if (TICKS < next)
        return;
    next = TICKS + 14;
    gFlame ^= 1;
    draw_flames();
}
