#!/usr/bin/env python3
"""Give a classic Mac application (MacBinary) its Finder icon and bundle.

    add_icon.py APP.bin --preset NAME [--out OUT.bin]
    add_icon.py APP.bin --icon art/x.txt [--doc TYPE=art/doc.txt ...] [--out OUT.bin]
    add_icon.py --list                      presets in icons.json
    add_icon.py --show APP.bin              the bundle resources and Finder flags of a file

Adds to the application's resource fork:
  ICN# 128           the application icon (32x32, one bit, plus mask)
  ICN# 129, 130 ...  one per distinct document icon
  FREF 128           'APPL' -> local icon 0
  FREF 129 ...       one per document type -> its local icon
  BNDL 128           owner = the creator, ICN# and FREF local -> resource IDs
  <creator> 0        the owner ("signature") resource: the application's name as a Pascal string
and in the MacBinary header sets hasBundle and clears "inited", so the Finder reads the bundle the
first time it sees the file (hcopy -m keeps hasBundle and also clears "inited" on copy-in).

Idempotent: every BNDL and FREF, the old owner resource and the ICN# resources the old BNDL named
are replaced, never duplicated. New resources go after the existing ones, so the data offsets of
the app's own resources don't change. The creator is never changed: a preset whose creator
differs from the file's is an error (set the creator in the app's build instead).

Presets (icons.json, next to this script) name the creator, the application icon and the document
types with their icons. Icon paths are relative to this folder: art/ (original designs, committed)
or private/ (designs of Nintendo/PopCap characters, gitignored).

Icon text format: 32 rows of 32 characters (shorter rows are padded with '.'); lines starting
with '#' are comments.
    X   black
    .   white; transparent if it connects (4-neighbours) to the edge through '.' or '_' cells,
        so the mask is the filled silhouette, as the classic Finder icons have it
    o   white, always opaque (inside the mask)
    _   transparent, always (holes)
Usable as a module: apply(app, preset=..., out=...), load_icon(path).
"""
import argparse
import json
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rsrc  # noqa: E402  (copy of mactools/rsrc.py)

PRESETS = os.path.join(HERE, "icons.json")
APP_ICON_ID = 128
HAS_BUNDLE, INITED = 0x20, 0x01          # high byte of the Finder flags (MacBinary byte 73)


