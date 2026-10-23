// The site's test suite (headless Playwright: chromium, chrome, webkit or firefox). Each test boots a
// fresh Mac in the 3D view.
//   node web/tools/run_tests.mjs <dist dir> <browser> <out dir> [test names...]
// Writes <out dir>/<browser>/results.json and screenshots (*.png). Mac coordinates are the
// Finder layout made by build_disk.py (apps in two columns on the left, documents in a third).
import fs from "node:fs";
import path from "node:path";
import { serve, launch, until, macApp, macStats, macShot, shot, sleep, macClick, macMenu, peek, macToClient, settled, iconPoint } from "./lib.mjs";

const [dir, kind, outRoot, ...only] = process.argv.slice(2);
const out = path.join(outRoot, kind);
fs.mkdirSync(out, { recursive: true });
// Desktop icons' top-left corners (build_disk.py's layout); iconPoint() finds where to click.
const ICON = { Endurance: [22, 32], "Rail Runner": [22, 94], Blockworld: [22, 156], "Glider 4.06": [22, 218],
               Doodle: [98, 32], Pixel: [98, 94], Dream: [98, 156], "Read Me": [174, 32], "Credits & Licenses": [174, 94] };
const results = [];
const srv = await serve(dir);

const mac = (page) => page.evaluate(() => window.__mac);
const evalMac = (page, fn, arg) => page.evaluate(fn, arg);
const isFront = (page) => page.evaluate(() => !!(window.__mac.view3d && window.__mac.view3d.isFront()));
const audio = (page) => page.evaluate(() => ({ level: window.__mac.audioLevel(), nonSilent: window.__mac.stats && window.__mac.stats.audioNonSilent, ctx: window.__mac.audioCtx && window.__mac.audioCtx.state }));
async function key(page, k, ms = 90) { await page.keyboard.down(k); await sleep(ms); await page.keyboard.up(k); await sleep(60); }
async function typeHeld(page, text) {    // like a person: each key held about 80 ms
  for (const ch of text) { const k = ch === " " ? "Space" : ch; await page.keyboard.down(k); await sleep(80); await page.keyboard.up(k); await sleep(70); }
}
async function ctrl(page, k) { await page.keyboard.down("Control"); await sleep(80); await key(page, k); await page.keyboard.up("Control"); await sleep(100); }
async function mouseWord(page, addr) { const b = await peek(page, addr, 4); return b && { v: (b[0] << 8 | b[1]), h: (b[2] << 8 | b[3]) }; }

async function boot(page) {
  const t0 = Date.now();
  await page.goto(srv.url);
  await until(page, isFront, 60000, "the 3D view facing front");
  const loadedMs = Date.now() - t0;
  await until(page, async (p) => (await macApp(p)) === "Finder", 60000, "the Finder");
  const bootMs = Date.now() - t0 - loadedMs;
  await settled(page);
  await sleep(4000);       // the Finder draws its desktop and drops early clicks right after start-up
  return { loadedMs, bootMs };
}
let retries = [];
async function launchApp(page, name, appName = name, timeout = 30000) {
  const [x, y] = await iconPoint(page, ...ICON[name]);
  await macClick(page, x, y, { double: true });
  try {
    await until(page, async (p) => (await macApp(p)) === appName, 8000, `${appName} running`);
  } catch {
    retries.push(name);                       // like a person: double-click once more
    await macClick(page, x, y, { double: true });
    await until(page, async (p) => (await macApp(p)) === appName, timeout, `${appName} running`);
  }
}

async function run(name, fn, opts = {}) {
  if (only.length && !only.includes(name)) return;
  const t0 = Date.now();
  const { browser, page } = await launch(kind, opts);
  const r = { test: name, pass: false, notes: [] };
  const note = (s) => { r.notes.push(s); console.log(`  ${kind}/${name}: ${s}`); };
  try {
    await fn(page, note, (f) => macShot(page, path.join(out, `${name}_${f}.png`)), (f) => shot(page, path.join(out, `${name}_${f}_page.png`)));
    r.pass = true;
  } catch (e) {
    r.error = e.message;
    await shot(page, path.join(out, `${name}_FAILED_page.png`)).catch(() => {});
    await macShot(page, path.join(out, `${name}_FAILED.png`)).catch(() => {});
    r.logs = await page.evaluate(() => window.__mac && window.__mac.logs.slice(-15)).catch(() => null);
  }
  if (retries.length) { r.notes.push(`needed a second double-click: ${retries.join(", ")}`); retries = []; }
  r.seconds = Math.round((Date.now() - t0) / 1000);
  console.log(`${r.pass ? "PASS" : "FAIL"} ${kind}/${name} (${r.seconds} s)${r.error ? ": " + r.error : ""}`);
  results.push(r);
  await browser.close();
}

// ------------------------------------------------------------------ tests
await run("boot_3d", async (page, note, macshot, pageshot) => {
  await page.goto(srv.url);
  await sleep(900);
  await pageshot("1_intro_three_quarter");
  await until(page, isFront, 60000, "front view");
  await pageshot("2_front_before_start");
  const t0 = Date.now();
  await until(page, async (p) => (await macApp(p)) === "Finder", 60000, "Finder");
  note(`Finder ${((Date.now() - t0) / 1000).toFixed(1)} s after Click to start`);
  await sleep(3000);
  await pageshot("3_finder");
  await macshot("3_finder");
  const s = await evalMac(page, () => ({ load: window.__mac.loadMs, tick: window.__mac.tickRate, q: window.__mac.view3d.quality() }));
  note(`assets downloaded+unpacked in ${Math.round(s.load)} ms (localhost); emulated time runs at ${(s.tick / 60.15 * 100).toFixed(0)}% of real time; 3D quality ${s.q}`);
  // The page shows no status line and no leftover prose: only the floating buttons.
  const ui = await page.evaluate(() => ({ status: !!document.getElementById("status"), text: document.body.innerText }));
  if (ui.status) throw new Error("the status element is still on the page");
  for (const bad of ["Behind the Bezel", "no tracking", "On the Mac", "Running:"]) if (ui.text.includes(bad)) throw new Error(`leftover text on the page: ${bad}`);
});

