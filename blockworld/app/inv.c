/* Blockworld: the inventory, containers, crafting, and the hotbar strip and panels (drawn straight
 * into screen memory, text with QuickDraw). */
#include <Quickdraw.h>
#include <Events.h>
#include <string.h>
#include "inv.h"
#include "sound.h"
#include "world.h"
#include "tables.h"

Slot inv[NINV];
Slot inv_arm[NARM];
short inv_sel;
Chest chests[NCHESTS];
Furnace furns[NFURN];

/* ---- mining rules ---------------------------------------------------------------------------- */

static short tool_speed_for(int block, int tool)
{
    const Mine *m = &block_mine[block & 31];
    if (IS_TOOL(tool) && m->tool && TOOL_KIND(tool) == m->tool - 1) return tool_speed[TOOL_TIER(tool)];
    return 1;
}

static int harvestable(int block, int tool)
{
    const Mine *m = &block_mine[block & 31];
    if (m->level < 0) return 1;
    return IS_TOOL(tool) && TOOL_KIND(tool) == K_PICK && m->tool == MT_PICK && tier_level[TOOL_TIER(tool)] >= m->level;
}

short mine_time(int block, int tool)
{
    const Mine *m = &block_mine[block & 31];
    short t;
    if (block <= 0 || block >= NBLOCKS || m->level == 99) return 0;
    t = (short)((m->hard * (harvestable(block, tool) ? 3 : 10)) / tool_speed_for(block, tool));
    return t < 1 ? 1 : t;
}

short mine_drop(int block, int tool, short *count)
{
    const Mine *m = &block_mine[block & 31];
    *count = 0;
    if (block <= 0 || block >= NBLOCKS || !harvestable(block, tool)) return 0;
    *count = m->count;
    return m->drop;
}

int inv_add(unsigned char item, int n)
{
    short i, st = item_stack[item] ? item_stack[item] : 64;
    if (st > 1)
        for (i = 0; i < NINV && n; i++)               /* first the stacks of the same item */
            if (inv[i].item == item && inv[i].count < st) {
                int k = st - inv[i].count;
                if (k > n) k = n;
                inv[i].count = (unsigned char)(inv[i].count + k);
                n -= k;
            }
    for (i = 0; i < NINV && n; i++)                   /* then empty slots, the hotbar first */
        if (!inv[i].count) {
            int k = n > st ? st : n;
            inv[i].item = item;
            inv[i].count = (unsigned char)k;
            inv[i].dur = item_dur[item];
            n -= k;
        }
    return n;
}

void inv_use_tool(void)
{
    Slot *s = &inv[inv_sel];
    if (s->count && item_dur[s->item]) {
        if (s->dur <= 1) { s->item = 0; s->count = 0; s->dur = 0; }
        else s->dur--;
    }
}

/* ---- containers ------------------------------------------------------------------------------ */

Chest *chest_at(short x, short y, short z, int create)
{
    short i, fr = -1;
    for (i = 0; i < NCHESTS; i++) {
        if (chests[i].used && chests[i].x == x && chests[i].y == y && chests[i].z == z) return &chests[i];
        if (!chests[i].used && fr < 0) fr = i;
    }
    if (!create || fr < 0) return 0;
    memset(&chests[fr], 0, sizeof chests[fr]);
    chests[fr].used = 1; chests[fr].x = x; chests[fr].y = y; chests[fr].z = z;
    return &chests[fr];
}

Furnace *furn_at(short x, short y, short z, int create)
{
    short i, fr = -1;
    for (i = 0; i < NFURN; i++) {
        if (furns[i].used && furns[i].x == x && furns[i].y == y && furns[i].z == z) return &furns[i];
        if (!furns[i].used && fr < 0) fr = i;
    }
    if (!create || fr < 0) return 0;
    memset(&furns[fr], 0, sizeof furns[fr]);
    furns[fr].used = 1; furns[fr].x = x; furns[fr].y = y; furns[fr].z = z;
    return &furns[fr];
}

static void give_back(Slot *s)
{
    if (s->count) inv_add(s->item, s->count);
    s->item = 0; s->count = 0;
}

void container_broken(short x, short y, short z)
{
    Chest *c = chest_at(x, y, z, 0);
    Furnace *f = furn_at(x, y, z, 0);
    short i;
    if (c) { for (i = 0; i < 27; i++) give_back(&c->s[i]); c->used = 0; }
    if (f) { give_back(&f->in); give_back(&f->fuel); give_back(&f->out); f->used = 0; }
}

static short smelt_result(int item)
{
    short i;
    for (i = 0; i < NSMELT; i++) if (smelt_in[i] == item) return smelt_out[i];
    return 0;
}

static short fuel_value(int item)
{
    short i;
    for (i = 0; i < NFUEL; i++) if (fuel_item[i] == item) return (short)fuel_ticks[i];
    return 0;
}

