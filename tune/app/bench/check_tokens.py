"""Compare TuneBench's tokens (bench_tokens.txt from a MAME run) with tune/ref/tuneref.py.

  python3 -I tune/app/bench/check_tokens.py tune/build/model.npz emu/runs/<run>/bench_tokens.txt
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "ref"))
sys.path.insert(0, os.path.join(HERE, "..", "..", "corpus"))
import tuneref as R  # noqa: E402
import vocab as V  # noqa: E402

CFG = [(0, 0, 0, 1000), (1, 1, 1, 8919), (2, 0, 2, 16838), (5, 2, 3, 24757)]   # bench_main.c


def main():
    m = R.Model(sys.argv[1])
    lines = [l for l in open(sys.argv[2]).read().split("\n") if l.strip()]
    ok = 0
    for (ty, me, mo, seed), line in zip(CFG, lines):
        py = R.Gen(m, V.TYPES[ty], V.METERS[me], V.MODES[mo], seed).run()[:400]
        mac = [int(x) for x in line.split()]
        same = py == mac
        ok += same
        print("%-10s python %d mac %d tokens: %s" % (V.TYPES[ty], len(py), len(mac), "same" if same else "DIFFERENT"))
    print("RESULT", "PASS" if ok == len(lines) else "FAIL")


if __name__ == "__main__":
    main()
