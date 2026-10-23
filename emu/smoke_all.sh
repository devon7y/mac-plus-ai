#!/bin/zsh
# Smoke-test every game in emu/games.json: boot each entry that has a disk or harddisk headless,
# with exactly emu/play.sh's settings (machine, RAM, floppy 1, flop2, hard disk; floppy
# reinserted after a restart), take screenshots, check them, and build one contact sheet.
#
#   emu/smoke_all.sh                 all entries with a disk or harddisk
#   emu/smoke_all.sh doodle pixel     only these entries
#
# Output: emu/runs/smoke-<date>/ with one folder per game (shots/tNNN.N.png, mame.log), sheet.png
# (one labelled row per game) and results.txt. Prints one "PASS name" / "FAIL name: reason" line
# per game; the exit status is 0 only if every game passed. At most 2 MAME instances run at once.
#
# Screenshots: at 20, 45, 90 s of emulated time; 60, 90, 150 s for 4 MB or macsefd entries.
# A game fails if, in any shot, it shows the sad Mac, the "?" disk (no startup disk), a system
# error bomb box, or the startup alert with the Mac icon ("this startup disk ... will not work on
# this model"), all detected from the pixels (32x32 icon templates, matched anywhere); or if
# the last shot is all one colour or blank (fewer than 200 pixels differ from the background);
# or, for entries marked "animates", if the last two shots are identical.
#
# Optional "smoke" field per entry in games.json:
#   "smoke": {
#     "times": [60, 90, 150],                 screenshot times (emulated seconds)
#     "actions": [[60, "key", "Return"],      press and release a key (0.15 s)
#                 [62, "hold", "Right", 3],   hold a key for 3 s
#                 [70, "click", 256, 171],    click at (h, v), global screen coordinates
#                 [75, "type", "hello"]],     type text (letters, digits, spaces; no commas)
#     "animates": true                         fail if the last two shots are identical
#   }
# Key names: Return Tab Space Esc Up Down Left Right Command Option Shift Backspace, or one
# letter or digit. The keys and mouse work on both the Plus and the SE (ADB) keyboards.
# (Action "crash" jumps to an ILLEGAL instruction; it only exists to test this script.)
#
# Environment: GAMES_JSON=<file> (default emu/games.json), SMOKE_DIR=<dir> (output folder).
E=${0:A:h}
ROOT=${E:h}
export GAMES_JSON=${GAMES_JSON:-$E/games.json}

if [[ "${1:-}" == "--one" ]]; then          # internal: run one game ($2) into folder $3
  NAME=$2; OUT=$3
  mkdir -p "$OUT/$NAME"
  eval "$(python3 -I - "$GAMES_JSON" "$NAME" "$ROOT" <<'EOF'
import json, os, shlex, sys
g = json.load(open(sys.argv[1]))[sys.argv[2]]
root = sys.argv[3]
def path(k):
    v = g.get(k)
    return os.path.join(root, v) if v else ""
ram, machine = g.get("ram", "1M"), g.get("machine", "macplus")
smoke = g.get("smoke") or {}
slow = ram == "4M" or machine == "macsefd"
times = smoke.get("times") or ([60, 90, 150] if slow else [20, 45, 90])
acts = []
for a in smoke.get("actions", []):
    acts.append(",".join(str(x) for x in a))
def act_end(a):
    if a[1] in ("key", "hold") and len(a) > 3:
        return float(a[0]) + float(a[3])
    if a[1] == "type" and len(a) > 2:
        return float(a[0]) + 0.3 * len(str(a[2]))
    return float(a[0]) + 1
end = max([float(t) for t in times] + [act_end(a) for a in smoke.get("actions", [])])
missing = [p for p in (path("disk"), path("flop2"), path("harddisk")) if p and not os.path.exists(p)]
out = {"DISK_SRC": path("disk"), "FLOP2_SRC": path("flop2"), "HARD_SRC": path("harddisk"),
       "G_RAM": ram, "G_MACHINE": machine, "G_TIMES": ",".join(str(t) for t in times),
       "G_ACTIONS": ";".join(acts), "G_SECONDS": str(int(end) + 5), "G_MISSING": " ".join(missing)}