#define COOK 200                 /* 10 s per item */
void furnaces_tick(void)
{
    short i;
    for (i = 0; i < NFURN; i++) {
        Furnace *f = &furns[i];
        short res, can;
        if (!f->used) continue;
        res = f->in.count ? smelt_result(f->in.item) : 0;
        can = res && (!f->out.count || (f->out.item == res && f->out.count < item_stack[res]));
        if (f->burn > 0) f->burn--;
        if (!f->burn && can && f->fuel.count && fuel_value(f->fuel.item)) {
            f->burn = f->burnmax = fuel_value(f->fuel.item);
            if (!--f->fuel.count) f->fuel.item = 0;
        }
        if (f->burn && can) {
            if (++f->cook >= COOK) {
                f->cook = 0;
                if (!--f->in.count) f->in.item = 0;
                if (!f->out.count) { f->out.item = (unsigned char)res; f->out.dur = 0; }
                f->out.count++;
            }
        } else if (f->cook > 0)
            f->cook = 0;
    }
}

/* ---- screen drawing -------------------------------------------------------------------------- */

static unsigned char *scr;
static short srb, hx, hy;

/* the top n (1..24) bits of a 32-bit value at (x, y): set where 1, cleared where 0 */
static void put_bits(short x, short y, unsigned long bits, short n)
{
    unsigned char *p = scr + (long)y * srb + (x >> 3);
    short sh = x & 7, i;
    unsigned long mask = ~(0xFFFFFFFFUL >> n), m = mask >> sh, b = (bits & mask) >> sh;
    for (i = 0; i < 4; i++) {
        unsigned char mi = (unsigned char)(m >> (24 - 8 * i)), bi = (unsigned char)(b >> (24 - 8 * i));
        if (mi) p[i] = (unsigned char)((p[i] & ~mi) | bi);
    }
}

/* a rectangle filled with an 8x8 pattern (anchored to the screen): whole bytes in the middle, masks
   at the ends (M7: about 10x faster than a put_bits per 24 pixels; the menu and its Yes / No box
   took 0.3 s each to fill) */
static void fill_pat(short x0, short y0, short x1, short y1, const unsigned char *pat)
{
    short y, bl, br, mid;
    unsigned char ml, mr;
    if (x1 <= x0 || y1 <= y0) return;
    bl = (short)(x0 >> 3); br = (short)((x1 - 1) >> 3); mid = (short)(br - bl - 1);
    ml = (unsigned char)(0xFF >> (x0 & 7));
    mr = (unsigned char)(0xFF << (7 - ((x1 - 1) & 7)));
    if (bl == br) ml &= mr;
    for (y = y0; y < y1; y++) {
        unsigned char *p = scr + (long)y * srb + bl, b = pat[y & 7];
        short i;
        *p = (unsigned char)((*p & ~ml) | (b & ml));
        if (bl == br) continue;
        p++;
        for (i = 0; i < mid; i++) *p++ = b;
        *p = (unsigned char)((*p & ~mr) | (b & mr));
    }
}

static const unsigned char PBLACK[8] = {255, 255, 255, 255, 255, 255, 255, 255}, PWHITE[8] = {0};
static const unsigned char PGREY[8] = {0x88, 0x22, 0x88, 0x22, 0x88, 0x22, 0x88, 0x22};

static void rfill(short x0, short y0, short x1, short y1, short black)
{
    fill_pat(x0, y0, x1, y1, black ? PBLACK : PWHITE);
}

static void icon(short x, short y, unsigned char item)
{
    short r;
    for (r = 0; r < 16; r++)
        put_bits(x, (short)(y + r), (unsigned long)item_icon[item][r] << 16, 16);
}

static const unsigned short font[10] = {    /* 3 x 5 digits, 15 bits, top row first */
    075557, 026227, 071747, 071717, 055711, 074717, 074757, 071222, 075757, 075717};

static void digit(short x, short y, short d)
{
    short r;
    for (r = 0; r < 5; r++)
        put_bits(x, (short)(y + r), (unsigned long)(((font[d] >> (12 - r * 3)) & 7) ^ 7) << 29, 3);
}

/* a slot's contents at (x, y) (22 x 22 box, framed): icon, count, durability bar */
static void draw_slot(short x, short y, const Slot *s, short frame)
{
    rfill(x, y, (short)(x + 22), (short)(y + 22), 1);
    rfill((short)(x + frame), (short)(y + frame), (short)(x + 22 - frame), (short)(y + 22 - frame), 0);
    if (!s->count) return;
    icon((short)(x + 3), (short)(y + 3), s->item);
    if (s->count > 1) {
        rfill((short)(x + 11), (short)(y + 14), (short)(x + 20), (short)(y + 21), 1);
        if (s->count >= 10) digit((short)(x + 12), (short)(y + 15), (short)(s->count / 10));
        digit((short)(x + 16), (short)(y + 15), (short)(s->count % 10));
    }
    if (item_dur[s->item] && s->dur < item_dur[s->item]) {       /* wear: a bar along the bottom */
        short w = (short)((long)s->dur * 16 / item_dur[s->item]);
        rfill((short)(x + 3), (short)(y + 18), (short)(x + 19), (short)(y + 20), 1);
        rfill((short)(x + 3), (short)(y + 18), (short)(x + 3 + w), (short)(y + 19), 0);
    }
}

