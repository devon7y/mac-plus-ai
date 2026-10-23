/* Blockworld world: blocks, generator (port of engine/world.py) and column segment lists. */
#ifndef BW_WORLD_H
#define BW_WORLD_H

#define WX 128          /* world size in x and z */
#define WY 48           /* world height */
#define SEA 27
#define BORDER 24       /* open sea around the world in the column table (> view distance) */
#define PWX (WX + 2 * BORDER)
#define CWX (WX / 4)    /* coarse map: highest surface per 4 x 4 blocks (distant hills) */

enum { AIR, TURF, SOIL, ROCK, RUBBLE, LOG, BOARDS, LEAVES, SAND, GLASS, COAL, IRON, BEDROCK, WATER,
       BENCH, GOLD, GEM, CHEST, FURNACE, BED, GRAVEL, LAVA, BRICKS, CLAY, DOOR, LADDER, FENCE, FARMLAND,
       WOOL, NBLOCKS };
/* (torches, plants and open doors are game objects, not blocks: the block there is air) */

/* One vertical run of a block type. A column's list is sorted top first and ends with top = 0. */
#define SEG_TOPEXP 1
#define SEG_BOTEXP 2

/* the type byte: the block type (bits 0-4) and the light class (bits 5-6): 0 open to the sky (the
   column's first segment), 1 under a tree (leaves or a log right above), 2 covered, 3 torch-lit */
#define SEG_TYPE 31
#define SEG_TREE 0x20
#define SEG_COVERED 0x40
#define SEG_LIT 0x60

typedef struct Seg {
    unsigned char top;      /* y of the top surface (highest block + 1) */
    unsigned char bot;      /* y of the lowest block */
    unsigned char type;     /* block type | light class << 5 */
    unsigned char flags;    /* SEG_TOPEXP: air above it, SEG_BOTEXP: air below it; bits 2-7: segments
                               from this one to the next run top or the end of the list */
} Seg;

extern unsigned char *bw_blocks;              /* WX * WX * WY bytes, (x * WX + z) * WY + y */
extern Seg **bw_cols;                         /* PWX * PWX column lists (border = open sea) */
extern unsigned char bw_coarse[CWX * CWX];

#define BIDX(x, z, y) (((long)(x) * WX + (z)) * WY + (y))

/* Allocate (with the given allocator) and generate the world for a seed. Returns 0 on success. */
int world_init(void *(*alloc)(long));
void world_generate(unsigned short seed);
/* Rebuild every column's segment list (after generation or loading). */
void world_build_segments(void);
/* Rebuild one column's list after a block change. */
void world_column_changed(int x, int z);
/* torch light per column: blocks [lo, hi) lit (hi 0: none). Rebuild the column after a change. */
extern unsigned char bw_lit[WX * WX][2];
short make_segs_c(const unsigned char *b, Seg *s);

/* The world in a save file (tools/bwsave.py is the host reference): the coarse map (1024 bytes),
 * the number of distinct columns (2 bytes), each column's id among them (2 bytes each, big-endian,
 * memory order x * WX + z; ids by first appearance), then each distinct column's record: its top
 * (the height above which all is air, 0..WY), then its runs from the top down: a type byte and a
 * length, except the run reaching y 0, whose type byte has bit 7 set and no length. A world has
 * about 2,500 distinct columns of 16,384. Loading builds each distinct column's blocks and segment
 * list once; the other columns copy the blocks and share the list (col_cap 0: a change gives the
 * column a list of its own first). */
#define WORLD_WORK (2L * 4096 + 10L * WX * WX + 8)
/* (out 0) the world's dictionary in work (from the segment lists), and the most room the stream
   needs (0: a column has no list); (out) write it and return its length. work: WORLD_WORK bytes */
long world_pack(unsigned char *out, void *work);
/* blocks, segment lists (with bw_lit's torch light), coarse map from it; 0 or -1. in: even address */
short world_unpack(const unsigned char *in, long len);
short world_unpack_c(const unsigned char *in, long len);    /* (the reference: unpack.s does the same) */
/* world_generate calls it with 0..1000 as it goes (0: none) */
extern void (*world_progress)(short permille);

#endif
