#!/usr/bin/env python3
"""sim68k: the Blockworld engine on an emulated bare 68000 (Musashi, via machine68k).

Builds tests/sim_main.c + the engine (render.c, world.c, tables.c, cast.s, raster.s) with
Retro68 into a flat binary, loads it into a 16 MB 68000 and calls its functions, counting
cycles exactly as a 68000 would (no wait states: MAME's Mac Plus runs at about the same rate,
7.8336 MHz; a real Plus is about 1.5x slower). Use it to check the 68000 code against the C
reference in seconds and to profile it cycle by cycle; MAME stays the judge of real speed.

    build/venv68k/bin/python tests/sim68k.py test [--n 32] [--cfg 0]   self-test (as bench.c)
    build/venv68k/bin/python tests/sim68k.py time                         cycles per phase
    build/venv68k/bin/python tests/sim68k.py prof [--cfg 0]               per-PC cycle profile

Needs: python -m venv build/venv68k && build/venv68k/bin/pip install machine68k
"""
import argparse
import collections
import hashlib
import os
import struct
import subprocess
import sys

import machine68k as M

HERE = os.path.dirname(os.path.abspath(__file__))
BW = os.path.dirname(HERE)
APP = os.path.join(BW, "app")
GEN = os.path.join(BW, "build", "gen")
OUT = os.path.join(BW, "build", "sim")
TB = os.path.expanduser("~/Retro68-build/toolchain/bin/")
CC = TB + "m68k-apple-macos-gcc"
LOAD = 0x1000
RAMKIB = 16384
STACK = 0xFF0000
TRAP_AT = 0x400

CFGS = [(96, 288, 16), (96, 288, 12), (80, 240, 16), (80, 240, 12), (64, 192, 12), (64, 192, 10),
        (72, 216, 12), (72, 216, 10), (80, 240, 10), (36, 216, 12), (88, 264, 12), (96, 288, 10),
        (96, 272, 10), (96, 272, 8), (112, 284, 10), (112, 284, 8), (128, 284, 10), (128, 284, 8), (96, 272, 12)]

LDS = """
SECTIONS {
  . = 0x%x;
  .text : { *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  .bss : { *(.bss .bss.* COMMON) }
  _end = .;
  /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
""" % LOAD


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit("failed: " + " ".join(cmd) + "\n" + r.stdout + r.stderr)
    return r.stdout


def build():
    os.makedirs(OUT, exist_ok=True)
    run(["python3", os.path.join(APP, "gen_tables.py"), GEN])
    run(["python3", os.path.join(APP, "gen_mobs.py"), GEN])
    srcs = [os.path.join(HERE, "sim_main.c"), os.path.join(APP, "render.c"), os.path.join(APP, "world.c"),
            os.path.join(APP, "obj.c"), os.path.join(GEN, "mobs_gen.c"),
            os.path.join(GEN, "tables.c"), os.path.join(APP, "cast.s"), os.path.join(APP, "raster.s"), os.path.join(APP, "segs.s"),
            os.path.join(APP, "lz.c"), os.path.join(APP, "unpack.s"), os.path.join(APP, "synth.c"),
            os.path.join(APP, "synth.s")] + ([os.path.join(APP, "lz.s")] if os.path.exists(os.path.join(APP, "lz.s")) else [])
    objs = []
    for s in srcs:
        o = os.path.join(OUT, os.path.basename(s) + ".o")
        cmd = [CC, "-c", "-o", o, s]
        if s.endswith(".c"):
            cmd[1:1] = ["-O2", "-Wall", "-Wno-pointer-sign", "--param=min-pagesize=0", "-ffunction-sections",
                        "-fno-asynchronous-unwind-tables", "-I" + APP, "-I" + GEN]
        run(cmd)
        objs.append(o)
    lds = os.path.join(OUT, "sim.ld")
    open(lds, "w").write(LDS)
    elf = os.path.join(OUT, "sim.elf")
    libgcc = run([CC, "-print-libgcc-file-name"]).strip()
    libc = run([CC, "-print-file-name=libc.a"]).strip()
    run([TB + "m68k-apple-macos-ld.bfd", "-T", lds, "-o", elf] + objs + [libc, libgcc])
    binf = os.path.join(OUT, "sim.bin")
    run([TB + "m68k-apple-macos-objcopy", "-O", "binary", "-R", ".bss", elf, binf])
    syms = {}
    for line in run([TB + "m68k-apple-macos-nm", elf]).splitlines():
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    return open(binf, "rb").read(), syms, elf


