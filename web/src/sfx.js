// The machine's own sounds (from Devon's licensed Epidemic Sound recordings, cut by
// tools/make_sfx.mjs): the power button, power-on with the start-up beep, the running hum (a
// seamless loop under everything), shutdown, and the floppy drive (insert and eject only).
// All of it goes into one bus under the Mac's own sound; the browser's tab mute silences it.
export function makeSfx(ctx, out, cfg) {
  const bus = ctx.createGain(); bus.gain.value = 1.0; bus.connect(out);
  // Per-clip levels (Devon: the sounds were far too quiet). The hum clip averages about -38 dB, so it
  // needs about +10 dB to be heard; the spin-down and power-on are quiet recordings too.
  const LEVEL = { shutdown: 4.0, powerOn: 2.0 }, HUM = 3.3;
  const hum = ctx.createGain(); hum.gain.value = 0; hum.connect(bus);
  const a = typeof Audio !== "undefined" ? new Audio() : null;
  const ext = a && a.canPlayType('audio/ogg; codecs="opus"') ? ".ogg" : ".mp3";
  const bufs = {};
  // (the head-seek clips are not used: drive sounds play only when the visitor inserts or ejects)
  const ready = Promise.all(Object.entries(cfg.clips).filter(([name]) => !name.startsWith("seek")).map(async ([name, c]) => {
    try {
      const res = await fetch(cfg.base + c.file + ext);
      bufs[name] = await ctx.decodeAudioData(await res.arrayBuffer());
    } catch (e) { console.warn("sfx:", name, e.message); }
  }));
  let humSrc = null, lastSeek = 0;
  function play(name, when = 0, gain = 1) {
    const b = bufs[name]; if (!b) return null;
    const s = ctx.createBufferSource(), g = ctx.createGain();
    s.buffer = b; g.gain.value = gain * (LEVEL[name] || 1);
    s.connect(g).connect(bus);
    s.start(ctx.currentTime + when);
    return s;
  }
  function humTo(level, seconds, when = 0) {
    const t = ctx.currentTime + when;
    hum.gain.cancelScheduledValues(t);
    hum.gain.setValueAtTime(hum.gain.value, t);
    hum.gain.linearRampToValueAtTime(level, t + seconds);
  }
  function startHum() {
    if (humSrc || !bufs.ambience) return;
    humSrc = ctx.createBufferSource(); humSrc.buffer = bufs.ambience; humSrc.loop = true;
    humSrc.connect(hum); humSrc.start();
  }
  return {
    ready,
    bus,
    // The running machine: the hum fades in (after the audio unlock, or after power-on).
    ambience(fade = 1.5) { ready.then(() => { startHum(); humTo(HUM, fade); }); },
    // Power on: the button, the power-on clip (switch, beep, the hum spinning up), into the hum.
    powerOn(delay = 0) {
      ready.then(() => {
        play("button", delay);
        const on = bufs.powerOn ? bufs.powerOn.duration : 0;
        play("powerOn", delay + 0.12);
        startHum(); humTo(0, 0.05); humTo(HUM, 1.8, delay + Math.max(0.5, on - 2.0));
      });
    },
    // Restart: the button, the spin-down (the hum fades with it), then power on again.
    restart() {
      ready.then(() => {
        play("button2");
        humTo(0, 0.6, 0.1);
        play("shutdown", 0.1);
        const down = bufs.shutdown ? Math.min(bufs.shutdown.duration, 3.2) : 1;
        this.powerOn(down);
      });
    },
    // Only for a disk the visitor inserts, started with the 3D disk's slide (buffers are decoded at
    // the audio unlock, so nothing waits on loading).
    insert() { play("insert"); },
    eject() { play("eject"); },
    // The drive reading: a short burst of head steps, at most one at a time.
    seek() {
      const now = performance.now();
      if (now - lastSeek < 1800) return;
      lastSeek = now;
      play(["seek1", "seek2", "seek3"][Math.floor(Math.random() * 3)], 0, 0.8);
    },
    timing: cfg.timing || {},
  };
}