void hud_init(unsigned char *screen, short rowBytes, short x, short y)
{
    scr = screen; srb = rowBytes; hx = x; hy = y;
}

#define HOTY 12                  /* the hotbar under the status row */
void hud_hotbar(void)
{
    short i, y = (short)(hy + HOTY);
    for (i = 0; i < NHOT; i++) {
        short x = (short)(hx + i * 24);
        rfill(x, y, (short)(x + 24), (short)(y + 24), 1);
        draw_slot((short)(x + 1), (short)(y + 1), &inv[i], 1);
        if (i == inv_sel) {                                       /* the selected one: a white ring */
            rfill(x, y, (short)(x + 24), (short)(y + 1), 0);
            rfill(x, (short)(y + 23), (short)(x + 24), (short)(y + 24), 0);
            rfill(x, y, (short)(x + 1), (short)(y + 24), 0);
            rfill((short)(x + 23), y, (short)(x + 24), (short)(y + 24), 0);
        }
    }
}

/* 8 x 7 status icons: full, half (left half), empty */
static const unsigned char HEART[7] = {0x6C, 0xFE, 0xFE, 0x7C, 0x38, 0x10, 0x00};
static const unsigned char HEART_O[7] = {0x6C, 0x92, 0x82, 0x44, 0x28, 0x10, 0x00};
static const unsigned char MEAT[7] = {0x0C, 0x1E, 0x3E, 0x3C, 0x70, 0xC0, 0x80};
static const unsigned char MEAT_O[7] = {0x0C, 0x12, 0x22, 0x24, 0x50, 0xC0, 0x80};
static const unsigned char BUBBLE[7] = {0x38, 0x44, 0x92, 0xA2, 0x82, 0x44, 0x38};

static void status_icon(short x, short y, const unsigned char *full, const unsigned char *empty, short level)
{
    short r;
    for (r = 0; r < 7; r++) {
        unsigned char v = level >= 2 ? full[r] : level == 1 ? (unsigned char)((full[r] & 0xF0) | (empty[r] & 0x0F)) : empty[r];
        put_bits(x, (short)(y + r), (unsigned long)(unsigned char)~v << 24, 8);   /* white on the black strip */
    }
}

void hud_status(short health, short food, short air)
{
    short i;
    rfill(hx, hy, (short)(hx + 216), (short)(hy + 10), 1);
    for (i = 0; i < 10; i++) {
        short h = (short)(health - 2 * i), f = (short)(food - 2 * (9 - i));
        status_icon((short)(hx + i * 9), (short)(hy + 1), HEART, HEART_O, (short)(h < 0 ? 0 : h > 2 ? 2 : h));
        status_icon((short)(hx + 216 - 90 + i * 9 + 1), (short)(hy + 1), MEAT, MEAT_O, (short)(f < 0 ? 0 : f > 2 ? 2 : f));
    }
    if (air < 20)
        for (i = 0; i < (air + 1) / 2; i++)
            status_icon((short)(hx + 91 + i * 9 - 4), (short)(hy + 1), BUBBLE, BUBBLE, 2);
}

void hud_message(const char *s)
{
    Rect b = qd.screenBits.bounds;
    Str255 p;
    short n = 0;
    rfill(0, (short)(hy + HOTY + 26), (short)(srb * 8), (short)(hy + HOTY + 42), 1);   /* the whole line */
    if (!s) return;
    while (s[n] && n < 60) { p[n + 1] = (unsigned char)s[n]; n++; }
    p[0] = (unsigned char)n;
    TextFont(0); TextMode(srcBic);
    MoveTo((short)(hx + 108 - StringWidth(p) / 2 + b.left), (short)(hy + HOTY + 38 + b.top));
    DrawString(p);
}

short armour_points(void)
{
    short i, p = 0;
    for (i = 0; i < NARM; i++) if (inv_arm[i].count) p = (short)(p + item_armour[inv_arm[i].item]);
    return p;
}

void armour_wear(void)
{
    short i;
    for (i = 0; i < NARM; i++)
        if (inv_arm[i].count) {
            if (inv_arm[i].dur <= 1) { inv_arm[i].item = 0; inv_arm[i].count = 0; }
            else inv_arm[i].dur--;
        }
}

/* ---- crafting ------------------------------------------------------------------------------ */