def load_icon(path):
    """(icon, mask): 128 bytes each, from the text format above."""
    rows = []
    for ln in open(path, encoding="utf-8").read().splitlines():
        if ln.startswith("#") or not ln.strip():
            continue
        ln = ln.rstrip()
        if len(ln) > 32 or set(ln) - set("X.o_"):
            sys.exit(f"{path}: bad row {len(rows) + 1}: {ln!r} (32 of X . o _)")
        rows.append(ln.ljust(32, "."))
    if len(rows) != 32:
        sys.exit(f"{path}: {len(rows)} rows, need 32")
    # transparent: every '_' plus every '.' reachable from the edge through '.'/'_' (4-connected)
    clear = {(x, y) for y in range(32) for x in range(32) if rows[y][x] == "_"}
    seen = set()
    stack = [(x, y) for y in range(32) for x in range(32) if x in (0, 31) or y in (0, 31)]
    while stack:
        x, y = stack.pop()
        if not (0 <= x < 32 and 0 <= y < 32) or (x, y) in seen or rows[y][x] not in "._":
            continue
        seen.add((x, y))
        clear.add((x, y))
        stack += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
    icon, mask = bytearray(128), bytearray(128)
    for y in range(32):
        for x in range(32):
            bit = 0x80 >> (x & 7)
            if rows[y][x] == "X":
                icon[y * 4 + x // 8] |= bit
            if (x, y) not in clear:
                mask[y * 4 + x // 8] |= bit
    return bytes(icon), bytes(mask)


def load_presets():
    return {k: v for k, v in json.load(open(PRESETS, encoding="utf-8")).items() if not k.startswith("_")}


def ostype(s):
    b = s.encode("mac_roman") if isinstance(s, str) else s
    if len(b) != 4:
        sys.exit(f"type/creator must be 4 characters: {s!r}")
    return b


def bundle_resources(creator, app_icon, docs, name):
    """[resource dicts] for an app icon file, [(doc type, icon file)] and the owner string."""
    icon_files = [app_icon]
    for _, f in docs:
        if f not in icon_files:
            icon_files.append(f)
    res = []
    for i, f in enumerate(icon_files):
        icon, mask = load_icon(os.path.join(HERE, f) if not os.path.isabs(f) else f)
        res.append({"type": b"ICN#", "id": APP_ICON_ID + i, "name": None, "attrs": 0, "data": icon + mask})
    frefs = [(b"APPL", 0)] + [(ostype(t), icon_files.index(f)) for t, f in docs]
    for i, (t, local) in enumerate(frefs):
        res.append({"type": b"FREF", "id": 128 + i, "name": None, "attrs": 0,
                    "data": t + struct.pack(">h", local) + b"\0"})
    bndl = creator + struct.pack(">hh", 0, 1)
    bndl += b"ICN#" + struct.pack(">h", len(icon_files) - 1)
    bndl += b"".join(struct.pack(">hh", i, APP_ICON_ID + i) for i in range(len(icon_files)))
    bndl += b"FREF" + struct.pack(">h", len(frefs) - 1)
    bndl += b"".join(struct.pack(">hh", i, 128 + i) for i in range(len(frefs)))
    res.append({"type": b"BNDL", "id": 128, "name": None, "attrs": 0, "data": bndl})
    res.append({"type": creator, "id": 0, "name": None, "attrs": 0, "data": bytes([len(name)]) + name})
    return res


def parse_bndl(data):
    """{type: [(local, res id)]} and the owner of a BNDL resource."""
    owner = data[:4]
    n = struct.unpack(">h", data[6:8])[0] + 1
    p, out = 8, {}
    for _ in range(n):
        t, cnt = data[p:p + 4], struct.unpack(">h", data[p + 4:p + 6])[0] + 1
        p += 6
        out[t] = [struct.unpack(">hh", data[p + 4 * i:p + 4 * i + 4]) for i in range(cnt)]
        p += 4 * cnt
    return owner, out


def apply(app, preset=None, out=None, icon=None, docs=(), quiet=False):
    """Add the bundle to MacBinary file `app` (in place unless `out`); returns the output path."""
    if preset:
        p = load_presets().get(preset)
        if p is None:
            sys.exit(f"no preset {preset!r} in {PRESETS}")
        want_creator, icon = ostype(p["creator"]), p["app"]
        docs = [(d["type"], d["icon"]) for d in p.get("docs", [])]
    elif not icon:
        sys.exit("give --preset or --icon")
    header, data, fork = rsrc.read_macbinary(app)
    if header[0] != 0 or not 1 <= header[1] <= 63:
        sys.exit(f"{app}: not a MacBinary file")
    ftype, creator = header[65:69], header[69:73]
    name = header[2:2 + header[1]]
    if preset and creator != want_creator:
        sys.exit(f"{app}: creator {creator.decode('mac_roman')!r}, preset {preset!r} is for "
                 f"{want_creator.decode('mac_roman')!r}: set the creator in the app's build")
    if ftype != b"APPL":
        sys.exit(f"{app}: type {ftype!r}, not an application")
    res = rsrc.parse(fork) if fork else []
    old_icons = set()
    for r in res:
        if r["type"] == b"BNDL":
            _, m = parse_bndl(r["data"])
            old_icons |= {rid for _, rid in m.get(b"ICN#", [])}
    new = bundle_resources(creator, icon, docs, name)
    new_icons = {r["id"] for r in new if r["type"] == b"ICN#"}
    kept = [r for r in res if not (r["type"] in (b"BNDL", b"FREF")
                                   or (r["type"] == creator and r["id"] == 0)
                                   or (r["type"] == b"ICN#" and r["id"] in old_icons | new_icons))]
    fork2 = rsrc.build(kept + new)
    h = bytearray(header)
    h[73] = (h[73] | HAS_BUNDLE) & ~INITED & 0xFF
    out = out or app
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(os.path.abspath(out)), suffix=".bin")
    os.close(fd)
    rsrc.write_macbinary(tmp, bytes(h), data, fork2)
    os.chmod(tmp, os.stat(out).st_mode & 0o777 if os.path.exists(out) else 0o644)
    os.replace(tmp, out)
    if not quiet:
        print(f"{name.decode('mac_roman')}: creator {creator.decode('mac_roman')}, "
              f"{len(new_icons)} ICN#, FREF {', '.join(['APPL'] + [t for t, _ in docs])}; "
              f"resource fork {len(fork):,} -> {len(fork2):,} bytes -> {out}")
    return out


def show(path):
    header, data, fork = rsrc.read_macbinary(path)
    res = rsrc.parse(fork) if fork else []
    creator = header[69:73]
    print(f"{header[2:2 + header[1]].decode('mac_roman')}: {header[65:69].decode('mac_roman')}/"
          f"{creator.decode('mac_roman')}, Finder flags {header[73]:02x}{header[101]:02x} "
          f"(hasBundle {'yes' if header[73] & HAS_BUNDLE else 'no'}, inited {'yes' if header[73] & INITED else 'no'})")
    for r in res:
        if r["type"] == b"BNDL":
            owner, m = parse_bndl(r["data"])
            print(f"  BNDL {r['id']}: owner {owner.decode('mac_roman')}, " +
                  "; ".join(f"{t.decode()} {v}" for t, v in m.items()))
        elif r["type"] == b"FREF":
            print(f"  FREF {r['id']}: {r['data'][:4].decode('mac_roman')} -> local icon "
                  f"{struct.unpack('>h', r['data'][4:6])[0]}")
        elif r["type"] == b"ICN#":
            print(f"  ICN# {r['id']}: {len(r['data'])} bytes")
        elif r["type"] == creator and r["id"] == 0:
            print(f"  {creator.decode('mac_roman')} 0: {r['data'][1:1 + r['data'][0]]!r}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("app", nargs="?", help="MacBinary application")
    ap.add_argument("--preset", help="name in icons.json")
    ap.add_argument("--icon", help="application icon file (instead of a preset)")
    ap.add_argument("--doc", action="append", default=[], help="TYPE=icon file (with --icon)")
    ap.add_argument("--out", help="output MacBinary file (default: change APP in place)")
    ap.add_argument("--list", action="store_true", help="list the presets")
    ap.add_argument("--show", action="store_true", help="print the bundle of APP")
    a = ap.parse_args()
    if a.list:
        for k, p in load_presets().items():
            print(f"{k:12s} {p['creator']}  {p['app']}  " + ", ".join(f"{d['type']}={d['icon']}" for d in p.get("docs", [])))
        return
    if not a.app:
        ap.error("APP is required")
    if a.show:
        return show(a.app)
    docs = [tuple(d.split("=", 1)) for d in a.doc]
    apply(a.app, preset=a.preset, out=a.out, icon=a.icon, docs=docs)


if __name__ == "__main__":
    main()