class Sim:
    def __init__(self, seed=2, world=None):
        self.code, self.sym, self.elf = build()
        self.m = M.Machine(M.CPUType.M68000, RAMKIB)
        self.cpu, self.mem = self.m.cpu, self.m.mem
        self.tid = self.m.traps.alloc(self._trap)
        self.mem.w_block(LOAD, self.code)
        self.mem.w16(TRAP_AT, 0xA000 | self.tid)
        self.mem.w32(0, STACK)
        self.mem.w32(4, TRAP_AT)
        self.cpu.pulse_reset()
        self.hook = None
        h = hashlib.sha1()
        for f in (os.path.join(APP, "world.c"), os.path.join(APP, "world.h"), os.path.join(GEN, "tables.c")):
            h.update(open(f, "rb").read())
        snap = os.path.join(OUT, "blocks_%s_%d.bin" % (h.hexdigest()[:12], seed))
        nb = 128 * 128 * 48
        self.w32("sim_args", seed, 0)
        if world:
            snap = world
        self.w32("sim_args", os.path.exists(snap), 1)
        blocks, cyc = self.call("sim_init")
        if os.path.exists(snap):
            self.mem.w_block(blocks, open(snap, "rb").read())
        else:
            print("world generated in %.1f M cycles (%.1f s at 7.83 MHz)" % (cyc / 1e6, cyc / 7.8336e6))
            open(snap, "wb").write(self.mem.r_block(blocks, nb))
        _, cyc = self.call("sim_segments")
        self.segcycles = cyc

    def _trap(self, op, pc):
        self.m.abort_execute(self.m.create_execute_end("ret"))

    def w32(self, name, v, i=0):
        self.mem.w32(self.sym[name] + 4 * i, v & 0xFFFFFFFF)

    def r32s(self, name, i=0):
        return self.mem.r32s(self.sym[name] + 4 * i)

    def call(self, fn, max_cycles=4_000_000_000):
        sp = STACK - 4
        self.mem.w32(sp, TRAP_AT)
        self.cpu.w_sp(sp)
        self.cpu.w_pc(self.sym[fn])
        total = 0
        while True:
            r = self.m.execute(10_000_000)
            total += r.cycles
            if r.result is not None:
                break
            if total > max_cycles:
                raise RuntimeError(fn + ": no return")
        return self.cpu.r_reg(0), total

    shade, lit, litdone = (0, 0, 0, 0), 0, False

    def config(self, nc, h, dmax):
        for i, v in enumerate((nc, h, dmax)):
            self.w32("sim_args", v, i)
        self.call("sim_config")
        if self.lit and not self.litdone:
            self.w32("sim_args", 7, 0)
            self.w32("sim_args", self.lit, 1)
            self.call("sim_lit")
            self.litdone = True
        for i, v in enumerate(self.shade):
            self.w32("sim_args", v, i)
        self.call("sim_shade")

    def cam(self, x, y, z, yaw, pitch):
        a = self.sym["sim_cam"]
        self.mem.w_block(a, struct.pack(">lllhh", x, y, z, yaw, pitch))

    def compare(self, step=1):
        self.w32("sim_args", step, 1)
        self.call("sim_compare")
        return [self.r32s("sim_out", i) for i in range(5)]

    def dumpcol(self, c):
        self.w32("sim_args", c)
        self.call("sim_dumpcol")
        a = self.sym["sim_dump"]
        out = []
        for w in range(2):
            l = []
            for i in range(120):
                v = struct.unpack(">4l", self.mem.r_block(a + (w * 120 + i) * 16, 16))
                if v[0] == -9999:
                    break
                l.append(v)
            out.append(l)
        return out


