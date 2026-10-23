// node web/tools/view_smoke.mjs <dist> <kind> <outdir>
import { serve, launch, until, macApp, macShot, shot, sleep } from "./lib.mjs";
const [dir, kind = "chromium", outDir] = process.argv.slice(2);
const srv = await serve(dir, 8720 + (kind === "webkit" ? 1 : 0));
const { browser, page } = await launch(kind);
try {
  await page.goto(srv.url);
  await sleep(1200);
  await shot(page, `${outDir}/${kind}_0_intro.png`);
  await until(page, (p) => p.evaluate(() => !!(window.__mac.view3d && window.__mac.view3d.isFront())), 30000, "front view");
  await sleep(500);
  await shot(page, `${outDir}/${kind}_1_front.png`);
  console.log(JSON.stringify(await page.evaluate(() => ({ logs: window.__mac.logs, mode: window.__mac.mode, keys: window.__mac.view3d && window.__mac.view3d.keyNames().length, button: window.__mac.view3d && window.__mac.view3d.hasMouseButton(), box: document.getElementById("screen-box").getBoundingClientRect() }))));
  await until(page, async (p) => (await macApp(p)) === "Finder", 60000, "Finder");
  await sleep(3000);
  await shot(page, `${outDir}/${kind}_2_finder.png`);
} catch (e) { console.log("FAIL", e.message); await shot(page, `${outDir}/${kind}_fail.png`); console.log(JSON.stringify(await page.evaluate(() => window.__mac && window.__mac.logs))); }
finally { await browser.close(); srv.stop(); }
