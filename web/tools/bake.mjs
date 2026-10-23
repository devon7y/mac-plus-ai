// First boot of the freshly built web disk, in the browser emulator itself (headless Chromium):
// the Finder builds its Desktop file (application icons) and settles the desktop layout; then
// Glider 4.06 is started once, set to keyboard control (Options > Controls > Hold Keyboard: its
// , and . keys) and quit, so it saves its preferences (otherwise it says "New keyboard. Switching
// to default keyboard controls." to every visitor, and steers with the mouse); then Special > Shut Down
// leaves the volume cleanly unmounted, and the disk is copied out.
//
//   node web/tools/bake.mjs <unbaked.dsk> <baked.dsk>
//
// It builds a throwaway site around the unbaked disk (build_site.py --disk ... --out <new dir>).
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";
import { WEB, serve, launch, until, macApp, macShot, sleep, macMenu, macClick, iconPoint, macToClient } from "./lib.mjs";

const [inDisk, outDisk] = process.argv.slice(2);
if (!inDisk || !outDisk) { console.log("usage: bake.mjs <unbaked.dsk> <baked.dsk>"); process.exit(2); }
const stamp = new Date().toISOString().replace(/[-:.TZ]/g, "").slice(0, 14);
const site = path.join(WEB, "build", `bake_site_${stamp}`);
execFileSync("python3", ["-I", path.join(WEB, "build_site.py"), "--disk", inDisk, "--out", site], { stdio: "ignore" });
const shots = path.join(WEB, "build", `bake_shots_${stamp}`);

const srv = await serve(site);
const { browser, page } = await launch("chromium");
try {
  await page.goto(srv.url + "?view=flat");
  await until(page, async (p) => (await macApp(p)) === "Finder", 60000, "the Finder");
  await sleep(8000);                                  // Desktop file built, icons drawn
  await macShot(page, path.join(shots, "1_desktop.png"));
  const [gx, gy] = await iconPoint(page, 22, 218);                    // Glider 4.06's icon
  await macClick(page, gx, gy, { double: true });
  await until(page, async (p) => (await macApp(p)) === "Glider 4.06", 20000, "Glider");
  await sleep(5000);
  await macShot(page, path.join(shots, "1b_glider_first.png"));
  await macClick(page, 375, 163);                                      // "New keyboard..." Okay
  await sleep(2500);
  // Options > Controls > Hold Keyboard (Glider's , and . keys; its saved default was Mouse).
  let [cx, cy] = await macToClient(page, 110, 9);
  await page.mouse.move(cx, cy, { steps: 5 }); await sleep(300); await page.mouse.down(); await sleep(1500);
  [cx, cy] = await macToClient(page, 130, 27); await page.mouse.move(cx, cy, { steps: 6 }); await sleep(1500);
  [cx, cy] = await macToClient(page, 210, 27); await page.mouse.move(cx, cy, { steps: 6 }); await sleep(800);
  [cx, cy] = await macToClient(page, 260, 33); await page.mouse.move(cx, cy, { steps: 6 }); await sleep(1000);
  await macShot(page, path.join(shots, "1b1_glider_hold_keyboard.png"));
  await page.mouse.up(); await sleep(1500);
  await macMenu(page, 50, 155);                                        // Game > Quit
  await sleep(3000);
  if ((await macApp(page)) !== "Finder") {                             // try Cmd-Q (Ctrl here)
    await macShot(page, path.join(shots, "1b2_still_glider.png"));
    await page.keyboard.down("Control"); await sleep(100); await page.keyboard.press("q"); await sleep(100); await page.keyboard.up("Control");
  }
  await until(page, async (p) => (await macApp(p)) === "Finder", 20000, "the Finder after Glider");
  await sleep(4000);
  await macShot(page, path.join(shots, "1c_back.png"));
  await macMenu(page, 180, 123, { shotFile: path.join(shots, "2_special_menu.png") });   // Special > Shut Down
  await sleep(6000);
  await macShot(page, path.join(shots, "3_shut_down.png"));
  const b64 = await page.evaluate(async () => {
    const disk = await window.__mac.dumpDisk();
    const gz = await new Response(new Blob([disk]).stream().pipeThrough(new CompressionStream("gzip"))).arrayBuffer();
    const u8 = new Uint8Array(gz); let s = "";
    for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
    return btoa(s);
  });
  const disk = zlib.gunzipSync(Buffer.from(b64, "base64"));
  const orig = fs.readFileSync(inDisk);
  if (disk.length !== orig.length) throw new Error("baked disk has the wrong size");
  // HFS: drAtrb bit 8 = the volume was unmounted cleanly.
  const atrb = disk.readUInt16BE(1024 + 10);
  console.log(`BAKE volume attributes 0x${atrb.toString(16)} (${atrb & 0x100 ? "clean" : "NOT clean"})`);
  fs.writeFileSync(outDisk, disk);
  console.log(`BAKE done: ${outDisk} (screenshots in ${shots})`);
} catch (e) {
  console.log("BAKE failed:", e.message);
  await macShot(page, path.join(shots, "9_failed.png")).catch(() => {});
  process.exitCode = 1;
} finally {
  await browser.close();
  srv.stop();
}
