/*
 * Broadside: the rules (fleets, shots, what each side knows). Plain C, no Toolbox: the same code
 * runs in the host simulator (host/sim.c) and in the Mac app.
 *
 * Board: 10 x 10 cells, cell = row * 10 + col (row 0 = top, A..J are the columns).
 * Fleet: five ships of lengths 5, 4, 3, 3, 2.
 */
#ifndef BS_GAME_H
#define BS_GAME_H

#include <stdint.h>

#define NSHIP 5
#define NCELL 100

/* rules (bit set) */
#define RULE_NOTOUCH 1      /* ships may not touch, not even at a corner */
#define RULE_SALVO   2      /* one shot per surviving ship each turn */

/* what a shooter knows about a cell of the enemy board */
enum { K_UNKNOWN = 0, K_MISS = 1, K_HIT = 2, K_SUNK = 3, K_EMPTY = 4 };
/* K_SUNK: part of a sunk ship (its cells are revealed when it sinks).
   K_EMPTY: not shot, but known to be water (next to a sunk ship under RULE_NOTOUCH). */

extern const uint8_t kShipLen[NSHIP];        /* 5 4 3 3 2 */
extern const uint8_t kRow[NCELL], kCol[NCELL];

typedef struct {
    uint8_t r, c, dir;      /* top-left cell; dir 0 = horizontal (along a row), 1 = vertical */
} Place;

typedef struct {
    Place ship[NSHIP];
    uint8_t owner[NCELL];   /* 0 = water, else ship index + 1 */
    uint8_t hits[NSHIP];
    uint8_t shot[NCELL];    /* 1 once fired at */
    uint8_t afloat;         /* ships not yet sunk */
} Fleet;

typedef struct {
    uint8_t cell[NCELL];    /* K_* */
    uint8_t sunk[NSHIP];    /* 1 once that ship is sunk */
    uint8_t rules;
    uint8_t shots;          /* shots fired so far */
} Know;

/* shot results */
enum { SHOT_MISS = 0, SHOT_HIT = 1, SHOT_SUNK = 2, SHOT_BAD = 3 };

uint32_t bs_rand(uint32_t *s);                 /* xorshift32 (state must be nonzero) */
uint16_t bs_below(uint32_t *s, uint16_t n);    /* uniform 0..n-1 (n <= 65535) */

/* 1 if ship i at p fits on the board */
int place_on_board(int len, const Place *p);
/* 1 if all five placements are on the board, don't overlap and (RULE_NOTOUCH) don't touch */
int fleet_valid(const Place *ships, uint8_t rules);
/* fills owner[] etc.; returns fleet_valid() */
int fleet_build(Fleet *f, const Place *ships, uint8_t rules);
/* a uniformly random valid fleet (rejection sampling) */
void fleet_random(Place *ships, uint8_t rules, uint32_t *rng);

void know_init(Know *k, uint8_t rules);
/* fire at cell: updates the fleet and the shooter's knowledge. Returns SHOT_*; *sunkShip = the
   ship index when SHOT_SUNK. */
int fire(Fleet *f, Know *k, int cell, int *sunkShip);
/* the cells of ship i (returns its length) */
int ship_cells(const Place *p, int len, uint8_t *cells);

#endif