// Snap-back: the view holds where it was left for 0.5 s, then eases home over about 1.5 s; a new
// drag during the return takes over from where it is. The cursor is hidden over the Mac's screen.
// Zooming keeps the Mac's centre in the middle of the area under the top buttons.
await run("view_orbit_zoom", async (page, note, macshot, pageshot) => {
  await boot(page);
  const box = await page.locator("#view3d").boundingBox();
  // Cursor: none over the screen, grab off the Mac, pointer over a key.
  const [sx, sy] = await macToClient(page, 256, 171);
  await page.mouse.move(sx, sy); await sleep(250);
  const curScreen = await evalMac(page, () => window.__mac.view3d.renderer.domElement.style.cursor);
  await page.mouse.move(box.x + 30, box.y + box.height - 30); await sleep(250);
  const curBg = await evalMac(page, () => window.__mac.view3d.renderer.domElement.style.cursor);
  await evalMac(page, () => window.__mac.view3d.jumpTo(0.12)); await sleep(700);
  const [kx, ky] = await evalMac(page, () => window.__mac.view3d.clientFromNode("H"));
  await page.mouse.move(kx, ky); await sleep(250);
  const curKey = await evalMac(page, () => window.__mac.view3d.renderer.domElement.style.cursor);
  note(`cursor over the screen "${curScreen}", off the Mac "${curBg}", over a 3D key "${curKey}"`);
  if (curScreen !== "none" || curBg === "none" || curKey !== "pointer") throw new Error("cursor styles wrong");
  // Drag on the background: turn the view.
  const z0 = await evalMac(page, () => window.__mac.view3d.view.zoomTarget);
  await page.mouse.move(box.x + 40, box.y + 120);
  await page.mouse.down();
  await page.mouse.move(box.x + 260, box.y + 190, { steps: 12 });
  await sleep(200);
  const mid = await evalMac(page, () => ({ yaw: window.__mac.view3d.view.yaw, pitch: window.__mac.view3d.view.pitch, front: window.__mac.view3d.isFront() }));
  await pageshot("1_dragging");
  if (Math.abs(mid.yaw) < 0.2 || mid.front) throw new Error("the drag did not turn the view");
  // Release and record yaw over time (page clock).
  await page.mouse.up();
  const trace = await page.evaluate(async () => {
    const v = window.__mac.view3d, t0 = performance.now(), y0 = v.view.yaw, out = [];
    while (performance.now() - t0 < 3200) { out.push([performance.now() - t0, v.view.yaw]); await new Promise((r) => setTimeout(r, 40)); if (v.isFront()) { out.push([performance.now() - t0, 0]); break; } }
    return { y0, out };
  });
  const firstMove = trace.out.find(([, y]) => y !== trace.y0);                 // the first frame that moved at all
  const home = trace.out.find(([, y]) => y === 0);
  const half = trace.out.find(([, y]) => Math.abs(y) < Math.abs(trace.y0) / 2);
  note(`released at yaw ${trace.y0.toFixed(2)} rad: held until ${firstMove ? Math.round(firstMove[0]) : "?"} ms, half-way at ${half ? Math.round(half[0]) : "?"} ms, front at ${home ? Math.round(home[0]) : "?"} ms`);
  if (!firstMove || firstMove[0] < 450 || firstMove[0] > 900) throw new Error("the view did not hold for about 0.5 s");
  if (!half || half[0] < 1000 || half[0] > 1600) throw new Error("the ease was not about 1.5 s long");
  if (!home || home[0] < 1600 || home[0] > 2800) throw new Error("the return did not take about 1.5 s");
  // A new drag during the return takes over from where the view is (no jump).
  await page.mouse.move(box.x + 40, box.y + 120); await page.mouse.down();
  await page.mouse.move(box.x + 300, box.y + 120, { steps: 10 }); await page.mouse.up();
  await sleep(1100);                                    // into the return
  await page.mouse.move(box.x + 40, box.y + 120);
  await page.evaluate(() => { window.__mac.view3d.renderer.domElement.addEventListener("pointerdown", () => { window.__yawAtDown = window.__mac.view3d.view.yaw; }, { capture: true, once: true }); });
  await page.mouse.down(); await sleep(150);
  const { before, after } = await page.evaluate(() => ({ before: window.__yawAtDown, after: window.__mac.view3d.view.yaw }));
  await page.mouse.move(box.x + 80, box.y + 120, { steps: 4 });
  const follow = await evalMac(page, () => window.__mac.view3d.view.yaw);
  await page.mouse.up();
  note(`drag during the return: yaw ${before.toFixed(3)} at the press, ${after.toFixed(3)} 150 ms later (held), ${follow.toFixed(3)} after moving 40 px`);
  if (Math.abs(after - before) > 0.01) throw new Error("the new drag jumped or the return kept going");
  if (Math.abs(follow - after) < 0.1) throw new Error("the new drag did not take over");
  await until(page, isFront, 4000, "front again");
  // Zoom centring at several zooms, and after a snap-back.
  let worst = 0;
  for (const z of [0, 0.25, 0.5, 0.75, 1]) {
    await evalMac(page, (z) => window.__mac.view3d.jumpTo(z), z); await sleep(300);
    const d = await evalMac(page, () => { const v = window.__mac.view3d, a = v.targetClient(), b = v.safeCenter(); return Math.hypot(a[0] - b[0], a[1] - b[1]); });
    worst = Math.max(worst, d);
  }
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  for (let i = 0; i < 4; i++) { await page.mouse.wheel(0, -120); await sleep(80); }
  await sleep(900);
  const z1 = await evalMac(page, () => window.__mac.view3d.view.zoom);
  await page.mouse.move(box.x + 30, box.y + box.height - 30); await page.mouse.down();
  await page.mouse.move(box.x + 200, box.y + box.height - 120, { steps: 10 }); await page.mouse.up();
  await until(page, isFront, 4000, "front again");
  await sleep(300);
  const after2 = await evalMac(page, () => { const v = window.__mac.view3d, a = v.targetClient(), b = v.safeCenter(); return { d: Math.hypot(a[0] - b[0], a[1] - b[1]), z: v.view.zoom }; });
  worst = Math.max(worst, after2.d);
  note(`zoom centring: the Mac's centre within ${worst.toFixed(2)} px of the middle (zooms 0-1 and after a snap-back); wheel zoom ${z0.toFixed(2)} -> ${z1.toFixed(2)}, kept after the drag: ${after2.z.toFixed(2)}`);
  if (worst > 2) throw new Error("zoom does not keep the Mac centred");
  if (Math.abs(after2.z - z1) > 0.01) throw new Error("zoom was not kept after the drag");
  await pageshot("2_zoomed");
});

