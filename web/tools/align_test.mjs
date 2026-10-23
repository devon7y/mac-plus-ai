// Alignment and glass test for the 3D view (W2: the Mac's screen is drawn in WebGL on the glass).
//   node web/tools/align_test.mjs <dist dir> <chromium|chrome|webkit|firefox> <out dir>
// For each viewport, after "Click to start", at several zooms (the default, zoomed out, zoomed in,
// the closest) and two turned views:
//   1. Mapping: for Mac pixels at the corners, edges and middle, clientFromMac() (the point input
//      uses) is read back from a debug render of the picture with each pixel's Mac coordinates
//      encoded in its colour: the GPU must draw exactly that Mac pixel there (within one pixel when
//      a Mac pixel is smaller than a screen pixel). Then the real pointer is moved to two of those
//      points and the Mac's own mouse position (low memory $830) must match: clicks land on the
//      right Mac pixel.
//   2. Glass only: the frame is rendered with the glass's reflections on and off; every pixel that
//      differs must lie on the glass's own silhouette (a render with the glass in flat magenta and
//      the picture hidden, grown by 2 px for anti-aliasing). Pixels of the case and bezel just
//      outside the glass must be identical.
//   3. Black is glass: with reflections off, the Finder's black corner pixel (0, 341) and the bare
//      glass 2.5 mm beyond the raster's corner must match within 6 levels (zoomed in enough to
//      see a Mac pixel), so the raster's edge and rounded corners vanish into the glass.
// Writes results.json and screenshots (<viewport>_<view>.png) into <out dir>/<browser>/.
import fs from "node:fs";
import path from "node:path";
import { serve, launch, until, macApp, sleep, settled, peek } from "./lib.mjs";

const [dir, kind, outRoot] = process.argv.slice(2);
const out = path.join(outRoot, kind);
fs.mkdirSync(out, { recursive: true });
const VIEWPORTS = [[1280, 860], [1440, 900], [1920, 1080], [1024, 768], [1366, 768], [800, 1100]]
  .filter(([w, h]) => !process.env.ONLY || process.env.ONLY.split(",").includes(`${w}x${h}`));
const MAC_POINTS = [[0, 0], [511, 0], [0, 341], [511, 341], [256, 171], [100, 30], [400, 300], [17, 200], [480, 120]];
const srv = await serve(dir);
const results = [];
let failures = 0;

