#!/usr/bin/env python3
"""Preview the car sprites at all sizes: car_sheet.py OUT.png"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "engine")); sys.path.insert(0, os.path.join(HERE, ".."))
import sprites
from art import cars
bank = sprites.SpriteBank()
names = []
for f in ("straight", "left", "right", "crash"):
    o, b = sprites.scale(cars.player(f), 56, 28); bank.add("p_" + f, o, b); names.append("p_" + f)
for kind in ("black", "grey", "white"):
    for w in (56, 44, 33, 24, 17, 11, 7, 4):
        o, b = sprites.scale(cars.rival(kind), w, max(2, w // 2), outline=w >= 9); n = f"{kind}{w}"
        bank.add(n, o, b, 8); names.append(n)
bank.sheet(sys.argv[1], names)
