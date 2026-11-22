#!/usr/bin/env python3
"""Broadside's sound effects, synthesised from scratch (no samples, nothing copied).

    gen_sounds.py OUT_DIR [--wav]

Writes OUT_DIR/sounds.c: 8-bit signed samples at 11,127 Hz (half the Mac Plus's 22,255 Hz
sound rate; the mixer plays each sample twice) and a table of (offset, length). --wav also writes
one WAV per sound into OUT_DIR for listening. Plain Python (no numpy), deterministic.
"""
import math
import os
import random
import struct
import sys
import wave

RATE = 11127
NAMES = ["fire", "splash", "explosion", "horn", "win", "lose", "click", "ping"]


def env_exp(t, attack, tau):
    if t < attack:
        return t / attack
    return math.exp(-(t - attack) / tau)


class LP:
    """one-pole low-pass"""
    def __init__(self, cutoff):
        self.y = 0.0
        self.set(cutoff)

    def set(self, cutoff):
        self.a = 1 - math.exp(-2 * math.pi * cutoff / RATE)

    def __call__(self, x):
        self.y += self.a * (x - self.y)
        return self.y


def fire(rng):
    n = int(0.32 * RATE)
    lp, lp2 = LP(500), LP(150)
    out = []
    for i in range(n):
        t = i / RATE
        noise = rng.uniform(-1, 1)
        thump = math.sin(2 * math.pi * (70 - 40 * t) * t) * math.exp(-t / 0.09)
        body = (lp(noise) * 2.2 + lp2(noise) * 2.5) * env_exp(t, 0.002, 0.07)
        out.append(0.75 * thump + body)
    return out


def splash(rng):
    n = int(0.6 * RATE)
    lp = LP(2200)
    lp_slow = LP(600)
    drops = [(0.16 + rng.random() * 0.35, 900 + rng.random() * 1300) for _ in range(6)]
    out = []
    prev = 0.0
    for i in range(n):
        t = i / RATE
        noise = rng.uniform(-1, 1)
        hiss = lp(noise) - lp_slow(noise)          # band-passed: "shhh"
        e = env_exp(t, 0.035, 0.16)
        s = hiss * 1.6 * e
        for (t0, f) in drops:
            if t0 <= t < t0 + 0.04:
                u = t - t0
                s += 0.22 * math.sin(2 * math.pi * f * (1 + 2.5 * u) * u) * (1 - u / 0.04)
        out.append(s)
        prev = s
    return out


def explosion(rng):
    n = int(0.85 * RATE)
    lp = LP(2000)
    lp2 = LP(120)
    out = []
    for i in range(n):
        t = i / RATE
        lp.set(2000 * math.exp(-t / 0.25) + 250)
        noise = rng.uniform(-1, 1)
        crackle = (rng.random() < 0.02 * math.exp(-t / 0.3)) * rng.uniform(-1, 1) * 1.5
        s = (lp(noise) * 2.0 + lp2(noise) * 3.5) * env_exp(t, 0.003, 0.22) + crackle
        s += 0.5 * math.sin(2 * math.pi * 45 * t) * math.exp(-t / 0.15)
        out.append(s)
    return out


def horn(rng):
    n = int(1.35 * RATE)
    lp = LP(700)
    bub = LP(900)
    out = []
    ph1 = ph2 = 0.0
    for i in range(n):
        t = i / RATE
        vib = 1 + 0.006 * math.sin(2 * math.pi * 5 * t)
        ph1 += 98 * vib / RATE
        ph2 += 147 * vib / RATE
        saw = (ph1 % 1.0) * 2 - 1 + 0.8 * ((ph2 % 1.0) * 2 - 1)
        a = min(1.0, t / 0.08) * (1.0 if t < 0.85 else max(0.0, 1 - (t - 0.85) / 0.25))
        s = lp(saw) * 1.3 * a
        if t > 0.9:                                  # bubbles as it goes down
            if rng.random() < 0.004:
                s += 0.0
            s += bub(rng.uniform(-1, 1)) * 0.6 * math.exp(-(t - 0.9) / 0.2) * (0.5 + 0.5 * math.sin(2 * math.pi * 23 * t))
        out.append(s)
    return out


def tune(notes, rng, duty=0.5, gain=0.45):
    out = []
    for (freq, dur) in notes:
        n = int(dur * RATE)
        ph = 0.0
        for i in range(n):
            t = i / RATE
            ph += freq / RATE
            sq = 1.0 if (ph % 1.0) < duty else -1.0
            sq += 0.5 * (1.0 if (2 * ph % 1.0) < duty else -1.0)
            out.append(gain * sq * env_exp(t, 0.004, dur * 0.6))
    return out


def win(rng):
    C5, E5, G5, C6 = 523.25, 659.25, 783.99, 1046.5
    return tune([(C5, 0.11), (E5, 0.11), (G5, 0.11), (C6, 0.4)], rng)


def lose(rng):
    G4, E4, C4, G3 = 392.0, 329.63, 261.63, 196.0
    return tune([(G4, 0.18), (E4, 0.18), (C4, 0.18), (G3, 0.45)], rng, duty=0.3)


def click(rng):
    return [math.sin(2 * math.pi * 1800 * i / RATE) * math.exp(-i / 40) * 0.6 for i in range(int(0.02 * RATE))]


def ping(rng):
    n = int(0.25 * RATE)
    return [math.sin(2 * math.pi * 1320 * i / RATE) * env_exp(i / RATE, 0.003, 0.07) * 0.5 for i in range(n)]


def to_s8(xs, peak=100):
    m = max(1e-9, max(abs(x) for x in xs))
    k = peak / m
    return [max(-127, min(127, int(round(x * k)))) for x in xs]


def main():
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    rng = random.Random(1986)
    funcs = [fire, splash, explosion, horn, win, lose, click, ping]
    peaks = [110, 70, 120, 95, 60, 60, 50, 60]
    data, table = [], []
    for f, pk in zip(funcs, peaks):
        s = to_s8(f(rng), pk)
        if len(s) & 1:
            s.append(0)
        table.append((len(data), len(s)))
        data += s
        if "--wav" in sys.argv:
            w = wave.open(os.path.join(out_dir, f.__name__ + ".wav"), "wb")
            w.setnchannels(1)
            w.setsampwidth(1)
            w.setframerate(RATE)
            w.writeframes(bytes((v + 128) & 255 for v in s))
            w.close()
    with open(os.path.join(out_dir, "sounds.c"), "w") as fo:
        fo.write("/* Generated by broadside/tools/gen_sounds.py: synthesised sound effects, 8-bit signed, %d Hz. */\n" % RATE)
        fo.write("#include \"sound.h\"\n")
        fo.write("const signed char snd_data[%d] = {\n" % len(data))
        for i in range(0, len(data), 24):
            fo.write("    " + ",".join(str(v) for v in data[i:i + 24]) + ",\n")
        fo.write("};\n")
        fo.write("const unsigned long snd_table[%d][2] = {\n" % len(table))
        for (o, n), name in zip(table, NAMES):
            fo.write("    {%d, %d},   /* %s */\n" % (o, n, name))
        fo.write("};\n")
    print("%d sounds, %d bytes (%.2f s)" % (len(table), len(data), len(data) / RATE))


if __name__ == "__main__":
    main()
