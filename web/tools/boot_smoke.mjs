// Quick check: load the page, click start, wait for the Finder, screenshot.
//   node web/tools/boot_smoke.mjs <dist dir> <chromium|webkit> <out dir> [seconds]
import { serve, launch, until, macApp, macStats, macShot, shot, sleep } from "./lib.mjs";

const [dir, kind = "chromium", outDir = "/tmp/boot", secs = "40"] = process.argv.slice(2);
const srv = await serve(dir, 8650 + (kind === "webkit" ? 1 : 0));
const { browser, page } = await launch(kind);
try {
  const t0 = Date.now();
  await page.goto(srv.url);
  const app = await until(page, async (p) => (await macApp(p)) === "Finder" && (await macApp(p)), Number(secs) * 1000, "Finder");
  console.log(`${kind}: ${app} after ${((Date.now() - t0) / 1000).toFixed(1)} s`);
  await sleep(4000);
  console.log(JSON.stringify(await macStats(page)));
  console.log(JSON.stringify(await page.evaluate(() => ({ logs: window.__mac.logs.slice(-12), load: window.__mac.loadMs, tick: window.__mac.tickRate }))));
  await macShot(page, `${outDir}/${kind}_finder.png`);
  await shot(page, `${outDir}/${kind}_page.png`);
} catch (e) {
  console.log("FAIL", e.message);
  console.log(JSON.stringify(await page.evaluate(() => window.__mac && { logs: window.__mac.logs.slice(-20), stats: window.__mac.stats })));
  await shot(page, `${outDir}/${kind}_fail.png`);
} finally {
  await browser.close();
  srv.stop();
}
