/* Orbit Duel: things main.c shares with link.c, and the debug block the MAME scripts read. */
#ifndef MAIN_H
#define MAIN_H

#include <Quickdraw.h>
#include <stdint.h>
#include "sim.h"

enum { K_ATTRACT = 0, K_SOLO = 1, K_HOTSEAT = 2, K_NET = 3 };

/* commands a test script writes into dbg.cmd (the app clears it when it acts) */
enum { CMD_SOLO = 1, CMD_HOTSEAT = 2, CMD_ATALK = 3, CMD_SERIAL = 4, CMD_TITLE = 5, CMD_ABOUT = 6,
       CMD_CLOSE = 7, CMD_QUIT = 8, CMD_INVITE = 9 };

#define DBG_MAGIC 0x4F44554CUL      /* 'ODUL' */

/* Field offsets are fixed: the Lua scripts in duel/tests read them. All big-endian. */
typedef struct Dbg {
    uint32_t magic, version, notmagic;  /*   0 */
    int32_t screen;                     /*  12: 1 title, 2 game, 3 link screen */
    int32_t kind;                       /*  16: K_* of the last game */
    int32_t frames;                     /*  20: frames drawn in this game */
    int32_t frame_ticks;                /*  24: Ticks spent in those frames (work, not waiting) */
    int32_t tick;                       /*  28: sim tick */
    int16_t score0, score1;             /*  32, 34 */
    int16_t phase, round;               /*  36, 38 */
    int32_t matches;                    /*  40 */
    int32_t cmd, arg;                   /*  44, 48: written by the test script */
    int32_t autopilot;                  /*  52: written by the test script: computer flies our ship */
    int32_t ai_level;                   /*  56 */
    int32_t games, about;               /*  60, 64 */
    int32_t freemem;                    /*  68 */
    Rect bounds;                        /*  72: screenBits.bounds */
    int16_t view_left, view_top;        /*  80, 82: arena on the screen */
    /* link */
    int32_t link_kind;                  /*  84: 0 none, 1 AppleTalk, 2 serial */
    int32_t link_state;                 /*  88: link.c's LS_* */
    int32_t role;                       /*  92: 0 host (Needle), 1 guest (Wedge), -1 not yet */
    int32_t csum_ok, csum_bad;          /*  96, 100: state checksums compared with the other Mac */
    int32_t last_csum;                  /* 104: our checksum at the last compared tick */
    int32_t remote_tick;                /* 108: inputs received from the other Mac up to here */
    int32_t pk_rx, pk_tx, pk_bad;       /* 112, 116, 120: packets received, sent, rejected (CRC) */
    int32_t waits;                      /* 124: frames spent waiting for the other Mac's input */
    int32_t err;                        /* 128: last link error (OSErr) */
    int32_t mpp_open;                   /* 132: result of opening .MPP */
    int32_t at_version;                 /* 136: AppleTalk (.MPP) version, from SysEnvirons */
    int32_t node, net;                  /* 140, 144: our AppleTalk address */
    int32_t socket;                     /* 148 */
    int32_t nbp_reg;                    /* 152: result of registering our name */
    int32_t nbp_found;                  /* 156: names found by the last lookup (all, incl. ours) */
    int32_t nbp_lookups;                /* 160 */
    int32_t self_seen;                  /* 164: our own name came back from a lookup */
    int32_t ddp_loop;                   /* 168: datagrams we sent to our own socket and got back */
    int32_t spconfig;                   /* 172: SPConfig and PortBUse at startup (byte, byte) */
    int32_t opponents;                  /* 176: entries in the opponent list */
    int32_t seed;                       /* 180: the game seed (same on both Macs) */
    int32_t delay;                      /* 184: input delay in steps */
    int32_t serial_open;                /* 188: result of opening the modem port */
    int32_t self_test;                  /* 192: AppleTalk self-test bits (see link.c) */
    int32_t sum_hist[8];                /* 196: checksums at ticks 30, 60, ... (for comparing runs) */
    int32_t title_frames;               /* 228: frames of the title's demo */
    int32_t front_ok;                   /* 232: our window is in front */
    int16_t p0_angle, p0_thrust;        /* 236, 238: the Needle */
    int16_t p0_torps, p0_hyper;         /* 240, 242: torpedoes in flight, hyperspace steps left */
    int32_t p0_shots, p0_jumps;         /* 244, 248: torpedoes fired, jumps (this game) */
    int16_t p1_angle, in0;              /* 252, 254: the Wedge; player 1's input byte */
} Dbg;
extern Dbg dbg;

/* main.c */
void keys_poll(void);
int esc_pressed(void);
int key_hit(int code);
void present_glow(short f);
unsigned char local_input(void);
short poll_events(Point *click);
short front_is_ours(void);
short steps_due(long tick);
void pace_reset(long tick);
void start_net_game(unsigned long seed, short ship);
Sim *game_sim(void);
void hud_note_set(short id, const unsigned char *text);
void set_screen_link(void);
void back_to_title(void);
void full_redraw(void);
short ui_take_dirty(void);

#endif
