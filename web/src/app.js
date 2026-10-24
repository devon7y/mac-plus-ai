// Page side of the browser Mac Plus: downloads the emulator, ROM and disk, starts the worker as soon
// as they are in (sound waits for the first click or key), draws the screen (flat view) or hands it
// to the 3D view, forwards mouse and keyboard, plays sound, runs the buttons and panels.
// Shared-memory layout: keep in sync with worker.js.
import { APPS, keyList } from "./controls.js";
import { makeClickPlayer } from "./clicks.js";
import { makeSfx } from "./sfx.js";

const IN_WRITE = 0, IN_READ = 1, IN_DX = 2, IN_DY = 3, IN_AUDIO_RUNNING = 4, IN_SPEED_FLAG = 5,
      IN_SPEED = 6, IN_CMD = 7, IN_CMD_ARG0 = 8, IN_CMD_ARG1 = 9, IN_CMD_ID = 10, IN_PAUSED = 11;
const IN_DISK_SEQ = 12, IN_DISK_LEN = 13, IN_DISK_NAMELEN = 14, IN_DISK_ACK = 15;   // Insert Disk (see worker.js)
const IN_RING = 16, IN_RING_EVENTS = 256, IN_EVENT_WORDS = 4;
const INBOX_DATA = 256, INBOX_MAX = INBOX_DATA + 64 * 1048576;
const IN_WORDS = IN_RING + IN_RING_EVENTS * IN_EVENT_WORDS;
const EV_MOVE = 1, EV_DOWN = 2, EV_UP = 3, EV_KEYDOWN = 4, EV_KEYUP = 5;
const CMD_PEEK = 1, CMD_DUMP_DISK = 2, CMD_POKE32 = 3;
const AUDIO_RING = 32768, AU_HEADER_BYTES = 16;
const SCREEN_W = 512, SCREEN_H = 342;

// Browser KeyboardEvent.code -> Mini vMac key code (Mac virtual key codes; Infinite Mac's
// key-codes.ts with its Mini vMac overrides). The Plus has no Control key: Control acts as
// Command, so Ctrl-Q quits a Mac app without tripping the browser's own Cmd-Q.
const KEYS = {
  KeyA: 0x00, KeyS: 0x01, KeyD: 0x02, KeyF: 0x03, KeyH: 0x04, KeyG: 0x05, KeyZ: 0x06, KeyX: 0x07,
  KeyC: 0x08, KeyV: 0x09, KeyB: 0x0b, KeyQ: 0x0c, KeyW: 0x0d, KeyE: 0x0e, KeyR: 0x0f, KeyY: 0x10,
  KeyT: 0x11, Digit1: 0x12, Digit2: 0x13, Digit3: 0x14, Digit4: 0x15, Digit6: 0x16, Digit5: 0x17,
  Equal: 0x18, Digit9: 0x19, Digit7: 0x1a, Minus: 0x1b, Digit8: 0x1c, Digit0: 0x1d,
  BracketRight: 0x1e, KeyO: 0x1f, KeyU: 0x20, BracketLeft: 0x21, KeyI: 0x22, KeyP: 0x23,
  KeyL: 0x25, KeyJ: 0x26, Quote: 0x27, KeyK: 0x28, Semicolon: 0x29, Backslash: 0x2a, Comma: 0x2b,
  Slash: 0x2c, KeyN: 0x2d, KeyM: 0x2e, Period: 0x2f, Backquote: 0x32, IntlBackslash: 0x32,
  NumpadDecimal: 0x41, NumpadMultiply: 0x43, NumpadAdd: 0x45, NumLock: 0x47, NumpadDivide: 0x4b,
  NumpadEnter: 0x4c, NumpadSubtract: 0x4e, NumpadEqual: 0x51, Numpad0: 0x52, Numpad1: 0x53,
  Numpad2: 0x54, Numpad3: 0x55, Numpad4: 0x56, Numpad5: 0x57, Numpad6: 0x58, Numpad7: 0x59,
  Numpad8: 0x5b, Numpad9: 0x5c, Enter: 0x24, Tab: 0x30, Space: 0x31, Backspace: 0x33,
  Escape: 0x35, MetaLeft: 0x37, MetaRight: 0x37, OSLeft: 0x37, OSRight: 0x37,
  ControlLeft: 0x37, ControlRight: 0x37, ShiftLeft: 0x38, ShiftRight: 0x38, CapsLock: 0x39,
  AltLeft: 0x3a, AltRight: 0x3a, ArrowLeft: 0x7b, ArrowRight: 0x7c, ArrowDown: 0x7d, ArrowUp: 0x7e,
};

const CONFIG = JSON.parse(document.getElementById("mac-config").textContent);
const $ = (id) => document.getElementById(id);
const canvas = $("screen");
const ctx = canvas.getContext("2d", { alpha: false, willReadFrequently: true });   // CPU canvas: cheap texture uploads

const state = {
  assets: null, pristineDisk: null, worker: null, input: null, audioBuffer: null,
  audioCtx: null, gain: null, analyser: null, clicks: null, clickAnalyser: null,
  started: false, wantStart: false,
  pressed: new Map(),      // real keys: code -> mac key code currently down
  clickKeys: new Set(),    // Mac keys held by clicks on the 3D keyboard (incl. latched modifiers)
  stats: null, logs: [], cmdId: 0, pending: new Map(), fracX: 0, fracY: 0,
  speed: 2, muted: false, mode: null, view3d: null, panel: null,
};
window.__mac = state;      // for the tests

// What the Mac is doing, for tests (window.__mac.statusText); it isn't shown on the page. Errors are
// shown (see fail()).
function setStatus(text) { state.statusText = text; }

// The floating buttons: a small line icon and a label (the label hides on phones; it stays the
// button's accessible name and tooltip).
const ICONS = {
  start: '<path d="M5.5 3.2v9.6l7.6-4.8z" fill="currentColor" stroke="none"/>',
  restart: '<path d="M3.2 8a4.8 4.8 0 1 0 1.5-3.5"/><path d="M4.3 1.9v2.8h2.8"/>',
  sound: '<path d="M2.4 6h2.4l3.4-2.8v9.6L4.8 10H2.4z" fill="currentColor"/><path d="M10.8 5.6a3.4 3.4 0 0 1 0 4.8M12.6 3.9a5.9 5.9 0 0 1 0 8.2"/>',
  mute: '<path d="M2.4 6h2.4l3.4-2.8v9.6L4.8 10H2.4z" fill="currentColor"/><path d="M10.8 6.1l3.6 3.8M14.4 6.1l-3.6 3.8"/>',
  speed: '<path d="M2.3 11.2a5.7 5.7 0 1 1 11.4 0"/><path d="M8 11.2l3.1-3.6"/>',
  reset: '<circle cx="8" cy="8" r="2.1"/><path d="M8 1.6v2.6M8 11.8v2.6M1.6 8h2.6M11.8 8h2.6"/>',
  full: '<path d="M2.4 6V2.4H6M10 2.4h3.6V6M13.6 10v3.6H10M6 13.6H2.4V10"/>',
  unfull: '<path d="M6 2.4V6H2.4M13.6 6H10V2.4M10 13.6V10h3.6M2.4 10H6v3.6"/>',
  lock: '<rect x="4.3" y="1.8" width="7.4" height="12.4" rx="3.7"/><path d="M8 1.8v4.3"/>',
  menu: '<path d="M2.5 4.2h11M2.5 8h11M2.5 11.8h11"/>',
  disk: '<path d="M2.6 2.4h8.6l2.2 2.2v9H2.6z"/><path d="M5 2.4v3.4h5.4V2.4M4.8 13.6V9.4h6.4v4.2"/>',
};
const svg = (name) => `<svg viewBox="0 0 16 16" aria-hidden="true">${ICONS[name]}</svg>`;
function dress(id, icon, label, title = label) {
  const b = $(id);
  b.innerHTML = `${svg(icon)}<span class="lbl">${label}</span>`;
  b.classList.add("ib");
  b.title = title; b.setAttribute("aria-label", label);
}
function prefGet(k) { try { return localStorage.getItem(k); } catch { return null; } }
function prefSet(k, v) { try { localStorage.setItem(k, v); } catch { /* private mode */ } }