/* the recipe the grid (gw x gw items) makes, or -1 */
static short craft_match(const Slot *g, short gw)
{
    short x0 = gw, y0 = gw, x1 = -1, y1 = -1, x, y, r;
    for (y = 0; y < gw; y++)
        for (x = 0; x < gw; x++)
            if (g[y * gw + x].count) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    if (x1 < 0) return -1;
    for (r = 0; r < NRECIPES; r++) {
        const Recipe *rc = &recipes[r];
        short mir;
        if (rc->w != x1 - x0 + 1 || rc->h != y1 - y0 + 1) continue;
        for (mir = 0; mir < 2; mir++) {
            short ok = 1;
            for (y = 0; y < rc->h && ok; y++)
                for (x = 0; x < rc->w && ok; x++) {
                    const Slot *s = &g[(y0 + y) * gw + x0 + x];
                    unsigned char want = rc->key[y * rc->w + (mir ? rc->w - 1 - x : x)];
                    if ((s->count ? s->item : 0) != want) ok = 0;
                }
            if (ok) return r;
        }
    }
    return -1;
}

/* ---- the panels ---------------------------------------------------------------------------- */

#define SL 24                    /* slot pitch */
typedef struct UiSlot { short x, y; Slot *s; short kind; } UiSlot;   /* kind: 0 normal, 1 output, 2 armour piece n */
static UiSlot ui[80];
static short nui;
static Slot grid[9], hand, outslot;
static short gw, pkind;
static Furnace *pf;
static short px0, py0;
static GrafPtr qport;

static void ui_add(short x, short y, Slot *s, short kind)
{
    ui[nui].x = (short)(px0 + x); ui[nui].y = (short)(py0 + y); ui[nui].s = s; ui[nui].kind = kind;
    nui++;
}

static void text(short x, short y, const char *s)
{
    Str255 p;
    Rect b = qd.screenBits.bounds;
    p[0] = (unsigned char)strlen(s); memcpy(p + 1, s, p[0]);
    MoveTo((short)(x + b.left), (short)(y + b.top));
    DrawString(p);
}

static void draw_all(const char *title)
{
    short i;
    fill_pat(px0, py0, (short)(px0 + 288), (short)(py0 + 216), PGREY);
    rfill(px0, py0, (short)(px0 + 288), (short)(py0 + 16), 0);
    text((short)(px0 + 6), (short)(py0 + 12), title);
    for (i = 0; i < nui; i++)
        draw_slot(ui[i].x, ui[i].y, ui[i].s, (short)(ui[i].kind == 1 ? 2 : 1));
    if (pkind != P_CHEST && pkind != P_FURNACE) {                 /* the crafting arrow */
        UiSlot *o = &ui[nui - 1];
        rfill((short)(o->x - 22), (short)(o->y + 10), (short)(o->x - 6), (short)(o->y + 12), 1);
        rfill((short)(o->x - 9), (short)(o->y + 7), (short)(o->x - 7), (short)(o->y + 15), 1);
    }
}

static void draw_hand(void)
{
    short x = (short)(px0 + 258), y = (short)(py0 + 20);
    draw_slot(x, y, &hand, 2);
}

static void draw_name(const Slot *s)
{
    char buf[48];
    short n = 0;
    rfill(px0, (short)(py0 + 200), (short)(px0 + 288), (short)(py0 + 216), 0);
    if (s && s->count) {
        const char *nm = item_name[s->item];
        while (*nm && n < 30) buf[n++] = *nm++;
        if (item_dur[s->item]) {
            short d = (short)s->dur, k = 0;
            char t[8];
            buf[n++] = ' '; buf[n++] = '(';
            do { t[k++] = (char)('0' + d % 10); d /= 10; } while (d);
            while (k) buf[n++] = t[--k];
            buf[n++] = ')';
        }
    }
    buf[n] = 0;
    text((short)(px0 + 6), (short)(py0 + 212), buf);
}

static void update_output(void)
{
    if (pkind == P_INV || pkind == P_BENCH) {
        short r = craft_match(grid, gw);
        if (r >= 0) {
            outslot.item = recipes[r].out; outslot.count = recipes[r].count; outslot.dur = item_dur[recipes[r].out];
        } else
            outslot.item = 0, outslot.count = 0;
    }
}

static int fits_armour(const Slot *s, short piece)
{
    return !s->count || (IS_ARMOUR(s->item) && (s->item - 96) % 4 == piece);
}

