#!/usr/bin/env python3
"""Assemble the static site in web/dist/ (no build step at deploy time).

    python3 -I web/build_site.py [--disk web/build/disk/hd.dsk] [--out web/dist]

What goes in:
  index.html, style.css, app.js, view3d.js, worker.js, audio-worklet.js   from web/src/
  assets/minivmac-Plus.<hash>.js/.wasm   Mini vMac (Mac Plus) as built in Infinite Mac, pinned to
                                         commit IM_COMMIT and checked by SHA-256
  assets/macplus.<hash>.rom              the Mac Plus ROM (emu/roms/vMac.ROM)
  assets/hd.<hash>.dsk.gz                the baked disk (build_disk.py, then tools/bake.mjs), gzipped
  assets/three-<ver>/...                 three.js (+ GLTFLoader, meshopt decoder), from npm, pinned
  assets/mac128k.<hash>.glb              the 3D Mac (tools/prepare_model.py), if it has been prepared
  licenses/                              GPL v2 (Mini vMac), Apache 2.0 (Infinite Mac), MIT (three.js),
                                         the FLUX Kommunity License (as a page and as text)
  _headers                               Cloudflare Pages headers: cross-origin isolation (needed for
                                         SharedArrayBuffer) and long caching for assets/

Every file must be 25 MiB or less (Cloudflare Pages); the script checks.
"""
import argparse
import gzip
import hashlib
import html
import json
import os
import shutil
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(HERE, "src")
VENDOR = os.path.join(HERE, "vendor")
BUILD = os.path.join(HERE, "build")

IM_COMMIT = "f988f537fff09c7b902e448a21cd8ac170e42c67"         # mihaip/infinite-mac
MINIVMAC_COMMIT = "0bcab1d51ce240489b98b21b744ddfcddf01659e"   # mihaip/minivmac (its submodule)
CORE = {   # path in infinite-mac -> sha256
    "src/emulator/worker/emscripten/minivmac-Plus.js": "fa37f2e5e05c704eca64feeff7d0d5b01f929403dddc2e8b52e0cb450a447a3b",
    "src/emulator/worker/emscripten/minivmac-Plus.wasm": "4b766647091ab0bacd876ab815f6e33d25d2848cd744c1e3c2cb4ef7b8f8aec5",
}
ROM = os.path.join(ROOT, "emu", "roms", "vMac.ROM")
ROM_SHA = "dd908e2b65772a6b1f0c859c24e9a0d3dcde17b1c6a24f4abd8955846d7895e7"
THREE_VERSION = "0.186.1"
THREE_FILES = ["build/three.module.js", "build/three.core.js", "examples/jsm/loaders/GLTFLoader.js",
               "examples/jsm/utils/BufferGeometryUtils.js", "examples/jsm/utils/SkeletonUtils.js",
               "examples/jsm/libs/meshopt_decoder.module.js", "examples/jsm/environments/RoomEnvironment.js",
               "LICENSE"]
MAX_FILE = 25 * 1024 * 1024


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def vendor_file(rel, url, want_sha):
    path = os.path.join(VENDOR, f"infinite-mac-{IM_COMMIT[:7]}", rel)
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        print("downloading", url)
        with urllib.request.urlopen(url, timeout=120) as r:
            data = r.read()
        open(path, "wb").write(data)
    data = open(path, "rb").read()
    if sha256(data) != want_sha:
        sys.exit(f"{path}: SHA-256 mismatch (expected the pinned build)")
    return data


def fetch_once(path, url):
    """A pinned file in web/vendor, downloaded the first time."""
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        print("downloading", url)
        with urllib.request.urlopen(url, timeout=120) as r:
            open(path, "wb").write(r.read())
    return path