// ------------------------------------------------------------------ downloads
async function fetchBytes(url, expected, onProgress) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
  if (!res.body || !res.body.getReader) {
    const buf = await res.arrayBuffer(); onProgress(buf.byteLength); return buf;
  }
  const reader = res.body.getReader();
  const chunks = []; let got = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value); got += value.length; onProgress(Math.min(got, expected));
  }
  const out = new Uint8Array(got); let o = 0;
  for (const c of chunks) { out.set(c, o); o += c.length; }
  return out.buffer;
}

async function gunzip(buf) {
  const b = new Uint8Array(buf);
  if (b[0] !== 0x1f || b[1] !== 0x8b) return buf;          // already decoded by the server
  if (typeof DecompressionStream === "undefined") throw new Error("This browser cannot unpack the disk (no DecompressionStream).");
  return await new Response(new Blob([b]).stream().pipeThrough(new DecompressionStream("gzip"))).arrayBuffer();
}

async function loadAssets() {
  const parts = [["wasm", CONFIG.wasm, CONFIG.wasmSize], ["rom", CONFIG.rom, CONFIG.romSize], ["disk", CONFIG.disk, CONFIG.diskGzSize]];
  const total = parts.reduce((s, p) => s + p[2], 0);
  const got = {};
  const t0 = performance.now();
  const update = () => {
    const sum = Object.values(got).reduce((s, v) => s + v, 0);
    state.loadProgress = sum / total;
  };
  const results = await Promise.all(parts.map(([key, url, size]) => fetchBytes(url, size, (n) => { got[key] = n; update(); })));
  const disk = await gunzip(results[2]);
  if (disk.byteLength !== CONFIG.diskSize) throw new Error(`disk is ${disk.byteLength} bytes, expected ${CONFIG.diskSize}`);
  state.loadMs = performance.now() - t0;
  state.assets = { wasm: results[0], rom: results[1] };
  state.pristineDisk = new Uint8Array(disk);
  state.assetsReady = true;
  if (state.wantStart) startMac();
}

// ------------------------------------------------------------------ audio
function makeAudioContext() {
  if (state.audioCtx || typeof AudioContext === "undefined") return;
  try {
    const ac = new AudioContext({ latencyHint: "interactive" });
    state.audioCtx = ac;
    ac.resume().catch(() => {});
    state.gain = ac.createGain();                     // master: Sound on/off
    state.gain.gain.value = state.muted ? 0 : 1;
    // The view: louder straight in front and close up, softer from the side or far away, with a
    // touch of stereo as the view turns (applied after everything, so after the two sliders too).
    state.spatial = ac.createGain();
    state.pan = ac.createStereoPanner ? ac.createStereoPanner() : null;
    // A limiter last: close up the view adds +2 dB, which must never clip.
    state.limiter = ac.createDynamicsCompressor();
    state.limiter.threshold.value = -1; state.limiter.knee.value = 0; state.limiter.ratio.value = 20;
    state.limiter.attack.value = 0.003; state.limiter.release.value = 0.12;
    state.gain.connect(state.spatial);
    if (state.pan) { state.spatial.connect(state.pan); state.pan.connect(state.limiter); } else state.spatial.connect(state.limiter);
    // Last of all, the embedding page's fade (see setAudible).
    state.embedGain = ac.createGain(); state.embedGain.gain.value = state.audible ? 1 : 0;
    state.limiter.connect(state.embedGain); state.embedGain.connect(ac.destination);
    if (state.lastView) spatialize(...state.lastView, true);
    // Two volumes (Help > Settings): the hardware (hum, power, floppy, 3D key and mouse clicks) and
    // the software (the emulated Mac's own sound).
    state.hwGain = ac.createGain(); state.hwGain.gain.value = state.volHw; state.hwGain.connect(state.gain);
    state.swGain = ac.createGain(); state.swGain.gain.value = state.volSw; state.swGain.connect(state.gain);
    state.clickAnalyser = ac.createAnalyser(); state.clickAnalyser.fftSize = 2048;
    if (CONFIG.sfx) state.sfx = makeSfx(ac, state.hwGain, CONFIG.sfx);   // the machine's own sounds
    if (CONFIG.clicks) {                               // recorded clicks (none: no click sounds)
      state.clicks = makeClickPlayer(ac, state.hwGain, CONFIG.clicks);
      state.clicks.bus.connect(state.clickAnalyser);
    }
  } catch (err) {
    state.logs.push("AudioContext failed: " + err);
  }
}

async function openAudio(sampleRate) {
  const ac = state.audioCtx;
  if (!ac || !ac.audioWorklet) { state.logs.push("no AudioWorklet: no sound"); return; }
  try {
    await ac.audioWorklet.addModule(CONFIG.worklet);
    const node = new AudioWorkletNode(ac, "mac-audio", {
      numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [1],
      processorOptions: { buffer: state.audioBuffer, srcRate: sampleRate },
    });
    if (state.audioNode) state.audioNode.disconnect();
    state.audioNode = node;
    node.port.onmessage = (ev) => { state.audioSkips = ev.data.skips; state.audioQueue = ev.data.avail; };
    state.analyser = ac.createAnalyser();
    state.analyser.fftSize = 2048;
    node.connect(state.swGain || state.gain);
    node.connect(state.analyser);
    const running = () => Atomics.store(state.input, IN_AUDIO_RUNNING, ac.state === "running" ? 1 : 0);
    ac.addEventListener("statechange", running);
    running();
    if (ac.state !== "running") ac.resume().then(running).catch(() => {});
  } catch (err) {
    state.logs.push("audio failed: " + err);
  }
}

