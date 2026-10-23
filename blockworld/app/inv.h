/* Blockworld: the inventory, containers (chests, furnaces), crafting and the screens that show
 * them (the hotbar strip under the view, the inventory / workbench / chest / furnace panels). */
#ifndef BW_INV_H
#define BW_INV_H

#include "items.h"

#define NHOT 9
#define NINV 36                  /* 0..8 the hotbar, 9..35 the pack */
#define NARM 4                   /* helmet, tunic, leggings, boots */

typedef struct Slot {
    unsigned char item, count;
    unsigned short dur;          /* uses left (tools, armour, the bow) */
} Slot;

extern Slot inv[NINV];
extern Slot inv_arm[NARM];
extern short inv_sel;            /* the selected hotbar slot */
#define inv_hot inv

/* the item a block drops when mined with the item held (0: nothing), how many, and the mining
   time in 1/20 s (0: can't be mined) */
short mine_time(int block, int tool);
short mine_drop(int block, int tool, short *count);

int inv_add(unsigned char item, int n);      /* returns how many did not fit */
void inv_use_tool(void);                     /* the selected tool wears by one use */

/* ---- containers ---------------------------------------------------------------------------- */
#define NCHESTS 48
#define NFURN 24
typedef struct Chest { short x, y, z, used; Slot s[27]; } Chest;
typedef struct Furnace {
    short x, y, z, used;
    Slot in, fuel, out;
    short burn, burnmax, cook;   /* fuel ticks left (and of the whole fuel item), cooking progress */
} Furnace;
extern Chest chests[NCHESTS];
extern Furnace furns[NFURN];
Chest *chest_at(short x, short y, short z, int create);
Furnace *furn_at(short x, short y, short z, int create);
/* a chest or furnace broken: its contents go into the inventory (what doesn't fit is lost) */
void container_broken(short x, short y, short z);
void furnaces_tick(void);                    /* every furnace, one 1/20 s step */

/* ---- drawing ------------------------------------------------------------------------------- */
void hud_init(unsigned char *screen, short rowBytes, short x, short y);   /* the status row's top-left */
void hud_hotbar(void);
void hud_status(short health, short food, short air);   /* 0..20 each (air: 0..20, 20 hides it) */
/* a message line under the hotbar (0: clear) */
void hud_message(const char *s);
/* the Esc menu over the view: returns 0 resume, 1 save and quit; *dist cycles 8 / 10 / 12, *size
   0 Normal / 1 Large / 2 Full (fullw pixels wide) */
/* the Esc menu: M_BACK, M_QUIT (save and quit), M_NEWWORLD or M_START (both confirmed first) */
enum { M_BACK, M_QUIT, M_NEWWORLD, M_START };
short menu_run(short vx, short vy, short *dist, short *size, short fullw);
/* armour points worn (of 20), and wear on each piece */
short armour_points(void);
void armour_wear(void);

/* the panels: the inventory (2x2 crafting), a workbench (3x3), a chest, a furnace */
enum { P_INV, P_BENCH, P_CHEST, P_FURNACE };
/* run a panel inside the view rectangle (screen pixels) until Tab / Esc / E closes it; the
   cursor is shown meanwhile. poll() is called each loop (furnaces keep cooking). */
void panel_run(short kind, void *container, short vx, short vy, short vw, short vh, void (*poll)(void));

#endif
