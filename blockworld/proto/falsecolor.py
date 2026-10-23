"""False-colour version of a render: colour by block type, brightness by face and fog."""
import numpy as np
import world as W
import patterns as PT

TCOL = {W.TURF: (90, 180, 70), W.SOIL: (140, 95, 55), W.ROCK: (140, 140, 140), W.RUBBLE: (110, 110, 110),
        W.LOG: (110, 75, 35), W.BOARDS: (190, 150, 90), W.LEAVES: (40, 120, 40), W.SAND: (225, 210, 150),
        W.GLASS: (200, 230, 240), W.COAL: (60, 60, 60), W.IRON: (190, 150, 130), W.BEDROCK: (30, 30, 30),
        W.WATER: (50, 90, 210), W.BENCH: (160, 100, 50)}


def falsecolor(ids, cfg):
    a = np.array(ids, dtype=np.int32).T          # H x NC
    lut = np.zeros((PT.NIDS, 3), np.uint8)
    lut[PT.SKY] = (200, 225, 255); lut[PT.HAZE] = (225, 235, 250); lut[PT.FOGBG] = (235, 235, 235)
    for f in range(4):
        lut[PT.LINE + f] = (0, 0, 0)
        lut[PT.TURFBAND + f] = lut[PT.TURFBAND_B + f] = (60, 200, 40)
    for t, c in TCOL.items():
        for v in range(2):
            for face in range(4):
                for f in range(4):
                    k = (1.0, 0.8, 0.65, 0.5)[face] * (1 - 0.15 * f) * (1 - 0.08 * v)
                    lut[t * 32 + v * 16 + face * 4 + f] = [min(255, int(x * k + 40 * f)) for x in c]
    img = lut[a]                                   # H x NC x 3
    return np.repeat(img, cfg.COLW, axis=1)