await run("screen_clicks_zoom_levels", async (page, note, macshot, pageshot) => {
  await boot(page);
  const levels = await evalMac(page, () => [0.0, window.__mac.view3d.zoomForScreen(0.45), window.__mac.view3d.zoomForScreen(0.62), 1.0]);
  for (const z of levels) {
    await evalMac(page, (z) => window.__mac.view3d.jumpTo(z), z);
    await settled(page);
    await sleep(300);
    const w = await page.evaluate(() => { const r = window.__mac.view3d.pictureClientRect(); return r.right - r.left; });
    // Mouse position: move to a few Mac points and read the Mac's own mouse location (low memory
    // $830, Mouse: v, h).
    let worst = 0;
    for (const [x, y] of [[100, 100], [400, 60], [256, 300], [20, 330]]) {
      const [cx, cy] = await macToClient(page, x, y);
      await page.mouse.move(cx, cy);
      await sleep(250);
      const m = await mouseWord(page, 0x830);
      worst = Math.max(worst, Math.abs(m.h - x), Math.abs(m.v - y));
    }
    // Double-click the Read Me on the desktop: TeachText opens it; Ctrl-Q (the Mac's Cmd-Q) quits.
    await launchApp(page, "Read Me", "TeachText");
    await sleep(1200);
    await macshot(`z${z.toFixed(2)}_teachtext`);
    await pageshot(`z${z.toFixed(2)}`);
    await ctrl(page, "q");
    await until(page, async (p) => (await macApp(p)) === "Finder", 15000, "back in the Finder");
    // One CSS pixel covers 512/w Mac pixels (3.4 at the widest view), and some browsers report
    // whole-pixel pointer positions, so allow that much.
    const allowed = Math.ceil(512 / w) + 1;
    note(`zoom ${z.toFixed(2)} (screen ${Math.round(w)} px wide, ${(512 / w).toFixed(2)} Mac px per CSS px): pointer lands within ${worst} Mac px (allowed ${allowed}); double-click opened Read Me in TeachText; Ctrl-Q quit`);
    if (worst > allowed) throw new Error(`pointer off by ${worst} px at zoom ${z}`);
    await sleep(1500);
  }
});

await run("keys_and_mouse_3d", async (page, note, macshot, pageshot) => {
  await boot(page);
  await evalMac(page, () => window.__mac.view3d.jumpTo(0.12));
  await sleep(800);
  // A close-up of the keyboard before and while keys are held (the 3D keys sink 2.5 mm).
  const kb = await page.evaluate(() => {
    const v = window.__mac.view3d, r = document.getElementById("view3d").getBoundingClientRect();
    return { x: r.left + r.width * 0.3, y: r.top + r.height * 0.62, width: r.width * 0.4, height: r.height * 0.3 };
  });
  await page.screenshot({ path: path.join(out, "keys_and_mouse_3d_0_keyboard_up.png"), clip: kb });
  // Hold W and Space: their 3D keys go down; the Mac's KeyMap ($174) shows them pressed.
  await page.keyboard.down("w"); await page.keyboard.down(" ");
  await sleep(300);
  const kw = await evalMac(page, () => window.__mac.view3d.keyState("KeyW"));
  const ks = await evalMac(page, () => window.__mac.view3d.keyState("Space"));
  const km = await peek(page, 0x174, 16);
  await pageshot("1_W_and_Space_held");
  await page.screenshot({ path: path.join(out, "keys_and_mouse_3d_1_keyboard_W_Space_down.png"), clip: kb });
  await page.keyboard.up("w"); await page.keyboard.up(" ");
  await sleep(300);
  const kw2 = await evalMac(page, () => window.__mac.view3d.keyState("KeyW"));
  const bit = (code) => (km[code >> 3] >> (code & 7)) & 1;
  note(`W key down ${(kw.base - kw.y).toFixed(3)} cm (t=${kw.t}), Space ${(ks.base - ks.y).toFixed(3)} cm; after release W at ${(kw2.base - kw2.y).toFixed(3)} cm; Mac KeyMap: W ${bit(0x0d)}, Space ${bit(0x31)}`);
  if (!(kw.t === 1 && ks.t === 1 && kw2.t === 0)) throw new Error("3D keys did not move");
  if (!bit(0x0d) || !bit(0x31)) throw new Error("the Mac did not see W and Space held");
  // Esc: the 3D ` key moves; what the Mac sees.
  await page.keyboard.down("Escape"); await sleep(250);
  const km2 = await peek(page, 0x174, 16);
  const ke = await evalMac(page, () => window.__mac.view3d.keyState("Escape"));
  await page.keyboard.up("Escape");
  const esc = (km2[0x35 >> 3] >> (0x35 & 7)) & 1, grave = (km2[0x32 >> 3] >> (0x32 & 7)) & 1;
  note(`Esc held: Mac KeyMap 0x35 (Esc) = ${esc}, 0x32 (\`) = ${grave}; 3D \` key t=${ke.t}`);
  // Mouse button: press on the screen.
  await evalMac(page, () => window.__mac.view3d.jumpTo(0.12));
  await sleep(600);
  const [cx, cy] = await macToClient(page, 300, 200);
  await page.mouse.move(cx, cy); await page.mouse.down(); await sleep(250);
  // (software WebGL under load can take a few frames; wait for the 40 ms press to finish)
  await until(page, (p) => p.evaluate(() => window.__mac.view3d.buttonState().t === 1), 2000, "the 3D button down").catch(() => {});
  const b1 = await evalMac(page, () => window.__mac.view3d.buttonState());
  await pageshot("2_mouse_button_down");
  await page.mouse.up(); await sleep(250);
  await until(page, (p) => p.evaluate(() => window.__mac.view3d.buttonState().t === 0), 2000, "the 3D button up").catch(() => {});
  const b2 = await evalMac(page, () => window.__mac.view3d.buttonState());
  note(`mouse button: down ${(b1.base - b1.y).toFixed(3)} cm (t=${b1.t}), after release t=${b2.t}`);
  if (!(b1.t === 1 && b2.t === 0)) throw new Error("the 3D mouse button did not move");
});

