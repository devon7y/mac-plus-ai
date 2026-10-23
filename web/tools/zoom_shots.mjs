// node web/tools/zoom_shots.mjs <dist> <kind> <outdir> z1 z2 ...
import { serve, launch, until, shot, sleep } from "./lib.mjs";
const [dir, kind, outDir, ...zs] = process.argv.slice(2);
const srv = await serve(dir, 8730 + Math.floor(Math.random() * 50));
const { browser, page } = await launch(kind);
try {
  await page.goto(srv.url + "?view=3d");
  await until(page, (p) => p.evaluate(() => !!(window.__mac.view3d && window.__mac.view3d.isFront())), 30000, "front view");
  for (const z of zs) {
    await page.evaluate((z) => window.__mac.view3d.jumpTo(Number(z)), z);
    await sleep(700);
    await shot(page, `${outDir}/${kind}_z${z}.png`);
  }
  console.log(await page.evaluate(() => [0.5, 0.62, 0.8].map((f) => window.__mac.view3d.zoomForScreen(f))));
} catch (e) { console.log("FAIL", e.message); }
finally { await browser.close(); srv.stop(); }
