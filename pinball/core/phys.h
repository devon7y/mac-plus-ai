/*
 * Neural Pinball: the physics core (portable C, integer only, deterministic).
 *
 * Compiled unchanged for the host (tests, tools) and for the 68000 (Retro68): every operation
 * is exact integer arithmetic, so the same inputs give bit-identical games on both.
 *
 * Units
 *   positions   fx  = int32, 16.16 pixels (table coordinates: x right, y down, origin top-left)
 *   velocities  fx  = int32, 16.16 pixels per physics step
 *   geometry    q6  = int16, pixels * 64 (10.6), used inside the collision tests
 *   directions  q14 = int16, unit vectors * 16384
 *   angles      uint16, 65536 per turn, measured from +x towards +y (clockwise on screen)
 *
 * Time: STEPS_PER_TICK physics steps per Mac tick (60.15 Hz), i.e. about 120 Hz. Every per-step
 * constant (gravity, drag, speeds, flipper and plunger dynamics, material speeds) is scaled to it.
 *
 * No tunnelling: each step is cut into 1 to 16 sub-moves so that no ball moves more than
 * 3.5 px (|dx| + |dy|) and no flipper tip near a ball more than 2.5 px between collision tests
 * (the speed cap is 16 px/step per axis). Every contact distance is at least the ball radius,
 * 4 px, and 6.5 px at a flipper tip, so a ball's centre can never jump across a wall, post,
 * flipper or another ball (tests/test_phys.c checks it).
 *
 * Sleeping: a ball that has not moved for 0.4 s (cradled on a flipper, on the plunger) is not
 * simulated until a flipper or the plunger moves, a target pops up, or the game gives it a
 * velocity (nudge, ball search). Its position is exact, so this changes nothing visible.
 */
#ifndef PHYS_H
#define PHYS_H

#include <stdint.h>

typedef int32_t fx;
typedef int16_t q6;

#define FX_ONE      65536L
#define PX(v)       ((q6)((v) * 64))           /* integer/constant pixels -> q6 */
#define FXPX(v)     ((fx)((v) * 65536L))        /* integer pixels -> fx */

#define BALL_R      4                          /* ball radius, pixels */
#define BALL_R6     (BALL_R * 64)
#define MAX_BALLS   4
#define MAX_SHAPES  320
#define MAX_FLIPPERS 3
#define MAX_EVENTS  48
#define STEPS_PER_TICK 2
#define STEPS_PER_SEC 120            /* 60.15 * 2, rounded (for converting speeds in tests) */
#define CELL_SHIFT  10                         /* grid cell = 16 px = 1024 in q6 */
#define GRID_MAX_W  20
#define GRID_MAX_H  24
#define GRID_ITEMS_MAX 3000

/* ---- materials (index into a table given by the table data) --------------------------- */
typedef struct Material {
    int16_t e8;         /* restitution * 256 for impacts faster than vrest */
    int16_t vrest;      /* q10 px/step: slower impacts don't bounce (resting contact) */
    int16_t mu8;        /* Coulomb friction * 256 (tangential impulse <= mu * normal impulse) */
    int16_t kick;       /* q10 px/step: speed added along the normal (slings, bumpers) */
    int16_t kmin;       /* q10 px/step: minimum impact speed that fires a sling */
} Material;

/* ---- shapes ---------------------------------------------------------------------------- */
enum {
    S_SEG = 1,          /* line segment (both sides, round ends) */
    S_CIRCLE,           /* post / bumper: centre (x0,y0), radius r */
    S_ARC,              /* circular wall: centre, radius, from angle a0 to a1 (<= 180 deg) */
    S_LINE,             /* sensor line (no collision): reports crossings */
    S_DISC              /* sensor disc (no collision): sink hole, bowl, zones */
};

/* flags */
#define SF_ONEWAY   0x0001   /* segment: solid only for balls on its normal side (n = (-uy,ux)) */
#define SF_INSIDE   0x0002   /* arc: the ball runs inside the circle (concave wall) */
#define SF_KICK     0x0004   /* slingshot face: kicks when hit at >= kmin */
#define SF_BUMPER   0x0008   /* pop bumper: kicks on every touch */
#define SF_DROP     0x0010   /* drop target: switches itself off when hit */
#define SF_EVENT    0x0020   /* report every hit as EV_HIT */
#define SF_OFF      0x0040   /* inactive (dropped target, open gate) */
#define SF_LAYER    0x0080   /* sensor line: crossing sets the ball's layer (id: lo byte towards
                                +n, hi byte towards -n; 0 = keep) */
