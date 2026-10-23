// Screenshots of Pixel and Dream on the site, for the public README:
//   node web/tools/readme_shots.mjs <dist> <outdir> pixel|dream
// Page shots of the 3D Mac (default view and a close-up) and the Mac's own screen (512x342).
import path from "node:path";
import { serve, launch, until, macApp, macShot, shot, sleep, macClick, settled, iconPoint } from "./lib.mjs";

const [dir, out, which] = process.argv.slice(2);
const ICON = { Pixel: [98, 94], Dream: [98, 156] };
const srv = await serve(dir);

async function key(page, k, ms = 90) { await page.keyboard.down(k); await sleep(ms); await page.keyboard.up(k); await sleep(60); }
// Type by pushing Mac key codes straight into the emulator's input queue (window.__mac.pushEvent):
// browser key events arrive late while software WebGL renders a frame, and a late key-up makes the
// Mac auto-repeat ("hhhhhooo"). Capitals and ? ! use Shift (0x38).
const MAC = { a: 0x00, s: 0x01, d: 0x02, f: 0x03, h: 0x04, g: 0x05, z: 0x06, x: 0x07, c: 0x08, v: 0x09,
  b: 0x0b, q: 0x0c, w: 0x0d, e: 0x0e, r: 0x0f, y: 0x10, t: 0x11, "1": 0x12, "2": 0x13, "3": 0x14,
  "4": 0x15, "6": 0x16, "5": 0x17, "=": 0x18, "9": 0x19, "7": 0x1a, "-": 0x1b, "8": 0x1c, "0": 0x1d,
  o: 0x1f, u: 0x20, i: 0x22, p: 0x23, l: 0x25, j: 0x26, "'": 0x27, k: 0x28, ";": 0x29, ",": 0x2b,
  "/": 0x2c, n: 0x2d, m: 0x2e, ".": 0x2f, " ": 0x31, "\n": 0x24, "\b": 0x33 };
const SHIFTED = { "?": "/", "!": "1", ":": ";" };
async function tap(page, code, shift = false) {
  await page.evaluate(([c, sh]) => {
    const m = window.__mac;
    if (sh) m.pushEvent(4, 0x38);
    m.pushEvent(4, c); m.pushEvent(5, c);
    if (sh) m.pushEvent(5, 0x38);
  }, [code, shift]);
  await sleep(110);
}
// Park the Mac's arrow at the right edge of the screen (mostly off it) before a capture.
const park = (page) => page.evaluate(() => window.__mac.pushEvent(1, 510, 200)).then(() => sleep(400));
async function type(page, text) {
  for (const ch of text) {
    let k = ch, shift = false;
    if (SHIFTED[ch]) { k = SHIFTED[ch]; shift = true; } else if (/[A-Z]/.test(ch)) { k = ch.toLowerCase(); shift = true; }
    await tap(page, MAC[k], shift);
  }
}
const screen = (page) => page.evaluate(() => Array.from(document.getElementById("screen").getContext("2d").getImageData(0, 20, 512, 322).data.filter((_, i) => i % 4 === 0)));
// Wait at least minMs, then until the screen has (nearly: a blinking caret) stopped changing for quietMs.
async function quiet(page, minMs, quietMs, maxMs) {
  const t0 = Date.now();
  await sleep(minMs);
  let prev = await screen(page), still = 0;
  while (Date.now() - t0 < maxMs) {
    await sleep(1000);
    const cur = await screen(page);
    let n = 0; for (let i = 0; i < cur.length; i++) if (cur[i] !== prev[i]) n++;
    prev = cur;
    still = n < 40 ? still + 1000 : 0;
    if (still >= quietMs) return Date.now() - t0;
  }
  return -1;
}
async function boot(page) {
  await page.goto(srv.url + "?quality=high");
  await until(page, (p) => p.evaluate(() => !!(window.__mac.view3d && window.__mac.view3d.isFront())), 60000, "3D view");
  await until(page, async (p) => (await macApp(p)) === "Finder", 90000, "Finder");
  await settled(page);
  await sleep(4000);
}
// Select the icon with one click, then File > Open (Cmd-O; the page sends Control as Command):
// a double-click can arrive too slowly under software WebGL.
async function launchApp(page, name) {
  const [x, y] = await iconPoint(page, ...ICON[name]);
  await macClick(page, x, y);
  await sleep(500);
  await page.keyboard.down("Control"); await sleep(80); await key(page, "o"); await page.keyboard.up("Control");
  await until(page, async (p) => (await macApp(p)) === name, 30000, name);
}
async function pageShots(page, name) {
  await park(page); await shot(page, path.join(out, `${name}_site.png`));
  await page.evaluate(() => window.__mac.view3d.resetView(0.92));
  await sleep(1500); await settled(page); await sleep(800);
  await shot(page, path.join(out, `${name}_site_closeup.png`));
  await page.evaluate(() => window.__mac.view3d.resetView());
  await sleep(1500); await settled(page);
}

