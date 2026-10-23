#!/usr/bin/env python3
"""Tests for the bit-exact reference (M0). Run: python3 llm/ref/test_ref.py  (needs numpy)."""
import math
import os
import random
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import export  # noqa: E402
import kernels as K  # noqa: E402
import model  # noqa: E402
import sf  # noqa: E402
import verify_dumps  # noqa: E402

TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


def mild_weights(seed=1):
    """Random weights scaled like a trained network (residual updates smaller than the stream)."""
    fw = export.random_float_weights(seed, 0.3)
    for k in fw:
        if k.endswith(("wq", "wk")):
            fw[k] = fw[k] * 0.5
        if k.endswith(("wo", "w_down")):
            fw[k] = fw[k] * 0.3
    return fw


_PM = {}


def packed(seed=0):
    if seed not in _PM:
        _PM[seed] = export.export(export.random_float_weights(seed))
    return _PM[seed]


# ------------------------------------------------------------------ scale floats
@test
def sf_arithmetic():
    rnd = random.Random(1)
    for _ in range(20000):
        a = sf.from_int(rnd.randrange(1, 1 << 31))
        b = sf.from_int(rnd.randrange(1, 1 << 31))
        for c in (a, b, sf.mul(a, b), sf.div(a, b)):
            assert sf.M_LO <= c[0] < sf.M_HI
        q = (a[0] << 15) // b[0]
        assert (1 << 14) < q < (1 << 16)                   # one divu.w, no overflow
        assert sf.value(sf.div(a, b)) <= sf.value(a) / sf.value(b) * (1 + 1e-12)
        x = rnd.randrange(-32768, 32768)
        f = (rnd.randrange(1 << 14, 1 << 15), rnd.randrange(-40, -1))
        exact = x * f[0] / 2.0 ** -f[1]
        assert sf.apply(x, f) == math.floor(exact + 0.5)  # round half up
        v = rnd.randrange(1, 1 << 32)
        r = sf.sqrt_int(v)
        assert abs(sf.value(r) - math.sqrt(v)) <= math.sqrt(v) * 2 ** -13
    assert sf.apply(np.array([-3, -2, 2, 3]), (1 << 14, -15)).tolist() == [-1, -1, 1, 2]


@test
def quantizer_bounds():
    rnd = np.random.default_rng(2)
    for Q in (7, 58, 117, 127):
        for _ in range(3000):
            x = rnd.integers(-32768, 32768, 40) >> rnd.integers(0, 15)
            y, F = model.quant16(x, Q)
            M = np.abs(x).max()
            assert np.abs(y).max() <= Q
            if M:
                assert np.abs(y).max() >= Q - 1
                assert np.abs(y - x * (Q / M)).max() <= 0.5 + Q * 2.0 ** -13


# ------------------------------------------------------------------ kernels vs the 68000
@test
def bench_dumps_bit_exact():
    """The benchmark program ran in MAME (Mac Plus, 4 MB) and dumped its kernel test buffers;
    the reference kernels reproduce every word. Fresh dumps: testdata/bench_dumps."""
    assert verify_dumps.check(os.path.join(HERE, "testdata", "bench_layout.json"),
                              os.path.join(HERE, "testdata", "bench_dumps"), quiet=True)