/* a click on slot u (shift: move it across; option: one item / half) */
static void click(UiSlot *u, short shift, short option)
{
    Slot *s = u->s;
    if (u->kind == 1) {                                            /* the output: take the result */
        Slot *src = pkind == P_FURNACE ? &pf->out : &outslot;
        short i;
        if (!src->count) return;
        if (shift && pkind != P_FURNACE) {                         /* craft as many as possible */
            short guard = 0;
            while (src->count && guard++ < 64) {
                if (inv_add(src->item, src->count)) break;
                for (i = 0; i < gw * gw; i++) if (grid[i].count && !--grid[i].count) grid[i].item = 0;
                update_output();
            }
            return;
        }
        if (hand.count && (hand.item != src->item || hand.count + src->count > item_stack[src->item])) return;
        snd_play(FX_CRAFT);
        if (!hand.count) { hand = *src; } else hand.count = (unsigned char)(hand.count + src->count);
        if (pkind == P_FURNACE) { src->item = 0; src->count = 0; }
        else {
            for (i = 0; i < gw * gw; i++) if (grid[i].count && !--grid[i].count) grid[i].item = 0;
            update_output();
        }
        return;
    }
    if (u->kind >= 2 && hand.count && !fits_armour(&hand, (short)(u->kind - 2))) return;
    if (shift && s->count) {                                       /* across: pack <-> container */
        Slot t = *s;
        s->item = 0; s->count = 0;
        if (pkind == P_CHEST && (s < inv || s >= inv + NINV)) {
            short left = (short)inv_add(t.item, t.count);
            if (left) { t.count = (unsigned char)left; *s = t; }
        } else if (pkind == P_CHEST) {                             /* into the chest */
            Chest *c = 0;
            short i, n = t.count;
            for (i = 0; i < nui; i++) if (ui[i].s >= inv + NINV || ui[i].s < inv) { c = (Chest *)0; break; }
            for (i = 0; i < nui && n; i++) {
                Slot *d = ui[i].s;
                if (d >= inv && d < inv + NINV) continue;
                if (ui[i].kind) continue;
                if (!d->count) { *d = t; d->count = (unsigned char)n; n = 0; }
                else if (d->item == t.item && d->count < item_stack[t.item]) {
                    short k = item_stack[t.item] - d->count;
                    if (k > n) k = n;
                    d->count = (unsigned char)(d->count + k); n -= k;
                }
            }
            if (n) { t.count = (unsigned char)n; *s = t; }
            (void)c;
        } else {                                                   /* hotbar <-> pack */
            short from = (short)(s - inv), i, a = from < NHOT ? NHOT : 0, b = from < NHOT ? NINV : NHOT;
            short n = t.count;
            if (s < inv || s >= inv + NINV) { a = 0; b = NINV; }
            for (i = a; i < b && n; i++)
                if (inv[i].count && inv[i].item == t.item && inv[i].count < item_stack[t.item]) {
                    short k = item_stack[t.item] - inv[i].count;
                    if (k > n) k = n;
                    inv[i].count = (unsigned char)(inv[i].count + k); n -= k;
                }
            for (i = a; i < b && n; i++)
                if (!inv[i].count) { inv[i] = t; inv[i].count = (unsigned char)n; n = 0; }
            if (n) { t.count = (unsigned char)n; *s = t; }
        }
        return;
    }
    if (!hand.count) {                                             /* pick up (option: half) */
        if (!s->count) return;
        hand = *s;
        if (option && s->count > 1) {
            hand.count = (unsigned char)((s->count + 1) / 2);
            s->count = (unsigned char)(s->count - hand.count);
        } else { s->item = 0; s->count = 0; }
    } else if (!s->count || (s->item == hand.item && s->count < item_stack[s->item])) {
        short room = (short)(item_stack[hand.item] - (s->count ? s->count : 0)), n = option ? 1 : hand.count;
        if (n > room) n = room;
        if (!s->count) { *s = hand; s->count = 0; }
        s->count = (unsigned char)(s->count + n);
        hand.count = (unsigned char)(hand.count - n);
        if (!hand.count) hand.item = 0;
    } else {                                                       /* swap */
        Slot t = *s; *s = hand; hand = t;
    }
}

static UiSlot *hit(short x, short y)
{
    short i;
    for (i = 0; i < nui; i++)
        if (x >= ui[i].x && x < ui[i].x + 22 && y >= ui[i].y && y < ui[i].y + 22) return &ui[i];
    return 0;
}