// Sound and the 3D view: direction (the angle between the view and the Mac's front: full in front,
// about 58% from the side, 30% from behind) times distance (0 dB at the normal working zoom, +2 dB
// at the closest, -5 dB fully out, eased between), and a little panning; changes glide (80 ms) so
// turning never crackles. A limiter after it keeps the +2 dB from clipping.
function spatialize(yaw, pitch, zoom, now = false) {
  state.lastView = [yaw, pitch, zoom];
  if (!state.spatial) return;
  const cosA = Math.cos(yaw) * Math.cos(pitch);
  const dir = 0.3 + 0.7 * Math.pow((1 + cosA) / 2, 1.3);
  const z = Math.max(0, Math.min(1, zoom));
  const zd = state.view3d ? Math.max(0.05, Math.min(0.95, state.view3d.zoomForScreen(innerWidth < innerHeight && innerWidth < 700 ? 0.92 : 0.62))) : 0.8;
  const ease = (t) => t * t * (3 - 2 * t);
  const db = z >= zd ? 2 * ease((z - zd) / (1 - zd)) : -5 * ease((zd - z) / zd);
  const dist = Math.pow(10, db / 20);
  const g = dir * dist, pan = Math.max(-0.3, Math.min(0.3, -0.3 * Math.sin(yaw)));
  if (!now && Math.abs(g - (state.spatialG ?? -1)) < 0.004 && Math.abs(pan - (state.spatialP ?? -9)) < 0.01) return;
  state.spatialG = g; state.spatialP = pan;
  const t = state.audioCtx.currentTime;
  state.spatial.gain.setTargetAtTime(g, t, now ? 0.005 : 0.08);
  if (state.pan) state.pan.pan.setTargetAtTime(pan, t, now ? 0.005 : 0.08);
}

// Embedded in a page (devon7y.com shows the Mac in a panel), the page says whether the panel is on
// screen: the sound fades out over about a second while it isn't, and back in when it returns.
const EMBEDDERS = /^(https:\/\/(www\.)?devon7y\.com|http:\/\/localhost(:\d+)?)$/;
state.audible = true;
function setAudible(on) {
  state.audible = on;
  if (!state.embedGain) return;
  state.embedGain.gain.setTargetAtTime(on ? 1 : 0, state.audioCtx.currentTime, on ? 0.15 : 0.35);
}
window.addEventListener("message", (ev) => {
  if (ev.source !== window.parent || !EMBEDDERS.test(ev.origin)) return;
  const d = ev.data;
  if (d && d.macplus === "audible" && typeof d.on === "boolean") setAudible(d.on);
});

// Root-mean-square levels for the tests: the Mac's sound, and the 3D click sounds.
const rms = (an) => { if (!an) return -1; const a = new Float32Array(an.fftSize); an.getFloatTimeDomainData(a); let s = 0; for (const v of a) s += v * v; return Math.sqrt(s / a.length); };
state.audioLevel = () => rms(state.analyser);
state.clickLevel = () => rms(state.clickAnalyser);

// Key and mouse-button sounds (Help > Settings > Keyboard and mouse sounds, on by default): for the
// real keyboard and mouse as well as the 3D ones, once audio is unlocked; through the hardware bus.
state.clickSounds = prefGet("macplus.clickSounds") !== "0";
// The real keys' names as the click player knows them (Space, Return, Enter and Shift sound bigger).
const keySoundName = (code) => (code === "Space" ? "Space" : code === "Enter" ? "Return" : code === "NumpadEnter" ? "Enter" : /^Shift/.test(code) ? "Shift" : code);
function clickSound(kind, name) {
  const c = state.clicks;
  if (!state.clickSounds || !c || !state.audioCtx || state.audioCtx.state !== "running") return;
  if (kind === "keydown") c.keyDown(name); else if (kind === "keyup") c.keyUp(name);
  else if (kind === "mousedown") c.mouseDown(); else if (kind === "mouseup") c.mouseUp();
  state.clickCount = (state.clickCount || 0) + 1;
}

// ------------------------------------------------------------------ emulator
function startMac() {
  if (state.started) return;
  if (!state.assets) { state.wantStart = true; setStatus("Starting as soon as it has loaded…"); return; }
  state.started = true;
  document.body.classList.add("running");
  if (state.sfx && state.sfxOn && !state.restarting) state.sfx.powerOn();
  state.restarting = false;
  glideIn();
  const inputBuffer = new SharedArrayBuffer(IN_WORDS * 4);
  state.input = new Int32Array(inputBuffer);
  state.audioBuffer = new SharedArrayBuffer(AU_HEADER_BYTES + AUDIO_RING);
  // Speed: Mini vMac's values 0 = 1x, 1 = 2x, 2 = 4x, 3 = 8x (its default). "Fast" is 4x: at 8x the
  // Plus keyboard protocol drops and repeats keys in Pixel (tested: "hellooo pixeel"; clean at 4x).
  state.input[IN_SPEED] = state.speed; state.input[IN_SPEED_FLAG] = 1; state.appliedSpeed = state.speed;
  const worker = new Worker(CONFIG.worker, { type: "module" });
  state.worker = worker;
  worker.onmessage = onWorkerMessage;
  worker.onerror = (e) => fail("The emulator crashed: " + (e.message || e));
  const disk = state.pristineDisk.slice().buffer;
  // The disk inbox for Insert Disk: a growable shared buffer (nothing is reserved until a disk comes),
  // or a fixed 64 MB one where growing isn't supported (its pages stay untouched until used).
  try { state.inbox = new SharedArrayBuffer(INBOX_DATA, { maxByteLength: INBOX_MAX }); if (!state.inbox.growable) throw 0; }
  catch { state.inbox = new SharedArrayBuffer(INBOX_MAX); }
  state.disks = state.disks || [];
  state.workerGen = (state.workerGen || 0) + 1;
  state.sentDisks = [];
  worker.postMessage({
    type: "start", coreUrl: new URL(CONFIG.core, location.href).href, wasm: state.assets.wasm,
    rom: state.assets.rom, disk, inputBuffer, audioBuffer: state.audioBuffer, inbox: state.inbox,
  }, [disk]);
  state.startedAt = performance.now();
  if (state.view3d) state.view3d.setStarted(true);
  setStatus("Starting up…");
}

async function restartMac() {
  if (!state.worker) return;
  // The floppies don't survive a restart: keep each inserted disk's current contents (what the app
  // saved on it, read from the emulator before it stops), show it as ejected, offer Re-insert.
  for (const d of state.disks || []) {
    if (d.ejected || d.gen !== state.workerGen) continue;
    try { d.image = await Promise.race([state.dumpDisk(d.id), new Promise((_, no) => setTimeout(() => no(new Error("timeout")), 1500))]); } catch { /* keep the original */ }
    d.ejected = true; d.byRestart = true;
  }
  renderDisks();
  if (state.sfx && state.sfxOn) state.sfx.restart();
  state.restarting = true;
  if (state.view3d) state.view3d.floppyReset();
  releaseAllKeys();
  state.worker.terminate();
  state.worker = null; state.started = false; state.stats = null;
  ctx.fillStyle = "#000"; ctx.fillRect(0, 0, SCREEN_W, SCREEN_H);
  startMac();                       // the new worker announces its audio again (audio_open)
}

