/*
 * Broadside's 1-bit art: 16x16 sprites (drawn from the strings below at startup) and the Mac's
 * face beside its messages. Original art.
 *   '#' black   'o' white (opaque)   '.' transparent
 */
#include "broadside.h"
#include "art.h"

static const char *const kArt[ART_COUNT][16] = {
    {   /* ART_FLAME_A */
        "................",
        ".......#........",
        "......##........",
        "......#o#.......",
        ".....#oo#...#...",
        ".....#oo##..##..",
        "....#oooo#.#o#..",
        "...##oo#oo##o#..",
        "...#oo###oo#o#..",
        "..#oo#####oooo#.",
        "..#oo######ooo#.",
        "..#ooo#####ooo#.",
        "...#ooo###ooo#..",
        "....##ooooo##...",
        "......#####.....",
        "................"},
    {   /* ART_FLAME_B */
        "................",
        "...........#....",
        "...#......##....",
        "...##.....#o#...",
        "...#o#...#oo#...",
        "...#o#..#ooo#...",
        "....#o#.#oo#....",
        "....#oo##oo#....",
        "...#ooo###oo#...",
        "..#oo#####ooo#..",
        "..#oo######oo#..",
        "..#ooo#####ooo#.",
        "...#ooo###ooo#..",
        "....##ooooo##...",
        "......#####.....",
        "................"},
    {   /* ART_WRECK: a sunk ship's cell */
        "................",
        "................",
        "..###.....###...",
        "..#oo#...#oo#...",
        "...#oo#.#oo#....",
        "....#oo#oo#.....",
        ".....#ooo#......",
        "......#o#.......",
        ".....#ooo#......",
        "....#oo#oo#.....",
        "...#oo#.#oo#....",
        "..#oo#...#oo#...",
        "..###.....###...",
        "................",
        "................",
        "................"},
    {   /* ART_EXP1 */
        "................",
        "................",
        "................",
        ".......#........",
        "......###.......",
        "....#.#o#.#.....",
        ".....#ooo#......",
        "....#ooooo#.....",
        "...##ooooo##....",
        "....#ooooo#.....",
        ".....#ooo#......",
        "....#.#o#.#.....",
        "......###.......",
        ".......#........",
        "................",
        "................"},
    {   /* ART_EXP2 */
        ".......#........",
        "...#...#...#....",
        "....#.###.#.....",
        ".....#ooo#......",
        ".#..#ooooo#..#..",
        "..##ooooooo##...",
        "...#oo#o#oo#....",
        "####ooo#ooo####.",
        "...#oo#o#oo#....",
        "..##ooooooo##...",
        ".#..#ooooo#..#..",
        ".....#ooo#......",
        "....#.###.#.....",
        "...#...#...#....",
        ".......#........",
        "................"},
    {   /* ART_EXP3: smoke */
        "................",
        ".....##..##.....",
        "....#o#o#oo#....",
        "...#o#o#o#o#....",
        "..#o#o#o#o#o#...",
        "..#oo#o#o#o##...",
        "...#o#o#o#o#....",
        "..##o#o#o#oo#...",
        ".#o#o#o#o#o#o#..",
        ".#oo#o#o#o#o##..",
        "..#o#o#o#o#o#...",
        "...###o#o####...",
        "......###.......",
        "................",
        "................",
        "................"},
    {   /* ART_SPL1 */
        "................",
        "................",
        "................",
        ".......#........",
        "......#o#.......",
        "......#o#.......",
        ".....#ooo#......",
        ".....#ooo#......",
        "....#ooooo#.....",
        "....#ooooo#.....",
        "...#ooooooo#....",
        "..##ooooooo##...",
        ".#ooooooooooo#..",
        "..###########...",
        "................",
        "................"},
    {   /* ART_SPL2 */
        "..#.....#....#..",
        ".#o#...#o#..#o#.",
        "..#.....#....#..",
        "....#.......#...",
        "...#o#.#.#.#o#..",
        "..#oo##o#o##oo#.",
        "..#ooo#ooo#ooo#.",
        "...#ooooooooo#..",
        "...#ooooooooo#..",
        "..#ooooooooooo#.",
        ".#ooooooooooooo#",
        ".#ooooooooooooo#",
        "..#############.",
        "................",
        "................",
        "................"},
    {   /* ART_SPL3: ripples */
        "................",
        "................",
        "................",
        "................",
        "....#######.....",
        "..##ooooooo##...",
        ".#oo#######oo#..",
        "#oo#ooooooo#oo#.",
        ".#oo#######oo#..",
        "..##ooooooo##...",
        "....#######.....",
        "................",
        "................",
        "................",
        "................",
        "................"},
};

