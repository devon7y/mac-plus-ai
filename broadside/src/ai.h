/*
 * Broadside: the Mac's targeting. Plain C shared by the host simulator and the Mac app.
 *
 *   Easy    random hunting, then finishing off hits (neighbours, then along the line).
 *   Normal  hunts with a probability map from counting each ship's possible placements on its
 *           own (the classic "density" map) plus a lot of noise; finishes hits like Easy.
 *   Hard    the probability that each cell holds a ship over all fleet placements consistent with
 *           every hit, miss and sunk ship so far: exact enumeration when the number of
 *           placements is small enough, otherwise importance sampling (each sample builds a
 *           whole fleet, covering the hits first; Rao-Blackwellised on the last ship); parity-
 *           aware hunting while no hit is waiting to be finished (cells off the lattice of the
 *           smallest ship left lose 1/16 of their weight).
 *
 * Use: ai_init once per game; after each change of knowledge call ai_new_state, then ai_work
 * (in slices, from idle time if you like) until it returns 1, then ai_choose. ai_heat gives the
 * map for display at any time.
 */
#ifndef BS_AI_H
#define BS_AI_H

#include "game.h"

enum { AI_EASY = 0, AI_NORMAL = 1, AI_HARD = 2 };

/* flags (tuning; the defaults are what the game uses) */
#define AIF_PARITY   1      /* Hard: prefer the parity lattice of the smallest ship left */
#define AIF_EXACT    4      /* Hard: exact enumeration when cheap */
#define AIF_DEFAULT  (AIF_PARITY | AIF_EXACT)

typedef struct {
    uint16_t Br[10], Bc[10];  /* cells no remaining ship may use (rows / columns bitboards) */
    uint16_t U[10];           /* hits not yet covered by a ship of this sample (rows) */
    uint8_t cnt[6];           /* remaining ships per length (2..5) */
    uint8_t nrem;             /* remaining ships */
    uint8_t npath;
    uint8_t path[NSHIP][4];   /* placed ships: len, dir, r, c */
} AISample;

typedef struct {
    uint8_t level, flags;
    uint32_t rng;
    uint16_t minSamples;      /* Hard: samples before a move (default AI_HARD_SAMPLES) */
    uint16_t exactCap;        /* Hard: most last-ship enumerations an exact count may take */
    uint16_t normalNoise;     /* Normal: noise amplitude in 1/256 of the best score */
    /* the analysis of the current knowledge */
    uint32_t acc[NCELL];      /* (relative) weight of fleets with a ship on each cell */
    uint32_t total;           /* (relative) weight of all fleets */
    int16_t ref;              /* Hard: log2 reference of the weights, 1/256 units */
    uint16_t samples;         /* Hard: samples so far (incl. dead ones) */
    uint16_t good;            /* Hard: samples that made a whole fleet */
    uint8_t exact;            /* 1: acc/total are exact counts */
    uint8_t done;             /* 1: analysis finished for this knowledge (ai_work returns 1) */
    uint8_t tried;            /* Hard: exact enumeration was tried */
    uint8_t hunting;          /* no hit waiting to be finished */
    uint8_t notouch;          /* RULE_NOTOUCH */
    int16_t maxPrefix;        /* Hard: largest log2 weight of a sample before its last ship */
    uint32_t lastCount;       /* Hard: exact number of consistent fleets when exact (labelled) */
    uint32_t work;            /* work units (last-ship enumerations + samples) for this state */
    /* per state (from the knowledge) */
    uint16_t H[10], Hc[10];   /* unsunk hits, rows / columns */
    AISample base;
} AI;

#define AI_HARD_SAMPLES 80

void ai_init(AI *a, int level, uint32_t seed);
/* the knowledge changed (new shots resolved): start a new analysis */
void ai_new_state(AI *a, const Know *k);
/* do up to `samples` units of work (Hard; Easy and Normal finish at once). Returns 1 when the
   analysis is ready for ai_choose (Hard keeps improving it if called again). */
int ai_work(AI *a, const Know *k, uint16_t samples);
/* pick n distinct targets (n > 1 in Salvo games); returns how many it could pick */
int ai_choose(AI *a, const Know *k, uint8_t *cells, int n);
/* the cell the Mac would pick now, ignoring random tie-breaks (for display) */
int ai_best(const AI *a, const Know *k);
/* the analysis as 1..255 per possible unknown cell (least to most likely, stretched), 0 = known or
   impossible */
void ai_heat(const AI *a, const Know *k, uint8_t *heat);

/* The Mac's own fleet: random, avoiding cells the opponent tends to shoot early (`early`, 100
   weights, may be NULL) and clusters of ships. */
void ai_place_fleet(Place *ships, uint8_t rules, uint32_t *rng, const uint16_t *early);

#endif
