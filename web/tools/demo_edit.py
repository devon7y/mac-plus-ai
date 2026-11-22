#!/usr/bin/env python3
"""Cut the final demo video from demo_video.mjs's recording:
    python3 web/tools/demo_edit.py <recording dir> <out.mp4> [--cut START:END ...] [--hold SECONDS] [--freeze MARK|SECONDS]
                                   [--overlay file=PNG,start=S,end=E[,x=X,y=Y][,fade=SECONDS] ...]
Plays the recording at normal speed except the stretches in marks.json["ff"] ([start, end, speed]), which
are sped up (sound too, keeping its pitch: atempo), and the --cut stretches (seconds of the recording),
which are left out. --hold freezes the last frame that much longer (silence under it), for the end card.
--freeze (a mark's name in marks.json["events"], or seconds of the recording) stops the picture there
and holds that frame to the end while the sound plays on (a sentence finishing under the end card).
--overlay lays a PNG (a card from demo_card.mjs, or a patch) over the output from start to end (seconds
of the output; negative counts back from its end; end=end for the end). Without x and y it sits where
demo_video.mjs's captions sit (centred, 22 px from the bottom); fade fades it in.
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
freeze = None
if "--freeze" in argv:
    i = argv.index("--freeze")
    freeze = argv[i + 1]
    del argv[i:i + 2]
overlays = []
while "--overlay" in argv:
    i = argv.index("--overlay")
    overlays.append(dict(kv.split("=", 1) for kv in argv[i + 1].split(",")))
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
total = sum((e - s) / sp for s, e, sp in segs)
if freeze is not None:                  # the picture stops at the freeze point (in output time) and holds
    ft = dict(m["events"]).get(freeze)
    ft = float(freeze) if ft is None else ft
    fo = sum((min(e, ft) - s) / sp for s, e, sp in segs if s < ft)
    f.append(f"[vc]fps=30,trim=end={fo:.3f},setpts=PTS-STARTPTS,tpad=stop_mode=clone:stop_duration={total - fo + hold:.3f}[vout]")
else:
    f.append(f"[vc]fps=30,tpad=stop_mode=clone:stop_duration={hold:.3f}[vout]" if hold else "[vc]fps=30[vout]")
f.append(f"[ac]apad=pad_dur={hold:.3f}[aout]" if hold else "[ac]anull[aout]")
end_t = total + hold
vlast = "vout"
for k, o in enumerate(overlays):                  # inputs 2, 3, ...: each PNG, looped
    t = lambda v: end_t if v == "end" else (end_t + float(v) if float(v) < 0 else float(v))    # noqa: E731
    a, b = t(o.get("start", "0")), t(o.get("end", "end"))
    fade = float(o.get("fade", 0))
    pos = f"x={o['x']}:y={o['y']}" if "x" in o else "x=(W-w)/2:y=H-h-22"
    src_ov = f"[{k + 2}:v]format=rgba" + (f",fade=in:st={a:.3f}:d={fade:.3f}:alpha=1" if fade else "") + f"[ov{k}]"
    f.append(src_ov)
    f.append(f"[{vlast}][ov{k}]overlay={pos}:enable='between(t,{a:.3f},{b:.3f})':shortest=1[vo{k}]")
    vlast = f"vo{k}"
ov_inputs = [x for o in overlays for x in ("-loop", "1", "-i", o["file"])]
cmd = ["ffmpeg", "-y", "-loglevel", "error", "-i", f"{src}/raw.mp4", "-i", f"{src}/audio.webm", *ov_inputs,
       "-filter_complex", ";".join(f), "-map", f"[{vlast}]", "-map", "[aout]",
       "-c:v", "libx264", "-preset", "slow", "-crf", "20", "-pix_fmt", "yuv420p", "-movflags", "+faststart",
       "-c:a", "aac", "-b:a", "160k", out]
subprocess.run(cmd, check=True)
final = sum((e - s) / sp for s, e, sp in segs) + hold
print(f"{len(segs)} segments, {dur:.1f} s recorded -> {final:.1f} s: {out}")