#define SF_SINK     0x0100   /* sensor disc: captures a slow ball (EV_SINK) */
#define SF_BOWL     0x0200   /* sensor disc: pulls the ball to its centre (gradient descent) */
#define SF_KICKER   0x0400   /* sensor line: when armed, kicks the ball (kickback) */
#define SF_ARMED    0x0800   /* kicker armed */
#define SF_SPINNER  0x1000   /* sensor line: reports EV_SPIN with the crossing speed */
#define SF_DRAIN    0x2000   /* sensor line: ball lost */

typedef struct ShapeDef {           /* table description (from the table tool, or tests) */
    uint8_t kind, mat;
    uint16_t flags;
    uint8_t layers;                 /* bit mask: 1 playfield, 2 ramp */
    uint8_t rad;                    /* segment: half thickness, q6 / 16 (so up to 15.9 px) */
    int16_t id;                     /* game id reported in events */
    q6 x0, y0, x1, y1;              /* seg/line: a=(x0,y0) b=(x1,y1); circle/arc/disc: centre,
                                       x1 = radius */
    uint16_t a0, a1;                /* arc: angle range; kicker: kick angle in a0, speed q10 in a1 */
} ShapeDef;

typedef struct Shape {
    uint8_t kind, mat;
    uint16_t flags;
    uint8_t layers, pad;
    int16_t id;
    q6 rr;                          /* seg: touching distance = ball radius + half thickness */
    q6 ax, ay, bx, by;              /* seg: ends; circle/arc/disc: centre in ax,ay */
    int16_t ux, uy;                 /* seg: unit a->b (q14); arc: start direction */
    int16_t ex, ey;                 /* arc: end direction (q14); kicker: kick velocity q10 */
    q6 len;                         /* seg: length; circle/arc/disc: radius */
    q6 minx, miny, maxx, maxy;      /* bounding box grown by the ball radius + 2 px */
    int16_t cool;                   /* bumper/sling refractory steps */
} Shape;

/* ---- flippers -------------------------------------------------------------------------- */
typedef struct FlipperDef {
    q6 px, py;                      /* pivot */
    q6 len;                         /* pivot to tip centre */
    q6 rb, rt;                      /* base and tip radius */
    uint16_t rest, up;              /* angles of pivot->tip at rest and fully up */
    int32_t acc, dec;               /* angular acceleration up / down (angle units*16 per step^2) */
    int32_t brake;                  /* deceleration of a still-rising flipper once released (taps) */
    int16_t rise;                   /* steps for the coil's pull to build up to acc (0 = at once) */
    int32_t wmax;                   /* max angular speed (angle units*16 per step) */
    uint8_t mat, layers;
} FlipperDef;

typedef struct Flipper {
    FlipperDef d;
    int32_t ang;                    /* current angle * 16 (unwrapped, between rest and up) */
    int32_t a_rest, a_up;           /* the same limits * 16, unwrapped */
    int32_t w;                      /* angular speed, angle units*16 per step (signed) */
    int32_t ang_from, ang_to;       /* this step's motion (sub-steps interpolate) */
    int32_t sub_th;                 /* angle change (units*16) that moves the tip 3 px */
    int16_t dir;                    /* +1: up = increasing angle (right flipper), -1 (left) */
    int16_t ux, uy;                 /* current unit vector pivot->tip */
    int16_t wq14;                   /* angular speed in radians per step * 16384 */
    uint8_t held;                   /* button state for this step */
    uint8_t moving;                 /* nonzero while not resting on a stop */
    int16_t on_steps;               /* steps since the button went down */
    int16_t acc_step;               /* acc / rise: the coil's pull grows by this each step */
    q6 minx, miny, maxx, maxy;      /* box swept over its whole range, grown by ball radius */
    uint32_t reach2;                /* (len + base radius + ball radius + margin)^2, q12 */
} Flipper;

/* ---- plunger --------------------------------------------------------------------------- */
typedef struct Plunger {
    q6 x0, x1;                      /* lane edges */
    fx yrest;                       /* tip y at rest */
    fx pull;                        /* how far it is pulled back (>= 0) */
    fx vy;                          /* tip velocity (negative = moving up) */
    fx tip_prev;                    /* tip y at the start of the step */
    fx pull_max, pull_rate;
    int32_t speed_per_px;           /* release speed: q16 px/step per pixel of pull */
    uint8_t held, firing;
} Plunger;

/* ---- balls ----------------------------------------------------------------------------- */
enum { BALL_OFF = 0, BALL_FREE, BALL_HELD };

typedef struct Ball {
    fx x, y, vx, vy;
    uint8_t state, layer;
    int16_t held_shape;             /* sink holding it */
    uint16_t still;                 /* steps with almost no motion (ball search) */
    uint8_t sleep;                  /* at rest: not simulated until woken */
} Ball;