function onWorkerMessage(e) {
  const m = e.data;
  switch (m.type) {
    case "blit":
      ctx.putImageData(new ImageData(m.data, m.w, m.h), m.x, m.y);
      if (state.view3d) state.view3d.screenChanged();
      break;
    case "stats": {
      const prev = state.stats;
      state.stats = m;
      applySpeed();
      if (!prev || prev.app !== m.app) { updateMouseMode(); if (state.panel === "help") renderHelp(); }
      if (prev && m.ticks && prev.ticks) state.tickRate = (m.ticks - prev.ticks) / ((m.t - prev.t) / 1000);
      const app = m.app || "starting up";
      const rate = state.tickRate ? ` · ${(state.tickRate / 60.15 * 100).toFixed(0)}% of real time` : "";
      const real = REAL_SPEED_APPS.has(m.app) && state.speed !== 0 ? " · at real Plus speed" : "";
      setStatus(`Running: ${app}${real}${rate}`);
      break;
    }
    case "audio_open":                              // (the AudioContext comes with the first gesture)
      state.audioRate = m.sampleRate; state.audioNode = null; state.audioOpening = false;
      if (state.audioCtx) { state.audioOpening = true; openAudio(m.sampleRate); }
      break;
    case "disk_inserted": {
      const sent = (state.sentDisks || []).shift();
      if (sent && sent.replaces) state.disks = state.disks.filter((x) => x !== sent.replaces);
      state.disks.push({ id: m.id, name: m.name, size: m.size, ejected: false, gen: state.workerGen, original: sent ? sent.data : null, image: null });
      toast(`Inserted ${m.name}`); renderDisks();
      // The 3D disk slides into the drive (the first one in; extra drives aren't shown), in time
      // with the drive's sound.
      // (the slide and its sound started when the image was sent: sendDisk; here it gets its number)
      if (state.view3d && sent && sent.animated) state.view3d.floppySetId(m.id);
      break;
    }
    case "disk_ejected": {
      const d = state.disks.find((x) => x.id === m.id && x.gen === state.workerGen);
      if (d) { d.ejected = true; renderDisks(); }
      if (state.view3d && state.view3d.floppyOut(m.id) && state.sfx) state.sfx.eject();
      break;
    }
    case "peek": case "disk": {
      const p = state.pending.get(m.id);
      if (p) { state.pending.delete(m.id); p(m.type === "peek" ? m.bytes : m.data); }
      break;
    }
    case "error": fail(m.text); break;
    case "log": state.logs.push(m.text); if (state.logs.length > 400) state.logs.shift(); break;
  }
}

function fail(text) {
  state.logs.push("ERROR " + text);
  setStatus("Error: " + text);
  $("error").textContent = text;
  $("error").hidden = false;
}

// Commands to the worker (it can't receive messages while the Mac runs): one at a time.
function command(cmd, a0 = 0, a1 = 0) {
  return new Promise((resolve, reject) => {
    if (!state.input) return reject(new Error("not running"));
    const tryIt = () => {
      if (Atomics.load(state.input, IN_CMD)) return setTimeout(tryIt, 10);
      const id = ++state.cmdId;
      state.pending.set(id, resolve);
      state.input[IN_CMD_ARG0] = a0; state.input[IN_CMD_ARG1] = a1; state.input[IN_CMD_ID] = id;
      Atomics.store(state.input, IN_CMD, cmd);
    };
    tryIt();
  });
}
state.peek = (addr, len) => command(CMD_PEEK, addr, len);
state.poke32 = (addr, value) => command(CMD_POKE32, addr, value | 0);   // tests: stage a scene
state.dumpDisk = (id = 0) => command(CMD_DUMP_DISK, id);

// ------------------------------------------------------------------ Insert Disk
// A disk image from the file picker, a drop or the 3D floppy slot: checked and unpacked in the page
// (disks.js), then handed to the worker through the shared inbox; Mini vMac mounts it like a
// floppy and the Mac shows it on the desktop. Several can be in at once (Mini vMac's drives).
async function insertDiskFile(file) {
  const errBox = $("disk-error");
  errBox.hidden = true;
  if (!state.started || !state.input) { showDiskError("The Mac is still starting. Try again in a moment."); return; }
  let list;
  try {
    const { disksFromFile } = await import(CONFIG.disks);
    list = await disksFromFile(file, { fflateUrl: CONFIG.fflate, wasmUrl: CONFIG.sitWasm, progress: (t) => toast(t) });
  } catch (e) { showDiskError(e.message || String(e)); return; }
  for (const d of list) if (!(await sendDisk(d))) return;
  if (list.length > 1) toast(`Inserted ${list.length} disks from ${file.name}`);
}
async function sendDisk(d) {
  // Wait for the worker to take the previous disk, then write this one.
  const t0 = performance.now();
  while (Atomics.load(state.input, IN_DISK_SEQ) !== Atomics.load(state.input, IN_DISK_ACK)) {
    if (performance.now() - t0 > 5000) { showDiskError("The Mac is busy. Try again in a moment."); return false; }
    await new Promise((r) => setTimeout(r, 50));
  }
  const name = new TextEncoder().encode(d.name).slice(0, 255);
  const need = INBOX_DATA + d.data.length;
  if (state.inbox.growable && state.inbox.byteLength < need) state.inbox.grow(need);
  const box = new Uint8Array(state.inbox);
  box.set(name, 0);
  box.set(d.data, INBOX_DATA);
  state.input[IN_DISK_LEN] = d.data.length; state.input[IN_DISK_NAMELEN] = name.length;
  // The 3D disk slides in, with the drive's sound, the moment the image goes to the emulator (only
  // the first disk in the drive is shown; the drive sounds play only for it).
  d.animated = !!(state.view3d && state.view3d.floppyIn(-1, d.name));
  if (d.animated && state.sfx) state.sfx.insert();
  state.sentDisks.push(d);
  Atomics.add(state.input, IN_DISK_SEQ, 1);
  state.logs.push(`insert disk: ${d.name} (${d.format}, ${d.data.length} bytes)`);
  return true;
}
function showDiskError(text) {
  const errBox = $("disk-error");
  errBox.textContent = text; errBox.hidden = false;
  if (state.panel !== "disk") { openPanel("disk"); }
}
function chooseDisk() { const i = $("disk-file"); i.value = ""; i.click(); }
state.chooseDisk = chooseDisk; state.insertDiskFile = insertDiskFile;
function renderDisks() {
  // The drives as they are: disks in the emulator now, and disks ejected (by the Mac or by a
  // restart) whose contents the page still has: Download them, or put them back in.
  const list = $("disk-list");
  list.textContent = "";
  const live = (d) => !d.ejected && d.gen === state.workerGen;
  for (const d of state.disks || []) {
    const row = document.createElement("li"), label = document.createElement("span"), btns = document.createElement("span");
    label.textContent = `${d.name}${live(d) ? "" : " (ejected)"}`;
    const mk = (text, fn) => { const b = document.createElement("button"); b.type = "button"; b.className = "fbtn small"; b.textContent = text; b.addEventListener("click", fn); btns.append(b); };
    const contents = async () => (d.gen === state.workerGen ? await state.dumpDisk(d.id) : d.image || d.original);
    mk("Download disk", async () => {
      const data = await contents();
      if (!data) return;
      const a = document.createElement("a");
      a.href = URL.createObjectURL(new Blob([data], { type: "application/octet-stream" }));
      a.download = d.name.replace(/(\.(zip|image|img|hfv|dsk|dc42|diskcopy|sit|sea|hqx|bin))+$/i, "") + ".dsk";
      document.body.append(a); a.click(); a.remove();
      setTimeout(() => URL.revokeObjectURL(a.href), 10000);
    });
    if (!live(d)) mk("Re-insert", async () => {
      const data = await contents();
      if (data && state.started) sendDisk({ name: d.name, data: data.slice(), format: "re-inserted", replaces: d });
    });
    btns.className = "disk-btns";
    row.append(label, btns); list.append(row);
  }
  $("disk-inserted").hidden = !(state.disks && state.disks.length);
  $("disk-inserted-title").textContent = (state.disks || []).some(live) ? "On the Mac now" : "Your disks";
}

