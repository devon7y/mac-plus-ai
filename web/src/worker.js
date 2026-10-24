/*! Loader for Mini vMac (Mac Plus) compiled to WebAssembly.
 * Mini vMac: Copyright Paul C. Pratt and contributors. GNU General Public License v2
 * (licenses/minivmac-gpl-2.0.txt). Source of the build used here:
 * https://github.com/mihaip/minivmac/tree/0bcab1d51ce240489b98b21b744ddfcddf01659e
 * as committed in https://github.com/mihaip/infinite-mac/tree/f988f537fff09c7b902e448a21cd8ac170e42c67
 * (Infinite Mac: Apache License 2.0, licenses/infinite-mac-apache-2.0.txt).
 * SPDX-License-Identifier of the emulator: GPL-2.0-only
 */
// Emulator worker: runs Infinite Mac's WebAssembly build of Mini vMac (Mac Plus) and implements
// the `workerApi` object its Emscripten glue calls (see infinite-mac src/emulator/worker/worker.ts
// and minivmac src/OSGLUESC.c). This is our own, much smaller host: one built-in disk held in
// memory, SharedArrayBuffer input/audio, video sent as dirty rectangles.
//
// Mini vMac's main loop never returns, so this worker cannot receive messages once it starts.
// Everything coming in after "start" goes through SharedArrayBuffers that the page writes:
//   input  Int32Array: a ring of input events plus a few control words (see app.js, IN_*)
//   audio  ring buffer of signed 8-bit samples read by the AudioWorklet
// Everything going out is postMessage.

// Shared layout (keep in sync with app.js).
const IN_WRITE = 0, IN_READ = 1, IN_DX = 2, IN_DY = 3, IN_AUDIO_RUNNING = 4, IN_SPEED_FLAG = 5,
      IN_SPEED = 6, IN_CMD = 7, IN_CMD_ARG0 = 8, IN_CMD_ARG1 = 9, IN_CMD_ID = 10, IN_PAUSED = 11;
const IN_DISK_SEQ = 12, IN_DISK_LEN = 13, IN_DISK_NAMELEN = 14, IN_DISK_ACK = 15;   // Insert Disk
const IN_RING = 16, IN_RING_EVENTS = 256, IN_EVENT_WORDS = 4;
const INBOX_DATA = 256;                         // the inbox: the disk's name, then its bytes from here
const EV_MOVE = 1, EV_DOWN = 2, EV_UP = 3, EV_KEYDOWN = 4, EV_KEYUP = 5;
const CMD_PEEK = 1, CMD_DUMP_DISK = 2, CMD_POKE32 = 3;
const AU_WRITE = 0, AU_READ = 1, AU_HEADER_BYTES = 16;

// The addresses Mini vMac's glue asks for with getInputValue (Infinite Mac's InputBufferAddresses).
const InputBufferAddresses = {
  globalLockAddr: 0, mousePositionFlagAddr: 1, mousePositionXAddr: 2, mousePositionYAddr: 3,
  mouseButtonStateAddr: 4, keyEventFlagAddr: 5, keyCodeAddr: 6, keyStateAddr: 7, stopFlagAddr: 8,
  ethernetInterruptFlagAddr: 9, audioContextRunningFlagAddr: 10, speedFlagAddr: 11, speedAddr: 12,
  mouseDeltaXAddr: 13, mouseDeltaYAddr: 14, keyModifiersAddr: 15, mouseButton2StateAddr: 16,
  useMouseDeltasFlagAddr: 17, useMouseDeltasAddr: 18, pausedAddr: 19,
};

const SCREEN_W = 512, SCREEN_H = 342;

let Module = null;
let input = null;          // Int32Array over the input SharedArrayBuffer
let audioHdr = null, audioData = null, audioSize = 0;
let disk = null;           // Uint8Array, the whole volume
let inbox = null;          // Uint8Array over the page's disk inbox (a SharedArrayBuffer)
// Disks inserted from the page (floppies): drive name -> {id, name, data}; id -> the same.
const extra = new Map(), extraById = new Map(), pendingNames = [];
let nextDiskId = 1, lastFloppyRead = 0;
let ramBase = -1;          // byte offset of the Mac's RAM in the wasm heap (found by signature)
let frame = new Int32Array(20);   // the values getInputValue returns for this tick
let lastStatsTime = 0, statsTicks = 0, audioSamples = 0, audioNonSilent = 0, blits = 0;
let startTime = 0, sleptMs = 0;
let lastRamScan = 0;

self.onmessage = (e) => {
  if (e.data.type === "start") start(e.data);
};

function post(msg, transfer) {
  self.postMessage(msg, transfer || []);
}

