/*
 * Broadside for the Mac Plus (System 6, 1 MB) and SE: shared declarations of the app's modules.
 *   main.c   Toolbox setup, menus, the event loop, the flow of a game, test mailbox
 *   board.c  layout and drawing (grids, ships, markers, heat map, buttons, messages)
 *   fx.c     animations (shells, splashes, explosions, fire, sinking)
 *   sound.c  one-shot sound effects mixed into the Plus's sound buffer
 *   speech.c MacinTalk 1.x when it is in the System Folder
 *   stats.c  options, record and statistics in "Broadside Prefs"
 */
#ifndef BROADSIDE_H
#define BROADSIDE_H

#include <Multiverse.h>      /* (every Toolbox header of the Multiversal Interfaces) */
#include <string.h>
#include "../src/game.h"
#include "../src/ai.h"

#define TICKS (*(volatile unsigned long *)0x16A)

/* ---- the game ------------------------------------------------------------------------------- */

enum { ST_PLACE, ST_PLAYER, ST_BUSY, ST_OVER };
enum { ME = 0, MAC = 1 };              /* grid 0: my fleet (the Mac shoots here); grid 1: the Mac's */

typedef struct {
    short state;
    uint8_t rules, level;
    Place mine[NSHIP];                 /* my placement */
    Fleet fleet[2];                    /* [ME] my fleet, [MAC] the Mac's */
    Know know[2];                      /* [ME] what the Mac knows of my board, [MAC] what I know of its */
    AI ai;
    uint32_t rng;
    uint8_t sel[NCELL];                /* Salvo: my selected targets */
    short nsel;
    short aim;                         /* keyboard aiming cursor on the Mac's grid (-1 none) */
    short winner;                      /* -1, ME, MAC */
    short myShots, macShots;
    short firstShots[25];              /* my first 25 shots (for the Mac's placement learning) */
    short nFirst;
} Game;

extern Game g;
extern WindowPtr gWin;

/* options (stats.c keeps them) */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t level, salvo, notouch, sound, think, speech;
    struct {
        uint16_t games, wins, losses, best;    /* best = fewest of my shots in a win (0 = none) */
        uint32_t myShotsWon, macShotsWon;      /* sums over won games (mine) and lost games (the Mac's) */
    } st[3];
    uint16_t early[NCELL];             /* where I tend to shoot first (decaying) */
} Prefs;
extern Prefs gPrefs;
void prefs_load(void);
void prefs_save(void);
void stats_record(int won, int myShots, int macShots, int level, const short *first, int nfirst);

/* board.c */
extern short CS;                       /* cell size in pixels */
extern Rect gGridR[2];                 /* the 10 x 10 cells of each grid (outer edge) */
extern Rect gMsgR, gBarR, gThinkR;
void layout(void);
void cell_rect(int grid, int cell, Rect *r);
int cell_at(int grid, Point p);        /* -1 if outside */
void draw_all(void);
void draw_cell(int grid, int cell);
void draw_cells_around(int grid, int cell);
void draw_ship_cells(int grid, int ship);   /* its block and the ring around it */
void draw_grid_cells(int grid);
void draw_status(void);                /* fleet silhouettes under the grids */
void draw_bar(void);                   /* buttons / turn text at the bottom */
void say(const char *text, int mood, int speak);  /* the Mac's message (mood: 0 plain, 1 happy, 2 sad, 3 smug) */
const char *ship_name(int i);
void draw_titles_now(void);
void place_rect(int grid, const Place *p, int len, Rect *r);
void draw_message(void);
void draw_think(void);                 /* the heat map and the AI's status line */
extern uint8_t gHeat[NCELL];
extern short gHeatTarget;              /* the Mac's next target (-1 none) */
extern short gFlame;                   /* fire animation frame */
void draw_flames(void);                /* redraw every burning cell with the current frame */
int button_at(Point p);                /* 0 none, else BTN_* */
void track_button(int b);
enum { BTN_NONE, BTN_RANDOM, BTN_START, BTN_NEW, BTN_ROTATE };
extern Rect gBtnR[5];
extern short gBtnShown[5];
void draw_reticle(int grid, int cell, int on);
void draw_drag(Rect *r);               /* XOR outline while dragging a ship */

/* fx.c */
void fx_shot(int grid, int cell, int result);       /* shell, then splash or explosion */
void fx_sink(int grid, int ship);
void fx_idle(void);                                  /* flames, from the idle loop */

/* sound.c */
enum { SND_FIRE, SND_SPLASH, SND_EXPLOSION, SND_HORN, SND_WIN, SND_LOSE, SND_CLICK, SND_PING };
short snd_init(void);
void snd_play(int which);
void snd_quit(void);
void snd_hold(int on);
short snd_busy(void);

/* speech.c */
short speech_init(void);              /* 1 if MacinTalk is there */
short speech_available(void);
void speech_say(const char *text);
void speech_quit(void);

/* main.c */
void pump(short ticks);                /* wait, keeping the Mac responsive (no new moves) */
void quit_app(void);

/* debug block for the MAME tests (found by its magic tag, 2-byte steps) */
#define DBG_MAGIC 0x42524453UL         /* 'BRDS' */
struct DebugBlock {                    /* (byte offsets, as the Lua tests in tests/ read them) */
    uint32_t magic, version, notMagic;           /* 0, 4, 8 */
    uint32_t state;                    /* 12: g.state + 1 (0 = starting; 2 = my turn) */
    uint32_t loops;                    /* 16: event loop iterations */
    uint32_t cmd, arg[3], cmdDone, cmdResult;    /* 20, 24..32, 36, 40: mailbox (cmd is cleared when done) */
    uint32_t myShots, macShots, winner;          /* 44, 48, 52 (winner: 0 none, 1 me, 2 the Mac) */
    uint32_t aiTicksLast, aiTicksMax, aiMoves, aiTicksSum;   /* 56..68: the Mac's moves (analysis + choice) */
    uint32_t aiSamples, aiExact;                 /* 72, 76: its last move */
    uint32_t freeMem, speech;                    /* 80, 84 */
    uint32_t bench[32];                          /* 88: bench results; [29] draw_all calls, [30] thinking on */
    uint32_t anchor;                             /* 216: address of ai_work (for PC profiles) */
};
extern struct DebugBlock dbg;

#endif
