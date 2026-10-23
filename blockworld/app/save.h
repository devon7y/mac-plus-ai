/* Blockworld: saving and loading the world (one file next to the game). */
#ifndef BW_SAVE_H
#define BW_SAVE_H

#include "inv.h"

typedef struct SaveState {
    long seed;
    long px, py, pz;              /* player feet (8.8) */
    short yaw, pitch;
    long time;                    /* time of day (ticks of the day cycle) */
    short health, food;
    short sel;
    short bed, bx, by, bz;        /* the respawn point (a bed) */
    long sx, sy, sz;              /* the world spawn */
    short dist;                   /* the view distance */
} SaveState;
/* (the inventory, armour, chests and furnaces are saved from inv.h's arrays) */

/* the player's world (written by save_world) and the shipped starting world (read only) */
extern unsigned char save_name[], default_name[];
extern unsigned char temp_name[];   /* "Blockworld Saving": a save being written (renamed when done) */
extern unsigned long save_t[6];   /* tests: ticks: load read, unpack, world; save pack, LZ, write */

/* write "Blockworld World" (format 7, LZ-packed; see save.c) or read a world file: the arrays, the
   blocks and the renderer's segment lists (prepare, e.g. the torch light, is called just before the
   segment lists are built). 0 on success, else the File Manager error, -1 (not a Blockworld save or
   damaged), -2 (saved by an incompatible build) or memFullErr. A failed load may have changed the
   arrays and blocks only when the file passed its checksum but its world part is wrong. */
short save_world(const SaveState *st);
short load_world(SaveState *st, const unsigned char *name, void (*prepare)(void));
unsigned long save_sum(const unsigned char *p, long n);

#endif