// ------------------------------------------------------------------ Mac RAM access
// Mini vMac keeps the Mac's RAM as one big-endian block inside the wasm heap. We find it by the
// Plus's low-memory globals once the ROM has set them up: ROMBase ($2AE) = $400000, MemTop ($108)
// = RAM size, ScrnBase ($824) = MemTop - $5900 (the main screen buffer of a Plus).
function heap() { return Module.HEAPU8; }
function rd32(o) { const h = heap(); return ((h[o] << 24) | (h[o + 1] << 16) | (h[o + 2] << 8) | h[o + 3]) >>> 0; }
function rd16(o) { const h = heap(); return (h[o] << 8) | h[o + 1]; }
function wr16(o, v) { const h = heap(); h[o] = (v >> 8) & 255; h[o + 1] = v & 255; }

function findRam() {
  const h = heap();
  const end = h.length - 0x1000;
  for (let o = 0; o < end; o += 8) {
    // ROMBase = $00400000
    if (h[o + 0x2AE] !== 0x00 || h[o + 0x2AF] !== 0x40 || h[o + 0x2B0] !== 0 || h[o + 0x2B1] !== 0) continue;
    const memTop = rd32(o + 0x108);
    if (memTop !== 0x400000 && memTop !== 0x200000 && memTop !== 0x100000 && memTop !== 0x280000) continue;
    if (rd32(o + 0x824) !== memTop - 0x5900) continue;
    return o;
  }
  return -1;
}

function macAppName() {
  if (ramBase < 0) return "";
  const h = heap(), o = ramBase + 0x910;          // CurApName, a Str31
  const n = Math.min(h[o], 31);
  let s = "";
  for (let i = 0; i < n; i++) s += String.fromCharCode(h[o + 1 + i]);
  return s;
}

// ------------------------------------------------------------------ the API the core calls
// Disk 0 is the built-in volume; inserted floppies get 1, 2, ... Mini vMac polls consumeDiskName()
// for disks to insert and closes a disk when the Mac ejects it (dragged to the Trash, File > Eject).
const diskData = (id) => (id === 0 ? disk : extraById.get(id) && extraById.get(id).data);
const disks = {
  open(name) {
    if (name === "hd.dsk") return 0;
    const d = extra.get(name);
    return d ? d.id : -1;
  },
  close(id) {
    const d = extraById.get(id);
    if (d && !d.ejected) { d.ejected = true; post({ type: "disk_ejected", id, name: d.name }); }
  },
  size(id) { const d = diskData(id); return d ? d.length : 0; },
  read(id, bufPtr, offset, length) {
    const d = diskData(id); if (!d) return 0;
    const n = Math.max(0, Math.min(length, d.length - offset));
    heap().set(d.subarray(offset, offset + n), bufPtr);
    return n;
  },
  write(id, bufPtr, offset, length) {
    const d = diskData(id); if (!d) return 0;
    const n = Math.max(0, Math.min(length, d.length - offset));
    d.set(heap().subarray(bufPtr, bufPtr + n), offset);
    return n;
  },
  consumeDiskName() { return pendingNames.shift(); },
};

// A disk image the page put in the inbox: copy it out, queue it for Mini vMac, acknowledge.
function takeInsertedDisk() {
  const seq = Atomics.load(input, IN_DISK_SEQ);
  if (!inbox || seq === Atomics.load(input, IN_DISK_ACK)) return;
  const len = input[IN_DISK_LEN], nl = input[IN_DISK_NAMELEN];
  const view = new Uint8Array(inbox.buffer);           // (tracks a growable buffer's length)
  const name = new TextDecoder().decode(view.slice(0, nl));
  const data = view.slice(INBOX_DATA, INBOX_DATA + len);
  const id = nextDiskId++, drive = `floppy${id}`;
  const d = { id, name, data, ejected: false };
  extra.set(drive, d); extraById.set(id, d);
  pendingNames.push(drive);
  Atomics.store(input, IN_DISK_ACK, seq);
  post({ type: "disk_inserted", id, name, size: len });
}

