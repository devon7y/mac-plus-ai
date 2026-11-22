"""Check the C engine (host build, tune/build/host_tune) against tuneref.py token for token.

  python3 -I tune/ref/test_host.py tune/build/model [n]
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "corpus"))
import tuneref as R  # noqa: E402
import vocab as V  # noqa: E402

BIN = os.path.join(HERE, "..", "build", "host_tune")


def main():
    base = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 6
    m = R.Model(base + ".npz")
    bad = 0
    for i in range(n):
        typ = list(R.TYPE_METERS)[i % len(R.TYPE_METERS)]
        mode = V.MODES[i % 4]
        seed = 1000 + 7919 * i
        meter = R.pick_meter(typ, seed)
        g = R.Gen(m, typ, meter, mode, seed)
        py = g.run()
        out = subprocess.run([BIN, base + ".bin", str(V.TYPES.index(typ)), str(V.METERS.index(meter)),
                              str(V.MODES.index(mode)), str(seed), "256", "1152", "40"],
                             capture_output=True, text=True).stdout.split()
        c = [int(x) for x in out]
        same = c == py
        bad += not same
        print("%-10s %s seed %d: %d tokens %s" % (typ, mode, seed, len(py), "same" if same else "DIFFERENT"))
    print("RESULT", "PASS" if bad == 0 else "FAIL")


if __name__ == "__main__":
    main()