// Typing on the 3D keyboard (Shift latches until the next key) gives the same text as the real
// keyboard; 3D clicks make click sounds, real typing and screen clicks don't.
await run("clicks_3d_keyboard", async (page, note, macshot, pageshot) => {
  await boot(page);
  await launchApp(page, "Read Me", "TeachText");
  await sleep(1500);
  const count = () => evalMac(page, () => window.__mac.clickCount || 0);
  const c0 = await count();
  await ctrl(page, "a"); await key(page, "Backspace"); await sleep(300);
  await typeHeld(page, "x"); await key(page, "Backspace");
  const [mx, my] = await macToClient(page, 300, 200);
  await page.mouse.move(mx, my); await sleep(100);
  const c1 = await count();
  note(`real typing: ${c1 - c0} click sounds (keyboard and mouse sounds are on by default: expected some)`);
  if (!(c1 > c0)) throw new Error("real typing made no click sounds");
  // Reference: "Hi", Return, "A" from the real keyboard.
  await page.keyboard.down("Shift"); await key(page, "h"); await page.keyboard.up("Shift");
  await key(page, "i"); await key(page, "Enter");
  await page.keyboard.down("Shift"); await key(page, "a"); await page.keyboard.up("Shift");
  await sleep(600);
  const grab = () => page.evaluate(() => Array.from(document.getElementById("screen").getContext("2d").getImageData(0, 38, 512, 60).data.filter((_, i) => i % 4 === 0)));
  const ref = await grab();
  await macshot("1_typed_real_keyboard");
  await ctrl(page, "a"); await key(page, "Backspace"); await sleep(400);
  // The same on the 3D keyboard.
  await evalMac(page, () => window.__mac.view3d.jumpTo(0.14)); await sleep(800);
  const clickNode = async (name) => {
    const [x, y] = await evalMac(page, (n) => window.__mac.view3d.clientFromNode(n), name);
    await page.mouse.move(x, y); await sleep(60);
    await page.mouse.down(); await sleep(110); await page.mouse.up(); await sleep(160);
  };
  await clickNode("Shift.001");
  const lat = await evalMac(page, () => ({ latched: window.__mac.view3d.latched(), held: window.__mac.view3d.keyStateNode("Shift.001").held }));
  // The click bus's loudness: a long analyser (0.74 s of samples) on the click bus, read a few times
  // after the next key's click; the peak sample. (Software WebGL can hold the main thread for a few
  // hundred ms per frame, so a short analyser polled from the page could miss a 70 ms click.)
  await page.evaluate(() => {
    const m = window.__mac, an = m.audioCtx.createAnalyser();
    an.fftSize = 32768; m.clicks.bus.connect(an); window.__longAnalyser = an;
  });
  const peakP = page.evaluate(async () => {
    const an = window.__longAnalyser, buf = new Float32Array(an.fftSize); let peak = 0;
    const t0 = performance.now();
    while (performance.now() - t0 < 1500) { an.getFloatTimeDomainData(buf); for (const v of buf) peak = Math.max(peak, Math.abs(v)); await new Promise((r) => setTimeout(r, 120)); }
    return peak;
  });
  await clickNode("H");
  const lvl = await peakP;
  const lat2 = await evalMac(page, () => window.__mac.view3d.latched());
  await clickNode("I"); await clickNode("Return"); await clickNode("Shift"); await clickNode("A");
  await sleep(600);
  const got = await grab();
  await macshot("2_typed_3d_keyboard");
  await pageshot("2_typed_3d_keyboard");
  let diff = 0; for (let i = 0; i < ref.length; i++) if (ref[i] !== got[i]) diff++;
  const c2 = await count();
  note(`3D Shift latched (${JSON.stringify(lat.latched)}, drawn held: ${lat.held}), released after H (${JSON.stringify(lat2)}); "Hi⏎A" typed on the 3D keys differs from the real keyboard's in ${diff} pixels (caret blink allowed); ${c2 - c1} click sounds, peak click-bus sample ${lvl.toFixed(3)}`);
  if (!lat.latched.length || !lat.held || lat2.length) throw new Error("Shift did not latch and release");
  if (diff > 40) throw new Error("3D typing gave different text");
  if (c2 - c1 < 12 || !(lvl > 0.01)) throw new Error("no click sounds from the 3D keys");
});

// The 3D mouse's button clicks where the Mac's cursor is: one click selects an icon, two open it.
await run("mouse_button_3d", async (page, note, macshot, pageshot) => {
  await boot(page);
  await evalMac(page, () => window.__mac.view3d.jumpTo(0.14)); await sleep(800);
  const [ix, iy] = await iconPoint(page, ...ICON.Endurance);
  const [cx, cy] = await macToClient(page, ix, iy);
  await page.mouse.move(cx, cy); await sleep(400);                     // the Mac's cursor onto the icon
  const btn = await evalMac(page, () => window.__mac.view3d.clientFromButton());
  await page.mouse.move(btn[0], btn[1]); await sleep(300);              // straight to the 3D button, off the screen
  const black = () => page.evaluate(([x, y]) => { const d = document.getElementById("screen").getContext("2d").getImageData(x, y, 32, 32).data; let n = 0; for (let i = 0; i < d.length; i += 4) if (d[i] < 128) n++; return n; }, ICON.Endurance);
  const b0 = await black();
  const c0 = await evalMac(page, () => window.__mac.clickCount || 0);
  await page.mouse.down(); await sleep(150);
  const bs = await evalMac(page, () => window.__mac.view3d.buttonState());
  await page.mouse.up(); await sleep(700);
  const b1 = await black();
  await macshot("1_selected");
  const c1 = await evalMac(page, () => window.__mac.clickCount || 0);
  note(`the Mac cursor on Endurance, then the 3D button: icon dark pixels ${b0} -> ${b1} (selected: inverted), button down ${(bs.base - bs.y).toFixed(3)} cm, ${c1 - c0} click sounds`);
  if (!(b1 > b0 + 50)) throw new Error("one click on the 3D button did not select the icon");
  if (c1 - c0 < 2) throw new Error("no mouse click sounds");
  await sleep(800);
  await page.mouse.down(); await sleep(70); await page.mouse.up(); await sleep(90);
  await page.mouse.down(); await sleep(70); await page.mouse.up();
  await until(page, async (p) => (await macApp(p)) === "Endurance", 15000, "Endurance opened by a double-click on the 3D button");
  note("two quick clicks on the 3D button opened Endurance");
  // Screen clicks make no click sounds.
  await sleep(1500);
  const c2 = await evalMac(page, () => window.__mac.clickCount || 0);
  const [qx, qy] = await macToClient(page, 300, 200);
  await page.mouse.move(qx, qy); await page.mouse.down(); await sleep(80); await page.mouse.up(); await sleep(300);
  const c3 = await evalMac(page, () => window.__mac.clickCount || 0);
  note(`a click on the screen: ${c3 - c2} click sounds (expected 2: down and up)`);
  if (c3 - c2 !== 2) throw new Error("a screen click didn't make its two click sounds");
});

