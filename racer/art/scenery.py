"""Horizon scenery: a wrapping mountain strip and the sky, as 1-bit arrays (1 = black)."""
import numpy as np

MTN_SEED = 7


def pat_plane(h, w, pair):
    out = np.zeros((h, w), dtype=np.uint8)
    for y in range(h):
        b = pair[y & 1]
        bits = np.array([(b >> (7 - i)) & 1 for i in range(8)], dtype=np.uint8)
        out[y] = np.tile(bits, w // 8 + 1)[:w]
    return out


def mountains(width=512, height=30):
    """Height map pair (near, far) of a wrapping ridge line, deterministic."""
    rng = np.random.default_rng(MTN_SEED)
    x = np.arange(width)
    near = np.zeros(width)
    far = np.zeros(width)
    for _ in range(7):
        c, hgt, slope = rng.uniform(0, width), rng.uniform(8, height - 2), rng.uniform(0.45, 0.9)
        d = np.minimum(np.abs(x - c), width - np.abs(x - c))
        near = np.maximum(near, hgt - d * slope)
    for _ in range(9):
        c, hgt, slope = rng.uniform(0, width), rng.uniform(10, height), rng.uniform(0.3, 0.6)
        d = np.minimum(np.abs(x - c), width - np.abs(x - c))
        far = np.maximum(far, hgt - d * slope)
    return np.round(near).astype(int), np.round(far).astype(int), height


def mountain_strip(S, mtn):
    """Render the strip (height x width) for a stage's patterns."""
    near, far, height = mtn
    width = len(near)
    sky = pat_plane(height, width, S["sky"])
    farp = pat_plane(height, width, S["far"])
    body = pat_plane(height, width, S["mtn"])
    cap = pat_plane(height, width, S["cap"])
    img = sky.copy()
    for x in range(width):
        if far[x] > 0:
            img[height - far[x]:, x] = farp[height - far[x]:, x]
        if near[x] > 0:
            top = height - near[x]
            img[top:, x] = body[top:, x]
            if near[x] > 14:   # snow cap on tall peaks
                img[top:top + max(2, (near[x] - 14) // 2), x] = cap[top:top + max(2, (near[x] - 14) // 2), x]
                img[top, x] = S["mtn"][0] >> 7 & 1 if S.get("night") else 1   # outline
    return img


def sky(cfg, S, mtn, scroll=0):
    """Rows 0..HY: sky pattern, mountains sitting on the horizon row, scrolled horizontally."""
    strip = mountain_strip(S, mtn)
    h, w = strip.shape
    out = pat_plane(cfg.HY + 1, cfg.W, S["sky"])
    cols = (np.arange(cfg.W) + scroll) % w
    out[cfg.HY + 1 - h:cfg.HY + 1] = strip[:, cols]
    return out


def apply_fog(img, cfg, S):
    """Fog: rows with k < fog_k become the fog pattern (also covers the mountains)."""
    fog = pat_plane(cfg.H, cfg.W, S["fog"])
    top = cfg.HY + S["fog_k"]
    img[:top] = fog[:top]