void panel_run(short kind, void *container, short vx, short vy, short vw, short vh, void (*poll)(void))
{
    static const char *titles[4] = {"Inventory", "Workbench", "Chest", "Furnace"};
    short i, last_cook = -1, last_burn = -1;
    UiSlot *hover = 0;
    Point pt;
    (void)vw; (void)vh;
    GetPort(&qport);
    pkind = kind; px0 = vx; py0 = vy; nui = 0;
    pf = kind == P_FURNACE ? (Furnace *)container : 0;
    gw = kind == P_BENCH ? 3 : 2;
    memset(grid, 0, sizeof grid);
    memset(&hand, 0, sizeof hand);
    TextFont(0); TextMode(srcOr);
    /* the layout */
    if (kind == P_INV) {
        for (i = 0; i < NARM; i++) ui_add((short)(12 + (i & 1) * SL), (short)(24 + (i >> 1) * SL), &inv_arm[i], (short)(2 + i));
        for (i = 0; i < 4; i++) ui_add((short)(120 + (i & 1) * SL), (short)(24 + (i >> 1) * SL), &grid[i], 0);
    } else if (kind == P_BENCH) {
        for (i = 0; i < 9; i++) ui_add((short)(72 + (i % 3) * SL), (short)(18 + (i / 3) * SL), &grid[i], 0);
    } else if (kind == P_CHEST) {
        Chest *c = (Chest *)container;
        for (i = 0; i < 27; i++) ui_add((short)(36 + (i % 9) * SL), (short)(18 + (i / 9) * SL), &c->s[i], 0);
    } else {
        ui_add(96, 18, &pf->in, 0);
        ui_add(96, 66, &pf->fuel, 0);
    }
    for (i = 0; i < 27; i++) ui_add((short)(36 + (i % 9) * SL), (short)(98 + (i / 9) * SL), &inv[NHOT + i], 0);
    for (i = 0; i < NHOT; i++) ui_add((short)(36 + i * SL), (short)(174 + 0), &inv[i], 0);
    if (kind == P_INV) ui_add(204, 36, &outslot, 1);
    else if (kind == P_BENCH) ui_add(204, 42, &outslot, 1);
    else if (kind == P_FURNACE) ui_add(180, 42, &pf->out, 1);
    update_output();
    draw_all(titles[kind]);
    draw_hand();
    draw_name(0);
    FlushEvents(everyEvent, 0);
    ShowCursor();
    for (;;) {
        EventRecord ev;
        UiSlot *h;
        short done = 0;
        while (GetNextEvent(keyDownMask | mDownMask, &ev)) {
            if (ev.what == keyDown) {
                short k = (short)((ev.message & keyCodeMask) >> 8);
                if (k == 0x30 || k == 0x0E || k == 0x35 || k == 0x32) done = 1;   /* Tab, E, Esc */
            } else {
                short x = (short)(ev.where.h - qd.screenBits.bounds.left), y = (short)(ev.where.v - qd.screenBits.bounds.top);
                h = hit(x, y);
                if (h) {
                    HideCursor();
                    click(h, (ev.modifiers & shiftKey) != 0, (ev.modifiers & optionKey) != 0);
                    if (kind == P_INV || kind == P_BENCH) update_output();
                    for (i = 0; i < nui; i++)
                        draw_slot(ui[i].x, ui[i].y, ui[i].s, (short)(ui[i].kind == 1 ? 2 : 1));
                    draw_hand();
                    draw_name(h->s);
                    ShowCursor();
                }
            }
        }
        if (done) break;
        GetMouse(&pt);
        pt.h = (short)(pt.h - qd.screenBits.bounds.left);
        pt.v = (short)(pt.v - qd.screenBits.bounds.top);
        h = hit(pt.h, pt.v);
        if (h != hover) {
            HideCursor();
            draw_name(h ? (h->kind == 1 && pkind != P_FURNACE ? &outslot : h->s) : 0);
            ShowCursor();
        }
        hover = h;
        if (poll) poll();
        if (pf && (pf->cook / 20 != last_cook || (pf->burn > 0) != last_burn)) {   /* the furnace's progress */
            short x = (short)(px0 + 124), y = (short)(py0 + 50);
            last_cook = (short)(pf->cook / 20); last_burn = pf->burn > 0;
            HideCursor();
            rfill(x, y, (short)(x + 44), (short)(y + 6), 1);
            rfill((short)(x + 1), (short)(y + 1), (short)(x + 1 + pf->cook * 42 / COOK), (short)(y + 5), 0);
            rfill((short)(px0 + 100), (short)(py0 + 44), (short)(px0 + 114), (short)(py0 + 62), 0);
            if (pf->burn) {                                        /* a flame */
                short fh = (short)((long)pf->burn * 16 / (pf->burnmax ? pf->burnmax : 1));
                rfill((short)(px0 + 103), (short)(py0 + 61 - fh), (short)(px0 + 111), (short)(py0 + 61), 1);
            }
            for (i = 0; i < nui; i++)
                if (ui[i].s == &pf->in || ui[i].s == &pf->fuel || ui[i].s == &pf->out)
                    draw_slot(ui[i].x, ui[i].y, ui[i].s, (short)(ui[i].kind == 1 ? 2 : 1));
            ShowCursor();
        }
    }
    FlushEvents(everyEvent, 0);
    HideCursor();
    for (i = 0; i < 9; i++) give_back(&grid[i]);                   /* the grid and the hand empty */
    give_back(&hand);
}

/* ---- the Esc menu ------------------------------------------------------------------------- */

static void button(short x, short y, short w, const char *label)
{
    rfill(x, y, (short)(x + w), (short)(y + 22), 1);
    rfill((short)(x + 2), (short)(y + 2), (short)(x + w - 2), (short)(y + 20), 0);
    {
        Str255 p;
        short n = 0;
        Rect b = qd.screenBits.bounds;
        while (label[n]) { p[n + 1] = (unsigned char)label[n]; n++; }
        p[0] = (unsigned char)n;
        TextFont(0); TextMode(srcOr);
        MoveTo((short)(x + w / 2 - StringWidth(p) / 2 + b.left), (short)(y + 16 + b.top));
        DrawString(p);
    }
}