@test
def tables_match_formulas():
    rnd = random.Random(3)
    for k in (4, 5):
        for _ in range(200):
            x = [rnd.randrange(-32768, 32768) for _ in range(k)]
            t = K.build_group_table(x, k)
            for idx in range(3 ** k):
                exact = sum(w * v for w, v in zip(K.trits_of(idx, k), x))
                assert t[idx] == K.s16(exact)
    for _ in range(100):
        h1, h2 = rnd.randrange(-32768, 32768), rnd.randrange(-32768, 32768)
        t = K.build_pair_table(h1, h2)
        for c in range(256):
            assert t[c] == K.s16((c // 16 - 8) * h1 + (c % 16 - 8) * h2)
    for u in (0, 1, 77, 4096, 4681):
        assert K.ptab_entries(u)[1:] == [(i - 8) * u for i in range(1, 16)]


@test
def packing_round_trips():
    rnd = np.random.default_rng(4)
    for n in (160, 512):
        T = rnd.integers(-1, 2, (37, n)).astype(np.int8)
        b5 = K.pack_B5(T)
        assert b5.shape == (37, K.groups(n, 5)) and b5.max() <= 242
        assert (K.unpack_B5(b5, n) == T).all()
        assert (K.w4_from_B5(b5, n) == K.pack_W(T, 4)).all()
    c = rnd.integers(-8, 8, (9, 160))
    assert (K.nibble_codes(K.pair_codes(c)) == c).all()


def replay_ternary(T, y, chunk, variant):
    """Run a traced ternary matvec through the emulated 68000 kernels, split like the Mac does."""
    k = 4 if variant[1] == "4" else 5
    n = T.shape[1]
    G = K.groups(n, k)
    tables = K.build_tables(list(y), k, G)
    if chunk is None:
        splits = [(0, G)]
    else:                                   # DN: two kernel calls per row, split at a group boundary
        g1 = K.groups(chunk, k) if (chunk % k == 0) else chunk // k
        splits = [(0, g1), (g1, G)]
    if variant[0] == "W":
        lut = np.array(K.lut_interleaved(tables), dtype=np.int64)
        codes = K.pack_W(T, k).astype(np.int64)
        parts = [lut[(codes[:, a:b] + 32768) // 2].sum(axis=1) for a, b in splits]
    else:
        lut = np.array(K.lut_contiguous(tables), dtype=np.int64)
        idx = (K.pack_B4(T) // 2 if k == 4 else K.pack_B5(T)).astype(np.int64)
        off = np.arange(G) * 3 ** k
        parts = [lut[idx[:, a:b] + off[a:b]].sum(axis=1) for a, b in splits]
    words = [np.array([K.s16(int(v)) for v in p]) for p in parts]   # each kernel call: 16-bit
    return sum(w.astype(np.int64) for w in words)


@test
def model_kernels_replay_bit_exact():
    """Capture every kernel call of real tokens (past the 64-token window) and replay the inputs
    through the emulated 68000 kernels (all four storage variants): identical integers."""
    m = model.IntModel(packed(0))
    for t in range(66):
        m.step((t * 37) % model.V, want_logits=False)
    model.TRACE = []
    try:
        m.step(123)
        trace = model.TRACE
    finally:
        model.TRACE = None
    kinds = {}
    for ev in trace:
        kinds[ev[0]] = kinds.get(ev[0], 0) + 1
        if ev[0] == "tern":
            _, T, y, chunk, out = ev
            for variant in ("W4", "W5", "B4", "B5"):
                assert (replay_ternary(T, y, chunk, variant) == out).all(), variant
        elif ev[0] == "score":
            _, kcs, qh, out = ev
            lut = np.array(K.build_pair_tables(list(qh)), dtype=np.int64)
            pc = K.pair_codes(kcs).astype(np.int64)
            got = [K.s16(int(v)) for v in lut[(pc + 32768) // 2].sum(axis=1)]
            assert got == out.tolist()
        elif ev[0] == "wsum":
            _, vcs, u, out = ev
            ptab = [w for ut in u for w in K.ptab_entries(int(ut))]
            vb = [b for row in vcs for b in K.vbytes_of(row)]
            assert K.wsum(vb, ptab) == out.tolist()
        elif ev[0] == "vocab":
            _, codes, yf, out = ev
            lut = np.array(K.build_pair_tables(list(yf)), dtype=np.int64)
            pc = K.pair_codes(codes).astype(np.int64)
            P = pc.shape[1]
            parts = [lut[(pc[:, a:b] + 32768) // 2].sum(axis=1) for a, b in ((0, P // 2), (P // 2, P))]
            got = sum(np.array([K.s16(int(v)) for v in p], dtype=np.int64) for p in parts)
            assert (got == out).all()
    assert kinds == {"tern": 6 * 5, "score": 6 * 4, "wsum": 6 * 4, "vocab": 1}, kinds


@test
def worst_case_bounds():
    """The overflow proofs (SPEC 4) at their extremes, through the emulated kernels."""
    T = np.ones((1, 160), dtype=np.int8)
    for v in ("W4", "W5", "B4", "B5"):
        assert replay_ternary(T, np.full(160, 127), None, v)[0] == 20320
        assert replay_ternary(-T, np.full(160, 127), None, v)[0] == -20320
    Td = np.ones((1, 512), dtype=np.int8)
    for v in ("W4", "W5", "B4", "B5"):
        assert replay_ternary(Td, np.full(512, 127), 256, v)[0] == 512 * 127   # halves 32512 / 32639
    lut = K.build_pair_tables([117] * 40)
    assert K.s16(sum(lut[p * 256 + 15 * 16 + 15] for p in range(20))) == 32760   # q.k: 40*7*117
    lut = K.build_pair_tables([58] * 80)
    assert K.s16(sum(lut[p * 256 + 15 * 16 + 15] for p in range(40))) == 32480   # vocab chunk
    u = [4128 // 64] * 64
    assert K.wsum([30] * 64 * 40, [w for ut in u for w in K.ptab_entries(ut)])[0] == 7 * sum(u)


# ------------------------------------------------------------------ model file and determinism
@test
def model_file_round_trip():
    pm = packed(0)
    raw = export.to_bytes(pm, huff=False)               # B5 only, no entropy coding
    data = export.to_bytes(pm)                          # shipped: Huffman-coded embedding + ternary bytes
    assert len(raw) == 1448780 and len(data) < len(raw) - 20000, (len(raw), len(data))
    assert raw[56:64] == data[56:64]                    # same decoded-model checksum
    pm2 = export.from_bytes(data)
    pm3 = export.from_bytes(raw)
    for a_, b_ in zip(pm2["layers"], pm3["layers"]):
        assert (a_["wqkv"] == b_["wqkv"]).all() and (a_["experts"][3]["wd"] == b_["experts"][3]["wd"]).all()
    bad = bytearray(data)
    bad[len(bad) // 2] ^= 1
    try:
        export.from_bytes(bytes(bad))
        raise AssertionError("corruption not detected")
    except AssertionError as e:
        assert "corruption" not in str(e)
    a, b = model.IntModel(pm), model.IntModel(pm2)
    for t in (3, 99, 2047):
        ka, sa = a.step(t)
        kb, sb = b.step(t)
        assert (ka == kb).all() and sa == sb


GOLDEN = None   # filled below from GOLDEN_FILE


@test
def deterministic_generation_golden():
    """Same model + prompt + seed -> same tokens (the M5 differential test uses these)."""
    pm = export.export(export.random_float_weights(2, 0.08))   # a "lively" random model
    prompt = [1, 2, 3, 500, 1000, 1500, 2000]
    runs = [model.generate(model.IntModel(pm), prompt, 80, seed=12345, temp256=256) for _ in range(2)]
    greedy = model.generate(model.IntModel(pm), prompt, 20, temp256=0)
    assert runs[0] == runs[1]
    assert len(runs[0]) == 80 and len(set(runs[0])) > 40          # sampling really varies
    gold = os.path.join(HERE, "testdata", "golden_tokens.txt")
    line = " ".join(map(str, runs[0])) + "\n" + " ".join(map(str, greedy)) + "\n"
    if not os.path.exists(gold):
        open(gold, "w").write(line)
    assert open(gold).read() == line, "reference output changed (delete testdata/golden_tokens.txt only if intended)"


# ------------------------------------------------------------------ float tracking
def logit_stats(fwf, pm, n=80, qat=True):
    im = model.IntModel(pm)
    fm = model.FloatModel(export.dequantize(pm) if fwf is None else fwf, qat=qat)
    toks = np.random.default_rng(101).integers(0, model.V, n)
    cs, t1 = [], []
    for t in toks:
        Kl, sig = im.step(int(t))
        li = Kl * sf.value(sig)
        lf = fm.step(int(t))
        cs.append(np.corrcoef(li, lf)[0, 1])
        t1.append(np.argmax(li) == np.argmax(lf))
    return float(np.mean(cs)), float(np.mean(t1))


@test
def integer_tracks_float():
    pm = export.export(mild_weights(1))
    c, t = logit_stats(None, pm, qat=True)
    print(f"      int vs QAT float (mild random model): logit corr {c:.4f}, top-1 agree {t:.2f}")
    assert c > 0.995 and t >= 0.95
    c2, t2 = logit_stats(None, pm, qat=False)
    print(f"      int vs float activations, same weights: logit corr {c2:.4f}, top-1 agree {t2:.2f}")
    assert c2 > 0.98 and t2 >= 0.9


def main():
    ok = True
    t0 = time.time()
    for fn in TESTS:
        t = time.time()
        try:
            fn()
            print(f"PASS  {fn.__name__}  ({time.time() - t:.1f}s)")
        except Exception as e:      # noqa: BLE001
            ok = False
            import traceback
            traceback.print_exc()
            print(f"FAIL  {fn.__name__}: {e!r}")
    print(f"{'ALL TESTS PASSED' if ok else 'SOME TESTS FAILED'} ({len(TESTS)} tests, {time.time() - t0:.0f}s)")
    return ok


if __name__ == "__main__":
    sys.exit(0 if main() else 1)
