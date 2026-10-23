#!/usr/bin/env python3
"""Write rtab.h and costab.h (Box-Muller tables, FORMAT.md) into the given folder."""
import math
import os
import sys

out = sys.argv[1] if len(sys.argv) > 1 else "."
os.makedirs(out, exist_ok=True)
R = [int(round(4096 * math.sqrt(-2 * math.log((i + 0.5) / 1024)))) for i in range(1024)]
C = [int(round(16384 * math.cos(2 * math.pi * a / 1024))) for a in range(1024)]
for name, t in (("rtab.h", R), ("costab.h", C)):
    with open(os.path.join(out, name), "w") as f:
        for i in range(0, 1024, 16):
            f.write(", ".join(str(v) for v in t[i:i + 16]) + ",\n")