// ------------------------------------------------------------------ speed
// Mini vMac's AutoSlow drops the emulator to 1x whenever the screen, disk and input have been
// quiet for about half a second, which is exactly when Blockworld builds a world, Pixel thinks,
// Dream paints and Doodle finishes a sketch (measured: Blockworld's first world took 62 s with
// AutoSlow, 15 s without). This build has no control mode to switch it off, so while an app is
// running at Fast speed we tap key code 0x3B (Control: not on the Plus keyboard, a modifier to
// System 6, so it types nothing; checked in TeachText) four times a second, and every key event
// ends Mini vMac's "quiet" time. Skipped while keys are held and in the Finder.
state.keepFast = true;
const keysHeld = () => state.pressed.size + state.clickKeys.size;
setInterval(() => applySpeed(), 250);
setInterval(() => {
  if (!state.keepFast || !state.started || state.paused || effectiveSpeed() === 0 || keysHeld()) return;
  const app = state.stats && state.stats.app;
  if (!app || app === "Finder") return;
  pushEvent(EV_KEYDOWN, 0x3b);
  pushEvent(EV_KEYUP, 0x3b);
}, 250);

// Apps that pace themselves by CPU speed rather than the clock run at the real Plus's speed:
// Glider 4.06 plays three to four times too fast at 4x (the glider fell in 1.3 s instead of 4 s).
const REAL_SPEED_APPS = new Set(["Glider 4.06"]);
// Apps where people type text run at real speed while they type (and 1.5 s after the last key),
// then go back to Fast for the thinking. Above 1x the Plus keyboard path can deliver a key's
// release late, and the Mac then auto-repeats the key: the redesigned Pixel showed
// "pixxxxxxxxxxel" at 4x and 2x, and types cleanly at 1x (tested 2026-10-20). Games keep Fast.
const TYPE_AT_REAL_SPEED = new Set(["Pixel", "Dream", "Doodle", "TeachText", "Finder"]);
function effectiveSpeed() {
  const app = state.stats && state.stats.app;
  if (REAL_SPEED_APPS.has(app)) return 0;
  if (TYPE_AT_REAL_SPEED.has(app) && (keysHeld() || performance.now() - (state.lastKeyAt || -1e9) < 1500)) return 0;
  return state.speed;
}
function applySpeed() {
  if (!state.input) return;
  const want = effectiveSpeed();
  if (want === state.appliedSpeed) return;
  state.appliedSpeed = want;
  Atomics.store(state.input, IN_SPEED, want); Atomics.store(state.input, IN_SPEED_FLAG, 1);
}

// ------------------------------------------------------------------ input to the Mac
function pushEvent(type, a = 0, b = 0) {
  const inp = state.input;
  if (!inp) return;
  const w = Atomics.load(inp, IN_WRITE) >>> 0, r = Atomics.load(inp, IN_READ) >>> 0;
  if (w - r >= IN_RING_EVENTS - 1) {
    if (type === EV_MOVE) return;                    // full: drop moves, never clicks or keys
    setTimeout(() => pushEvent(type, a, b), 5);
    return;
  }
  const base = IN_RING + (w % IN_RING_EVENTS) * IN_EVENT_WORDS;
  inp[base] = type; inp[base + 1] = a; inp[base + 2] = b;
  Atomics.store(inp, IN_WRITE, (w + 1) | 0);
}
state.pushEvent = pushEvent;

// The Mac's mouse and keyboard, as the 3D view and the flat screen use them.
const macApi = {
  move(x, y) { if (state.started) pushEvent(EV_MOVE, x, y); },
  down() { if (!state.started) return; pushEvent(EV_DOWN); state.macButtonDown = true; },
  up() { if (!state.started) return; if (state.macButtonDown) pushEvent(EV_UP); state.macButtonDown = false; },
  key(code, down) {                                  // clicks on the 3D keyboard
    if (!state.started) return;
    state.lastKeyAt = performance.now();
    if (down) { state.clickKeys.add(code); applySpeed(); } else state.clickKeys.delete(code);
    pushEvent(down ? EV_KEYDOWN : EV_KEYUP, code);
  },
  // Programs that read the mouse relatively (Blockworld re-centres the cursor every frame and turns
  // by how far it moved): they get only movement, never absolute positions (an absolute position
  // would read as "pointer minus centre" on every move: a huge, uncontrollable turn).
  relative: () => !!state.relativeMouse,
  // Mouse look: host movement (CSS px) to Mac pixels. In Blockworld by the sensitivity setting
  // (default 0.4: 64 px of mouse movement turn about 18°; the turn depends only on the total
  // movement, so it feels the same at 1× and 4×); otherwise 1:1 with the picture (scale).
  look(dx, dy, scale = 1) {
    if (!state.started) return;
    const k = state.relativeMouse ? state.lookSens : scale;
    macApi.delta(dx * k, dy * k);
  },
  // The first click on the screen in Blockworld captures the mouse (the browser needs a click).
  capture(target) {
    if (!state.started || document.pointerLockElement || state.lockFailed) return false;   // (no lock here: plain clicks)
    requestLock(target);
    return true;
  },
  delta(dx, dy) {                                    // relative motion (locked pointer, sliding the 3D mouse)
    if (!state.input) return;
    state.fracX += dx; state.fracY += dy;
    const ix = Math.trunc(state.fracX), iy = Math.trunc(state.fracY);
    state.fracX -= ix; state.fracY -= iy;
    if (ix) Atomics.add(state.input, IN_DX, ix);
    if (iy) Atomics.add(state.input, IN_DY, iy);
  },
};
state.macApi = macApi;