const { browser, page } = await launch("chrome", { viewport: { width: 1600, height: 1000 } });
try {
  await boot(page);
  if (which === "hero") {
    // The whole Mac, keyboard and mouse, with Dream's banana on the screen.
    await launchApp(page, "Dream");
    await quiet(page, 8000, 4000, 120000);
    await type(page, "banana\n");
    await quiet(page, 15000, 6000, 150000);
    await park(page);
    for (const [z, f] of [[0.42, "hero_wide"], [0.3, "hero_wider"]]) {
      await page.evaluate((zz) => window.__mac.view3d.resetView(zz), z);
      await sleep(1500); await settled(page); await sleep(800);
      await shot(page, path.join(out, `${f}.png`));
    }
  } else if (which === "pixel") {
    await launchApp(page, "Pixel");
    await quiet(page, 10000, 4000, 90000);
    await park(page); await macShot(page, path.join(out, "pixel_0_awake.png"));
    const msgs = ["hi pixel", "what year is it?", "do you like floppy disks?", "tell me a joke"];
    for (let i = 0; i < msgs.length; i++) {
      await type(page, msgs[i] + "\n");
      const ms = await quiet(page, 8000, 5000, 90000);
      console.log(`pixel: "${msgs[i]}" answered after ${ms} ms`);
      await park(page); await macShot(page, path.join(out, `pixel_${i + 1}.png`));
    }
    await pageShots(page, "pixel");
  } else {
    await launchApp(page, "Dream");
    await quiet(page, 8000, 4000, 120000);
    await park(page); await macShot(page, path.join(out, "dream_0_loaded.png"));
    const subjects = ["banana", "house", "lighthouse", "sailboat", "snowman", "owl", "teapot", "castle", "hot air balloon", "cactus", "umbrella", "pineapple", "guitar"];
    for (let i = 0; i < subjects.length; i++) {
      await type(page, "\b".repeat(18) + subjects[i] + "\n");
      if (i === 0) {
        await sleep(6000);
        await park(page); await macShot(page, path.join(out, "dream_developing.png"));
        await shot(page, path.join(out, "dream_developing_site.png"));
      }
      const ms = await quiet(page, 15000, 6000, 150000);
      console.log(`dream: "${subjects[i]}" done after ${ms} ms`);
      await park(page); await macShot(page, path.join(out, `dream_${String(i + 1).padStart(2, "0")}_${subjects[i].replace(/ /g, "_")}.png`));
      if (i === 0) await pageShots(page, "dream");
    }
  }
} catch (e) {
  console.log("FAIL", e.message);
  await shot(page, path.join(out, `${which}_FAILED_site.png`)).catch(() => {});
  await park(page); await macShot(page, path.join(out, `${which}_FAILED.png`)).catch(() => {});
}
await browser.close(); srv.stop();
