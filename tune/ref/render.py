"""Render Tune tokens to a WAV on the host, imitating the Mac's player (tune/app/player.c, main.c):
fiddle wavetable melody with the same envelope and vibrato, and the same bass rules (root on the
first beat of each harmony span, fifth below on the others), repeats expanded.

  python3 -I tune/ref/render.py tune/build/model.npz --type jig --mode maj --key D --seed 7 --out x.wav
"""
import argparse
import math
import os
import struct
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "corpus"))
import abcout  # noqa: E402
import tuneref as R  # noqa: E402
import vocab as V  # noqa: E402

FS = 22255
CHORDS = {"maj": [0, 4, 3, 5, 1], "min": [0, 4, 3, 6, 2, 5], "dor": [0, 3, 6, 2, 4], "mix": [0, 3, 6, 4, 1]}
BPM = {"reel": (168, 12), "jig": (104, 18), "hornpipe": (126, 12), "waltz": (150, 12), "march": (112, 12),
       "air": (76, 12), "slipjig": (104, 18), "strathspey": (116, 12), "polka": (136, 12)}


def wavetable(harm):
    i = np.arange(256)
    w = sum(a * np.sin(2 * np.pi * k * i / 256 + ph) for k, a, ph in harm)
    return w / np.abs(w).max()


FIDDLE = wavetable([(1, 1.0, 0), (2, 0.55, 0.3), (3, 0.42, 0.9), (4, 0.30, 1.4), (5, 0.22, 2.0), (6, 0.12, 2.6),
                    (7, 0.07, 3.1)])
BASS = wavetable([(1, 1.0, 0), (2, 0.35, 0), (3, 0.12, 0)])


def performance(toks, meter, mode, tonic):
    """-> melody [(midi or 0, ticks)], bass [(midi or 0, ticks)] in playing order (as main.c builds them)."""
    li, pc, tmidi, toct, sig = abcout.key_info(tonic, mode)
    st = V.State(meter)
    blen = st.blen
    bt = 18 if meter[1] == 8 else 12
    hs = blen if meter[1] == 8 else min(24, blen)
    mel, bas = [], []
    frag, frag_start = [], 0
    sec_m = sec_b = e1_m = e1_b = 0
    alt = 0

    def bass_midi(deg):
        m = tmidi - 12 + 12 * (deg // 7) + V.MODE_STEPS[mode][deg % 7]
        while m > 52:
            m -= 12
        while m < 38:
            m += 12
        return m

    def chord(h0, h1):
        best, bs = 0, -1
        for c, r in enumerate(CHORDS[mode]):
            sc = 0
            for p, d, deg in frag:
                o0, o1 = max(p, h0), min(p + d, h1)
                if o1 <= o0:
                    continue
                if (deg - r) % 7 in (0, 2, 4):
                    w = o1 - o0
                    w *= 3 if p == h0 else 2 if (p - h0) % bt == 0 else 1
                    sc += w + w // 2 if (deg - r) % 7 == 0 else w
            if c == 0:
                sc += (h1 - h0) // 2
            if sc > bs:
                best, bs = r, sc
        return best

    def close(p1):
        q = frag_start
        while q < p1:
            b0 = (q // bt) * bt
            b1 = min(b0 + bt, p1)
            if q != b0:
                bas.append((0, b1 - q))
                q = b1
                continue
            h0 = (q // hs) * hs
            h1 = min(h0 + hs, blen)
            r = chord(h0, h1)
            root, fifth = bass_midi(r), bass_midi(r + 4)
            if fifth > root:
                fifth -= 12
            if fifth < 33:
                fifth += 12
            bas.append((root if q == h0 else fifth, b1 - q))
            q = b1
        frag.clear()

    for t in toks[1:]:
        pos0, trip0 = st.pos, st.trip
        i = V.tok_info(t)
        if t != V.EOS:
            st.step(t)
        if i[0] == "pick":
            frag_start = (blen - i[1]) % blen
        elif i[0] in ("up", "down"):
            alt = 1 if i[0] == "up" else -1
        elif i[0] in ("note", "rest"):
            d = i[2] if i[0] == "note" else i[1]
            real = d * 2 // 3 if trip0 else d
            if i[0] == "note":
                mel.append((abcout.note_pitch(i[1], alt, mode, tmidi), real))
                frag.append((pos0, real, i[1]))
            else:
                mel.append((0, real))
            alt = 0
        elif i[0] in ("bar", "eos"):
            full = pos0 >= blen
            close(blen if full else pos0)
            if t in (V.RS, V.DBL):
                sec_m, sec_b = len(mel), len(bas)
            elif t in (V.RE, V.RER):
                mel += mel[sec_m:]
                bas += bas[sec_b:]
                sec_m, sec_b = len(mel), len(bas)
            elif t == V.E1:
                e1_m, e1_b = len(mel), len(bas)
            elif t == V.E2:
                mel += mel[sec_m:e1_m]
                bas += bas[sec_b:e1_b]
            frag_start = 0 if t == V.EOS else (st.pos if st.pos < blen else 0)
    return mel, bas


def synth(mel, bas, bpm, beat):
    tps = bpm * beat / 60.0                       # ticks per second
    n = int(sum(d for _, d in mel) / tps * FS) + FS
    out = np.zeros(n)
    t = 0.0
    for midi, d in mel:
        dur = d / tps
        s0, ns = int(t * FS), int(dur * FS)
        if midi:
            f = 440 * 2 ** ((midi - 69) / 12)
            k = np.arange(ns)
            gate = max(1, ns - int(FS * (2 if d >= 8 else 1 if d >= 4 else 0) / tps))
            env = np.where(k < gate, np.clip(0.8 + k / (FS / 60) * 0.2, 0, 1) * (1 - 0.25 * np.minimum(k / FS / 0.6, 1)),
                           np.maximum(0, 1 - (k - gate) / (FS / 25)))
            vib = 1 + 0.0035 * np.sin(2 * np.pi * 5.6 * k / FS) * (k > FS * 0.2)
            ph = np.cumsum(np.full(ns, f * 256 / FS) * vib)
            out[s0:s0 + ns] += 0.6 * env * FIDDLE[(ph.astype(int)) % 256]
        t += dur
    t = 0.0
    for midi, d in bas:
        dur = d / tps
        s0, ns = int(t * FS), int(dur * FS)
        if midi:
            f = 440 * 2 ** ((midi - 69) / 12)
            ph = (np.arange(ns) * f * 256 / FS).astype(int) % 256
            out[s0:s0 + ns] += 0.32 * BASS[ph]
        t += dur
    return np.clip(out, -1, 1)


def write_wav(path, x):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes((x * 30000).astype("<i2").tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("--type", default="reel")
    ap.add_argument("--mode", default="maj")
    ap.add_argument("--key", default="D")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    m = R.Model(a.model)
    meter, toks = R.compose(m, a.type, a.mode, a.seed, maxbars=32)
    mel, bas = performance(toks, meter, a.mode, a.key)
    bpm, beat = BPM[a.type]
    if meter[1] == 8:
        beat = 18
        if a.type in ("march", "air"):
            bpm = 100 if a.type == "march" else 52
    write_wav(a.out, synth(mel, bas, bpm, beat))
    open(a.out[:-4] + ".abc", "w").write(abcout.to_abc(toks, meter, a.mode, a.key, a.type,
                                                       "%s, seed %d" % (a.type.capitalize(), a.seed), a.seed) + "\n")
    print(a.out, "%.1f s" % (sum(d for _, d in mel) / (bpm * beat / 60.0)))


if __name__ == "__main__":
    main()