const workerApi = {
  InputBufferAddresses,
  disks,

  didOpenVideo(width, height) { post({ type: "video_open", width, height }); },

  blit(bufPtr, bufSize, rect) {
    if (!bufPtr || !rect) return;
    const top = rect.top, bottom = Math.min(rect.bottom, SCREEN_H);
    const left = Math.max(0, rect.left), right = Math.min(rect.right, SCREEN_W);
    const w = right - left, hgt = bottom - top;
    if (w <= 0 || hgt <= 0) return;
    const out = new Uint8ClampedArray(w * hgt * 4);
    const h = heap();
    for (let y = 0; y < hgt; y++) {
      const src = bufPtr + ((top + y) * SCREEN_W + left) * 4;
      out.set(h.subarray(src, src + w * 4), y * w * 4);
    }
    for (let i = 3; i < out.length; i += 4) out[i] = 255;      // opaque (the core leaves alpha 0)
    blits++;
    post({ type: "blit", x: left, y: top, w, h: hgt, data: out }, [out.buffer]);
  },

  didOpenAudio(sampleRate, sampleSize, channels) {
    post({ type: "audio_open", sampleRate, sampleSize, channels });
  },

  enqueueAudio(bufPtr, nbytes) {
    if (!audioHdr || !Atomics.load(input, IN_AUDIO_RUNNING)) return 0;
    const h = heap();
    let w = Atomics.load(audioHdr, AU_WRITE);
    const r = Atomics.load(audioHdr, AU_READ);
    const free = audioSize - 1 - ((w - r + audioSize) % audioSize);
    const n = Math.min(nbytes, free);
    for (let i = 0; i < n; i++) {
      const s = h[bufPtr + i];
      audioData[w] = s;
      if (s !== 0) audioNonSilent++;
      w = (w + 1) % audioSize;
    }
    audioSamples += n;
    Atomics.store(audioHdr, AU_WRITE, w);
    return n;
  },

  sleep(seconds) {
    if (seconds > 0) {
      const t = performance.now();
      Atomics.wait(sleepCell, 0, 0, seconds * 1000);
      sleptMs += performance.now() - t;
    }
  },

  idleWait() { return false; },

  acquireInputLock() {
    buildFrame();
    return 1;
  },
  releaseInputLock() {},
  getInputValue(addr) { return frame[addr] | 0; },
};
const sleepCell = new Int32Array(new SharedArrayBuffer(4));

// Called once per emulated tick (60.15 Hz) from Mini vMac's WaitForNextTick. Takes the queued
// input events in order and decides what this tick delivers: the core reads one mouse position,
// one button state and one key event per tick, so a click is spread over two ticks (down, then up)
// and a move after a button change waits for the next tick (the click lands where it was made).
function buildFrame() {
  // Paused (the page is hidden): wait here; Mini vMac skips the lost time when it resumes.
  while (Atomics.load(input, IN_PAUSED)) Atomics.wait(input, IN_PAUSED, 1, 1000);
  frame.fill(0);
  frame[InputBufferAddresses.mouseButtonStateAddr] = -1;     // -1 = unchanged
  let r = Atomics.load(input, IN_READ);
  const wIdx = Atomics.load(input, IN_WRITE);
  let haveButton = false, haveKey = false, haveMove = false;
  while (r !== wIdx) {
    const base = IN_RING + (r % IN_RING_EVENTS) * IN_EVENT_WORDS;
    const type = input[base], a = input[base + 1], b = input[base + 2];
    if (type === EV_MOVE) {
      if (haveButton) break;
      frame[InputBufferAddresses.mousePositionFlagAddr] = 1;
      frame[InputBufferAddresses.mousePositionXAddr] = a;
      frame[InputBufferAddresses.mousePositionYAddr] = b;
      haveMove = true;
    } else if (type === EV_DOWN || type === EV_UP) {
      if (haveButton) break;
      frame[InputBufferAddresses.mouseButtonStateAddr] = type === EV_DOWN ? 1 : 0;
      haveButton = true;
    } else if (type === EV_KEYDOWN || type === EV_KEYUP) {
      if (haveKey) break;
      frame[InputBufferAddresses.keyEventFlagAddr] = 1;
      frame[InputBufferAddresses.keyCodeAddr] = a;
      frame[InputBufferAddresses.keyStateAddr] = type === EV_KEYDOWN ? 1 : 0;
      haveKey = true;
    }
    r = (r + 1) >>> 0;
  }
  Atomics.store(input, IN_READ, r);

  if (Atomics.load(input, IN_SPEED_FLAG)) {
    frame[InputBufferAddresses.speedFlagAddr] = 1;
    frame[InputBufferAddresses.speedAddr] = Atomics.load(input, IN_SPEED);
    Atomics.store(input, IN_SPEED_FLAG, 0);
  }
  frame[InputBufferAddresses.audioContextRunningFlagAddr] = Atomics.load(input, IN_AUDIO_RUNNING);

  const now = performance.now();
  if (ramBase < 0 && now - lastRamScan > 500) {
    lastRamScan = now;
    ramBase = findRam();
    if (ramBase >= 0) post({ type: "log", text: `Mac RAM found at heap offset ${ramBase}` });
  }

  // Locked-pointer mouse: move the Mac's mouse by the browser's deltas, the way Mini vMac's own
  // mouse-motion mode does (MOUSEMDV.c): RawMouse and MTemp += delta, CrsrNew = CrsrCouple.
  // Needed by Blockworld, which re-centres the cursor every frame and reads the movement.
  const dx = Atomics.exchange(input, IN_DX, 0), dy = Atomics.exchange(input, IN_DY, 0);
  if ((dx || dy) && ramBase >= 0 && !haveMove) {
    const o = ramBase;
    let v = rd16(o + 0x828), hh = rd16(o + 0x82A);
    if (v & 0x8000) v -= 0x10000;
    if (hh & 0x8000) hh -= 0x10000;
    v = Math.max(0, Math.min(SCREEN_H - 1, v + dy));
    hh = Math.max(0, Math.min(SCREEN_W - 1, hh + dx));
    wr16(o + 0x828, v); wr16(o + 0x82A, hh);           // RawMouse
    wr16(o + 0x82C, v); wr16(o + 0x82E, hh);           // MTemp
    heap()[o + 0x8CE] = heap()[o + 0x8CF];              // CrsrNew = CrsrCouple
  } else if (dx || dy) {
    Atomics.add(input, IN_DX, dx); Atomics.add(input, IN_DY, dy);   // keep them for later
  }

  handleCommand();
  takeInsertedDisk();

  if (now - lastStatsTime > 500) {
    const ticks = ramBase >= 0 ? rd32(ramBase + 0x16A) : 0;
    post({
      type: "stats", t: now - startTime, ticks, app: macAppName(), ramFound: ramBase >= 0,
      audioSamples, audioNonSilent, blits, sleptMs,
    });
    lastStatsTime = now;
  }
}

