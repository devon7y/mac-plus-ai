// Screenshots of Pixel and Dream on the site, for the public README:
//   node web/tools/readme_shots.mjs <dist> <outdir> pixel|dream
// Page shots of the 3D Mac (default view and a close-up) and the Mac's own screen (512x342).
import path from "node:path";
import { serve, launch, until, macApp, macShot, shot, sleep, macClick, settled, iconPoint } from "./lib.mjs";

const [dir, out, which] = process.argv.slice(2);
const ICON = { Pixel: [98, 94], Dream: [98, 156], Blockworld: [22, 156] };
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
// Find a debug block in Mac RAM: a magic word with its complement notOff bytes later (2-byte steps).
async function findBlock(page, magic, notOff) {
  return page.evaluate(async ([magic, notOff]) => {
    const b = await window.__mac.peek(0, 4 * 1024 * 1024);
    if (!b) return -1;
    const rd = (o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;
    for (let o = 0; o + notOff + 4 <= b.length; o += 2) if (rd(o) === magic && rd(o + notOff) === (~magic >>> 0)) return o;
    return -1;
  }, [magic, notOff]);
}
const rd32 = (page, a) => page.evaluate(async (a) => { const b = await window.__mac.peek(a, 4); return ((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]) >>> 0; }, a);
const rd16 = (page, a) => page.evaluate(async (a) => { const b = await window.__mac.peek(a, 2); return (b[0] << 8) | b[1]; }, a);
const poke32 = (page, a, v) => page.evaluate(([a, v]) => window.__mac.poke32(a, v), [a, v]);
// Zoomed out far enough to show the keyboard and mouse (view zoom 0 = the whole Mac on its desk).
async function zoomShot(page, z, file) {
  await page.evaluate((zz) => window.__mac.view3d.jumpTo(zz), z);
  await sleep(1500); await settled(page); await sleep(1000);
  // Higher pixel density only for the capture: rendered continuously, software WebGL can't keep up.
  const dpr = Number(process.env.DPR || 1), cdp = dpr > 1 ? await page.context().newCDPSession(page) : null;
  if (cdp) { await cdp.send("Emulation.setDeviceMetricsOverride", { width: 1600, height: 1000, deviceScaleFactor: dpr, mobile: false }); await sleep(3000); }
  await page.screenshot({ path: file, timeout: 180000 });
  if (cdp) { await cdp.send("Emulation.clearDeviceMetricsOverride"); await cdp.detach(); await sleep(1000); }
}
// Screen pixels that differ between two grabs (x >= x0, below the menu bar): their bounding box.
async function changedBox(page, before, x0 = 0) {
  const after = await screen(page);
  let x1 = 1e9, y1 = 1e9, x2 = -1, y2 = -1;
  for (let i = 0; i < after.length; i++) if (after[i] !== before[i]) {
    const x = i % 512, y = 20 + Math.floor(i / 512);
    if (x < x0) continue;
    x1 = Math.min(x1, x); y1 = Math.min(y1, y); x2 = Math.max(x2, x); y2 = Math.max(y2, y);
  }
  return x2 < 0 ? null : [x1, y1, x2, y2];
}
async function cmdKey(page, k) { await page.keyboard.down("Control"); await sleep(80); await key(page, k); await page.keyboard.up("Control"); await sleep(300); }

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
  if (which === "framing") {
    for (const z of [0, 0.08, 0.16, 0.24, 0.32]) await zoomShot(page, z, path.join(out, `framing_${z}.png`));
  } else if (which === "blockworld") {
    // The starting world (seed 146, morning), full-screen view, a yak standing a few blocks ahead.
    await launchApp(page, "Blockworld");
    let blk = -1;
    await until(page, async (p) => (blk = await findBlock(p, 0x4257474D, 4)) >= 0 && (await rd32(p, blk + 8)) === 2, 90000, "Blockworld playing");
    const cmd = async (c, a0 = 0, a1 = 0, a2 = 0) => {
      await poke32(page, blk + 166, ((a1 & 0xffff) << 16) | (a2 & 0xffff));
      await poke32(page, blk + 162, ((c & 0xffff) << 16) | (a0 & 0xffff));
      await until(page, async (p) => (await rd16(p, blk + 162)) === 0, 10000, `command ${c}`);
    };
    const diag = async (tag) => {
      const d = await page.evaluate(async (a) => Array.from(await window.__mac.peek(a, 240)), blk);
      const s16 = (o) => { const v = (d[o] << 8) | d[o + 1]; return v > 32767 ? v - 65536 : v; };
      const s32 = (o) => (d[o] << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3];
      const ma = ((d[216] << 24) | (d[217] << 16) | (d[218] << 8) | d[219]) >>> 0;
      const m = Array.from(await page.evaluate(async (a) => Array.from(await window.__mac.peek(a, 36 * 4)), ma - 0)) ;
      const mobs = [0, 1, 2, 3].map((i) => { const o = i * 36; const r = (k) => (m[o + k] << 24) | (m[o + k + 1] << 16) | (m[o + k + 2] << 8) | m[o + k + 3];
        return `kind ${m[o + 20]} alive ${m[o + 21]} at ${(r(0) / 256).toFixed(1)},${(r(4) / 256).toFixed(1)},${(r(8) / 256).toFixed(1)}`; });
      console.log(`bw ${tag}: player ${(s32(16) / 256).toFixed(1)},${(s32(20) / 256).toFixed(1)},${(s32(24) / 256).toFixed(1)} yaw ${s16(28)} pitch ${s16(30)} loaded ${s16(152)} tod ${s32(208)} mobsAddr ${ma.toString(16)} | ${mobs.join(" | ")}`);
      await macShot(page, path.join(out, `bwdiag_${tag}.png`));
    };
    if (process.env.DIAG) await diag("start");
    await poke32(page, blk + 212, 1);            // lastKey 0, spawnOff 1: no natural spawning
    await cmd(10, 2);                             // full-screen view
    await cmd(7);                                 // no mobs
    const yaw = (await rd32(page, blk + 28)) >>> 16;
    await cmd(6, Number(process.env.KIND || 0), Number(process.env.AHEAD || 4), (yaw + 256) & 1023);   // side-on
    await sleep(6000);                            // the still frame's refinement
    if (process.env.DIAG) { await diag("spawned"); throw new Error("diagnostics done"); }
    await macShot(page, path.join(out, "blockworld_screen.png"));   // (no park: a mouse move turns the view here)
    for (const z of (process.env.ZOOMS || "0.12,0.2").split(",").map(Number)) await zoomShot(page, z, path.join(out, `blockworld_z${z}.png`));
  } else if (which === "pvz") {
    // Level 1-9 from the inserted disk, played by the game's built-in test bot.
    let before = await screen(page);
    await page.setInputFiles("#disk-file", process.env.PVZ_DISK, { timeout: 120000 });
    await sleep(9000);
    const box = await changedBox(page, before, 300);
    console.log("pvz: new disk icon", JSON.stringify(box));
    await macShot(page, path.join(out, "pvz_0_inserted.png"));
    const [bx1, , bx2, by2] = box;               // the new disk's icon is the lowest change (under Macintosh HD)
    await macClick(page, Math.round((bx1 + bx2) / 2), by2 - 28); await sleep(500);
    await cmdKey(page, "o"); await sleep(3000);
    await cmdKey(page, "a"); await cmdKey(page, "o");
    await until(page, async (p) => (await macApp(p)) === "PlantsVsZombies", 30000, "PvZ running");
    let blk = -1;
    await until(page, async (p) => (blk = await findBlock(p, 0x50565A47, 8)) >= 0, 60000, "PvZ debug block");
    await sleep(8000);
    await park(page);
    await macShot(page, path.join(out, "pvz_1_title.png"));
    await poke32(page, blk + 176, 1);             // world 1 (Day)
    await poke32(page, blk + 56, 1);              // the built-in bot plays
    await macClick(page, 256 - 5 * 34 + 8 * 34 + 17, 243);   // the 1-9 button
    await sleep(3000); await park(page);
    await macShot(page, path.join(out, "pvz_2_started.png"));
    await until(page, async (p) => (await rd32(p, blk + 16)) === 9 && (await rd32(p, blk + 64)) > 0, 15000, "level 1-9 running");
    for (const at of (process.env.AT || "26000,32000,36000,39000").split(",").map(Number)) {
      await until(page, async (p) => (await rd32(p, blk + 64)) >= at || (await rd32(p, blk + 32)) !== 0, 600000, `update ${at}`);
      const u = await rd32(page, blk + 64), r = await rd32(page, blk + 32);
      console.log(`pvz: update ${u}, result ${r}`);
      await macShot(page, path.join(out, `pvz_u${at}.png`));
      for (const z of (process.env.ZOOMS || "0.12,0.2").split(",").map(Number)) await zoomShot(page, z, path.join(out, `pvz_u${at}_z${z}.png`));
      await page.evaluate(() => window.__mac.view3d.resetView());
      if (r !== 0) break;
    }
  } else if (which === "hero") {
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
