#!/bin/zsh
# Build the whole site into web/dist (the folder to deploy to Cloudflare Pages):
#   1. build_disk.py       the Mac's disk from the projects' current builds (web/build/disk/hd_unbaked.dsk)
#   2. tools/bake.mjs      first boot in the browser emulator: Desktop file, Glider's prefs, clean
#                          shut down (web/build/disk/hd.dsk)
#   3. tools/prepare_model.mjs   the 3D Mac as a compressed .glb (only if it isn't there yet);
#      tools/make_clicks.mjs     the click sounds, cut from the recordings listed in clicks.json
#      tools/prepare_floppy.mjs  the 3D floppy disk (once); tools/make_sfx.mjs the machine's sounds (sfx.json)
#   4. build_site.py       the static site, into a new folder; then it replaces web/dist (the old one
#                          is moved aside to web/build/old_dist_<time>, never deleted)
# Needs hfsutils, python3 and node; the npm packages (exact versions) are installed into web/build
# on the first run. Playwright uses the browsers cached in ~/Library/Caches/ms-playwright
# (playwright-core 1.58.2 = chromium-1208 + webkit-2248).
set -e
cd "$(dirname "$0")"
stamp=$(date +%Y%m%d%H%M%S)
if [ ! -d build/node_modules/three ] || [ ! -d build/node_modules/@fontsource/eb-garamond ] || [ ! -d build/node_modules/fflate ]; then
  mkdir -p build
  [ -f build/package.json ] || echo '{"name":"macplus-web-build","private":true}' > build/package.json
  (cd build && npm install --no-audit --no-fund --save-exact playwright-core@1.58.2 three@0.186.1 \
    @gltf-transform/core@4.2.1 @gltf-transform/extensions@4.2.1 @gltf-transform/functions@4.2.1 meshoptimizer@0.22.0 \
    @fontsource/eb-garamond@5.2.7 fflate@0.8.2)
fi
python3 -I build_disk.py
node tools/bake.mjs build/disk/hd_unbaked.dsk build/disk/hd.dsk
[ -f build/model/mac128k.glb ] || node tools/prepare_model.mjs
node tools/make_clicks.mjs clicks.json build/clicks
[ -f build/model/floppy.glb ] || node tools/prepare_floppy.mjs
node tools/make_sfx.mjs sfx.json build/sfx
[ -f build/sit_wasm/sit.wasm ] || zsh tools/build_sit_wasm.sh
python3 -I build_site.py --out build/site_$stamp
if [ -e dist ]; then mv dist build/old_dist_$stamp; fi
mv build/site_$stamp dist
echo "web/dist is ready (built $stamp)"
