// A caption card for demo_edit.py --overlay, in the style of demo_video.mjs's captions but opaque, so it
// can sit over a caption already in the recording and hide it:
//   node web/tools/demo_card.mjs <out.png> "<title>" "<small line>" ["<old title>" "<old small line>"]
// With an old caption given, the card is made at least as wide and as tall as that caption's box (both
// are centred at the bottom, 22 px up), so it covers it completely. Renders in Google Chrome at 1x.
import { pw } from "./lib.mjs";

const [out, title, small, oldTitle, oldSmall] = process.argv.slice(2);
if (!out || !title) { console.log("usage: demo_card.mjs <out.png> <title> <small> [<old title> <old small>]"); process.exit(1); }
const esc = (s) => String(s || "").replace(/&/g, "&amp;").replace(/</g, "&lt;");
const box = (t, s) => `${esc(t)}${s ? `<small>${esc(s)}</small>` : ""}`;
const browser = await pw.chromium.launch({ executablePath: "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", headless: true });
const page = await browser.newPage({ viewport: { width: 1920, height: 1080 }, deviceScaleFactor: 1 });
await page.setContent(`<html><head><style>
  html, body { margin: 0; background: transparent; }
  .cap { position: absolute; left: 50%; top: 100px; transform: translateX(-50%); white-space: nowrap; box-sizing: border-box;
    padding: 11px 26px; border-radius: 16px; color: #fff; text-align: center;
    font: 600 29px/1.25 -apple-system, "Helvetica Neue", Arial, sans-serif; }
  .cap small { display: block; font-weight: 400; font-size: 22px; opacity: .85; margin-top: 4px; }
  #card { background: rgb(25, 25, 27); display: flex; flex-direction: column; justify-content: center; }
  #old { visibility: hidden; top: 400px; }
</style></head><body><div class="cap" id="card">${box(title, small)}</div><div class="cap" id="old">${box(oldTitle, oldSmall)}</div></body></html>`);
if (oldTitle) {
  const o = await page.locator("#old").boundingBox();
  await page.evaluate(([w, h]) => { const c = document.getElementById("card"); c.style.minWidth = `${Math.ceil(w) + 4}px`; c.style.minHeight = `${Math.ceil(h) + 4}px`; }, [o.width, o.height]);
}
await page.locator("#card").screenshot({ path: out, omitBackground: true });
const b = await page.locator("#card").boundingBox();
console.log(`${out}: ${Math.round(b.width)}x${Math.round(b.height)}`);
await browser.close();