def put(out, rel, data):
    path = os.path.join(out, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(data)
    return rel


def hashed(out, stem, ext, data):
    return put(out, f"assets/{stem}.{sha256(data)[:10]}{ext}", data)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--disk", default=os.path.join(BUILD, "disk", "hd.dsk"))
    ap.add_argument("--model", default=os.path.join(BUILD, "model", "mac128k.glb"))
    ap.add_argument("--out", default=os.path.join(HERE, "dist"))
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    if os.path.exists(out) and os.listdir(out):
        # Never delete: build into a fresh folder and let the caller swap it in.
        sys.exit(f"{out} is not empty; pass --out to a new folder")
    os.makedirs(out, exist_ok=True)

    cfg = {}
    # Emulator core (pinned, verified).
    js = vendor_file(list(CORE)[0], f"https://raw.githubusercontent.com/mihaip/infinite-mac/{IM_COMMIT}/{list(CORE)[0]}", CORE[list(CORE)[0]])
    wasm = vendor_file(list(CORE)[1], f"https://raw.githubusercontent.com/mihaip/infinite-mac/{IM_COMMIT}/{list(CORE)[1]}", CORE[list(CORE)[1]])
    cfg["core"] = hashed(out, "minivmac-Plus", ".js", js)
    cfg["wasm"] = hashed(out, "minivmac-Plus", ".wasm", wasm)
    cfg["wasmSize"] = len(wasm)

    rom = open(ROM, "rb").read()
    if sha256(rom) != ROM_SHA or len(rom) != 131072:
        sys.exit("unexpected ROM (want the Mac Plus ROM, 128 KB)")
    cfg["rom"] = hashed(out, "macplus", ".rom", rom)
    cfg["romSize"] = len(rom)

    if not os.path.exists(a.disk):
        sys.exit(f"no disk at {a.disk}: run build_disk.py and tools/bake.mjs first")
    disk = open(a.disk, "rb").read()
    gz = gzip.compress(disk, compresslevel=9, mtime=0)
    cfg["disk"] = hashed(out, "hd", ".dsk.gz", gz)
    cfg["diskSize"] = len(disk)
    cfg["diskGzSize"] = len(gz)

    # three.js, pinned (npm install three@THREE_VERSION in web/build).
    tdir = os.path.join(BUILD, "node_modules", "three")
    pkg = json.load(open(os.path.join(tdir, "package.json")))
    if pkg["version"] != THREE_VERSION:
        sys.exit(f"web/build/node_modules/three is {pkg['version']}, want {THREE_VERSION}")
    tbase = f"assets/three-{THREE_VERSION}"
    for rel in THREE_FILES:
        put(out, f"{tbase}/{rel}", open(os.path.join(tdir, rel), "rb").read())
    cfg["three"] = f"{tbase}/build/three.module.js"
    cfg["threeAddons"] = f"{tbase}/examples/jsm/"

    if os.path.exists(a.model):
        model = open(a.model, "rb").read()
        cfg["model"] = hashed(out, "mac128k", ".glb", model)
        cfg["modelSize"] = len(model)
        meta = a.model[:-4] + ".json"
        if os.path.exists(meta):
            cfg["modelInfo"] = json.load(open(meta))
    else:
        print("note: no prepared 3D model; the page uses its built-in stand-in")

    # The badge font (EB Garamond, SIL OFL 1.1), self-hosted.
    fdir = os.path.join(BUILD, "node_modules", "@fontsource", "eb-garamond")
    font = open(os.path.join(fdir, "files", "eb-garamond-latin-500-normal.woff2"), "rb").read()
    cfg["badgeFont"] = hashed(out, "eb-garamond-500", ".woff2", font)

    # The StuffIt decoder for Insert Disk (tools/build_sit_wasm.sh; the `stuffit` crate, MIT).
    sitw = os.path.join(BUILD, "sit_wasm", "sit.wasm")
    if os.path.exists(sitw):
        cfg["sitWasm"] = "./" + hashed(out, "sit", ".wasm", open(sitw, "rb").read())
        lic_src = os.path.join(BUILD, "sit_wasm", "LICENSE-MIT")
        if os.path.exists(lic_src):
            os.makedirs(os.path.join(out, "licenses"), exist_ok=True)
            shutil.copyfile(lic_src, os.path.join(out, "licenses", "stuffit-rs-mit.txt"))
    else:
        print("note: no StuffIt decoder (build/sit_wasm/sit.wasm missing): .sit files can't be inserted")

    # The 3D floppy disk (tools/prepare_floppy.mjs; "Floppy disk" by drumdorf, CC BY-SA 4.0, modified).
    fl_glb = os.path.join(BUILD, "model", "floppy.glb")
    if os.path.exists(fl_glb):
        cfg["floppy"] = hashed(out, "floppy", ".glb", open(fl_glb, "rb").read())
        os.makedirs(os.path.join(out, "licenses"), exist_ok=True)
        notice = open(os.path.join(VENDOR, "floppy", "license.txt"), encoding="utf-8").read().strip()
        open(os.path.join(out, "licenses", "floppy-model-cc-by-sa-4.0.txt"), "w", encoding="utf-8").write(
            notice + "\n\nModified (resized, compressed textures; spec-gloss converted to metal-rough) for this site; "
            "the modified model is also licensed CC BY-SA 4.0: https://creativecommons.org/licenses/by-sa/4.0/\n")
    else:
        print("note: no 3D floppy (build/model/floppy.glb missing)")

    # The machine's sounds (tools/make_sfx.mjs from Devon's licensed Epidemic Sound recordings).
    sman = os.path.join(BUILD, "sfx", "manifest.json")
    if os.path.exists(sman):
        m = json.load(open(sman))
        sdir = os.path.join(out, "assets", "sfx")
        os.makedirs(sdir, exist_ok=True)
        for c in m["clips"].values():
            for ext in (".ogg", ".mp3"):
                shutil.copyfile(os.path.join(BUILD, "sfx", c["file"] + ext), os.path.join(sdir, c["file"] + ext))
        cfg["sfx"] = {"base": "assets/sfx/", "clips": {k: {"file": v["file"]} for k, v in m["clips"].items()}, "timing": m.get("timing", {})}
        print("machine sounds:", ", ".join(m["clips"]))
    else:
        print("note: no machine sounds (build/sfx/manifest.json missing)")

    # fflate (MIT), for unzipping a .zip disk image in Insert Disk: self-hosted, loaded only then.
    fl = os.path.join(BUILD, "node_modules", "fflate")
    flver = json.load(open(os.path.join(fl, "package.json")))["version"]
    cfg["fflate"] = "./" + hashed(out, f"fflate-{flver}", ".js", open(os.path.join(fl, "esm", "browser.js"), "rb").read())
    os.makedirs(os.path.join(out, "licenses"), exist_ok=True)
    shutil.copyfile(os.path.join(fl, "LICENSE"), os.path.join(out, "licenses", "fflate-mit.txt"))

    # The 3D keyboard's and mouse's click sounds: clips cut from real recordings by
    # tools/make_clicks.mjs (build/clicks; sources and licences in clicks.json). Without them the page
    # has no click sounds (it never synthesises them).
    clicks = os.path.join(BUILD, "clicks")
    man = os.path.join(clicks, "manifest.json")
    if os.path.exists(man):
        m = json.load(open(man))
        cdir = os.path.join(out, "assets", "clicks")
        os.makedirs(cdir, exist_ok=True)
        roles = ["keyDown", "keyUp", "bigDown", "bigUp", "mouseDown", "mouseUp"]
        for r in roles:
            for name in m.get(r, []):
                for ext in (".ogg", ".mp3"):
                    shutil.copyfile(os.path.join(clicks, name + ext), os.path.join(cdir, name + ext))
        cfg["clicks"] = {"base": "assets/clicks/", **{r: m.get(r, []) for r in roles}}
        print("click sounds:", ", ".join(f"{r} {len(m.get(r, []))}" for r in roles))
    else:
        print("note: no click sounds (build/clicks/manifest.json missing)")

    # Licences.
    lic = os.path.join(out, "licenses")
    os.makedirs(lic, exist_ok=True)
    shutil.copyfile(fetch_once(os.path.join(VENDOR, f"minivmac-{MINIVMAC_COMMIT[:7]}", "COPYING.txt"),
                               f"https://raw.githubusercontent.com/mihaip/minivmac/{MINIVMAC_COMMIT}/COPYING.txt"),
                    os.path.join(lic, "minivmac-gpl-2.0.txt"))
    shutil.copyfile(fetch_once(os.path.join(VENDOR, f"infinite-mac-{IM_COMMIT[:7]}", "LICENSE"),
                               f"https://raw.githubusercontent.com/mihaip/infinite-mac/{IM_COMMIT}/LICENSE"),
                    os.path.join(lic, "infinite-mac-apache-2.0.txt"))
    shutil.copyfile(os.path.join(tdir, "LICENSE"), os.path.join(lic, "three.js-mit.txt"))
    shutil.copyfile(os.path.join(fdir, "LICENSE"), os.path.join(lic, "eb-garamond-ofl-1.1.txt"))
    model_lic = os.path.join(VENDOR, "mac128k_gltf", "license.txt")
    if "model" in cfg and os.path.exists(model_lic):
        shutil.copyfile(model_lic, os.path.join(lic, "mac128k-model-cc-by-nc-4.0.txt"))
    flux = open(os.path.join(HERE, "LICENSE-FLUX.md"), encoding="utf-8").read()
    open(os.path.join(lic, "flux-kommunity-license.txt"), "w", encoding="utf-8").write(flux)
    open(os.path.join(lic, "flux-kommunity-license.html"), "w", encoding="utf-8").write(
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>FLUX Kommunity License</title><style>body{margin:0;padding:24px 16px;background:#fff;color:#161616;"
        "font:15px/1.55 Geneva,Verdana,sans-serif}main{max-width:72ch;margin:0 auto}pre{white-space:pre-wrap;"
        "font:inherit}@media (prefers-color-scheme:dark){body{background:#1c1b19;color:#ecebe6}a{color:#9db4ff}}"
        "</style></head><body><main><p><a href=\"../#credits\">Back to the Mac</a></p><pre>"
        + html.escape(flux) + "</pre></main></body></html>")
    cfg["fluxLicense"] = "licenses/flux-kommunity-license.html"

    # Page sources.
    ver = {}
    for name in ("style.css", "app.js", "view3d.js", "controls.js", "clicks.js", "disks.js", "unpack.js", "hfs.js", "sfx.js", "worker.js", "audio-worklet.js", "favicon.svg"):
        p = os.path.join(SRC, name)
        if os.path.exists(p):
            data = open(p, "rb").read()
            put(out, name, data)
            ver[name] = sha256(data)[:10]
    cfg["worker"] = f"worker.js?v={ver['worker.js']}"
    cfg["worklet"] = f"audio-worklet.js?v={ver['audio-worklet.js']}"
    cfg["view3d"] = f"./view3d.js?v={ver.get('view3d.js', '0')}"
    cfg["disks"] = f"./disks.js?v={ver.get('disks.js', '0')}"
    page = open(os.path.join(SRC, "index.html"), encoding="utf-8").read()
    importmap = json.dumps({"imports": {"three": "./" + cfg["three"], "three/addons/": "./" + cfg["threeAddons"]}})
    for k, v in {
        "__MAC_CONFIG__": json.dumps(cfg, indent=1).replace("</", "<\\/"),
        "__IMPORTMAP__": importmap,
        "__STYLE__": f"style.css?v={ver['style.css']}",
        "__APP__": f"app.js?v={ver['app.js']}",
        "__FLUX_LICENSE__": cfg["fluxLicense"],
        "__GPL__": "licenses/minivmac-gpl-2.0.txt",
        "__APACHE__": "licenses/infinite-mac-apache-2.0.txt",
        "__THREE_LICENSE__": "licenses/three.js-mit.txt",
        "__MINIVMAC_COMMIT__": MINIVMAC_COMMIT, "__MINIVMAC_SHORT__": MINIVMAC_COMMIT[:7],
        "__IM_COMMIT__": IM_COMMIT, "__IM_SHORT__": IM_COMMIT[:7],
        "__THREE_VERSION__": THREE_VERSION,
    }.items():
        page = page.replace(k, v)
    if "__" in page.replace("__mac", ""):
        left = [w for w in page.split() if "__" in w]
        print("warning: placeholders left:", left[:5])
    put(out, "index.html", page.encode("utf-8"))

    # Cloudflare Pages headers. COOP + COEP make the page cross-origin isolated, which browsers
    # require before they allow SharedArrayBuffer (the emulator's input, sound and waiting).
    # Document-Isolation-Policy does the same inside an iframe on a page that isn't isolated
    # (devon7y.com embeds the Mac); only desktop Chromium 137+ supports it so far.
    open(os.path.join(out, "_headers"), "w").write(
        "/*\n"
        "  Cross-Origin-Opener-Policy: same-origin\n"
        "  Cross-Origin-Embedder-Policy: require-corp\n"
        "  Document-Isolation-Policy: isolate-and-require-corp\n"
        "  Cross-Origin-Resource-Policy: same-origin\n"
        "  X-Content-Type-Options: nosniff\n"
        "  Referrer-Policy: strict-origin-when-cross-origin\n"
        "/assets/*\n"
        "  Cache-Control: public, max-age=31536000, immutable\n"
        # The page and its unhashed scripts must be revalidated on every visit (ETag, cheap 304s);
        # otherwise Safari kept serving a stale index.html (and its old audio worklet) for hours.
        + "".join(f"/{n}\n  Cache-Control: no-cache\n" for n in ["", *sorted(
            f for f in os.listdir(out) if os.path.isfile(os.path.join(out, f)) and not f.startswith("_"))])
        + "/licenses/*\n  Cache-Control: no-cache\n")

    total = 0
    for dp, dn, fn in os.walk(out):
        for f in fn:
            s = os.path.getsize(os.path.join(dp, f))
            total += s
            if s > MAX_FILE:
                sys.exit(f"{f} is {s} bytes, over Cloudflare's 25 MiB limit")
    print(json.dumps({k: v for k, v in cfg.items() if k != "modelInfo"}, indent=1))
    print(f"{out}: {total / 1048576:.2f} MiB in all; download for a visit: "
          f"{(cfg['wasmSize'] + cfg['romSize'] + cfg['diskGzSize'] + cfg.get('modelSize', 0)) / 1048576:.2f} MiB + page and three.js")


if __name__ == "__main__":
    main()
