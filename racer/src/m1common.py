"""Shared definitions for the M1 benchmark (data generator and verifier)."""
import os
import sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "engine"))
sys.path.insert(0, os.path.join(HERE, ".."))
import road  # noqa: E402,F401
from art import stages, scenery  # noqa: E402,F401

C = 1 << 16
TRACK = [(2000, 0), (3000, int(0.006 * C)), (1500, 0), (3000, int(-0.009 * C)), (4000, 0)]
CHK_Z, CHK_X = 2600, 30
STAGE = "morning"


def bits_to_bytes(rows):
    """uint8 0/1 array (h x w) -> bytes, MSB = leftmost pixel."""
    import numpy as np
    return np.packbits(rows.astype(np.uint8), axis=1).tobytes()


def sky_rows(cfg):
    mtn = scenery.mountains(512, 30)
    return scenery.sky(cfg, stages.STAGES[STAGE], mtn, scroll=0)
