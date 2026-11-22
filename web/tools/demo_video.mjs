// Demo video of Pixel, Dream and Guess My Doodle on the website, for sharing:
//   node web/tools/demo_video.mjs <outdir> [url]
// Records the page in Google Chrome with the GPU (CDP screencast, 30 fps, piped through ffmpeg) and its
// sound (a tap on every Web Audio connection to the speakers), with captions drawn into the page.
// Writes raw.mp4, audio.webm and marks.json (the stretches to speed up); web/tools/demo_edit.py cuts the
// final video from them.
import { spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { pw, sleep, until, macApp, macClick, iconPoint, settled } from "./lib.mjs";

const [out, url = "https://macintosh.devon7y.com/"] = process.argv.slice(2);
fs.mkdirSync(out, { recursive: true });
const W = 1920, H = 1080, FPS = 30;
const CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const ICON = { Pixel: [98, 94], Dream: [98, 156], "Guess My Doodle": [98, 218] };

const browser = await pw.chromium.launch({
  executablePath: CHROME, headless: false,
  args: ["--autoplay-policy=no-user-gesture-required", `--window-size=${W},${H + 120}`, "--window-position=0,0",
         "--disable-backgrounding-occluded-windows", "--disable-renderer-backgrounding",
         "--disable-background-timer-throttling"],
});
const context = await browser.newContext({ viewport: { width: W, height: H }, deviceScaleFactor: 1 });
// The sound: everything the page connects to the speakers is also connected to a MediaStream we record.
await context.addInitScript(() => {
  const orig = AudioNode.prototype.connect;
  const R = (window.__rec = { chunks: [], rec: null, ctx: null, dest: null });
  AudioNode.prototype.connect = function (dst, ...rest) {
    const r = orig.call(this, dst, ...rest);
    try {
      if (dst instanceof AudioDestinationNode) {
        if (R.ctx !== this.context) { R.ctx = this.context; R.dest = this.context.createMediaStreamDestination(); }
        orig.call(this, R.dest);
      }
    } catch (e) { /* ignore */ }
    return r;
  };
  window.__startRec = () => {
    if (!R.dest) return false;
    R.rec = new MediaRecorder(R.dest.stream, { mimeType: "audio/webm;codecs=opus", audioBitsPerSecond: 160000 });
    R.rec.ondataavailable = (e) => R.chunks.push(e.data);
    R.rec.start(1000);
    return true;
  };
  window.__stopRec = () => new Promise((res) => {
    R.rec.onstop = async () => {
      const buf = new Uint8Array(await new Blob(R.chunks, { type: "audio/webm" }).arrayBuffer());
      let s = "";
      for (let i = 0; i < buf.length; i += 0x8000) s += String.fromCharCode.apply(null, buf.subarray(i, i + 0x8000));
      res(btoa(s));
    };
    R.rec.stop();
  });
});
const page = await context.newPage();
page.on("pageerror", (e) => console.log("[pageerror]", e.message));

// ------------------------------------------------------------------ recording
const cdp = await context.newCDPSession(page);
let latest = null, t0 = 0, n = 0, timer = null, ff = null;
cdp.on("Page.screencastFrame", (f) => {
  latest = Buffer.from(f.data, "base64");
  cdp.send("Page.screencastFrameAck", { sessionId: f.sessionId }).catch(() => {});
});
const marks = { ff: [], events: [] };
const now = () => (Date.now() - t0) / 1000;
const mark = (name) => { marks.events.push([name, now()]); console.log(`  ${now().toFixed(1)} s  ${name}`); };
let ffOpen = null;
const fast = async (speed) => {                       // a stretch shown faster in the edit, with a badge
  if (ffOpen) { marks.ff.push([ffOpen[0], now(), ffOpen[1]]); ffOpen = null; await badge(""); }
  if (speed) { ffOpen = [now(), speed]; await badge(`⏩ ${speed}× speed`); }
};
async function startRecording() {
  await cdp.send("Page.startScreencast", { format: "jpeg", quality: 92, maxWidth: W, maxHeight: H, everyNthFrame: 1 });
  while (!latest) await sleep(50);
  ff = spawn("ffmpeg", ["-y", "-loglevel", "error", "-f", "image2pipe", "-framerate", String(FPS), "-c:v", "mjpeg", "-i", "-",
    "-vf", `scale=${W}:${H}:flags=lanczos`, "-c:v", "libx264", "-preset", "veryfast", "-crf", "16", "-pix_fmt", "yuv420p",
    path.join(out, "raw.mp4")], { stdio: ["pipe", "ignore", "inherit"] });
  const audioOk = await page.evaluate(() => window.__startRec());
  t0 = Date.now();
  timer = setInterval(() => {
    const due = Math.floor((Date.now() - t0) * FPS / 1000);
    while (n < due) { ff.stdin.write(latest); n++; }
  }, 5);
  console.log("recording; sound tap " + (audioOk ? "on" : "NOT FOUND"));
}
async function stopRecording() {
  clearInterval(timer);
  const b64 = await page.evaluate(() => window.__stopRec());
  fs.writeFileSync(path.join(out, "audio.webm"), Buffer.from(b64, "base64"));
  await new Promise((res) => { ff.on("close", res); ff.stdin.end(); });
  await cdp.send("Page.stopScreencast").catch(() => {});
  marks.duration = n / FPS;
  fs.writeFileSync(path.join(out, "marks.json"), JSON.stringify(marks, null, 1));
}

// ------------------------------------------------------------------ captions
async function overlay() {
  await page.evaluate(() => {
    const css = document.createElement("style");
    css.textContent = `#demo-cap{position:fixed;left:50%;bottom:22px;transform:translateX(-50%);max-width:1760px;white-space:nowrap;
      padding:11px 26px;border-radius:16px;background:rgba(12,12,14,.82);color:#fff;text-align:center;z-index:99999;
      pointer-events:none;transition:opacity .45s;opacity:0;font:600 29px/1.25 -apple-system,"Helvetica Neue",Arial,sans-serif;
      box-shadow:0 6px 30px rgba(0,0,0,.35)}
      #demo-cap small{display:block;font-weight:400;font-size:22px;opacity:.85;margin-top:4px}
      #demo-ff{position:fixed;right:36px;top:76px;padding:9px 18px;border-radius:12px;background:rgba(12,12,14,.82);
      color:#fff;font:700 28px -apple-system,"Helvetica Neue",Arial,sans-serif;z-index:99999;opacity:0;transition:opacity .3s}`;
    document.head.appendChild(css);
    for (const id of ["demo-cap", "demo-ff"]) { const d = document.createElement("div"); d.id = id; document.body.appendChild(d); }
  });
}
const caption = (html) => page.evaluate((h) => {
  const d = document.getElementById("demo-cap");
  if (!h) { d.style.opacity = 0; return; }
  d.innerHTML = h; d.style.opacity = 1;
}, html);
const badge = (t) => page.evaluate((t) => {
  const b = document.getElementById("demo-ff");
  if (t) b.textContent = t;
  b.style.opacity = t ? 1 : 0;
}, t);

// ------------------------------------------------------------------ the Mac
const push = (...a) => page.evaluate((a) => window.__mac.pushEvent(...a), a);
// Checks on the Mac's screen (below the menu bar), worked out inside the page so that only a number
// comes back: copying the whole screen out (175,000 values as JSON) blocks the page for about 0.2 s,
// which shows as a hitch in the video.
//   "change"   pixels changed since the last "change" check     "setDesk"  remember the desktop
//   "desk"     pixels in the icons' area that differ from it     "ink"      dark pixels in the input
//   "title"    a hash of the window's title bar, right side                  line (left of x = 470)
const scr = (op) => page.evaluate((op) => {
  const W = window.__scr || (window.__scr = {});
  const d = document.getElementById("screen").getContext("2d").getImageData(0, 20, 512, 322).data;
  const g = new Uint8Array(512 * 322);
  for (let i = 0; i < g.length; i++) g[i] = d[i * 4];
  const diff = (a, b, y1, x1) => { let n = 0; for (let y = 0; y < y1; y++) for (let x = 0; x < x1; x++) if (a[y * 512 + x] !== b[y * 512 + x]) n++; return n; };
  if (op === "change") { const p = W.prev; W.prev = g; return p ? diff(g, p, 322, 512) : 1e9; }
  if (op === "setDesk") { W.desk = g; return 0; }
  if (op === "desk") return diff(g, W.desk, 200, 340);
  if (op === "ink") { let n = 0; for (let y = 280; y < 322; y++) for (let x = 0; x < 470; x++) if (g[y * 512 + x] < 128) n++; return n; }
  let h = 2166136261;                                  // "title"
  for (let y = 2; y < 20; y++) for (let x = 380; x < 512; x++) { h ^= g[y * 512 + x]; h = Math.imul(h, 16777619); }
  return h >>> 0;
}, op);
async function quiet(minMs, quietMs, maxMs) {     // wait at least minMs, then until the screen is still for quietMs
  const s0 = Date.now();
  await sleep(minMs);
  await scr("change");
  let still = 0;
  while (Date.now() - s0 < maxMs) {
    await sleep(500);
    const d = await scr("change");
    still = d < 40 ? still + 500 : 0;
    if (still >= quietMs) return;
  }
}
async function key(k, ms = 90) { await page.keyboard.down(k); await sleep(ms); await page.keyboard.up(k); await sleep(70); }
async function type(text) {                        // real key events: the 3D keys move and click
  const SHIFTED = { "?": "/", "!": "1" };
  for (const ch of text) {
    let k = ch === " " ? "Space" : ch, shift = false;
    if (SHIFTED[ch]) { k = SHIFTED[ch]; shift = true; } else if (/[A-Z]/.test(ch)) { k = ch.toLowerCase(); shift = true; }
    if (shift) await page.keyboard.down("Shift");
    await key(k, 85);
    if (shift) await page.keyboard.up("Shift");
    await sleep(40 + Math.random() * 60);
  }
}
async function cmd(k) { await page.keyboard.down("Control"); await sleep(80); await key(k); await page.keyboard.up("Control"); await sleep(200); }
// Type a line and press Return; if the app didn't take it (the typed text is still in the input line:
// Pixel ignores Return while it's still reading its last reply aloud), wait and press again. The input
// line's ink is counted left of x = 470, clear of the pointer parked at the right edge (its shape
// changes when the app gets busy or idle).
async function send(text) {
  const empty = await scr("ink");                                       // the input line before typing
  await type(text);
  for (let tries = 0; tries < 6; tries++) {
    const before = await scr("ink");
    await key("Enter");
    await sleep(1500);
    const after = await scr("ink");
    if (after - empty < (before - empty) * 0.5) return;                 // the typed text left the input line
    console.log(`  Return not taken for "${text}", trying again`);
    await sleep(2500);
  }
}
// The desktop with its icons drawn (after an app quits, the Finder first shows an empty desktop for
// about a second), held for half a second before the next app opens. Compared with the desktop grabbed
// before the recording, in the icons' area (a selected icon or the pointer differ a little).
async function desktopPause() {
  const t0 = Date.now();
  while (Date.now() - t0 < 10000 && (await scr("desk")) >= 3000) await sleep(100);
  await sleep(500);
}
async function launch(name) {
  const [x, y] = await iconPoint(page, ...ICON[name]);
  await macClick(page, x, y, { double: true });
  try { await until(page, async (p) => (await macApp(p)) === name, 6000, name); }
  catch { await macClick(page, x, y); await sleep(400); await cmd("o"); await until(page, async (p) => (await macApp(p)) === name, 30000, name); }
}
async function findBlock(magic, notOff) {
  return page.evaluate(async ([magic, notOff]) => {
    const b = await window.__mac.peek(0, 4 * 1024 * 1024);
    const rd = (o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;
    for (let o = 0; o + notOff + 4 <= b.length; o += 2) if (rd(o) === magic && rd(o + notOff) === (~magic >>> 0)) return o;
    return -1;
  }, [magic, notOff]);
}
const rd32 = (a) => page.evaluate(async (a) => { const b = await window.__mac.peek(a, 4); return ((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]) >>> 0; }, a);
const rd16s = (a) => page.evaluate(async (a) => { const b = await window.__mac.peek(a, 2); const v = (b[0] << 8) | b[1]; return v > 32767 ? v - 65536 : v; }, a);
const poke32 = (a, v) => page.evaluate(([a, v]) => window.__mac.poke32(a, v), [a, v]);
const glide = async (z, tau = 700, wait = 2200) => { await page.evaluate(([z, t]) => window.__mac.view3d.glideTo(z, t), [z, tau]); await sleep(wait); };
const screenView = async () => { await page.evaluate(() => window.__mac.view3d.resetView(0.68)); await sleep(2000); await settled(page); };

// ------------------------------------------------------------------ the film
// Straight to the point (for Reddit): the Mac boots before the recording starts; the video opens on
// the whole Mac, zooms into the screen and goes straight to Pixel (two questions), Dream (a banana)
// and Guess My Doodle (a house). It ends on the Mac's guess with the links; demo_edit.py --hold
// holds that last frame a little longer.
// Quality fixed at high: on "auto" the page measures its first animated frames and can step down a
// level (rebuilding its shaders, a visible hitch) in the middle of the opening zoom.
await page.goto(url + (url.includes("?") ? "&" : "?") + "quality=high");
await until(page, (p) => p.evaluate(() => !!(window.__mac && window.__mac.view3d)), 60000, "page");
await overlay();
await page.mouse.click(40, H - 40);                    // a click on the desk: browsers start sound only after one
await until(page, (p) => p.evaluate(() => !!(window.__rec && window.__rec.dest)), 30000, "sound").catch(() => console.log("no sound yet"));
await until(page, async (p) => (await macApp(p)) === "Finder", 120000, "Finder");
await settled(page); await sleep(1500);
await scr("setDesk");
await page.evaluate(() => window.__mac.view3d.resetView(0.68));   // a practice zoom off camera: first-time
await sleep(2000);                                                 // texture and shader work happens now
await page.evaluate(() => window.__mac.view3d.jumpTo(0.1));   // the whole Mac on its desk
await sleep(1500); await settled(page);
await startRecording();
mark("start");
await caption("A 1986 Macintosh Plus running three neural networks<small>In your browser at macintosh.devon7y.com</small>");
await sleep(1300);
await page.evaluate(() => window.__mac.view3d.resetView(0.68));   // zoom into the screen
await sleep(700);
await desktopPause();

// Pixel
await caption("Pixel: a 6.7-million-parameter language model<small>Ternary transformer with a mixture of experts. It reads its replies aloud with MacinTalk.</small>");
await launch("Pixel");
await push(1, 509, 300);                               // the Mac's pointer out of the way, at the screen's edge
await fast(4);
await quiet(6000, 3000, 90000);                       // the model loads from disk
await fast(0);
mark("pixel ready");
// Pixel's title bar shows "thinking..." while it writes (and reads each sentence aloud before writing the
// next, so the screen can sit still mid-reply) and "V4" when it's done: compare with it now, idle.
const idleTitle = await scr("title");
const pixelIdle = async () => (await scr("title")) === idleTitle;
const msgs = [["what year is it?", 1], ["tell me a joke", 2]];      // lowercase, like most of its training chats: steadier answers
for (const [m, speed] of msgs) {
  await send(m);
  if (speed > 1) await fast(speed);
  else await caption("Pixel: a 6.7-million-parameter language model<small>The website runs the Mac at 4×. A real Plus makes about 0.2 tokens per second.</small>");
  await sleep(3000);
  await until(page, pixelIdle, 120000, "Pixel's reply");
  await sleep(m === msgs[msgs.length - 1][0] ? 3500 : 800);   // the last sentence read aloud before quitting
  await fast(0);
  await sleep(800);
}
mark("pixel done");
await cmd("q");
await until(page, async (p) => (await macApp(p)) === "Finder", 30000, "Finder after Pixel");
await desktopPause();

// Dream
await caption("Dream: a diffusion model that paints from a word<small>0.9M ternary weights, 6 denoising steps. About 1.6 minutes per picture on a real Plus.</small>");
await launch("Dream");
await push(1, 509, 300);
await fast(4);
await quiet(5000, 3000, 90000);
await fast(0);
await type("banana"); await key("Enter");
await sleep(1200);
await fast(2);
await quiet(12000, 5000, 150000);
await fast(0);
await sleep(1500);
mark("dream done");
await cmd("q");
await until(page, async (p) => (await macApp(p)) === "Finder", 30000, "Finder after Dream");
await desktopPause();

// Guess My Doodle
await caption("Guess My Doodle: you draw, the Mac guesses<small>A sketch classifier trained on Google's Quick, Draw! 150 things, about 1 s per guess on a real Plus.</small>");
await launch("Guess My Doodle");
let blk = -1;
await until(page, async () => (blk = await findBlock(0x47534442, 8)) >= 0, 60000, "Guess debug block");
await fast(4);
await until(page, async () => (await rd32(blk + 12)) === 1, 120000, "Guess title");
await fast(0);
await sleep(800);
const done0 = await rd32(blk + 72);
await poke32(blk + 68, 69); await poke32(blk + 64, 1);       // round 1's word: "house" (class 69)
await until(page, async () => (await rd32(blk + 72)) !== done0, 10000, "mailbox");
await key("Enter");
await until(page, async () => (await rd32(blk + 12)) === 2, 15000, "card");
await sleep(1500);
await key("Enter");
await until(page, async () => (await rd32(blk + 12)) === 3, 15000, "drawing");
await sleep(600);
const c = { top: await rd16s(blk + 40), left: await rd16s(blk + 42) };
const strokes = [
  [[50, 120], [50, 230], [206, 230], [206, 120], [50, 120]],
  [[40, 126], [128, 40], [216, 126]],
  [[110, 230], [110, 175], [146, 175], [146, 230]],
];
for (const s of strokes) {                         // the Mac's mouse, moved smoothly along each stroke
  await push(1, c.left + s[0][0], c.top + s[0][1]); await sleep(120);
  await push(2); await sleep(80);
  for (let i = 1; i < s.length; i++) {
    const [x0, y0] = s[i - 1], [x1, y1] = s[i];
    const steps = Math.max(4, Math.round(Math.hypot(x1 - x0, y1 - y0) / 6));
    for (let k = 1; k <= steps; k++) { await push(1, Math.round(c.left + x0 + (x1 - x0) * k / steps), Math.round(c.top + y0 + (y1 - y0) * k / steps)); await sleep(28); }
  }
  await sleep(80); await push(3);
  try { await until(page, async () => (await rd32(blk + 12)) === 4, 1800, "guessed"); break; } catch { /* keep drawing */ }
}
await until(page, async () => (await rd32(blk + 12)) === 4, 8000, "guessed").catch(() => console.log("no guess yet"));
mark("guessed");
await sleep(400);

// The end: the links over the Mac's answer. The game says "Oh, I know! It's a house!" from about
// 1.2 s to 3.8 s after its answer and moves on to round 2 at about 2.6 s, so the picture is frozen at
// "freeze" (demo_edit.py --freeze freeze) while the sound runs on to the end of the sentence.
await caption("Try it yourself: macintosh.devon7y.com<small>Code: github.com/devon7y/mac-plus-ai</small>");
await sleep(1600);
mark("freeze");
await sleep(2500);
mark("end");
await stopRecording();
await browser.close();
console.log(`done: ${marks.duration.toFixed(1)} s of video in ${out}`);
