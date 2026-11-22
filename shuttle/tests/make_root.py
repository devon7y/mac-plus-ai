#!/usr/bin/env python3
"""Make the folder the test server serves: "Test Files/Big Test" (310,000 bytes: a 160,000-byte data fork
and a 150,000-byte resource fork of random bytes, type TSTF, creator Shtl, Finder flags 0x0100 "inited"
which the Mac must clear) and an empty "From Plus".

    python3 -I make_root.py <folder>
"""
import os
import random
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "server"))
import shuttle_server as S  # noqa: E402


def big_test():
    rng = random.Random(1986)
    return bytes(rng.getrandbits(8) for _ in range(160000)), bytes(rng.getrandbits(8) for _ in range(150000))


def main():
    root = sys.argv[1]
    os.makedirs(os.path.join(root, "Test Files"), exist_ok=True)
    os.makedirs(os.path.join(root, "From Plus"), exist_ok=True)
    data, rsrc = big_test()
    p = os.path.join(root, "Test Files", "Big Test")
    with open(p, "wb") as f:
        f.write(data)
    with open(S.rsrc_path(p), "wb") as f:
        f.write(rsrc)
    if not S.set_finder_info(p, b"TSTFShtl\x01\x00" + bytes(22)):
        sys.exit("couldn't set the Finder info (needs macOS)")
    print("test root:", root)


if __name__ == "__main__":
    main()