def bench_cams():
    import json
    return json.load(open(os.path.join(GEN, "bench_cams.json")))


PITCHMUL = 1


def selftest_cams(n):
    """the cameras of bench.c's self-test (i & 7 cycles the benchmark cameras)"""
    cams = bench_cams()
    for i in range(n):
        x, y, z, yaw, pitch = cams[i & 7][:5]
        yaw = (yaw + i * 37) & 1023
        pitch = (((i * 29) % 200) - 100) * PITCHMUL
        if i & 8:
            y += (i * 53) % 1600
        if i & 16:
            x += (i * 97) % 2048 - 1024
            z += (i * 61) % 2048 - 1024
        yield i, (x, y, z, yaw, pitch)


def file_cams(fn, n):
    for i, line in enumerate(open(fn)):
        if i >= n:
            break
        yield i, tuple(int(v) for v in line.split())


def cmd_test(s, a):
    bad = 0
    for ci in a.cfg:
        s.config(*CFGS[ci])
        for i, cam in (file_cams(a.cams, a.n) if a.cams else selftest_cams(a.n)):
            s.cam(*cam)
            if a.wide:
                s.call("sim_castw_c")
                s.call("sim_castw_asm")
                s.call("sim_raster16_c")
                s.call("sim_raster16_asm")
                o = s.compare(4)
            elif a.moving:
                s.call("sim_castm_c")
                s.call("sim_castm_asm")
                s.call("sim_raster8_c")
                s.call("sim_raster8_asm")
                o = s.compare(2)
            else:
                s.call("sim_cast_c")
                s.call("sim_cast_asm")
                s.call("sim_edges_c")
                s.call("sim_raster_c")
                s.call("sim_edges_asm")
                s.call("sim_raster_asm")
                o = s.compare()
            pres = s.call("sim_present")[0]
            if pres:
                o[4] = 100 + pres
            if o[0] or o[3] or o[4]:
                bad += 1
                if bad <= 3:
                    print("cfg %d cam %d %s: %d columns differ (first %d, run %d), edges %d, raster %d"
                          % (ci, i, cam, o[0], o[1], o[2], o[3], o[4]))
                    if o[0] and a.verbose:
                        c_runs, a_runs = s.dumpcol(o[1])
                        for k in range(max(len(c_runs), len(a_runs))):
                            print("   ", k, c_runs[k] if k < len(c_runs) else "-", a_runs[k] if k < len(a_runs) else "-",
                                  "<<" if k == o[2] else "")
    total = len(a.cfg) * a.n
    print("self-test: %d/%d frames bit-exact (cast, edges, raster)" % (total - bad, total))
    return bad


def cmd_sprites(s, a):
    """sprite.s against the C walk over the self-test cameras: moving and still frames"""
    bad = 0
    for ci in a.cfg:
        s.config(*CFGS[ci])
        for i, cam in selftest_cams(a.n):
            s.cam(*cam)
            for moving in (1, 0):
                s.call("sim_castm_asm" if moving else "sim_cast_asm")
                s.w32("sim_args", moving, 0)
                d = s.call("sim_spritecheck")[0]
                if d:
                    bad += 1
                    print("cfg %d cam %d moving %d: %d bytes differ" % (ci, i, moving, d))
    print("sprites: %d/%d frames identical (asm vs C)" % (len(a.cfg) * a.n * 2 - bad, len(a.cfg) * a.n * 2))
    return bad


