#!/usr/bin/env python3
"""Cut the final demo video from demo_video.mjs's recording:
    python3 web/tools/demo_edit.py <recording dir> <out.mp4> [--cut START:END ...] [--hold SECONDS]
Plays the recording at normal speed except the stretches in marks.json["ff"] ([start, end, speed]), which
are sped up (sound too, keeping its pitch: atempo), and the --cut stretches (seconds of the recording),
which are left out. --hold freezes the last frame that much longer (silence under it), for the end card.
H.264 + AAC, 30 fps, for Reddit and the like."""
import json
import subprocess
import sys

argv = sys.argv[1:]
cuts = []
while "--cut" in argv:
    i = argv.index("--cut")
    a, b = argv[i + 1].split(":")
    cuts.append((float(a), float(b)))
    del argv[i:i + 2]
hold = 0.0
if "--hold" in argv:
    i = argv.index("--hold")
    hold = float(argv[i + 1])
    del argv[i:i + 2]
src, out = argv[0], argv[1]
m = json.load(open(f"{src}/marks.json"))
dur = m["duration"]
segs, t = [], 0.0
for s, e, speed in sorted(m["ff"]):
    if s > t + 0.05:
        segs.append((t, s, 1))
    segs.append((s, e, speed))
    t = e
if t < dur - 0.05:
    segs.append((t, dur, 1))
for c0, c1 in cuts:                     # leave the cut stretches out
    kept = []
    for s, e, sp in segs:
        if s < c0:
            kept.append((s, min(e, c0), sp))
        if e > c1:
            kept.append((max(s, c1), e, sp))
    segs = [g for g in kept if g[1] - g[0] > 0.02]


def atempo(speed):
    parts, x = [], float(speed)
    while x > 2.0:
        parts.append("atempo=2.0")
        x /= 2.0
    parts.append(f"atempo={x:.4f}")
    return ",".join(parts)


f = []
for i, (s, e, speed) in enumerate(segs):
    f.append(f"[0:v]trim=start={s:.3f}:end={e:.3f},setpts=(PTS-STARTPTS)/{speed}[v{i}]")
    f.append(f"[1:a]atrim=start={s:.3f}:end={e:.3f},asetpts=PTS-STARTPTS,{atempo(speed)}[a{i}]")
f.append("".join(f"[v{i}][a{i}]" for i in range(len(segs))) + f"concat=n={len(segs)}:v=1:a=1[vc][ac]")
f.append(f"[vc]fps=30,tpad=stop_mode=clone:stop_duration={hold:.3f}[vout]" if hold else "[vc]fps=30[vout]")
f.append(f"[ac]apad=pad_dur={hold:.3f}[aout]" if hold else "[ac]anull[aout]")
cmd = ["ffmpeg", "-y", "-loglevel", "error", "-i", f"{src}/raw.mp4", "-i", f"{src}/audio.webm",
       "-filter_complex", ";".join(f), "-map", "[vout]", "-map", "[aout]",
       "-c:v", "libx264", "-preset", "slow", "-crf", "20", "-pix_fmt", "yuv420p", "-movflags", "+faststart",
       "-c:a", "aac", "-b:a", "160k", out]
subprocess.run(cmd, check=True)
final = sum((e - s) / sp for s, e, sp in segs) + hold
print(f"{len(segs)} segments, {dur:.1f} s recorded -> {final:.1f} s: {out}")