// The flat view's screen: the canvas itself takes the mouse.
function macPoint(ev) {
  const r = canvas.getBoundingClientRect();
  const x = Math.floor(((ev.clientX - r.left) / r.width) * SCREEN_W);
  const y = Math.floor(((ev.clientY - r.top) / r.height) * SCREEN_H);
  return [Math.max(0, Math.min(SCREEN_W - 1, x)), Math.max(0, Math.min(SCREEN_H - 1, y))];
}
const flatLocked = () => document.pointerLockElement === canvas;
let flatLast = null;
canvas.addEventListener("pointerleave", () => { flatLast = null; });
canvas.addEventListener("pointermove", (ev) => {
  if (!state.started || state.ignorePointer) return;
  const mx = flatLast ? ev.clientX - flatLast[0] : 0, my = flatLast ? ev.clientY - flatLast[1] : 0;
  flatLast = [ev.clientX, ev.clientY];
  if (flatLocked() || state.relativeMouse) {
    const scale = canvas.getBoundingClientRect().width / SCREEN_W;
    if (flatLocked()) macApi.look(ev.movementX, ev.movementY, 1 / scale);
    else macApi.look(mx, my, 1 / scale);                 // (movementX isn't filled in everywhere)
    return;
  }
  const evs = ev.getCoalescedEvents ? ev.getCoalescedEvents() : [];
  const [x, y] = macPoint(evs.length ? evs[evs.length - 1] : ev);
  macApi.move(x, y);
});
canvas.addEventListener("pointerdown", (ev) => {
  if (!state.started) return;
  ev.preventDefault();
  if (ev.button !== 0) return;
  if (state.relativeMouse && !flatLocked() && macApi.capture(canvas)) return;
  try { canvas.setPointerCapture(ev.pointerId); } catch { /* ignore */ }
  if (!flatLocked() && !state.relativeMouse) macApi.move(...macPoint(ev));
  clickSound("mousedown");
  macApi.down();
});
const flatUp = (ev) => { if (state.started && ev.button === 0 && state.macButtonDown) { macApi.up(); clickSound("mouseup"); } };
canvas.addEventListener("pointerup", flatUp);
canvas.addEventListener("pointercancel", flatUp);
canvas.addEventListener("contextmenu", (ev) => ev.preventDefault());

// Keys always go to the Mac while it runs, except while a panel is open (Esc closes it) and in
// the page's own form controls.
function keyEvent(ev, down) {
  if (down && state.view3d) state.view3d.stopDolly();
  if (state.panel) { if (down && ev.code === "Escape") { ev.preventDefault(); closePanel(); } return; }
  if (state.menuOpen && down && ev.code === "Escape") { ev.preventDefault(); setMenu(false); return; }
  if (!state.started) return;
  const t = ev.target;
  if (t && (t.tagName === "INPUT" || t.tagName === "TEXTAREA" || t.tagName === "SELECT")) return;
  const code = KEYS[ev.code];
  if (code === undefined) return;
  if (ev.code === "Escape" && down) state.lastEscAt = performance.now();
  if (ev.code === "F11" || (ev.metaKey && ev.code === "KeyR")) return;   // the browser's full screen and reload
  ev.preventDefault();
  state.lastKeyAt = performance.now();
  if (down) {
    if (ev.repeat || state.pressed.has(ev.code)) return;   // the Mac repeats keys itself
    applySpeed();                    // typing apps drop to real speed before the key goes out
    state.pressed.set(ev.code, code);
    pushEvent(EV_KEYDOWN, code);
    clickSound("keydown", keySoundName(ev.code));
    if (state.view3d) state.view3d.key(ev.code, true);
  } else {
    if (!state.pressed.has(ev.code)) return;
    state.pressed.delete(ev.code);
    pushEvent(EV_KEYUP, code);
    clickSound("keyup", keySoundName(ev.code));
    if (state.view3d) state.view3d.key(ev.code, false);
  }
}
function releaseAllKeys() {
  for (const code of state.pressed.values()) pushEvent(EV_KEYUP, code);
  state.pressed.clear();
  if (state.view3d) state.view3d.releaseKeys();
}
window.addEventListener("keydown", (ev) => keyEvent(ev, true));
window.addEventListener("keyup", (ev) => keyEvent(ev, false));
window.addEventListener("blur", releaseAllKeys);
document.addEventListener("visibilitychange", () => {
  if (document.hidden) releaseAllKeys();
  if (!state.input) return;
  state.paused = document.hidden;                    // pause the Mac while the page is hidden
  Atomics.store(state.input, IN_PAUSED, document.hidden ? 1 : 0);
  if (!document.hidden) Atomics.notify(state.input, IN_PAUSED);
});

// ------------------------------------------------------------------ bottom bar
function isFullscreen() { return !!(document.fullscreenElement || document.webkitFullscreenElement); }
$("btn-full").addEventListener("click", async () => {
  const el = document.documentElement;
  try {
    if (isFullscreen()) { await (document.exitFullscreen ? document.exitFullscreen() : document.webkitExitFullscreen()); return; }
    if (el.requestFullscreen) await el.requestFullscreen({ navigationUI: "hide" });
    else if (el.webkitRequestFullscreen) el.webkitRequestFullscreen();
    // Chrome and Edge can pass Esc to the page in full screen (hold Esc to leave).
    if (navigator.keyboard && navigator.keyboard.lock) navigator.keyboard.lock(["Escape"]).catch(() => {});
  } catch (err) { state.logs.push("fullscreen: " + err); }
});
const onFsChange = () => {
  dress("btn-full", isFullscreen() ? "unfull" : "full", isFullscreen() ? "Exit full screen" : "Full screen");
  if (!isFullscreen() && navigator.keyboard && navigator.keyboard.unlock) navigator.keyboard.unlock();
};
document.addEventListener("fullscreenchange", onFsChange);
document.addEventListener("webkitfullscreenchange", onFsChange);

function requestLock(target) {
  const plain = () => {
    try { const q = target.requestPointerLock(); if (q && q.catch) q.catch((e) => { state.lockFailed = true; state.logs.push("pointer lock: " + e); }); }
    catch (e) { state.lockFailed = true; state.logs.push("pointer lock: " + e); }
  };
  try {
    const p = target.requestPointerLock({ unadjustedMovement: true });   // raw mouse where supported
    if (p && p.catch) p.catch(plain);
  } catch { plain(); }
}
$("btn-lock").addEventListener("click", () => {
  if (!state.started) return;
  requestLock(state.mode === "3d" && state.view3d ? state.view3d.renderer.domElement : canvas);
});
// A small transient note at the top.
let toastTimer = 0;
function toast(text) {
  const t = $("toast"); t.textContent = text; t.hidden = false;
  clearTimeout(toastTimer); toastTimer = setTimeout(() => { t.hidden = true; }, 2600);
}
// Esc while the mouse is captured: the browser takes it to give the mouse back. In Blockworld it
// also opens the game's menu, as Esc does on the Mac (sent here unless the key already reached the
// Mac). To carry on: Esc again (back to the game), then a click on the screen to capture again.
document.addEventListener("pointerlockchange", () => {
  const on = !!document.pointerLockElement;
  $("btn-lock").setAttribute("aria-pressed", on ? "true" : "false");
  if (on && state.relativeMouse) toast("Mouse captured. Esc releases it.");
  if (!on && state.relativeMouse && state.started && performance.now() - (state.lastEscAt || 0) > 400) {
    pushEvent(EV_KEYDOWN, 0x35);
    setTimeout(() => pushEvent(EV_KEYUP, 0x35), 80);
  }
});
document.addEventListener("pointerlockerror", () => { state.lockFailed = true; });
// Blockworld (and only Blockworld) gets relative mouse input; the others keep the absolute mouse.
const RELATIVE_MOUSE_APPS = new Set(["Blockworld"]);
function updateMouseMode() {
  const rel = !!(state.started && state.stats && RELATIVE_MOUSE_APPS.has(state.stats.app));
  if (rel === !!state.relativeMouse) return;
  state.relativeMouse = rel;
  if (!rel && document.pointerLockElement) document.exitPointerLock();
}
state.lookSens = Math.min(1.2, Math.max(0.1, Number(prefGet("macplus.lookSens")) || 0.4));