short menu_why;
short *key_trace;                  /* tests: the last key code seen (game.c points it at its debug block) */
/* the Esc menu, driven by the event queue (key-downs and clicks) */
/* How to play: the controls and the first steps, until a key or a click */
static void help_page(short vx, short vy)
{
    static const char *const lines[] = {
        "Mouse: look. Button: mine, hit. W A S D: walk.",
        "Space: jump, swim up. 1-9: hotbar. Tab: inventory.",
        "E: place, use a workbench / chest / furnace / bed,",
        "   eat, shoot the bow. Shift-E: place against one.",
        "V: view distance. M: sound. Esc: the menu.",
        "Menu: New world (N) or the Starting world (R).",
        "",
        "First steps:",
        "1. Hold the button on a tree trunk: logs.",
        "2. Tab: logs in the 2x2 grid make boards;",
        "   2 boards in a column make sticks.",
        "3. 4 boards make a workbench. Place it (E), use it (E).",
        "4. 3 boards on top, 2 sticks below: a wood pick.",
        "5. Stone gives rubble: rock tools; 8 in a ring:",
        "   a furnace. Coal over a stick: torches.",
        "6. Monsters come out in the dark: light up your home.",
        "7. Smelt iron ore; dig deep for gold and gems.",
        "8. 3 wool over 3 boards: a bed. Sleep at night.",
    };
    short i;
    EventRecord ev;
    rfill(vx, vy, (short)(vx + 288), (short)(vy + 216), 0);
    rfill(vx, vy, (short)(vx + 288), (short)(vy + 16), 1);
    TextFont(0); TextMode(srcBic);
    text((short)(vx + 6), (short)(vy + 12), "How to play");
    TextFont(3); TextSize(9); TextMode(srcOr);                 /* Geneva 9 (in ROM) */
    for (i = 0; i < (short)(sizeof lines / sizeof lines[0]); i++)
        text((short)(vx + 8), (short)(vy + 29 + i * 10), lines[i]);
    text((short)(vx + 8), (short)(vy + 211), "(a key or a click: back)");
    TextFont(0); TextSize(0);
    FlushEvents(everyEvent, 0);
    while (!GetNextEvent(mDownMask | keyDownMask, &ev)) ;
    FlushEvents(everyEvent, 0);
}

static void size_button(short x, short y, short w, short size, short fullw)
{
    static const char *sn[3] = {"View size: Normal (288)", "View size: Large (384)", "View size: Full ("};
    char t[40];
    short n = 0;
    const char *p = sn[size];
    while (*p) t[n++] = *p++;
    if (size == 2) { t[n++] = (char)('0' + fullw / 100); t[n++] = (char)('0' + fullw / 10 % 10); t[n++] = (char)('0' + fullw % 10); t[n++] = ')'; }
    t[n] = 0;
    button(x, y, w, t);
}

/* a framed box with a question, "This world will be replaced." and Yes / No (No is the default:
   Return, Esc and N say no, Y says yes). 1 for yes. */
static short confirm(short vx, short vy, const char *q)
{
    short x = (short)(vx + 24), y = (short)(vy + 58), w = 240, yes = -1;
    Rect b = qd.screenBits.bounds;
    Str255 p;
    HideCursor();
    rfill(x, y, (short)(x + w), (short)(y + 100), 1);
    rfill((short)(x + 2), (short)(y + 2), (short)(x + w - 2), (short)(y + 98), 0);
    TextFont(0); TextMode(srcOr);
    p[0] = (unsigned char)strlen(q); memcpy(p + 1, q, p[0]);
    text((short)(x + w / 2 - StringWidth(p) / 2), (short)(y + 24), q);
    button((short)(x + 22), (short)(y + 62), 88, "Yes (Y)");
    rfill((short)(x + 126), (short)(y + 58), (short)(x + 222), (short)(y + 88), 1);   /* the default: No */
    rfill((short)(x + 129), (short)(y + 61), (short)(x + 219), (short)(y + 85), 0);
    button((short)(x + 130), (short)(y + 62), 88, "No (N)");
    TextFont(3); TextSize(9);                  /* Geneva 9 last: on the Plus floppy (its System trimmed */
    p[0] = 28; memcpy(p + 1, "This world will be replaced.", 28);   /*  of fonts) it takes 0.3 s */
    text((short)(x + w / 2 - StringWidth(p) / 2), (short)(y + 42), "This world will be replaced.");
    TextFont(0); TextSize(0);
    FlushEvents(everyEvent, 0);
    ShowCursor();
    while (yes < 0) {
        EventRecord ev;
        if (!GetNextEvent(mDownMask | keyDownMask, &ev)) continue;
        if (ev.what == keyDown) {
            short k = (short)((ev.message & keyCodeMask) >> 8);
            if (key_trace) *key_trace = (short)(k | 0x200);
            if (k == 0x10) yes = 1;                                     /* Y */
            else if (k == 0x2D || k == 0x35 || k == 0x32 || k == 0x24 || k == 0x4C) yes = 0;   /* N, Esc, Return, Enter */
        } else {
            short h = (short)(ev.where.h - b.left), v = (short)(ev.where.v - b.top);
            if (v >= y + 62 && v < y + 84) {
                if (h >= x + 22 && h < x + 110) yes = 1;
                else if (h >= x + 130 && h < x + 218) yes = 0;
            }
        }
    }
    FlushEvents(everyEvent, 0);
    HideCursor();
    return yes;
}

