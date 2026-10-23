#!/usr/bin/env python3
"""Sound check for MAME -wavwrite recordings: prints the time spans with sound (RMS over a window).
    wav_env.py out.wav [window seconds, default 0.05] [start seconds]
Record with: RUN_DIR=<dir> MAME_ARGS="-wavwrite <dir>/out.wav" emu/run_test.sh <disk> <script.lua>"""
import wave, sys, struct, array
w = wave.open(sys.argv[1]); n = w.getnframes(); ch = w.getnchannels(); sr = w.getframerate(); sw = w.getsampwidth()
print("rate", sr, "ch", ch, "width", sw, "seconds", n / sr)
data = array.array('h', w.readframes(n))
best = None
for k in range(ch):
    d = data[k::ch]
    e = sum(abs(x) for x in d[::97])
    print("channel", k, "energy", e)
    if best is None or e > best[0]: best = (e, d)
data = best[1]
win = int(sr * float(sys.argv[2]) if len(sys.argv) > 2 else sr * 0.05)
t0 = float(sys.argv[3]) if len(sys.argv) > 3 else 0
segs = []; cur = None
import math
for i in range(int(t0 * sr), len(data) - win, win):
    seg = data[i:i + win]
    m = sum(seg) / win
    rms = math.sqrt(sum((x - m) ** 2 for x in seg) / win)
    on = rms > 300
    t = i / sr
    if on and cur is None: cur = [t, t, rms]
    elif on: cur[1] = t; cur[2] = max(cur[2], rms)
    elif cur is not None: segs.append(cur); cur = None
if cur: segs.append(cur)
for a, b, r in segs: print(f"sound {a:7.2f} - {b + win / sr:7.2f} s  peak rms {r:6.0f}")
print("segments", len(segs))