// Help opens on the keys of the program in front (CurApName); in the Finder on the general tips.
// The other programs' keys are there too, folded. Nothing pops up on its own.
await run("help_keys", async (page, note, macshot, pageshot) => {
  await boot(page);
  const help = async () => {
    await page.click('[data-panel="help"]'); await sleep(300);
    const r = await page.evaluate(() => ({
      app: document.getElementById("help-app").hidden ? null : document.querySelector("#help-app h3").textContent,
      rows: document.querySelectorAll("#help-app .keylist dt").length,
      tipsOpen: document.getElementById("help-tips").open,
      others: Array.from(document.querySelectorAll("#help-others-list h4")).map((h) => h.textContent),
      popups: Array.from(document.querySelectorAll("aside, .appcard")).filter((e) => !e.hidden).length,
    }));
    await key(page, "Escape"); await sleep(200);
    return r;
  };
  const f = await help();
  await launchApp(page, "Endurance"); await sleep(1500);
  const popup = await page.evaluate(() => Array.from(document.querySelectorAll("aside, .appcard")).filter((e) => !e.hidden).length);
  const e = await help();
  await pageshot("1_help_endurance");
  await key(page, "Escape"); await sleep(1200); await key(page, "Escape");
  await until(page, async (p) => (await macApp(p)) === "Finder", 10000, "the Finder");
  await launchApp(page, "Dream"); await sleep(1500);
  const d = await help();
  note(`Finder: tips open ${f.tipsOpen}, no app section (${f.app}), ${f.others.length} programs folded below; Endurance running: no pop-up (${popup}), Help shows "${e.app}" (${e.rows} rows), tips folded (${!e.tipsOpen}); Dream: "${d.app}"`);
  if (f.app !== null || !f.tipsOpen || f.others.length < 8) throw new Error("Help in the Finder is wrong");
  if (popup || e.app !== "Endurance" || e.rows < 5 || e.tipsOpen || e.others.includes("Endurance")) throw new Error("Help did not show Endurance's keys");
  if (d.app !== "Dream") throw new Error("Help did not follow the program in front");
});

// Shadows stay soft whatever moves: the floor's shadow edge is the same while the Mac's cursor moves
// (the 3D mouse slides), while 3D keys are typed and after the view is turned and comes back.
await run("shadows_stay_soft", async (page, note, macshot, pageshot) => {
  await boot(page);
  // High quality (shadow maps on; headless software WebGL would have stepped down to low).
  await evalMac(page, () => { window.__mac.view3d.setQuality("high"); window.__mac.view3d.jumpTo(0.1); }); await sleep(2500);
  const crop = { x: 1000, y: 450, width: 90, height: 110 };          // the Mac's shadow edge, right of it, clear of the cable
  const grab = async () => Array.from(await page.screenshot({ clip: crop }));
  const lum = async () => page.evaluate(async (b64) => {
    const img = new Image(); img.src = "data:image/png;base64," + b64; await img.decode();
    const c = document.createElement("canvas"); c.width = img.width; c.height = img.height; const x = c.getContext("2d"); x.drawImage(img, 0, 0);
    const d = x.getImageData(0, 0, c.width, c.height).data; const out = []; for (let i = 0; i < d.length; i += 4) out.push(0.2126 * d[i] + 0.7152 * d[i + 1] + 0.0722 * d[i + 2]); return out;
  }, (await page.screenshot({ clip: crop })).toString("base64"));
  const diff = (a, b) => a.reduce((s, v, i) => s + Math.abs(v - b[i]), 0) / a.length;
  const edge = (a) => { let g = 0; for (let y = 0; y < crop.height; y++) for (let x = 1; x < crop.width; x++) g = Math.max(g, Math.abs(a[y * crop.width + x] - a[y * crop.width + x - 1])); return g; };
  await page.screenshot({ path: path.join(out, "shadows_stay_soft_1_still_page.png") });
  const still = await lum();
  // The Mac's cursor moving (the 3D mouse follows it).
  const pts = await evalMac(page, () => [[100, 100], [400, 250]].map(([x, y]) => window.__mac.view3d.clientFromMac(x, y)));
  let stop = false;
  const mover = (async () => { let k = 0; while (!stop) { const t = (k++ % 20) / 20; await page.mouse.move(pts[0][0] + (pts[1][0] - pts[0][0]) * t, pts[0][1] + (pts[1][1] - pts[0][1]) * t); await sleep(30); } })();
  await sleep(1200);
  const moving = await lum();
  await page.screenshot({ path: path.join(out, "shadows_stay_soft_2_cursor_moving_page.png") });
  stop = true; await mover;
  // Typing on the 3D keys.
  const typing = await (async () => {
    const names = ["H", "I", "J", "K"];
    const p = (async () => { for (const n of names) { const [x, y] = await evalMac(page, (n) => window.__mac.view3d.clientFromNode(n), n); await page.mouse.move(x, y); await page.mouse.down(); await sleep(120); await page.mouse.up(); await sleep(60); } })();
    await sleep(250); const l = await lum(); await p; return l;
  })();
  // Turned and back.
  const box = await page.locator("#view3d").boundingBox();
  await page.mouse.move(box.x + 40, box.y + 200); await page.mouse.down();
  await page.mouse.move(box.x + 300, box.y + 260, { steps: 10 }); await page.mouse.up();
  await until(page, isFront, 6000, "front again"); await sleep(800);
  const back = await lum();
  const dm = diff(still, moving), dt = diff(still, typing), db = diff(still, back);
  note(`shadow edge crop: mean change ${dm.toFixed(2)} levels with the cursor moving, ${dt.toFixed(2)} while typing on the 3D keys, ${db.toFixed(2)} after turning the view; sharpest step across the edge ${edge(still).toFixed(1)} / ${edge(moving).toFixed(1)} / ${edge(typing).toFixed(1)} / ${edge(back).toFixed(1)}`);
  if (edge(still) < 2) throw new Error("no shadow edge in the crop");
  if (dm > 1.5 || dt > 1.5 || db > 1.5) throw new Error("the shadow changed");
});