print("; ".join(f"{k}={shlex.quote(v)}" for k, v in out.items()))
EOF
)"
  if [[ -n "$G_MISSING" ]]; then echo "disk image not found: $G_MISSING" > "$OUT/$NAME/error.txt"; exit 0; fi
  RUN_DIR="$OUT/$NAME" RAM=$G_RAM MACHINE=$G_MACHINE FLOP2="$FLOP2_SRC" HARDDISK="$HARD_SRC" \
    SMOKE_TIMES="$G_TIMES" SMOKE_ACTIONS="$G_ACTIONS" \
    "$E/run_test.sh" "${DISK_SRC:-none}" "$E/scripts/smoke_game.lua" $G_SECONDS > "$OUT/$NAME/mame.log" 2>&1
  echo "  ran $NAME ($G_MACHINE, $G_RAM, shots at $G_TIMES s)"
  exit 0
fi

OUT=${SMOKE_DIR:-$E/runs/smoke-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"
GAMES=("${(@f)$(python3 -I - "$GAMES_JSON" "$@" <<'EOF'
import json, sys
g = json.load(open(sys.argv[1]))
want = sys.argv[2:]
for k, v in g.items():
    if k.startswith("_") or not isinstance(v, dict):
        continue
    if (v.get("disk") or v.get("harddisk")) and (not want or k in want):
        print(k)
EOF
)}")
[[ -z "${GAMES[1]:-}" ]] && { echo "no games with a disk or harddisk in $GAMES_JSON"; exit 1; }
echo "smoke test of ${#GAMES} game(s) -> $OUT"
print -l -- $GAMES | xargs -P 2 -I{} "${0:A}" --one {} "$OUT"

python3 -P - "$GAMES_JSON" "$OUT" $GAMES <<'EOF'   # -P, not -I: numpy and PIL live in user site-packages
import glob, json, os, re, sys, textwrap
import numpy as np
from PIL import Image, ImageDraw, ImageFont

games = json.load(open(sys.argv[1]))
out, names = sys.argv[2], sys.argv[3:]

# 32x32 icon templates (1 = black), cut from MAME screenshots of the real screens.
TEMPLATES = {
    "the sad Mac": ("57ffffd5a800002a50000015b1ffff1a52000095b200009a52282895b210109a52282895b200009a52048095"
                    "b203009a52000095b20fc09a52103095b200089a52000095b1ffff1a50000015b000001a50000015b000001a"
                    "53003f15b000001a50000015b000001a50000015afffffea58000035a800002a58000035afffffea", 0.97),
    "the \"?\" disk (no startup disk)":
                   ("7ffffff58100010a810071058100890281008901810089018100890181008901810071018100010180fffe01"
                    "80000001800000018000000187ffffe188000011880000118800001188000011880000118800001188000011"
                    "880000118800001188000011880000118800001188000011880000118800001188000011fffffffe", 0.93),
    "a system error (bomb)":
                   ("000008080000001000000420000080400000220000000800001f002a0020c70000403800004000200040091003"
                    "f8100803f8210403f800000ffe01003fff80003fff80007fffc0007fffc000fffde000ffffe000fffee000fffe"
                    "e000fffee000ffffe000fffde0007fffc0007ff7c0003fff80003fff80000ffe000003f80000", 0.97),
    "a startup alert (\"this startup disk will not work on this model\")":
                   ("000000007ffffe0080000100800001008ffff1009000090095550900900009009550090090000900950009"
                    "009000090095000900900009009540090090000900900009008ffff10080000100800001008000010080000"
                    "100800ff100800001008000010080000100800001007ffffe004000020040000200400002007ffffe00", 0.97),
}
def decode(hexstr):
    rows = [int(hexstr[i:i + 8], 16) for i in range(0, 256, 8)]
    return np.array([[(r >> (31 - b)) & 1 for b in range(32)] for r in rows], dtype=np.float32)
TPL = {k: (decode(h), thr) for k, (h, thr) in TEMPLATES.items()}

def best_match(img, tpl):
    """Highest fraction of equal pixels between tpl and any 32x32 window of img (FFT correlation)."""
    I = img.astype(np.float32)
    H, W = I.shape
    F = np.fft.rfft2(I)
    corr = lambda t: np.fft.irfft2(F * np.conj(np.fft.rfft2(t, s=I.shape)), s=I.shape)
    a = corr(tpl)                                  # black-black matches
    b = corr(np.ones_like(tpl))                    # black pixels in the window
    m = np.rint(2 * a - b - tpl.sum() + tpl.size)[:H - 31, :W - 31]
    y, x = np.unravel_index(np.argmax(m), m.shape)
    return m[y, x] / tpl.size, (int(x), int(y))

def load(p):
    return np.array(Image.open(p).convert("L")) < 128      # True = black