/* the menu's buttons, top to bottom */
enum { B_BACK, B_DIST, B_SIZE, B_HELP, B_NEW, B_START, B_QUIT, NBUT };
#define MB_Y(i) ((short)(vy + 24 + (i) * 26))

short menu_run(short vx, short vy, short *dist, short *size, short fullw)
{
    static const char *dn[3] = {"View distance: Short (8)", "View distance: Normal (10)", "View distance: Far (12)"};
    short bx = (short)(vx + 44), bw = 200, res = -1, redraw = 1;
    px0 = vx; py0 = vy;
    ShowCursor();
    while (res < 0) {
        EventRecord ev;
        short act = -1;
        if (redraw) {
            FlushEvents(everyEvent, 0);         /* (before drawing: keys pressed while it draws count) */
            HideCursor();
            fill_pat(vx, vy, (short)(vx + 288), (short)(vy + 216), PGREY);
            rfill(vx, vy, (short)(vx + 288), (short)(vy + 16), 0);
            TextFont(0); TextMode(srcOr);
            text((short)(vx + 6), (short)(vy + 12), "Paused");
            button(bx, MB_Y(B_BACK), bw, "Back to the game (Esc)");
            button(bx, MB_Y(B_DIST), bw, dn[(*dist - 8) / 2]);
            size_button(bx, MB_Y(B_SIZE), bw, *size, fullw);
            button(bx, MB_Y(B_HELP), bw, "How to play (H)");
            button(bx, MB_Y(B_NEW), bw, "New world (N)");
            button(bx, MB_Y(B_START), bw, "Starting world (R)");
            button(bx, MB_Y(B_QUIT), bw, "Save and quit (Q)");
            if (redraw == 1) {          /* the cursor starts on the first button */
                short h = (short)(bx + bw / 2 + qd.screenBits.bounds.left), v = (short)(MB_Y(B_BACK) + 11 + qd.screenBits.bounds.top);
                *(volatile short *)0x828 = v; *(volatile short *)0x82A = h;
                *(volatile short *)0x82C = v; *(volatile short *)0x82E = h;
                *(volatile unsigned char *)0x8CE = *(volatile unsigned char *)0x8CF;
            }
            ShowCursor();
            redraw = 0;
        }
        if (!GetNextEvent(mDownMask | keyDownMask, &ev)) continue;
        if (ev.what == keyDown) {
            short k = (short)((ev.message & keyCodeMask) >> 8);
            if (key_trace) *key_trace = (short)(k | 0x100);
            act = k == 0x35 || k == 0x32 ? B_BACK : k == 0x0C ? B_QUIT : k == 0x04 ? B_HELP : k == 0x09 ? B_DIST
                : k == 0x01 ? B_SIZE : k == 0x2D ? B_NEW : k == 0x0F ? B_START : -1;   /* Esc (` on the Plus) Q H V S N R */
            menu_why = (short)(k * 10 + act + 1);
        } else if (ev.what == mouseDown) {
            short h = (short)(ev.where.h - qd.screenBits.bounds.left), v = (short)(ev.where.v - qd.screenBits.bounds.top), i;
            if (h >= bx && h < bx + bw)
                for (i = 0; i < NBUT; i++)
                    if (v >= MB_Y(i) && v < MB_Y(i) + 22) act = i;
            menu_why = (short)(9000 + act + 1);
        }
        switch (act) {
        case B_BACK: res = M_BACK; break;
        case B_QUIT: res = M_QUIT; break;
        case B_DIST:
            *dist = *dist == 12 ? 8 : (short)(*dist + 2);
            HideCursor();
            button(bx, MB_Y(B_DIST), bw, dn[(*dist - 8) / 2]);
            ShowCursor();
            break;
        case B_SIZE:
            *size = (short)((*size + 1) % 3);
            HideCursor();
            size_button(bx, MB_Y(B_SIZE), bw, *size, fullw);
            ShowCursor();
            break;
        case B_HELP:
            HideCursor();
            help_page(vx, vy);
            ShowCursor();                       /* (the redraw hides and shows it again) */
            redraw = 2;
            break;
        case B_NEW:
        case B_START:
            if (confirm(vx, vy, act == B_NEW ? "Make a new world?" : "Go back to the starting world?"))
                res = act == B_NEW ? M_NEWWORLD : M_START;
            else {
                ShowCursor();
                redraw = 2;
            }
            break;
        }
    }
    FlushEvents(everyEvent, 0);
    HideCursor();
    return res;
}
