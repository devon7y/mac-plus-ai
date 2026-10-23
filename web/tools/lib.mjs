// Shared helpers for the Playwright scripts (tests and bake): local server, browser, Mac access.
import { spawn } from "node:child_process";
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";
import { fileURLToPath } from "node:url";

export const WEB = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const require = createRequire(path.join(WEB, "build", "package.json"));
export const pw = require("playwright-core");

export function sleep(ms) { return new Promise((r) => setTimeout(r, ms)); }

// Serve a folder with the Cloudflare-like headers (tools/serve.py) on a free-ish port.
export async function serve(dir) {
  const proc = spawn("python3", ["-I", path.join(WEB, "tools", "serve.py"), "--dir", dir, "--port", "0"],
    { stdio: ["ignore", "pipe", "inherit"] });
  const url = await new Promise((resolve, reject) => {
    proc.stdout.on("data", (d) => { const m = String(d).match(/on (http:\/\/\S+)/); if (m) resolve(m[1]); });
    proc.on("exit", (c) => reject(new Error("server exited " + c)));
  });
  return { url, stop: () => proc.kill() };
}

export async function launch(kind, opts = {}) {
  // "chrome" = the full Chrome for Testing build (chromium-1243 in the Playwright cache), run the
  // way the manager's check runs it (--use-gl=swiftshader), next to the headless shell ("chromium").
  const CHROME = path.join(process.env.HOME, "Library/Caches/ms-playwright/chromium-1243/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing");
  const type = kind === "webkit" ? pw.webkit : kind === "firefox" ? pw.firefox : pw.chromium;
  const args = kind === "chromium" ? ["--autoplay-policy=no-user-gesture-required", "--use-angle=swiftshader", "--enable-unsafe-swiftshader"]
    : kind === "chrome" ? ["--autoplay-policy=no-user-gesture-required", "--use-gl=swiftshader", "--enable-unsafe-swiftshader"] : [];
  const firefoxUserPrefs = kind === "firefox" ? { "media.autoplay.default": 0, "media.autoplay.blocking_policy": 0 } : undefined;
  const browser = await type.launch({ headless: true, args, firefoxUserPrefs, executablePath: kind === "chrome" ? CHROME : undefined });
  const context = await browser.newContext({ viewport: opts.viewport || { width: 1280, height: 900 }, deviceScaleFactor: opts.dpr || 1 });
  const page = await context.newPage();
  page.on("pageerror", (e) => console.log(`[${kind} pageerror]`, e.message));
  page.on("console", (m) => {
    if ((m.type() === "error" || m.type() === "warning") && !m.text().includes("GPU stall due to ReadPixels")) console.log(`[${kind} console.${m.type()}]`, m.text());
  });
  return { browser, context, page };
}

// Wait until fn(page) returns truthy (polling), or throw after timeout ms.
export async function until(page, fn, timeout, what) {
  const t0 = Date.now();
  for (;;) {
    const v = await fn(page);
    if (v) return v;
    if (Date.now() - t0 > timeout) throw new Error("timed out waiting for " + what);
    await sleep(250);
  }
}

export const macStats = (page) => page.evaluate(() => window.__mac && window.__mac.stats);
export const macApp = async (page) => ((await macStats(page)) || {}).app || "";

// Mac memory via the worker (Mac addresses).
export async function peek(page, addr, len) {
  return page.evaluate(async ([a, l]) => {
    const b = await window.__mac.peek(a, l);
    return b ? Array.from(b) : null;
  }, [addr, len]);
}

// Client (page) coordinates of a Mac screen point, from wherever the emulator canvas is shown.
// In the 3D view: where Mac pixel (x, y) is drawn on the curved 3D screen (exact, same geometry the
// GPU interpolates); in the flat view: the canvas's rectangle.
export async function macToClient(page, x, y) {
  return page.evaluate(([x, y]) => {
    const m = window.__mac;
    if (m.mode === "3d" && m.view3d) return m.view3d.clientFromMac(x, y);
    const r = document.getElementById("screen").getBoundingClientRect();
    return [r.left + (x + 0.5) * r.width / 512, r.top + (y + 0.5) * r.height / 342];
  }, [x, y]);
}

export async function macClick(page, x, y, { double = false } = {}) {
  const [cx, cy] = await macToClient(page, x, y);
  await page.mouse.move(cx, cy);
  await sleep(120);
  await page.mouse.down(); await sleep(60); await page.mouse.up();
  if (double) { await sleep(110); await page.mouse.down(); await sleep(60); await page.mouse.up(); }
  await sleep(150);
}

export async function shot(page, file) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  await page.screenshot({ path: file });
}

// Screenshot of just the Mac's screen (the emulator canvas's pixels, 512x342).
export async function macShot(page, file) {
  const url = await page.evaluate(() => document.getElementById("screen").toDataURL("image/png"));
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, Buffer.from(url.split(",")[1], "base64"));
}

// Press the mouse on a menu title, move to an item, release (pull-down menus need a held button).
export async function macMenu(page, titleX, itemY, { shotFile } = {}) {
  const [tx, ty] = await macToClient(page, titleX, 9);
  await page.mouse.move(tx, ty); await sleep(120);
  await page.mouse.down(); await sleep(400);
  const [ix, iy] = await macToClient(page, titleX + 20, itemY);
  await page.mouse.move(ix, iy, { steps: 6 }); await sleep(300);
  if (shotFile) await macShot(page, shotFile);
  await page.mouse.up(); await sleep(200);
}

// In the 3D view: wait until the camera has settled (facing front, zoom no longer gliding), so the
// screen doesn't move between the two clicks of a double-click. Software WebGL in headless browsers
// renders slowly, which stretches the glide after "Click to start".
export async function settled(page, timeout = 30000) {
  await until(page, (p) => p.evaluate(() => {
    const v = window.__mac.view3d;
    return !v || window.__mac.mode !== "3d" || (v.isFront() && !v.view.dolly && Math.abs(v.view.zoom - v.view.zoomTarget) < 0.001);
  }), timeout, "the 3D view to settle");
}

// A point on a desktop icon that the Finder will accept: it hit-tests icons against their masks, and
// some icons are hollow in the middle (Glider's paper plane, Dream's crescent), while a click on the
// name only selects it (a double-click there edits the name). So take the solid 3x3 block (all
// white or all black: the grey desktop pattern never has one) nearest the icon's centre, read from
// the live screen. (x0, y0) is the icon's top-left corner.
export async function iconPoint(page, x0, y0) {
  return page.evaluate(([x0, y0]) => {
    const d = document.getElementById("screen").getContext("2d").getImageData(x0, y0, 32, 32).data;
    const v = (x, y) => d[(y * 32 + x) * 4];
    let best = null;
    for (let y = 1; y < 31; y++) for (let x = 1; x < 31; x++) {
      let w = true, b = true;
      for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) { const p = v(x + dx, y + dy); w = w && p > 200; b = b && p < 60; }
      if (!w && !b) continue;
      const dist = (x - 16) ** 2 + (y - 16) ** 2;
      if (!best || dist < best[0]) best = [dist, x0 + x, y0 + y];
    }
    return best ? [best[1], best[2]] : [x0 + 16, y0 + 16];
  }, [x0, y0]);
}
