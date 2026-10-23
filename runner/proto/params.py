"""Rail Runner game constants: the single source of truth.

Shared by the Python prototype (sim.py, test_escape.py) and the 68000 game (gen_params.py writes
runner/game/params.h from this file), so the escape-route test checks the numbers the game uses.

Units
  zu   world distance: 1 lane spacing = 1024 zu ("1 u"; think of 1 u as about 4 m)
  tick one logic step = one Mac tick (60.15 Hz). Logic always runs at this fixed rate.
All values are integers; the game does the same integer arithmetic in the same order.
"""

U = 1024                      # zu per lane spacing
TICK_HZ = 60.15

# --- speed (zu per tick). Speed rises by 1 zu/tick every SPEED_STEP_ZU of distance.
V0 = 136                      # 8.0 u/s at the start
VMAX = 238                    # 14.0 u/s
SPEED_STEP_ZU = 24 * U        # +1 zu/tick every 24 u: VMAX after 2448 u (about 3.6 minutes)

# --- player
LANES = 3
SWITCH_T = 9                  # ticks for a lane change (0.15 s)
PZ = 160                      # player half-depth for collisions (zu)
STAND_H = 460                 # body height standing / running / jumping
ROLL_H = 200                  # body height while rolling
G = 4                         # gravity, zu per tick^2
JUMP_V = 72                   # jump: 36 ticks, apex ~0.65 u
JUMP_V_HI = 104               # with super sneakers: 52 ticks, apex ~1.3 u (reaches train roofs)
DIVE_V = 48                   # pressing roll in the air sets vy = -DIVE_V
ROLL_T = 32                   # roll lasts 32 ticks (0.53 s)
STEP_TOL = 180                # max step up per tick (ramps); more than this is a wall
STUMBLE_T = 18                # ticks of stumble animation (bounce back)

# --- obstacles
TRAIN_H = 1024                # train roof height
TRAIN_W = 768                 # train width (x), lane spacing 1024
LOW_H = 300                   # low barrier: solid block of this height (jump over it)
LOW_D = 256                   # low barrier depth (z)
HIGH_B = 260                  # high barrier: bar from HIGH_B to HIGH_T (roll under it)
HIGH_T = 1300
HIGH_D = 256
WALL_D = 512                  # full barrier ('#'): TRAIN_H tall, WALL_D deep
RAMP_LEN_MIN = 2 * U          # ramps are at least 2 u long (rise per tick < STEP_TOL at VMAX)
ONCOMING_V = 102              # oncoming trains move toward the player at 6 u/s
DRAW_DIST = 22 * U            # objects are spawned/drawn up to this far ahead of the player

# --- patterns
ROW = U                       # one pattern row = 1 u
GAP_ROWS = 3                  # empty rows the chainer inserts between patterns at V0 ...
GAP_PER_V = 30                # ... plus one more row per GAP_PER_V zu/tick above V0

# --- pickups and power-ups (ticks)
COIN_R_Z = 400                # coin pickup half-depth window
COIN_R_Y = 600                # coin pickup vertical window
MAGNET_T = 600
SNEAKERS_T = 600
MULT_T = 900
JET_T = 420
JET_Y = 1500                  # jetpack flying height (feet), clear of train roofs
JET_RISE = 40                 # zu per tick climbing / descending
INVULN_T = 90                 # after a hoverboard save or the end of a jetpack
HOVER_T = 1800                # a hoverboard lasts 30 s or until it absorbs a crash
CHASE_T = 300                 # the guard chases for 5 s after a stumble
CHASE_START_T = 180           # ... and for 3 s at the start of a run
POWERUP_EVERY = 4             # at most one power-up per this many patterns (on average)

# --- score
SCORE_PER_U = 1               # points per u travelled (x multiplier)
COIN_POINTS = 5


def gap_rows(v):
    """Empty rows between patterns at speed v (more room to land at higher speeds)."""
    return GAP_ROWS + (v - V0) // GAP_PER_V


def speed_at(dist_zu):
    """Speed (zu/tick) after travelling dist_zu."""
    return min(VMAX, V0 + dist_zu // SPEED_STEP_ZU)
