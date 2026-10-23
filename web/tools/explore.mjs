// Exploration helper: boot, run a list of actions, screenshot after each.
//   node web/tools/explore.mjs <dist> <kind> <outdir> '<json actions>'
// actions: ["wait",ms] ["click",x,y] ["dbl",x,y] ["key",code,holdMs] ["shot",name] ["app"]
import { serve, launch, until, macApp, macShot, shot, sleep, macClick, peek, macMenu, settled } from "./lib.mjs";
const [dir, kind, outDir, json] = process.argv.slice(2);
const actions = JSON.parse(json);
const srv = await serve(dir, 8660 + Math.floor(Math.random() * 200));
const { browser, page } = await launch(kind);
try {
  await page.goto(srv.url);
  await until(page, async (p) => (await macApp(p)) === "Finder", 60000, "Finder");
  await settled(page);
  for (const a of actions) {
    if (a[0] === "wait") await sleep(a[1]);
    else if (a[0] === "click") await macClick(page, a[1], a[2]);
    else if (a[0] === "dbl") await macClick(page, a[1], a[2], { double: true });
    else if (a[0] === "key") { await page.keyboard.down(a[1]); await sleep(a[2] || 80); await page.keyboard.up(a[1]); }
    else if (a[0] === "type") { await page.keyboard.type(a[1], { delay: 90 }); }
    else if (a[0] === "typeheld") { for (const ch of a[1]) { const k = ch === " " ? "Space" : ch; await page.keyboard.down(k); await sleep(a[2] || 80); await page.keyboard.up(k); await sleep(a[3] || 70); } }
    else if (a[0] === "shot") { await macShot(page, `${outDir}/${a[1]}.png`); }
    else if (a[0] === "page") { await shot(page, `${outDir}/${a[1]}.png`); }
    else if (a[0] === "menu") await macMenu(page, a[1], a[2], { shotFile: a[3] ? `${outDir}/${a[3]}.png` : undefined });
    else if (a[0] === "ctrl") { await page.keyboard.down("Control"); await sleep(80); await page.keyboard.down(a[1]); await sleep(90); await page.keyboard.up(a[1]); await page.keyboard.up("Control"); }
    else if (a[0] === "move") { const r = await page.evaluate(() => { const b = document.getElementById("screen").getBoundingClientRect(); return [b.left, b.top, b.width, b.height]; }); await page.mouse.move(r[0] + (a[1] + 0.5) * r[2] / 512, r[1] + (a[2] + 0.5) * r[3] / 342, { steps: a[3] || 8 }); }
    else if (a[0] === "app") console.log("app:", await macApp(page));
    else if (a[0] === "peek") console.log("peek", a[1], JSON.stringify(await peek(page, a[1], a[2])));
    else if (a[0] === "eval") console.log("eval:", JSON.stringify(await page.evaluate(a[1])));
  }
} catch (e) { console.log("FAIL", e.message); await shot(page, `${outDir}/fail.png`); }
finally { await browser.close(); srv.stop(); }