// In the page: render the scene to the canvas now and read its pixels back (same task, so the
// drawing buffer is still there).
const PAGE_HELPERS = `
window.__grab = () => {
  const v = window.__mac.view3d, r = v.renderer, gl = r.getContext();
  r.setRenderTarget(null); r.render(v.scene, v.camera);
  const w = gl.drawingBufferWidth, h = gl.drawingBufferHeight, px = new Uint8Array(w * h * 4);
  gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
  return { w, h, px };
};
window.__glassCheck = async () => {
  const THREE = await import("three");
  const v = window.__mac.view3d;
  v.setGlare(true); const a = window.__grab();
  v.setGlare(false); const b = window.__grab();
  v.setGlare(true);
  const gm = v.glass.material, pv = v.picture.visible;
  v.glass.material = new THREE.MeshBasicMaterial({ color: 0xff00ff, toneMapped: false });
  v.picture.visible = false;
  const m = window.__grab();
  v.glass.material.dispose(); v.glass.material = gm; v.picture.visible = pv;
  const { w, h } = a, glassAt = new Uint8Array(w * h);
  for (let i = 0; i < w * h; i++) { const k = i * 4; glassAt[i] = m.px[k] > 150 && m.px[k + 1] < 110 && m.px[k + 2] > 150 ? 1 : 0; }
  const near = (x, y) => { for (let dy = -2; dy <= 2; dy++) for (let dx = -2; dx <= 2; dx++) { const xx = x + dx, yy = y + dy; if (xx >= 0 && yy >= 0 && xx < w && yy < h && glassAt[yy * w + xx]) return true; } return false; };
  let changed = 0, leaks = 0, glassPx = 0, outsideMax = 0;
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const i = y * w + x, k = i * 4;
    glassPx += glassAt[i];
    const d = Math.max(Math.abs(a.px[k] - b.px[k]), Math.abs(a.px[k + 1] - b.px[k + 1]), Math.abs(a.px[k + 2] - b.px[k + 2]));
    if (d > 2) { changed++; if (!near(x, y)) { leaks++; outsideMax = Math.max(outsideMax, d); } }
  }
  // Pixels just outside the glass (a 1-6 px ring around its silhouette): with and without reflections.
  let ring = 0, ringDiff = 0;
  for (let y = 6; y < h - 6; y += 1) for (let x = 6; x < w - 6; x += 1) {
    const i = y * w + x;
    if (glassAt[i] || near(x, y)) continue;
    let close = false;
    for (let r = 3; r <= 6 && !close; r += 3) close = glassAt[i + r] || glassAt[i - r] || glassAt[i + r * w] || glassAt[i - r * w];
    if (!close) continue;
    ring++;
    const k = i * 4;
    ringDiff = Math.max(ringDiff, Math.abs(a.px[k] - b.px[k]), Math.abs(a.px[k + 1] - b.px[k + 1]), Math.abs(a.px[k + 2] - b.px[k + 2]));
  }
  return { changed, leaks, outsideMax, glassPx, ring, ringDiff };
};
window.__blackIsGlass = () => {
  const v = window.__mac.view3d, r = v.renderer.domElement.getBoundingClientRect(), pr = v.renderer.getPixelRatio();
  v.setGlare(false); const g = window.__grab(); v.setGlare(true);
  const lumaAt = ([cx, cy]) => {
    const x = Math.floor((cx - r.left) * pr), y = g.h - 1 - Math.floor((cy - r.top) * pr), k = (y * g.w + x) * 4;
    return 0.2126 * g.px[k] + 0.7152 * g.px[k + 1] + 0.0722 * g.px[k + 2];
  };
  const [iw, ih] = v.imageSize;
  const res = [];
  for (const [mx, my, sx] of [[0, 341, -1], [511, 341, 1]]) {
    const corner = lumaAt(v.clientFromMac(mx, my));
    const beyond = v.clientFromGlass(sx * (iw / 2 + 0.25), -(ih / 2 + 0.25));
    res.push({ corner: +corner.toFixed(1), glass: beyond ? +lumaAt(beyond).toFixed(1) : null });
  }
  return res;
};`;

async function view(page, name, z, yaw = 0, pitch = 0) {
  await page.evaluate(([z, yaw, pitch]) => {
    const v = window.__mac.view3d;
    v.jumpTo(z); v.view.yaw = yaw; v.view.pitch = pitch;
    if (yaw || pitch) v.view.spring = null;
  }, [z, yaw, pitch]);
  await sleep(700);
}

