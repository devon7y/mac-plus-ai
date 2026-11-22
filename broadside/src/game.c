/* Broadside rules: see game.h. */
#include "game.h"
#include <string.h>

const uint8_t kShipLen[NSHIP] = {5, 4, 3, 3, 2};

#define R10(x) x, x, x, x, x, x, x, x, x, x
const uint8_t kRow[NCELL] = {R10(0), R10(1), R10(2), R10(3), R10(4), R10(5), R10(6), R10(7), R10(8), R10(9)};
#define C10 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
const uint8_t kCol[NCELL] = {C10, C10, C10, C10, C10, C10, C10, C10, C10, C10};

uint32_t bs_rand(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

uint16_t bs_below(uint32_t *s, uint16_t n)
{
    return (uint16_t)(((uint32_t)(uint16_t)(bs_rand(s) >> 16) * n) >> 16);
}

int place_on_board(int len, const Place *p)
{
    if (p->r > 9 || p->c > 9 || p->dir > 1)
        return 0;
    return p->dir ? p->r + len <= 10 : p->c + len <= 10;
}

int ship_cells(const Place *p, int len, uint8_t *cells)
{
    int i, step = p->dir ? 10 : 1, at = p->r * 10 + p->c;
    for (i = 0; i < len; i++, at += step)
        cells[i] = (uint8_t)at;
    return len;
}

int fleet_valid(const Place *ships, uint8_t rules)
{
    uint8_t occ[NCELL];
    uint8_t cells[5];
    int i, j, n;
    memset(occ, 0, sizeof occ);
    for (i = 0; i < NSHIP; i++) {
        if (!place_on_board(kShipLen[i], &ships[i]))
            return 0;
        n = ship_cells(&ships[i], kShipLen[i], cells);
        for (j = 0; j < n; j++) {
            if (occ[cells[j]])
                return 0;
            occ[cells[j]] = (uint8_t)(i + 1);
        }
    }
    if (rules & RULE_NOTOUCH) {
        for (i = 0; i < NCELL; i++) {
            int r = kRow[i], c = kCol[i], dr, dc;
            if (!occ[i])
                continue;
            for (dr = -1; dr <= 1; dr++)
                for (dc = -1; dc <= 1; dc++) {
                    int rr = r + dr, cc = c + dc;
                    if (rr < 0 || rr > 9 || cc < 0 || cc > 9)
                        continue;
                    if (occ[rr * 10 + cc] && occ[rr * 10 + cc] != occ[i])
                        return 0;
                }
        }
    }
    return 1;
}

int fleet_build(Fleet *f, const Place *ships, uint8_t rules)
{
    uint8_t cells[5];
    int i, j;
    memset(f, 0, sizeof *f);
    if (!fleet_valid(ships, rules))
        return 0;
    for (i = 0; i < NSHIP; i++) {
        f->ship[i] = ships[i];
        ship_cells(&ships[i], kShipLen[i], cells);
        for (j = 0; j < kShipLen[i]; j++)
            f->owner[cells[j]] = (uint8_t)(i + 1);
    }
    f->afloat = NSHIP;
    return 1;
}

void fleet_random(Place *ships, uint8_t rules, uint32_t *rng)
{
    int i;
    for (;;) {
        for (i = 0; i < NSHIP; i++) {
            int len = kShipLen[i];
            ships[i].dir = (uint8_t)(bs_rand(rng) >> 31);
            if (ships[i].dir) {
                ships[i].r = (uint8_t)bs_below(rng, (uint16_t)(11 - len));
                ships[i].c = (uint8_t)bs_below(rng, 10);
            } else {
                ships[i].r = (uint8_t)bs_below(rng, 10);
                ships[i].c = (uint8_t)bs_below(rng, (uint16_t)(11 - len));
            }
        }
        if (fleet_valid(ships, rules))
            return;
    }
}

void know_init(Know *k, uint8_t rules)
{
    memset(k, 0, sizeof *k);
    k->rules = rules;
}

int fire(Fleet *f, Know *k, int cell, int *sunkShip)
{
    int s, i;
    uint8_t cells[5];
    if (cell < 0 || cell >= NCELL || f->shot[cell])
        return SHOT_BAD;
    f->shot[cell] = 1;
    k->shots++;
    s = f->owner[cell];
    if (!s) {
        k->cell[cell] = K_MISS;
        return SHOT_MISS;
    }
    s--;
    k->cell[cell] = K_HIT;
    if (++f->hits[s] < kShipLen[s])
        return SHOT_HIT;
    /* sunk: its cells are revealed, and under RULE_NOTOUCH the water around it is known */
    f->afloat--;
    k->sunk[s] = 1;
    ship_cells(&f->ship[s], kShipLen[s], cells);
    for (i = 0; i < kShipLen[s]; i++)
        k->cell[cells[i]] = K_SUNK;
    if (k->rules & RULE_NOTOUCH) {
        for (i = 0; i < kShipLen[s]; i++) {
            int r = kRow[cells[i]], c = kCol[cells[i]], dr, dc;
            for (dr = -1; dr <= 1; dr++)
                for (dc = -1; dc <= 1; dc++) {
                    int rr = r + dr, cc = c + dc;
                    if (rr < 0 || rr > 9 || cc < 0 || cc > 9)
                        continue;
                    if (k->cell[rr * 10 + cc] == K_UNKNOWN)
                        k->cell[rr * 10 + cc] = K_EMPTY;
                }
        }
    }
    if (sunkShip)
        *sunkShip = s;
    return SHOT_SUNK;
}