def cmd_time(s, a):
    cams = bench_cams()
    if a.wide:
        print("Full size moving frames: columns 0, 4, ... cast, 16 px wide (raster16)")
    elif a.moving:
        print("moving frames: even columns cast, 8 px wide (raster8 has the edges)")
    print("cycles per frame (8 benchmark cameras), ms at 7.8336 MHz; x1.5 for a real Plus")
    print("%-14s %8s %8s %8s %8s %8s %6s" % ("config", "cast", "edges", "raster", "present", "total", "fps*"))
    for ci in a.cfg:
        s.config(*CFGS[ci])
        t = collections.Counter()
        for cam in cams:
            s.cam(*cam[:5])
            if a.wide:
                t["sim_cast_asm"] += s.call("sim_castw_asm")[1]
                t["sim_raster_asm"] += s.call("sim_raster16_asm")[1]
            elif a.moving:
                t["sim_cast_asm"] += s.call("sim_castm_asm")[1]
                t["sim_raster_asm"] += s.call("sim_raster8_asm")[1]
            else:
                for fn in ("sim_cast_asm", "sim_edges_asm", "sim_raster_asm"):
                    t[fn] += s.call(fn)[1]
            t["present"] += s.call("sim_present_only")[1]
        n = len(cams)
        c, e, r, p = (t[f] / n for f in ("sim_cast_asm", "sim_edges_asm", "sim_raster_asm", "present"))
        tot = c + e + r + p
        nc, h, d = CFGS[ci]
        ms = lambda v: v / 7833.6
        print("%-14s %8.1f %8.1f %8.1f %8.1f %8.1f %6.2f" % ("%dx%d/%d" % (nc * 4, h, d),
              ms(c), ms(e), ms(r), ms(p), ms(tot), 7.8336e6 / 1.5 / tot))