// Commands from the page (tests and the bake script): read Mac memory, copy the disk out.
function handleCommand() {
  const cmd = Atomics.load(input, IN_CMD);
  if (!cmd) return;
  const id = input[IN_CMD_ID], a0 = input[IN_CMD_ARG0], a1 = input[IN_CMD_ARG1];
  if (cmd === CMD_PEEK && ramBase >= 0) {
    const bytes = heap().slice(ramBase + a0, ramBase + a0 + a1);
    post({ type: "peek", id, addr: a0, bytes }, [bytes.buffer]);
  } else if (cmd === CMD_PEEK) {
    post({ type: "peek", id, addr: a0, bytes: null });
  } else if (cmd === CMD_POKE32) {                     // tests: write one big-endian 32-bit word
    if (ramBase >= 0) {
      const h = heap(), o = ramBase + a0;
      h[o] = a1 >>> 24; h[o + 1] = (a1 >>> 16) & 255; h[o + 2] = (a1 >>> 8) & 255; h[o + 3] = a1 & 255;
    }
    post({ type: "peek", id, addr: a0, bytes: null });
  } else if (cmd === CMD_DUMP_DISK) {
    const d = diskData(a0) || disk;                     // a0: which disk (0 = the built-in one)
    const copy = d.slice();
    post({ type: "disk", id, data: copy }, [copy.buffer]);
  }
  Atomics.store(input, IN_CMD, 0);
}

// ------------------------------------------------------------------ start-up
async function start(cfg) {
  input = new Int32Array(cfg.inputBuffer);
  audioHdr = new Int32Array(cfg.audioBuffer, 0, 4);
  audioData = new Int8Array(cfg.audioBuffer, AU_HEADER_BYTES);
  audioSize = audioData.length;
  disk = new Uint8Array(cfg.disk);
  if (cfg.inbox) inbox = new Uint8Array(cfg.inbox);
  startTime = performance.now();
  self.workerApi = workerApi;
  let emulator;
  try {
    emulator = (await import(cfg.coreUrl)).default;
  } catch (err) {
    post({ type: "error", text: "Could not load the emulator: " + err });
    return;
  }
  const moduleArg = {
    arguments: [],
    instantiateWasm(imports, successCallback) {
      WebAssembly.instantiate(cfg.wasm, imports)
        .then((out) => successCallback(out.instance))
        .catch((err) => post({ type: "error", text: "Cannot instantiate WebAssembly: " + err }));
      return {};
    },
    preRun: [function () {
      const FS = moduleArg.FS;
      FS.writeFile("/MacPlus.rom", new Uint8Array(cfg.rom));
      FS.writeFile("/prefs", "rom /MacPlus.rom\ndisk hd.dsk\nscreen win/512/342\n");
    }],
    onRuntimeInitialized() { post({ type: "ready" }); },
    print(...args) { post({ type: "log", text: args.join(" ") }); },
    printErr(...args) { post({ type: "log", text: args.join(" ") }); },
    onAbort(what) { post({ type: "error", text: "Emulator stopped: " + what }); },
  };
  Module = moduleArg;
  try {
    await emulator(moduleArg);
  } catch (err) {
    post({ type: "error", text: String(err && err.message || err) });
  }
}