for (const [w, h] of VIEWPORTS) {
  const { browser, page } = await launch(kind, { viewport: { width: w, height: h } });
  try {
    await page.goto(srv.url);
    await until(page, (p) => p.evaluate(() => !!(window.__mac.view3d && window.__mac.view3d.isFront())), 60000, "the 3D view");
    await until(page, async (p) => (await macApp(p)) === "Finder", 60000, "the Finder");
    await settled(page);
    await sleep(4000);
    await page.addScriptTag({ content: PAGE_HELPERS });
    const zDefault = await page.evaluate(() => window.__mac.view3d.view.zoom);
    const views = [["default", zDefault], ["zoomed_out", 0.1], ["zoomed_in", 0.95], ["closest", 1.0], ["turned_right", zDefault, 0.5, 0.05], ["turned_left_up", zDefault, -0.45, 0.25]];
    for (const [name, z, yaw = 0, pitch = 0] of views) {
      await view(page, name, z, yaw, pitch);
      const front = !yaw && !pitch;
      const r = { viewport: `${w}x${h}`, view: name, zoom: +z.toFixed(3), pass: true, notes: [] };
      // 1. mapping (front views only: input goes to the screen only when facing front)
      if (front) {
        const m = await page.evaluate((pts) => {
          const v = window.__mac.view3d, rect = v.pictureClientRect();
          const perPx = 512 / Math.max(1, rect.right - rect.left);
          let worst = 0, missing = 0;
          // When a Mac pixel is smaller than a screen pixel, the corner pixels' centres can sit in a
          // screen pixel that is mostly outside the picture: check points that far inside instead.
          const inset = perPx > 1 ? Math.ceil(perPx) : 0;
          for (let [x, y] of pts) {
            x = Math.min(511 - inset, Math.max(inset, x)); y = Math.min(341 - inset, Math.max(inset, y));
            const got = v.renderedMacPixelAt(...v.clientFromMac(x, y));
            if (!got) { missing++; continue; }
            worst = Math.max(worst, Math.abs(got[0] - x), Math.abs(got[1] - y));
          }
          return { worst, missing, perPx, width: rect.right - rect.left };
        }, MAC_POINTS);
        const allowed = m.perPx > 1 ? Math.ceil(m.perPx) : 0;
        r.mapping = { worstMacPx: m.worst, allowed, missing: m.missing, screenWidthPx: Math.round(m.width) };
        if (m.missing || m.worst > allowed) r.pass = false;
        let worstPtr = 0;
        for (const [x, y] of [[100, 30], [400, 300]]) {
          const [cx, cy] = await page.evaluate(([x, y]) => window.__mac.view3d.clientFromMac(x, y), [x, y]);
          await page.mouse.move(cx, cy); await sleep(300);
          const b = await peek(page, 0x830, 4);
          worstPtr = Math.max(worstPtr, Math.abs(((b[2] << 8) | b[3]) - x), Math.abs(((b[0] << 8) | b[1]) - y));
        }
        const allowedPtr = Math.ceil(m.perPx) + 1;
        r.pointer = { worstMacPx: worstPtr, allowed: allowedPtr };
        if (worstPtr > allowedPtr) r.pass = false;
        await page.mouse.move(5, h - 5);        // off the screen again (the camera doesn't care)
      }
      // 2. reflections only on the glass
      const g = await page.evaluate(() => window.__glassCheck());
      r.glass = g;
      if (g.leaks > 0 || g.ringDiff > 0 || g.glassPx === 0) r.pass = false;
      // 3. black pixels look like the bare glass
      if (front && name !== "zoomed_out") {
        const b = await page.evaluate(() => window.__blackIsGlass());
        r.blackIsGlass = b;
        if (b.some((q) => q.glass === null || Math.abs(q.corner - q.glass) > 6)) r.pass = false;
      }
      const file = path.join(out, `${w}x${h}_${name}.png`);
      await page.screenshot({ path: file });
      r.screenshot = file;
      if (!r.pass) failures++;
      results.push(r);
      console.log(`${r.pass ? "PASS" : "FAIL"} ${kind} ${w}x${h} ${name.padEnd(14)} zoom ${r.zoom}` +
        (r.mapping ? `: GPU pixel vs clientFromMac worst ${r.mapping.worstMacPx} Mac px (allowed ${r.mapping.allowed}, screen ${r.mapping.screenWidthPx} px wide); pointer -> Mac mouse worst ${r.pointer.worstMacPx} (allowed ${r.pointer.allowed})` : ":") +
        `; reflections changed ${g.changed} px, ${g.leaks} outside the glass, ring of ${g.ring} px just outside the glass differs by ${g.ringDiff}` +
        (r.blackIsGlass ? `; black corner vs bare glass luma ${r.blackIsGlass.map((q) => `${q.corner}/${q.glass}`).join(", ")}` : ""));
    }
  } catch (e) {
    failures++;
    results.push({ viewport: `${w}x${h}`, pass: false, error: e.message });
    console.log(`FAIL ${kind} ${w}x${h}: ${e.message}`);
  }
  await browser.close();
}
fs.writeFileSync(path.join(out, "results.json"), JSON.stringify(results, null, 1));
console.log(`${kind}: ${results.filter((r) => r.pass).length}/${results.length} checks passed -> ${out}`);
srv.stop();
process.exitCode = failures ? 1 : 0;