dress("btn-start", "restart", "Restart", "Restart the Mac");
dress("btn-reset", "reset", "Reset view", "Reset the view");
dress("btn-disk", "disk", "Insert Disk", "Insert a floppy disk image");
dress("btn-full", "full", "Full screen");
dress("btn-lock", "lock", "Lock mouse", "Lock the mouse (Blockworld's mouse look); Esc gives it back");
$("speed-ico").innerHTML = svg("speed");
const speedSel = $("speed");
state.speed = prefGet("macplus.speed") === "0" ? 0 : 2;
speedSel.value = String(state.speed);
// On phones the speed pill shows just the multiplier.
const narrowMQ = window.matchMedia("(max-width: 960px) and (min-width: 641px)");
const speedLabels = () => {
  speedSel.options[0].textContent = narrowMQ.matches ? "4×" : "Fast (4×)";
  speedSel.options[1].textContent = narrowMQ.matches ? "1×" : "Real Plus (1×)";
};
if (narrowMQ.addEventListener) narrowMQ.addEventListener("change", speedLabels);
speedLabels();
speedSel.addEventListener("change", () => {
  state.speed = Number(speedSel.value);
  prefSet("macplus.speed", String(state.speed));
  state.appliedSpeed = null;
  applySpeed();
  speedSel.blur();
});

state.muted = false;                                // (no Sound button: the browser's tab mute does that)


// The Mac starts by itself as soon as it has loaded. The 3D view opens head-on on the whole desk and
// dollies in until the screen is a comfortable size (on a portrait phone, nearly the full width).
function glideIn() {
  // The opening camera: head-on on the whole desk, then an eased dolly in (about 2.6 s) as soon as
  // the 3D view is there, alongside the boot.
  if (!state.view3d || state.glided) return;
  state.glided = true;
  const share = innerWidth < innerHeight && innerWidth < 700 ? 0.92 : 0.62;
  state.view3d.dollyIn(state.view3d.zoomForScreen(share));
}
// Sound: browsers allow audio only after a gesture, so the AudioContext is made (or resumed) silently
// on the first pointer press or key press anywhere on the page; the Mac's audio is hooked up then.
function unlockAudio() {
  makeAudioContext();
  const ac = state.audioCtx;
  if (!ac) return;
  if (ac.state !== "running") ac.resume().catch(() => {});
  // The machine's sounds: if the Mac already booted (before sound was allowed), its power-on is
  // skipped and the running hum just starts; if not, startMac() plays the power-on.
  if (state.sfx && !state.sfxOn) { state.sfxOn = true; if (state.started) state.sfx.ambience(); }
  if (state.audioRate && !state.audioNode && !state.audioOpening) { state.audioOpening = true; openAudio(state.audioRate); }
}
for (const t of ["pointerdown", "keydown", "touchend"]) document.addEventListener(t, unlockAudio, { capture: true });
$("btn-start").addEventListener("click", () => {
  if (state.started) restartMac();                   // at once (inserted disks are kept: Download / Re-insert)
});
$("btn-reset").addEventListener("click", () => {
  if (!state.view3d) return;
  state.view3d.resetView(innerWidth < innerHeight && innerWidth < 700 ? 0.92 : 0.62);
});

// ------------------------------------------------------------------ panels
function openPanel(name) {
  closePanel();
  const p = $(`panel-${name}`);
  if (!p) return;
  state.panel = name;
  if (name === "help") renderHelp();
  p.hidden = false; $("scrim").hidden = false;
  releaseAllKeys();
  p.querySelector(".close").focus();
}
function closePanel() {
  if (!state.panel) return;
  $(`panel-${state.panel}`).hidden = true; $("scrim").hidden = true;
  state.panel = null;
}
state.openPanel = openPanel; state.closePanel = closePanel;
for (const b of document.querySelectorAll("[data-panel]")) b.addEventListener("click", () => openPanel(b.dataset.panel));
for (const b of document.querySelectorAll(".panel .close")) b.addEventListener("click", closePanel);
$("scrim").addEventListener("click", closePanel);
// Insert Disk panel: the file picker, the copy button, drag and drop anywhere on the page.
$("disk-choose").addEventListener("click", chooseDisk);
$("disk-file").addEventListener("change", () => { const f = $("disk-file").files[0]; if (f) { closePanel(); insertDiskFile(f); } });
$("prompt-copy").addEventListener("click", async () => {
  const text = $("ai-prompt").textContent.trim();
  try { await navigator.clipboard.writeText(text); $("prompt-copy").textContent = "Copied"; }
  catch { const r = document.createRange(); r.selectNodeContents($("ai-prompt")); const sel = getSelection(); sel.removeAllRanges(); sel.addRange(r); $("prompt-copy").textContent = "Selected: press Ctrl C"; }
  setTimeout(() => { $("prompt-copy").textContent = "Copy"; }, 2000);
});
let dragDepth = 0;
const hasFiles = (ev) => ev.dataTransfer && Array.from(ev.dataTransfer.types || []).includes("Files");
window.addEventListener("dragenter", (ev) => { if (!hasFiles(ev)) return; ev.preventDefault(); dragDepth++; document.body.classList.add("dropping"); });
window.addEventListener("dragover", (ev) => { if (!hasFiles(ev)) return; ev.preventDefault(); ev.dataTransfer.dropEffect = "copy"; });
window.addEventListener("dragleave", () => { dragDepth = Math.max(0, dragDepth - 1); if (!dragDepth) document.body.classList.remove("dropping"); });
window.addEventListener("drop", (ev) => {
  if (!hasFiles(ev)) return;
  ev.preventDefault(); dragDepth = 0; document.body.classList.remove("dropping");
  const f = ev.dataTransfer.files[0];
  if (f) { closePanel(); insertDiskFile(f); }
});
// Toolbar buttons give the keyboard back to the Mac after a click.
for (const b of document.querySelectorAll(".topbar button")) b.addEventListener("click", () => b.blur());