results = []
for name in names:
    g = games[name]
    d = os.path.join(out, name)
    shot_t = lambda p: float(re.findall(r"t([\d.]+)\.png$", p)[-1])
    shots = sorted(glob.glob(os.path.join(d, "shots", "t*.png")), key=shot_t)
    reasons = []
    if os.path.exists(os.path.join(d, "error.txt")):
        reasons.append(open(os.path.join(d, "error.txt")).read().strip())
    elif not shots:
        log = open(os.path.join(d, "mame.log"), errors="replace").read().strip().splitlines() if os.path.exists(os.path.join(d, "mame.log")) else []
        reasons.append("no screenshots (MAME failed?): " + (log[-1] if log else "no log"))
    arrs = [load(p) for p in shots]
    times = [f"{shot_t(p):g}" for p in shots]
    for t, a in zip(times, arrs):
        for what, (tpl, thr) in TPL.items():
            score, pos = best_match(a, tpl)
            if score >= thr:
                reasons.append(f"shows {what} at {t} s (match {score:.0%} at {pos})")
                break
        if reasons:
            break
    if arrs and not reasons:
        a, t = arrs[-1], times[-1]
        black = int(a.sum()); white = a.size - black
        if black == 0 or white == 0:
            reasons.append(f"screen all {'white' if black == 0 else 'black'} at {t} s")
        elif min(black, white) < 200:
            reasons.append(f"screen blank at {t} s (only {min(black, white)} pixels differ from the background)")
        if (g.get("smoke") or {}).get("animates") and len(arrs) >= 2 and np.array_equal(arrs[-1], arrs[-2]):
            reasons.append(f"screen unchanged between {times[-2]} s and {t} s, but the game should animate")
    ok = not reasons
    line = f"PASS {name}" if ok else f"FAIL {name}: " + "; ".join(reasons)
    print(line)
    results.append((name, g, ok, reasons, shots, times))

open(os.path.join(out, "results.txt"), "w").write(
    "".join((f"PASS {n}\n" if ok else f"FAIL {n}: " + "; ".join(r) + "\n") for n, g, ok, r, s, t in results))

# Contact sheet: one row per game, a label panel then half-size screenshots.
TW, TH, LW, PAD, CAP = 256, 171, 250, 8, 16
ncol = max([len(r[4]) for r in results] + [1])
W = LW + ncol * (TW + PAD) + PAD
HDR = 30
Hh = HDR + len(results) * (TH + CAP + 2 * PAD)
sheet = Image.new("RGB", (W, Hh), (245, 245, 245))
dr = ImageDraw.Draw(sheet)
try:
    font = ImageFont.load_default(size=13); bold = ImageFont.load_default(size=15)
except TypeError:
    font = bold = ImageFont.load_default()
npass = sum(r[2] for r in results)
dr.text((PAD, 8), f"Smoke test {os.path.basename(out)}: {npass}/{len(results)} passed", fill=(0, 0, 0), font=bold)
for i, (name, g, ok, reasons, shots, times) in enumerate(results):
    y0 = HDR + i * (TH + CAP + 2 * PAD)
    dr.rectangle([0, y0, LW - PAD, y0 + TH + CAP + PAD], fill=(214, 240, 214) if ok else (250, 210, 210))
    dr.text((PAD, y0 + 6), ("PASS  " if ok else "FAIL  ") + name, fill=(0, 90, 0) if ok else (160, 0, 0), font=bold)
    yy = y0 + 28
    info = [g.get("title", "")] + [f"{g.get('machine', 'macplus')}, {g.get('ram', '1M')}"] + reasons
    for para in info:
        for ln in textwrap.wrap(para, 34)[:6]:
            dr.text((PAD, yy), ln, fill=(20, 20, 20), font=font); yy += 15
            if yy > y0 + TH + CAP: break
    for j, (p, t) in enumerate(zip(shots, times)):
        x = LW + j * (TW + PAD)
        dr.text((x, y0 + 2), f"t = {t} s", fill=(60, 60, 60), font=font)
        im = Image.open(p).convert("L").resize((TW, TH), Image.LANCZOS).convert("RGB")
        sheet.paste(im, (x, y0 + CAP + 2))
        dr.rectangle([x - 1, y0 + CAP + 1, x + TW, y0 + CAP + 2 + TH], outline=(120, 120, 120))
sheet.save(os.path.join(out, "sheet.png"))
print(f"contact sheet: {os.path.join(out, 'sheet.png')}  ({npass}/{len(results)} passed)")
sys.exit(0 if npass == len(results) else 1)
EOF