// The two documents on the Mac open in TeachText: "Credits & Licenses" on the desktop and "Dream
// FLUX License" in the Macintosh HD window.
await run("credits_documents", async (page, note, macshot, pageshot) => {
  await boot(page);
  await launchApp(page, "Credits & Licenses", "TeachText");
  await sleep(1500);
  await until(page, (p) => p.evaluate(() => {
    const d = document.getElementById("screen").getContext("2d").getImageData(20, 60, 470, 260).data;
    let w = 0; for (let i = 0; i < d.length; i += 4) if (d[i] > 128) w++;
    return w / (d.length / 4) > 0.85;
  }), 30000, "the credits' window");
  await macshot("1_credits");
  await pageshot("1_credits");
  await ctrl(page, "q");
  await until(page, async (p) => (await macApp(p)) === "Finder", 15000, "the Finder");
  await sleep(1500);
  const [hx, hy] = await iconPoint(page, 457, 24);           // the Macintosh HD icon, top right
  await macClick(page, hx, hy, { double: true });
  await sleep(2500);
  await macshot("2_hd_window");
  const [fx, fy] = await iconPoint(page, 94, 86);            // "Dream FLUX License" in the window (second icon)
  await macClick(page, fx, fy, { double: true });
  try { await until(page, async (p) => (await macApp(p)) === "TeachText", 8000, "TeachText"); }
  catch { retries.push("Dream FLUX License"); await macClick(page, fx, fy, { double: true }); await until(page, async (p) => (await macApp(p)) === "TeachText", 15000, "TeachText"); }
  // TeachText takes a few seconds (watch cursor) to lay out the 21 KB licence: wait for its window,
  // a mostly white page.
  const t0 = Date.now();
  await until(page, (p) => p.evaluate(() => {
    const d = document.getElementById("screen").getContext("2d").getImageData(20, 60, 470, 260).data;
    let w = 0; for (let i = 0; i < d.length; i += 4) if (d[i] > 128) w++;
    return w / (d.length / 4) > 0.85;
  }), 30000, "the licence's window");
  await sleep(1000);
  await macshot("3_flux_license");
  await pageshot("3_flux_license");
  note(`"Credits & Licenses" (desktop) and "Dream FLUX License" (Macintosh HD window) both open in TeachText (the licence's window after ${((Date.now() - t0) / 1000).toFixed(1)} s)`);
});

await run("endurance_audio_esc", async (page, note, macshot) => {
  await boot(page);
  await launchApp(page, "Endurance");
  await sleep(2000);
  await macshot("1_title");
  await key(page, "Enter");
  await sleep(1200);
  const a0 = await audio(page);
  await page.keyboard.down("w");
  await sleep(2500);
  const a1 = await audio(page);
  await macshot("2_racing");
  await sleep(800);
  const a2 = await audio(page);
  await page.keyboard.up("w");
  note(`AudioContext ${a1.ctx}; analyser RMS while the engine runs: ${a1.level.toFixed(3)}, ${a2.level.toFixed(3)}; non-silent samples from the Mac ${a0.nonSilent} -> ${a2.nonSilent}`);
  if (!(Math.max(a1.level, a2.level) > 0.005) || !(a2.nonSilent > a0.nonSilent)) throw new Error("no sound from the engine");
  await key(page, "Escape");
  await sleep(1500);
  const t = await macApp(page);
  await macshot("3_after_esc");
  await key(page, "Escape");
  await until(page, async (p) => (await macApp(p)) === "Finder", 10000, "the Finder after the second Esc");
  note(`Esc: back to the title (${t}), Esc again: quit to the Finder`);
});

await run("rail_runner", async (page, note, macshot) => {
  await boot(page);
  await launchApp(page, "Rail Runner", "Rail Runner");
  await sleep(2000);
  await macshot("1_title");
  await key(page, "Enter");
  await sleep(1200);
  await key(page, "a", 120); await sleep(400); await key(page, "w", 120); await sleep(300);
  await macshot("2_running");
  await sleep(4000);
  await macshot("3_later");
  await key(page, "Escape"); await sleep(1500);
  await key(page, "Escape");
  await until(page, async (p) => (await macApp(p)) === "Finder", 10000, "the Finder after Esc Esc");
  note("ran (Return, A, W), Esc twice quit to the Finder");
});

await run("blockworld", async (page, note, macshot) => {
  await boot(page);
  const t0 = Date.now();
  await launchApp(page, "Blockworld");
  await sleep(3000);
  await macshot("1_making_world");
  // The world is ready when the HUD's hotbar appears at the bottom (white squares on black).
  await until(page, async (p) => p.evaluate(() => {
    const d = document.getElementById("screen").getContext("2d").getImageData(200, 300, 112, 20).data;
    let white = 0; for (let i = 0; i < d.length; i += 4) if (d[i] > 200) white++;
    return white > 300;
  }), 120000, "the world");
  note(`world ready ${((Date.now() - t0) / 1000).toFixed(0)} s after launch`);
  await sleep(1500);
  await macshot("2_world");
  await page.keyboard.down("w"); await sleep(1500); await page.keyboard.up("w");
  await page.keyboard.down("d"); await sleep(800); await page.keyboard.up("d");
  await macshot("3_walked");
  await page.click("#btn-lock").catch(() => {});
  await sleep(300);
  const locked = await page.evaluate(() => document.pointerLockElement === window.__mac.view3d.renderer.domElement);
  await page.mouse.move(700, 450); await page.mouse.move(760, 450, { steps: 5 });
  await sleep(600);
  await macshot("4_after_mouse_look");
  note(`walked with W and D; Lock mouse ${locked ? "engaged pointer lock" : "could not lock the pointer here (headless)"}`);
  await key(page, "Escape");
  await sleep(1500);
  await macshot("5_esc_menu");
});

