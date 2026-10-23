"""Rail Runner vector art: fill patterns and face art for trains, barriers and walls.

These are the "art masters" for everything drawn procedurally (exact perspective at every
depth, no size popping, no memory for pre-scaled copies). gen_data.py turns them into C tables.

Patterns: 8 rows of 16-bit words (MSB = leftmost pixel), indexed by screen row & 7.
Faces: lists of (kind, u0, v0, u1, v1, pattern) in 1/64ths of the face (u across, v down).
kind FILL fills the rectangle; FRAME draws a 1-pixel black border and fills the inside.
"""

FILL, FRAME = 0, 1


def rot(w, n):
    n &= 15
    return ((w >> n) | (w << (16 - n))) & 0xFFFF


WHITE, BLACK, GREY, LIGHT, DARK, STRIPES, HATCH, VLINES, DOTS = range(9)
PATTERNS = [
    [0x0000] * 8,                                              # WHITE
    [0xFFFF] * 8,                                              # BLACK
    [0xAAAA, 0x5555] * 4,                                      # GREY 50%
    [0x8888, 0x0000, 0x2222, 0x0000] * 2,                      # LIGHT 12.5%
    [0x7777, 0xFFFF, 0xDDDD, 0xFFFF] * 2,                      # DARK 87.5%
    [rot(0xF0F0, i) for i in range(8)],                        # STRIPES diagonal (hazard)
    [0x8888, 0x4444, 0x2222, 0x1111] * 2,                      # HATCH thin diagonal
    [0x8888] * 8,                                              # VLINES
    [0xAAAA, 0x0000, 0x5555, 0x0000] * 2,                      # DOTS 25%
]
RAMP_PAT = HATCH
ROOF_PAT = GREY

# --- parked train: white carriage, black windscreen, dark bumper
TRAIN_FRONT = [
    (FRAME, 0, 0, 64, 64, WHITE),
    (FILL, 0, 0, 64, 4, GREY),          # roof lip
    (FILL, 22, 5, 42, 9, BLACK),        # destination sign
    (FILL, 6, 12, 29, 30, BLACK),       # windscreen left
    (FILL, 35, 12, 58, 30, BLACK),      # windscreen right
    (FILL, 0, 36, 64, 40, BLACK),       # stripe
    (FRAME, 7, 44, 17, 51, WHITE),      # lamps
    (FRAME, 47, 44, 57, 51, WHITE),
    (FILL, 0, 56, 64, 64, DARK),        # bumper
]
# side face bands, top to bottom: (from height, to height, pattern) in zu
TRAIN_SIDE = [(1024, 830, LIGHT), (830, 500, BLACK), (500, 60, WHITE), (60, 0, BLACK)]

# --- oncoming train: dark body, blazing white lamps (it is coming at you)
ONCOMING_FRONT = [
    (FRAME, 0, 0, 64, 64, DARK),
    (FRAME, 6, 10, 29, 28, GREY),       # windscreens
    (FRAME, 35, 10, 58, 28, GREY),
    (FRAME, 4, 38, 20, 52, WHITE),      # big lamps
    (FRAME, 44, 38, 60, 52, WHITE),
    (FILL, 26, 40, 38, 50, STRIPES),    # coupler warning plate
    (FILL, 0, 57, 64, 64, BLACK),
]
ONCOMING_SIDE = [(1024, 830, DARK), (830, 500, WHITE), (500, 60, DARK), (60, 0, BLACK)]

# --- wall barrier ('#'): a concrete block with a hazard band
WALL_FRONT = [
    (FRAME, 0, 0, 64, 64, GREY),
    (FRAME, 0, 22, 64, 40, STRIPES),
]
WALL_SIDE = [(1024, 0, GREY)]
WALL_TOP = LIGHT

# --- barriers: faces only (thin)
BARRIER_W = 860
LOW_FACE = [                            # face is LOW_H tall
    (FRAME, 4, 0, 12, 64, WHITE),       # posts
    (FRAME, 52, 0, 60, 64, WHITE),
    (FRAME, 0, 6, 64, 40, STRIPES),     # striped board
]
HIGH_FACE = [                           # face is HIGH_T tall (1300 zu): sign high up, gap below
    (FILL, 5, 0, 9, 64, BLACK),         # posts
    (FILL, 55, 0, 59, 64, BLACK),
    (FRAME, 0, 0, 64, 24, STRIPES),     # striped beam
    (FRAME, 18, 24, 46, 34, WHITE),     # hanging sign
    (FILL, 22, 27, 42, 31, BLACK),
]