// Phones: the top controls fold into one menu button.
const topbar = $("topbar"), menuBtn = $("btn-menu");
menuBtn.innerHTML = svg("menu");
function setMenu(open) {
  topbar.classList.toggle("open", open);
  menuBtn.setAttribute("aria-expanded", open ? "true" : "false");
  state.menuOpen = open;
}
menuBtn.addEventListener("click", (ev) => { ev.stopPropagation(); setMenu(!state.menuOpen); });
for (const b of document.querySelectorAll("#topitems button")) b.addEventListener("click", () => setMenu(false));
document.addEventListener("pointerdown", (ev) => { if (state.menuOpen && !topbar.contains(ev.target)) setMenu(false); });

// ------------------------------------------------------------------ keys in the Help panel
// Help opens on the keys of the program in front (CurApName); in the Finder, on the general tips.
// The other programs' keys follow, folded.
function renderHelp() {
  const app = state.started ? (state.stats && state.stats.app) || "Finder" : null;
  const a = app && APPS[app];
  const box = $("help-app");
  box.textContent = "";
  box.hidden = !a;
  if (a) {
    const h = document.createElement("h3"); h.textContent = a.title; box.append(h);
    if (a.hint) { const p = document.createElement("p"); p.className = "apphint"; p.textContent = a.hint; box.append(p); }
    box.append(keyList(a.rows));
  }
  $("help-tips").open = !a;
  const others = $("help-others-list");
  others.textContent = "";
  for (const [name, o] of Object.entries(APPS)) {
    if (name === app) continue;
    const d = document.createElement("div"), h = document.createElement("h4");
    h.textContent = o.title; d.append(h, keyList(o.rows));
    others.append(d);
  }
}
state.renderHelp = renderHelp;

// ------------------------------------------------------------------ 3D or flat
const insets = () => ({ top: $("topbar").offsetHeight, bottom: 0 });
function webglOK() {
  try { const c = document.createElement("canvas"); return !!(c.getContext("webgl2") || c.getContext("webgl")); } catch { return false; }
}
function setMode(mode) {
  state.mode = mode;
  document.body.classList.toggle("mode-3d", mode === "3d");
  document.body.classList.toggle("mode-flat", mode === "flat");
  $("view3d").hidden = mode !== "3d";
  $("flat").hidden = mode !== "flat";
  $("btn-reset").hidden = mode !== "3d";
  fitScreen();
}
async function start3D() {
  if (!CONFIG.model) throw new Error("no 3D model in this build");
  setMode("3d");
  const { createView3D } = await import(CONFIG.view3d);
  const q = new URLSearchParams(location.search).get("quality") || prefGet("macplus.quality");
  state.view3d = await createView3D({
    container: $("view3d"), canvas, modelUrl: CONFIG.model, mac: macApi, insets, quality: q && q !== "auto" ? q : undefined,
    badgeFont: CONFIG.badgeFont,
    sound: clickSound, onSlot: () => chooseDisk(), onView: (yaw, pitch, zoom) => spatialize(yaw, pitch, zoom),
    floppyUrl: CONFIG.floppy, floppyTiming: CONFIG.sfx && CONFIG.sfx.timing,
    onProgress: (f) => { $("view3d-loading").textContent = `Loading the 3D Mac… ${Math.round(f * 100)}%`; },
  });
  state.view3d.setStarted(state.started);
  $("view3d-loading").hidden = true;
  glideIn();
  $("quality").value = q || "auto";
}
// The 3D view everywhere WebGL works (phones too, with lower quality on weak GPUs); the flat
// screen is only a silent safety net when WebGL is missing or the 3D view can't start.
async function chooseView() {
  if (webglOK()) {
    try { await start3D(); return; }
    catch (err) { state.logs.push("3D view failed: " + err); $("view3d-loading").textContent = ""; }
  }
  setMode("flat");
}
{
  const cb = $("click-sounds");
  cb.checked = state.clickSounds;
  cb.addEventListener("change", () => { state.clickSounds = cb.checked; prefSet("macplus.clickSounds", cb.checked ? "1" : "0"); });
}
// The two volume sliders, remembered.
for (const [id, key, prop, node] of [["vol-hw", "macplus.volHw", "volHw", "hwGain"], ["vol-sw", "macplus.volSw", "volSw", "swGain"]]) {
  const r = $(id), out = $(id + "-val");
  const v = prefGet(key);
  state[prop] = v === null || v === "" || isNaN(Number(v)) ? 1 : Math.min(1, Math.max(0, Number(v)));
  r.value = String(Math.round(state[prop] * 100)); out.textContent = `${r.value}%`;
  r.addEventListener("input", () => {
    state[prop] = Number(r.value) / 100; out.textContent = `${r.value}%`;
    prefSet(key, String(state[prop]));
    if (state[node]) state[node].gain.setTargetAtTime(state[prop], state.audioCtx.currentTime, 0.02);
  });
}
// Blockworld's mouse speed (Mac pixels per pixel of mouse movement), remembered.
{
  const r = $("look-sens"), out = $("look-sens-val");
  const show = () => { out.textContent = `${Math.round(state.lookSens * 64 * 2 * 360 / 1024)}° per 64 px`; };
  r.value = String(state.lookSens); show();
  r.addEventListener("input", () => { state.lookSens = Number(r.value); prefSet("macplus.lookSens", r.value); show(); });
}
$("quality").addEventListener("change", () => {
  const q = $("quality").value;
  prefSet("macplus.quality", q);
  if (state.view3d && q !== "auto") state.view3d.setQuality(q);
});

// The flat view: integer scaling when it fits (crisp pixels), between the bars.
function fitScreen() {
  if (state.mode !== "flat") { canvas.style.width = ""; canvas.style.height = ""; return; }
  const i = insets();
  const chromeW = window.innerWidth < 640 ? 40 : 84, chromeH = window.innerWidth < 640 ? 52 : 96;
  const availW = innerWidth - 24 - chromeW, availH = innerHeight - i.top - i.bottom - 48 - chromeH;
  let scale = Math.min(availW / SCREEN_W, availH / SCREEN_H);
  if (scale >= 1) scale = Math.max(1, Math.floor(scale * 2) / 2);   // 1, 1.5, 2, 2.5 ...
  canvas.style.width = `${Math.floor(SCREEN_W * scale)}px`;
  canvas.style.height = `${Math.floor(SCREEN_H * scale)}px`;
}
window.addEventListener("resize", () => { fitScreen(); });

// ------------------------------------------------------------------ go
if (!window.crossOriginIsolated || typeof SharedArrayBuffer === "undefined") {
  fail("This page needs a browser with SharedArrayBuffer (served cross-origin isolated). Try a current Chrome, Safari or Firefox.");
  setMode("flat");
} else {
  ctx.fillStyle = "#000"; ctx.fillRect(0, 0, SCREEN_W, SCREEN_H);
  state.wantStart = true;                        // boots by itself once loaded
  if (new URLSearchParams(location.search).get("view") === "flat") setMode("flat");   // tests of the safety net only
  else chooseView();
  loadAssets().catch((err) => fail("Could not load the Mac: " + err.message));
}