// Blockworld reads the mouse relatively: the page sends only movement. Turns are proportional to the
// movement (Help's mouse speed, default 0.4 Mac px per px: 2 yaw units per Mac px, 1024 a turn) and
// stop when the mouse stops; a click still mines.
await run("blockworld_mouse_look", async (page, note, macshot, pageshot) => {
  await boot(page);
  await launchApp(page, "Blockworld");
  await until(page, async (p) => p.evaluate(() => {
    const d = document.getElementById("screen").getContext("2d").getImageData(200, 300, 112, 20).data;
    let white = 0; for (let i = 0; i < d.length; i += 4) if (d[i] > 200) white++;
    return white > 300;
  }), 60000, "the world");
  await sleep(1500);
  // Blockworld's debug block: magic 'BWGM' then its complement; yaw (int16) at +28, mined at +100.
  const ram = await peek(page, 0, 0x400000);
  let blk = -1;
  for (let a = 0x1000; a < ram.length - 8; a += 2)
    if (ram[a] === 0x42 && ram[a + 1] === 0x57 && ram[a + 2] === 0x47 && ram[a + 3] === 0x4D && ram[a + 4] === 0xBD && ram[a + 5] === 0xA8) { blk = a; break; }
  if (blk < 0) throw new Error("Blockworld's debug block not found");
  const yaw = async () => { const b = await peek(page, blk + 28, 2); return ((b[0] << 8) | b[1]) << 16 >> 16; };
  const sens = await evalMac(page, () => window.__mac.lookSens);
  const [cx, cy] = await macToClient(page, 256, 120);
  await page.mouse.move(cx, cy); await sleep(800);
  const y0 = await yaw();
  await page.mouse.move(cx + 100, cy, { steps: 10 }); await sleep(800);
  const y1 = await yaw();
  await sleep(1000);
  const y1b = await yaw();
  await page.mouse.move(cx + 200, cy, { steps: 10 }); await sleep(800);
  const y2 = await yaw();
  await page.mouse.move(cx + 150, cy, { steps: 5 }); await sleep(800);
  const y3 = await yaw();
  const wrap = (d) => ((d + 512) % 1024 + 1024) % 1024 - 512;
  const d1 = wrap(y1 - y0), d2 = wrap(y2 - y1b), d3 = wrap(y3 - y2), still = wrap(y1b - y1);
  const expect = Math.round(100 * sens * 2);
  note(`mouse speed ${sens}: 100 px right turned ${d1} units (${(d1 * 360 / 1024).toFixed(1)}°; expected about ${expect}), still for 1 s: ${still}, another 100 px: ${d2}, 50 px back: ${d3}`);
  if (Math.abs(d1 - expect) > expect * 0.25 + 2 || Math.abs(d2 - expect) > expect * 0.25 + 2) throw new Error("turns not proportional to the movement");
  if (still !== 0) throw new Error("the view kept turning with the mouse still");
  if (!(d3 < 0 && Math.abs(d3 + expect / 2) <= expect * 0.25 + 2)) throw new Error("moving back did not turn back");
  // A click: the first one captures the mouse where the browser allows it (headless can't: then
  // clicks are plain clicks); holding the button mines.
  // Look straight down at the block under the feet (the fastest mouse speed, so the pointer stays on
  // the screen without a captured mouse).
  await evalMac(page, () => { window.__mac.lookSens = 1.2; });
  await page.mouse.move(cx + 150, cy + 240, { steps: 12 }); await sleep(800);
  await evalMac(page, (v) => { window.__mac.lookSens = v; }, sens);
  const hit = await peek(page, blk + 88, 2);
  const mined0 = (await peek(page, blk + 100, 4)).reduce((a, b) => a * 256 + b, 0);
  const locked = await page.evaluate(() => !!document.pointerLockElement);
  await page.mouse.down(); await sleep(150); await page.mouse.up(); await sleep(300);
  const lockedAfter = await page.evaluate(() => !!document.pointerLockElement);
  await page.mouse.down(); await sleep(4000); await page.mouse.up(); await sleep(500);
  const mined1 = (await peek(page, blk + 100, 4)).reduce((a, b) => a * 256 + b, 0);
  await macshot("1_after_mining");
  note(`looking down at a block (picked: ${((hit[0] << 8) | hit[1]) ? "yes" : "no"}); pointer lock ${lockedAfter ? "taken by the first click" : "not available here (headless): clicks stay plain clicks"}; blocks mined while holding the button: ${mined1 - mined0}`);
  if (!(mined1 > mined0)) throw new Error("holding the button did not mine");
  await key(page, "Escape"); await sleep(1500);
  await macshot("2_esc_menu");
});

// Where the moving thing is: the centroid of pixels that change between two frames 120 ms apart
// (in Glider's first room only the glider moves).
async function motionX(page) {
  const grab = () => page.evaluate(() => Array.from(document.getElementById("screen").getContext("2d").getImageData(0, 24, 512, 300).data.filter((_, i) => i % 4 === 0)));
  const a = await grab(); await sleep(120); const b = await grab();
  let sx = 0, n = 0;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) { sx += i % 512; n++; }
  return n > 20 ? sx / n : null;
}

await run("glider", async (page, note, macshot) => {
  await boot(page);
  await launchApp(page, "Glider 4.06");
  await sleep(3000);
  await macshot("1_title");
  const sp = await evalMac(page, () => ({ applied: window.__mac.appliedSpeed, status: window.__mac.statusText }));
  await ctrl(page, "n");                       // Game > New Game (Cmd-N: Ctrl in the browser)
  // Wait for the glider to come in at the left of the first room (the room is drawn first).
  let x0 = null;
  const tw = Date.now();
  let near = 0;                                // two readings in a row near the left wall
  while (Date.now() - tw < 12000) { x0 = await motionX(page); near = x0 !== null && x0 < 60 ? near + 1 : 0; if (near >= 2) break; await sleep(150); }
  await macshot("2_room");
  await page.keyboard.down("."); await sleep(1300);
  const x1 = await motionX(page);
  await page.keyboard.up(".");
  await macshot("3_after_period");
  await page.keyboard.down(","); await sleep(1100);
  const x2 = await motionX(page);
  await page.keyboard.up(",");
  await macshot("4_after_comma");
  note(`no dialogs; page speed while Glider runs: ${sp.applied === 0 ? "real Plus (1x)" : sp.applied} ("${sp.status}"); Ctrl-N starts; glider x ${x0 && x0.toFixed(0)} -> ${x1 && x1.toFixed(0)} after holding . -> ${x2 && x2.toFixed(0)} after holding ,`);
  if (sp.applied !== 0) throw new Error("Glider was not run at real Plus speed");
  if (!(x0 !== null && x1 !== null && x1 > x0 + 25)) throw new Error("holding . did not move the glider right");
  if (!(x2 !== null && x2 < x1 - 15)) throw new Error("holding , did not move the glider left");
});