def cmd_prof(s, a):
    """per-label cycle profile of one function (default the 68000 caster) over the benchmark
    cameras: one instruction per execute() call, its cycles charged to its PC"""
    import bisect
    cams = bench_cams()
    s.config(*CFGS[a.cfg[0]])
    hist = collections.Counter()
    hits = collections.Counter()
    m, cpu = s.m, s.cpu
    for cam in (cams if a.fn != "sim_gen" else cams[:1]):
        s.cam(*cam[:5])
        if a.fn == "sim_gen":
            s.w32("sim_args", a.seed, 0)
        for fn in a.pre:
            s.call(fn)
        for i, v in enumerate(a.args or []):
            s.w32("sim_args", v, i)
        sp = STACK - 4
        s.mem.w32(sp, TRAP_AT)
        cpu.w_sp(sp)
        cpu.w_pc(s.sym[a.fn])
        while True:
            pc = cpu.r_pc()
            r = m.execute(a.sample)
            if a.sample > 1:
                pc = cpu.r_pc()             # sampling: charge the chunk to where it stopped
            hist[pc] += r.cycles
            hits[pc] += 1
            if r.result is not None:
                break
    syms = sorted((v, k) for k, v in labels(s.elf).items())
    keys = [v for v, k in syms]
    by = collections.Counter()
    for pc, n in hist.items():
        i = bisect.bisect_right(keys, pc) - 1
        by[syms[i][1] if i >= 0 else "?"] += n
    tot = sum(hist.values())
    n = len(cams)
    print("%s, config %s: %.0f cycles per frame (%.1f ms at 7.83 MHz)" % (a.fn, CFGS[a.cfg[0]], tot / n, tot / n / 7833.6))
    for k, v in by.most_common(a.top):
        print("%-24s %6.2f%%  %9d" % (k, 100 * v / tot, v // n))
    print("entries per frame:", "  ".join("%s %d" % (k, hits[v] // n) for v, k in syms
                                         if hits[v] and k[0] != "_" and not k.startswith("sim")))
    if a.pcs:
        print("hottest instructions:")
        for pc, v in hist.most_common(a.pcs):
            i = bisect.bisect_right(keys, pc) - 1
            print("  %06x %-12s+%-4d %6.2f%%" % (pc, syms[i][1], pc - syms[i][0], 100 * v / tot))


def cmd_save(s, a):
    """M7: world_pack / lz_pack / lz_unpack (C and 68000) / world_unpack against tools/bwsave.py and
    against world_build_segments: identical output, and their cycles"""
    sys.path.insert(0, os.path.join(BW, "tools"))
    sys.path.insert(0, os.path.join(BW, "engine"))
    import bwsave as S
    import world as W
    nb = 128 * 128 * 48
    blocks = s.mem.r_block(s.call("sim_init_blocks")[0], nb) if "sim_init_blocks" in s.sym else None
    ref = bytes(W.generate(a.seed)[0])
    ok = True
    def say(what, good, cyc=None):
        nonlocal ok
        ok &= bool(good)
        print("%-58s %s%s" % (what, "ok" if good else "FAIL", "" if cyc is None else "  %10d cycles = %6.0f ms at 7.83 MHz" % (cyc, cyc / 7833.6)))
    say("segment lists built by world_build_segments", True, s.segcycles)
    s.call("sim_snap")
    n, cyc = s.call("sim_pack")
    stream = s.mem.r_block(s.call("sim_pkraw")[0], n)
    pyst = S.pack_world(ref)
    say("world_pack: %d bytes, the same as bwsave.pack_world (%d columns)" % (n, stream[1024] * 256 + stream[1025]), stream == pyst, cyc)
    say("world_pack's room for it", s.call("sim_packcount")[0] >= n)
    pylz = S.lz_pack_mac(pyst)
    m, cyc = s.call("sim_lzpack_c")
    lz = s.mem.r_block(s.call("sim_pklz")[0], m)
    say("lz_pack_c: %d bytes, the same as bwsave.lz_pack_mac" % m, lz == pylz, cyc)
    m, cyc = s.call("sim_lzpack")
    lz = s.mem.r_block(s.call("sim_pklz")[0], m)
    say("lz_pack_asm: the same bytes", lz == pylz, cyc)
    for asm in (0, 1):
        s.w32("sim_args", asm, 0)
        r, cyc = s.call("sim_lzunpack")
        back = s.mem.r_block(s.call("sim_pkraw")[0], n)
        say("lz_unpack %s: the stream back" % ("68000" if asm else "C"), r == 0 and back == stream, cyc)
    best = S.lz_pack_best(pyst)
    s.mem.w_block(s.call("sim_pklz")[0], best)
    s.w32("sim_pklen", len(best), 1)
    s.w32("sim_args", 1, 0)
    r, cyc = s.call("sim_lzunpack")
    back = s.mem.r_block(s.call("sim_pkraw")[0], n)
    say("lz_unpack 68000 of bwsave.lz_pack_best (%d bytes)" % len(best), r == 0 and back == stream, cyc)
    for fn, what in (("sim_unpack_c", "world_unpack_c (C)"), ("sim_unpack", "world_unpack (unpack.s)")):
        s.call("sim_trash")
        r, cyc = s.call(fn)
        say("%s: 0" % what, r == 0, cyc)
        d = s.call("sim_snapcmp")[0]
        say("  the blocks and every segment list as before (%d)" % d, d == 0)
    n2, cyc = s.call("sim_pack")
    stream2 = s.mem.r_block(s.call("sim_pkraw")[0], n2)
    say("world_pack again after loading (lists shared): the same", stream2 == pyst, cyc)
    return 0 if ok else 1


def cmd_synth(s, a):
    """M7: synth.s against synth.c for every part of every effect (sound.c), and its cycles"""
    parts = [(35, 2, 3000, 1500, 22), (60, 2, 2500, 900, 44), (80, 2, 900, 400, 30), (50, 1, 160, 110, 40),
             (40, 1, 900, 900, 26), (10, 0, 0, 0, 0), (50, 1, 1300, 1300, 26), (50, 2, 1800, 1200, 30),
             (60, 2, 1600, 1000, 30), (90, 1, 500, 1000, 30), (700, 2, 7000, 6000, 18), (70, 1, 320, 260, 46),
             (110, 1, 330, 220, 46), (150, 1, 220, 90, 40), (90, 1, 420, 300, 54), (120, 1, 300, 170, 46),
             (120, 2, 3000, 800, 70), (300, 2, 800, 150, 60), (350, 2, 150, 60, 45)]
    bad, cyc_all, n_all = 0, 0, 0
    for ms, kind, f0, f1, amp in parts:
        for i, v in enumerate(((f0 << 16) | kind, ms, f1, amp)):
            s.w32("sim_args", v, i)
        r = s.call("sim_synth")[0]
        n, cyc = s.call("sim_synth_asm")
        cyc_all += cyc; n_all += n
        if a.verbose:
            print("  part %-28s %6d samples %5.1f cycles each" % ((ms, kind, f0, f1, amp), n, cyc / n))
        if r:
            bad += 1
            print("part", (ms, kind, f0, f1, amp), "differs at", r)
    print("synth: %d/%d parts identical; %d samples in %d cycles (%.0f a sample, %.0f ms at 7.83 MHz)"
          % (len(parts) - bad, len(parts), n_all, cyc_all, cyc_all / n_all, cyc_all / 7833.6))
    return bad


def labels(elf):
    """all symbols, local labels of the .s files included"""
    out = {}
    for line in run([TB + "m68k-apple-macos-objdump", "-t", elf]).splitlines():
        p = line.split()
        if len(p) >= 5 and p[-2] != "*ABS*" and ".text" in line:
            try:
                out[p[-1]] = int(p[0], 16)
            except ValueError:
                pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["test", "time", "prof", "sprites", "save", "synth"])
    ap.add_argument("--n", type=int, default=32)
    ap.add_argument("--cfg", type=int, nargs="*", default=None)
    ap.add_argument("--fn", default="sim_cast_asm")
    ap.add_argument("--pre", nargs="*", default=[], help="functions to call before --fn (e.g. sim_cast_asm)")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--pcs", type=int, default=0)
    ap.add_argument("--sample", type=int, default=1, help="cycles per sample (1: every instruction)")
    ap.add_argument("--seed", type=int, default=2)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("-m", "--moving", action="store_true", help="moving frames (even columns, 8 px)")
    ap.add_argument("-w", "--wide", action="store_true", help="Full size moving frames (columns 0, 4, ..., 16 px)")
    ap.add_argument("--world", help="blocks file (WX * WX * WY bytes) instead of the seed-2 world")
    ap.add_argument("--cams", help="camera file: x y z yaw pitch per line")
    ap.add_argument("--pitchmul", type=int, default=1, help="self-test cameras' pitch times this")
    ap.add_argument("--args", type=int, nargs="*", help="sim_args for --fn (e.g. sim_sprite: kind, distance 8.8)")
    ap.add_argument("--shade", default="0,0,0,0", help="dither per light class (0 sky, 1 tree, 2 covered, 3 torch)")
    ap.add_argument("--lit", type=int, default=0, help="torch light in 1 of N columns (random ranges)")
    a = ap.parse_args()
    if a.cfg is None:
        a.cfg = [0, 4] if a.cmd == "test" else [0, 1, 3, 5]
    global PITCHMUL
    PITCHMUL = a.pitchmul
    s = Sim(seed=a.seed, world=a.world)
    s.shade = [int(v) for v in a.shade.split(",")]
    s.lit = a.lit
    sys.exit(1 if {"test": cmd_test, "time": cmd_time, "prof": cmd_prof, "sprites": cmd_sprites, "save": cmd_save, "synth": cmd_synth}[a.cmd](s, a) else 0)


if __name__ == "__main__":
    main()
