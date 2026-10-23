"""A small hand-built test world: flat turf, one 2x2x2 sample of each material, a tree, a pond, a cliff."""
import world as W


def build():
    b = bytearray(W.WX * W.WX * W.WY)
    def put(x, z, y, t): b[W.idx(x, z, y)] = t
    for x in range(64):
        for z in range(64):
            put(x, z, 0, W.BEDROCK)
            for y in range(1, 7): put(x, z, y, W.ROCK)
            for y in range(7, 10): put(x, z, y, W.SOIL)
            put(x, z, 10, W.TURF)
    mats = [W.ROCK, W.SOIL, W.RUBBLE, W.LOG, W.BOARDS, W.LEAVES, W.SAND, W.GLASS, W.COAL, W.IRON, W.BEDROCK, W.BENCH]
    for i, t in enumerate(mats):     # a row along z at x = 36..37
        z0 = 20 + i * 3
        for x in (36, 37):
            for z in (z0, z0 + 1):
                for y in (11, 12): put(x, z, y, t)
    # pond at x 28..33, z 36..41
    for x in range(28, 34):
        for z in range(36, 42):
            put(x, z, 10, W.WATER); put(x, z, 9, W.WATER); put(x, z, 8, W.SAND)
    # cliff: raise x 40..50, z 40..52 to height 14 with rock showing
    for x in range(40, 51):
        for z in range(40, 53):
            for y in range(10, 14): put(x, z, y, W.ROCK if y < 12 else W.SOIL)
            put(x, z, 14, W.TURF)
    # tree at (30, 26)
    tx, tz, h = 30, 26, 10
    top = h + 5
    for yy in range(top - 2, top + 2):
        rad = 2 if yy < top else 1
        for xx in range(tx - rad, tx + rad + 1):
            for zz in range(tz - rad, tz + rad + 1):
                if abs(xx - tx) == rad and abs(zz - tz) == rad and rad == 1: continue
                put(xx, zz, yy, W.LEAVES)
    for yy in range(h + 1, top + 1): put(tx, tz, yy, W.LOG)
    return b