await run("doodle", async (page, note, macshot) => {
  await boot(page);
  await launchApp(page, "Doodle");
  await sleep(2500);
  await macshot("1_open");
  // A sketch: half a face inside the dotted frame.
  const pts = [];
  for (let a = 0.6; a <= 3.6; a += 0.12) pts.push([290 + 70 * Math.cos(a), 160 + 70 * Math.sin(a)]);
  let [cx, cy] = await macToClient(page, pts[0][0], pts[0][1]);
  await page.mouse.move(cx, cy); await sleep(150); await page.mouse.down();
  for (const [x, y] of pts.slice(1)) { [cx, cy] = await macToClient(page, x, y); await page.mouse.move(cx, cy); await sleep(40); }
  await page.mouse.up();
  await sleep(500);
  await macshot("2_sketch");
  await macClick(page, 88, 160);               // Finish it
  await sleep(1500);
  await typeHeld(page, "cat");
  await key(page, "Enter");
  await sleep(2500);
  await macshot("3_finishing");
  await sleep(20000);
  await macshot("4_finished");
  note("drew a stroke; Finish it with \"cat\" ran (screenshots 3, 4)");
});

await run("pixel", async (page, note, macshot) => {
  await boot(page);
  const t0 = Date.now();
  await launchApp(page, "Pixel");
  await sleep(14000);
  await macshot("1_awake");
  await typeHeld(page, "hello pixel");
  await key(page, "Enter");
  await sleep(18000);
  await macshot("2_reply");
  // The redesigned Pixel fills the screen under the menu bar (no black backdrop): the outermost
  // columns, x 0-1 and 510-511, must not be black from top to bottom.
  const edges = await page.evaluate(() => {
    const c = document.getElementById("screen").getContext("2d");
    const lit = (x) => { const d = c.getImageData(x, 22, 2, 318).data; let w = 0; for (let i = 0; i < d.length; i += 4) if (d[i] > 128) w++; return w; };
    return { left: lit(0), right: lit(510) };
  });
  note(`window edges: ${edges.left} light pixels in x 0-1, ${edges.right} in x 510-511 (636 each if all white)`);
  if (edges.left < 100 || edges.right < 100) throw new Error("Pixel does not reach the screen edges");
  note(`launched; typed "hello pixel"; reply after about 18 s (screenshot 2); ${((Date.now() - t0) / 1000).toFixed(0)} s in all`);
});

await run("dream", async (page, note, macshot) => {
  await boot(page);
  await launchApp(page, "Dream");
  await sleep(10000);
  await macshot("1_loaded");
  await typeHeld(page, "house");
  await key(page, "Enter");
  await sleep(5000);
  await macshot("2_developing");
  await sleep(15000);
  await macshot("3_done");
  note("model loaded; \"house\" developing (2) and done (3)");
});

await run("talking_moose_audio", async (page, note, macshot) => {
  await boot(page);
  await macMenu(page, 20, 171);                // Apple menu > Talking Moose
  await sleep(2000);
  await macshot("1_settings");
  const a0 = await audio(page);
  let best = 0, at = 0;
  const t0 = Date.now();
  while (Date.now() - t0 < 100000) {
    const a = await audio(page);
    if (a.level > best) { best = a.level; at = Date.now() - t0; }
    if (a.level > 0.02) { await macshot("2_moose_talking"); break; }
    await sleep(200);
  }
  const a1 = await audio(page);
  note(`after opening the DA: best RMS ${best.toFixed(3)} at ${(at / 1000).toFixed(0)} s; non-silent samples ${a0.nonSilent} -> ${a1.nonSilent}`);
  if (best < 0.01) throw new Error("the Moose did not talk within 100 s");
  await sleep(2500);
  await macshot("3_after");
});

// Phones get the 3D view with touch-sized controls folded into one menu button.
for (const [name, w, h] of [["phone_portrait", 390, 844], ["phone_landscape", 844, 390]]) {
  await run(name, async (page, note, macshot, pageshot) => {
    await page.goto(srv.url);
    await until(page, isFront, 60000, "the 3D view");
    const m = await evalMac(page, () => ({ mode: window.__mac.mode, q: window.__mac.view3d.quality(), sw: document.documentElement.scrollWidth, w: window.innerWidth,
      menu: getComputedStyle(document.getElementById("btn-menu")).display !== "none" }));
    await pageshot("1_before_start");
    note(`${w}x${h}: ${m.mode} view (quality ${m.q}), page width ${m.sw} for a ${m.w} px viewport, menu button ${m.menu ? "shown" : "hidden"}`);
    if (m.mode !== "3d") throw new Error("no 3D view on a phone");
    if (m.sw > m.w + 1) throw new Error("horizontal scroll on a phone");
    if (m.menu) {
      await page.click("#btn-menu"); await sleep(300);
      await pageshot("2_menu_open");
      const open = await evalMac(page, () => getComputedStyle(document.getElementById("topitems")).display !== "none");
      await page.mouse.click(w / 2, h - 30); await sleep(300);          // a tap on the desk, away from Click to start
      const closed = await evalMac(page, () => getComputedStyle(document.getElementById("topitems")).display === "none");
      note(`menu opens (${open}) and closes on a tap outside (${closed})`);
      if (!open || !closed) throw new Error("the phone menu does not open and close");
    }
    await until(page, async (p) => (await macApp(p)) === "Finder", 60000, "Finder");
    await settled(page);
    await sleep(4000);
    await pageshot("3_finder");
    await launchApp(page, "Read Me", "TeachText");
    await sleep(1200);
    await pageshot("4_read_me");
    const pr = await evalMac(page, () => { const r = window.__mac.view3d.pictureClientRect(); return Math.round(r.right - r.left); });
    note(`the Mac's screen is ${pr} px wide; double-click opened Read Me`);
  }, { viewport: { width: w, height: h }, dpr: 2 });
}

fs.writeFileSync(path.join(out, "results.json"), JSON.stringify(results, null, 1));
console.log(`${kind}: ${results.filter((r) => r.pass).length}/${results.length} passed -> ${out}`);
srv.stop();