static unsigned short gInk[ART_COUNT][16], gMask[ART_COUNT][16];
static BitMap gBm;

void art_init(void)
{
    short a, y, x;
    for (a = 0; a < ART_COUNT; a++)
        for (y = 0; y < 16; y++) {
            unsigned short ink = 0, mask = 0;
            for (x = 0; x < 16; x++) {
                char ch = kArt[a][y][x];
                if (ch == '#')
                    ink |= (unsigned short)(0x8000 >> x);
                if (ch != '.')
                    mask |= (unsigned short)(0x8000 >> x);
            }
            gInk[a][y] = ink;
            gMask[a][y] = mask;
        }
    gBm.rowBytes = 2;
    SetRect(&gBm.bounds, 0, 0, 16, 16);
}

/* draw sprite a centred at (cx, cy) in the current port */
void art_draw(int a, short cx, short cy)
{
    Rect dst;
    SetRect(&dst, cx - 8, cy - 8, cx + 8, cy + 8);
    gBm.baseAddr = (Ptr)gMask[a];
    CopyBits(&gBm, &qd.thePort->portBits, &gBm.bounds, &dst, srcBic, NULL);
    gBm.baseAddr = (Ptr)gInk[a];
    CopyBits(&gBm, &qd.thePort->portBits, &gBm.bounds, &dst, srcOr, NULL);
}

/* the Mac (a little compact Mac) with a face: 0 plain, 1 happy, 2 sad, 3 smug */
void art_draw_face(int mood, const Rect *r)
{
    Rect c = *r, s, e, k;
    short cx = (short)((r->left + r->right) / 2);
    PenNormal();
    EraseRect(&c);
    FrameRoundRect(&c, 6, 6);
    SetRect(&s, c.left + 3, c.top + 3, c.right - 3, c.top + 17);
    FrameRect(&s);
    /* floppy slot and foot */
    MoveTo(cx + 1, c.bottom - 7);
    LineTo(c.right - 5, c.bottom - 7);
    MoveTo(c.left + 3, c.bottom - 4);
    LineTo(c.right - 4, c.bottom - 4);
    /* eyes */
    SetRect(&e, cx - 5, s.top + 3, cx - 3, s.top + 6);
    PaintRect(&e);
    if (mood == 3) {
        MoveTo(cx + 3, s.top + 5);
        LineTo(cx + 5, s.top + 5);
    } else {
        SetRect(&e, cx + 3, s.top + 3, cx + 5, s.top + 6);
        PaintRect(&e);
    }
    /* mouth */
    SetRect(&k, cx - 4, s.top + 5, cx + 4, s.top + 12);
    if (mood == 1) {
        FrameArc(&k, 90, 180);
    } else if (mood == 2) {
        OffsetRect(&k, 0, 5);
        FrameArc(&k, 270, 180);
    } else if (mood == 3) {
        MoveTo(cx - 3, s.top + 10);
        LineTo(cx + 3, s.top + 9);
    } else {
        MoveTo(cx - 3, s.top + 10);
        LineTo(cx + 2, s.top + 10);
    }
}
