"""Car masters (rear views) drawn at 8x with PIL, for engine/sprites.py.

Unit box 48 x 24 (x right, y down), scaled by S. Colours: 0 black, 255 white, 128 grey (dithered).
"""
import math
from PIL import Image, ImageDraw

S = 8
UW, UH = 48, 24


def _canvas():
    return Image.new("LA", (UW * S, UH * S), (255, 0))


def _poly(d, pts, fill):
    d.polygon([(x * S, y * S) for x, y in pts], fill=(fill, 255))


def _rect(d, x0, y0, x1, y1, fill, r=0):
    if r:
        d.rounded_rectangle([x0 * S, y0 * S, x1 * S - 1, y1 * S - 1], radius=r * S, fill=(fill, 255))
    else:
        d.rectangle([x0 * S, y0 * S, x1 * S - 1, y1 * S - 1], fill=(fill, 255))


def car(body=0, roof=None, lean=0, spoiler=False, window=255):
    """Rear view of a car. lean shifts the cabin (steering frames)."""
    im = _canvas()
    d = ImageDraw.Draw(im)
    roof = body if roof is None else roof
    _rect(d, 1, 14, 11, 24, 0, r=1.5)                       # tyres
    _rect(d, 37, 14, 47, 24, 0, r=1.5)
    _poly(d, [(12 + lean, 2), (36 + lean, 2), (41, 12), (7, 12)], roof)       # cabin
    _poly(d, [(14.5 + lean, 4.5), (33.5 + lean, 4.5), (37, 10.5), (11, 10.5)], window)   # rear window
    _poly(d, [(5, 11), (43, 11), (46, 15), (46, 21), (2, 21), (2, 15)], body)  # body
    _rect(d, 4.5, 13.5, 13, 17.5, 255)                      # tail lights
    _rect(d, 35, 13.5, 43.5, 17.5, 255)
    _rect(d, 19.5, 15, 28.5, 19, 255)                       # plate
    _rect(d, 21, 16.4, 27, 17.6, 0)
    if spoiler:
        _rect(d, 1, 7.5, 47, 10, 0)
        _rect(d, 2, 8.2, 46, 9.2, 255)
    return im


def player(frame="straight"):
    lean = {"straight": 0, "left": -2.5, "right": 2.5}.get(frame, 0)
    im = car(body=0, lean=lean, spoiler=True)
    if frame == "crash":
        im = im.rotate(28, resample=Image.NEAREST, expand=False)
    return im


def rival(kind):
    return {"black": car(body=0, window=255),
            "grey": car(body=128, roof=0, window=255),
            "white": car(body=255, roof=0, window=128)}[kind]


def taillights():
    """Night: only the two tail lights (white) with a thin dark surround."""
    im = _canvas()
    d = ImageDraw.Draw(im)
    _rect(d, 3.5, 12.5, 14, 18.5, 0)
    _rect(d, 34, 12.5, 44.5, 18.5, 0)
    _rect(d, 4.5, 13.5, 13, 17.5, 255)
    _rect(d, 35, 13.5, 43.5, 17.5, 255)
    return im