/* ---- events ---------------------------------------------------------------------------- */
enum {
    EV_HIT = 1,      /* id = shape id, val = impact speed q10 */
    EV_SLING,        /* id */
    EV_BUMPER,       /* id */
    EV_DROP,         /* id */
    EV_SENSOR,       /* id, val = +1 crossed towards +n, -1 towards -n */
    EV_SPIN,         /* id, val = crossing speed q10 */
    EV_SINK,         /* id: ball captured */
    EV_DRAIN,        /* ball lost */
    EV_KICKBACK,     /* id */
    EV_FLIPHIT,      /* id = flipper index, val = impact speed q10 */
    EV_WALL,         /* any hard impact (sound): val = speed q10 */
    EV_BALLHIT,      /* two balls collided */
    EV_ESCAPE        /* a ball left the table other than through the drain (a bug) */
};

typedef struct Event { uint8_t type, ball; int16_t id; int16_t val; } Event;

/* ---- the world ------------------------------------------------------------------------- */
typedef struct World {
    Shape shapes[MAX_SHAPES];
    int16_t nshapes;
    const Material *mats;
    int16_t gw, gh;                                /* grid size in cells */
    uint16_t cell_start[GRID_MAX_W * GRID_MAX_H + 1];
    uint16_t cell_items[GRID_ITEMS_MAX];
    uint8_t cell_open[GRID_MAX_W * GRID_MAX_H];    /* the cell and its 8 neighbours list no shapes */
    Flipper flip[MAX_FLIPPERS];
    int16_t nflip;
    Plunger plunger;
    int16_t has_plunger;
    Ball ball[MAX_BALLS];
    fx gravity;                                    /* per step^2, along +y */
    int16_t fric_shift;                            /* rolling drag: v -= v >> fric_shift */
    int16_t ramp_g_mul;                            /* gravity multiplier on layer 2 */
    int16_t bowl_k;                                /* bowl pull: dv(q16) = -d(q6) * k >> 8 */
    int16_t bowls[4], nbowls;                      /* shapes with SF_BOWL */
    fx bowl_box[4][4];                             /* x0, y0, x1, y1 of each bowl (fx) */
    fx gravity2;                                   /* gravity on the ramp layer */
    int16_t bowl_drag;                             /* extra drag shift inside the bowl */
    int16_t sink_vmax;                             /* q10: fastest ball a sink captures */
    Event ev[MAX_EVENTS];
    int16_t nev;
    uint32_t steps;
    int16_t tilted;                                /* flippers dead */
    q6 ramp_x0, ramp_y0, ramp_x1, ramp_y1;         /* layer 2 exists only inside this box */
    fx drain_y, out_x0, out_x1, out_y0;            /* lost-ball limits */
    q6 fbox_x0, fbox_y0, fbox_x1, fbox_y1;         /* union of the flippers' and plunger's boxes */
    q6 table_w, table_h;
} World;

/* setup */
int16_t phys_init(World *w, const ShapeDef *defs, int16_t n, const Material *mats,
                  q6 table_w, q6 table_h);         /* returns 0, or -1 if a limit was hit */
int16_t phys_add_flipper(World *w, const FlipperDef *d);
void phys_set_ramp_box(World *w, q6 x0, q6 y0, q6 x1, q6 y1);
void phys_set_plunger(World *w, q6 x0, q6 x1, q6 yrest, q6 pull_max, fx pull_rate,
                      int32_t speed_per_px);

/* one physics step (call STEPS_PER_TICK times per tick; set flipper/plunger buttons first) */
void phys_step(World *w);

/* control */
#define PHYS_FLIPPER(w, i, h) ((w)->flip[i].held = (uint8_t)((h) != 0))
#define PHYS_PLUNGER(w, h) ((w)->plunger.held = (uint8_t)((h) != 0))
void phys_flipper_button(World *w, int16_t i, int16_t held);
void phys_plunger_button(World *w, int16_t held);
int16_t phys_add_ball(World *w, fx x, fx y, fx vx, fx vy);     /* returns ball index or -1 */
void phys_eject(World *w, int16_t ball, fx vx, fx vy);           /* release a held ball */
void phys_nudge(World *w, fx dvx, fx dvy);
void phys_shape_on(World *w, int16_t shape, int16_t on);         /* drop targets, gates */
void phys_arm(World *w, int16_t shape, int16_t armed);          /* kickers */
int16_t phys_find(const World *w, uint8_t kind, int16_t id);     /* first shape with that id */
int16_t phys_balls_in_play(const World *w);

/* helpers shared with the tools */
int16_t fsin(uint16_t a);           /* q14 */
int16_t fcos(uint16_t a);
int32_t isqrt32(uint32_t v);
uint32_t phys_hash(const World *w); /* state checksum for determinism tests */

#endif
