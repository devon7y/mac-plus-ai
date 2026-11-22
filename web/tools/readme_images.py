#!/usr/bin/env python3
"""Regenerate the public README's pictures of the website (public/docs/images/):
    python3 web/tools/readme_images.py <work dir> [hero pixel dream blockworld pvz] [--screens] [--no-shots] [--site DIR]

For each part: takes the screenshots with web/tools/readme_shots.mjs (Google Chrome on the GPU, in a
window, against a local copy of web/dist), then crops, scales and tiles them into the README's images.
Default: all five parts, and only the pictures of the 3D Mac (*_site.jpg, hero.jpg). --screens also
remakes the pictures of the Mac's own screen (pixel_screen.png, dream_screen.png, dream_gallery.png):
new conversations and new random pictures, so check them against the README's text (it names the
pictures and says the guitar came out unrecognizable). --no-shots reuses the screenshots already in
<work dir>/<part>/.

  hero        hero.jpg              the whole 3D Mac with Dream's banana (crop of hero_wider.png)
  pixel       pixel_site.jpg        Pixel on the 3D Mac (default view)
              pixel_screen.png      the Mac's screen after "what year is it?", 2x
  dream       dream_site.jpg        Dream's banana on the 3D Mac (default view)
              dream_screen.png      the Mac's screen with the banana, 2x
              dream_gallery.png     12 first-try pictures (banana ... pineapple; guitar left out), 2x, 4 x 3
  blockworld  blockworld_site.jpg   the starting world with a yak ahead, zoomed out to show the desk
  pvz         pvz_site.jpg          level 1-9 played by the port's test bot (needs PVZ_DISK=<a disk
                                    image with the game>; the port isn't published, so it's skipped without)

Needs node (web/build/node_modules: playwright-core), ffmpeg and Google Chrome. Writes only into
<work dir> and public/docs/images/. A part whose screenshots failed is skipped (its old image stays)."""
import os
import subprocess
import sys

WEB = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(WEB)
IMAGES = os.path.join(ROOT, "public", "docs", "images")
PARTS = ["hero", "pixel", "dream", "blockworld", "pvz"]
GALLERY = ["banana", "house", "lighthouse", "sailboat", "snowman", "owl", "teapot", "castle",
           "hot_air_balloon", "cactus", "umbrella", "pineapple"]

args = [a for a in sys.argv[1:] if not a.startswith("--")]
if not args:
    sys.exit(__doc__)
work = os.path.abspath(args[0])
parts = args[1:] or PARTS
site = sys.argv[sys.argv.index("--site") + 1] if "--site" in sys.argv else os.path.join(WEB, "dist")
if "--site" in sys.argv:
    parts = [p for p in parts if p != site]


def ff(*a):
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", *a], check=True)


def jpg(src, dst, crop=None):           # crop = (x, y, w, h) in the screenshot's pixels
    vf = [f"crop={crop[2]}:{crop[3]}:{crop[0]}:{crop[1]}"] if crop else []
    ff("-i", src, *(["-vf", ",".join(vf)] if vf else []), "-q:v", "2", dst)


def x2(src, dst):                       # a Mac screen (or part of one), doubled with hard pixel edges
    ff("-i", src, "-vf", "scale=iw*2:ih*2:flags=neighbor", dst)


def gray(src):                          # (w, h, bytes) of a small PNG, one byte per pixel
    w, h = map(int, subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                    "stream=width,height", "-of", "csv=p=0", src],
                                   capture_output=True, text=True, check=True).stdout.strip().split(","))
    return w, h, subprocess.run(["ffmpeg", "-loglevel", "error", "-i", src, "-f", "rawvideo", "-pix_fmt", "gray", "-"],
                                capture_output=True, check=True).stdout


def picture_frame(src):
    """Dream's picture on its 512x342 screen: the square framed by the first black line 180-210 px long."""
    w, h, px = gray(src)
    for y in range(h):
        x = 0
        while x < w:
            if px[y * w + x] < 128:
                s = x
                while x < w and px[y * w + x] < 128:
                    x += 1
                if 180 <= x - s <= 210:
                    return s, y, x - s
            else:
                x += 1
    raise SystemExit(f"{src}: no picture frame found")


def shots(part):
    out = os.path.join(work, part)
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ, BROWSER="gpu", DPR=os.environ.get("DPR", "2"))
    if part == "dream" and "--screens" not in sys.argv:
        env["SUBJECTS"] = "1"
    if part in ("blockworld", "pvz"):
        env.setdefault("ZOOMS", "0.1")
    if part == "blockworld":
        env.setdefault("AHEAD", "3")                                      # the yak 3 blocks ahead
    if part == "pvz":
        env.setdefault("AT", "30000")
    with open(os.path.join(out, "log.txt"), "w") as log:
        subprocess.run(["node", os.path.join(WEB, "tools", "readme_shots.mjs"), site, out, part],
                       cwd=WEB, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=3600)
    return out


def have(*files):
    missing = [f for f in files if not os.path.exists(f)]
    if missing:
        print("  missing, skipped:", ", ".join(os.path.relpath(m, work) for m in missing))
    return not missing


for part in parts:
    print(part)
    if part == "pvz" and not os.environ.get("PVZ_DISK") and "--no-shots" not in sys.argv:
        print("  skipped: set PVZ_DISK to a disk image with the game")
        continue
    d = shots(part) if "--no-shots" not in sys.argv else os.path.join(work, part)
    p = lambda f: os.path.join(d, f)                                      # noqa: E731
    img = lambda f: os.path.join(IMAGES, f)                               # noqa: E731
    if part == "hero" and have(p("hero_wider.png")):
        jpg(p("hero_wider.png"), img("hero.jpg"), (330, 170, 1000, 830))
    elif part == "pixel" and have(p("pixel_site.png"), p("pixel_2.png")):
        jpg(p("pixel_site.png"), img("pixel_site.jpg"))
        if "--screens" in sys.argv:
            x2(p("pixel_2.png"), img("pixel_screen.png"))
    elif part == "dream":
        pics = [p(f"dream_{i + 1:02d}_{s}.png") for i, s in enumerate(GALLERY)] if "--screens" in sys.argv else []
        if have(p("dream_site.png"), *pics):
            jpg(p("dream_site.png"), img("dream_site.jpg"))
        if pics and have(*pics):
            x2(pics[0], img("dream_screen.png"))
            x, y, n = picture_frame(pics[0])
            gap, t = 26, 2 * n                                            # tiles of the frame, doubled
            ins, chain = [], []
            for i, f in enumerate(pics):
                ins += ["-i", f]
                chain.append(f"[{i}]crop={n}:{n}:{x}:{y},scale={t}:{t}:flags=neighbor[t{i}]")
            layout = "|".join(f"{(i % 4) * (t + gap)}_{(i // 4) * (t + gap)}" for i in range(len(pics)))
            chain.append("".join(f"[t{i}]" for i in range(len(pics))) + f"xstack=inputs={len(pics)}:layout={layout}:fill=white")
            ff(*ins, "-filter_complex", ";".join(chain), "-frames:v", "1", img("dream_gallery.png"))
    elif part == "blockworld" and have(p("blockworld_z0.1.png")):
        jpg(p("blockworld_z0.1.png"), img("blockworld_site.jpg"), (430, 300, 920, 700))
    elif part == "pvz" and have(p("pvz_u30000_z0.1.png")):
        jpg(p("pvz_u30000_z0.1.png"), img("pvz_site.jpg"), (430, 300, 920, 700))
    else:
        continue
    print("  done")
