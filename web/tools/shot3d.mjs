// Screenshots of the 3D page: node web/tools/shot3d.mjs <dist> <kind> <out.png> <w> <h> [quality] [zoom] [start]
import { serve, launch, until, macApp, sleep, settled, shot } from "./lib.mjs";
const [dir, kind, out, w, h, quality = "high", zoom = "", start = "1"] = process.argv.slice(2);
const srv = await serve(dir);
const { browser, page } = await launch(kind, { viewport: { width: +w, height: +h } });
try {
  await page.goto(srv.url + (quality ? `?quality=${quality}` : ""));
  await until(page, (p) => p.evaluate(() => !!(window.__mac.view3d && window.__mac.view3d.isFront())), 60000, "3D view");
  if (start === "1") {
    await until(page, async (p) => (await macApp(p)) === "Finder", 60000, "Finder");
    await settled(page); await sleep(2500);
  } else await sleep(1500);
  if (zoom) { await page.evaluate((z) => window.__mac.view3d.jumpTo(+z), zoom); await sleep(1200); }
  await shot(page, out);
  console.log(JSON.stringify(await page.evaluate(() => ({ q: window.__mac.view3d.quality(), logs: window.__mac.logs.slice(-5), badge: !!window.__mac.view3d.badge }))));
} catch (e) { console.log("FAIL", e.message); await shot(page, out); console.log(JSON.stringify(await page.evaluate(() => window.__mac && window.__mac.logs))); }
await browser.close(); srv.stop();
