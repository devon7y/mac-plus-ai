// Click sounds for the 3D keyboard and mouse: short clips cut from real recordings by
// tools/make_clicks.mjs (sources and licences in web/clicks.json; credited on the Mac's "Credits &
// Licenses"). Played only for clicks on the 3D keys and mouse button, never for real typing.
// Each press picks a random clip (never the same one twice in a row) with a little pitch (±3%) and
// level (±12%) variation; a key's release plays its own release clip on pointer-up. Space, Return,
// Enter and Shift use the "big" clips when there are any, otherwise the ordinary ones a little deeper.
// Clips are decoded once (Ogg Opus, or MP3 where Opus isn't supported) and started at their first
// sound, so a click is heard as soon as the pointer goes down.
export function makeClickPlayer(ctx, out, cfg) {
  const bus = ctx.createGain();
  bus.gain.value = 0.7;
  bus.connect(out);
  const roles = { keyDown: [], keyUp: [], bigDown: [], bigUp: [], mouseDown: [], mouseUp: [] };
  const last = {};
  const a = typeof Audio !== "undefined" ? new Audio() : null;
  const ext = a && a.canPlayType('audio/ogg; codecs="opus"') ? ".ogg" : ".mp3";
  async function load(name) {
    const res = await fetch(cfg.base + name + ext);
    if (!res.ok) throw new Error(`${name}: HTTP ${res.status}`);
    const buf = await ctx.decodeAudioData(await res.arrayBuffer());
    // Where the click starts (codecs add a little silence in front).
    const d = buf.getChannelData(0);
    let peak = 0; for (let i = 0; i < d.length; i++) peak = Math.max(peak, Math.abs(d[i]));
    let i0 = 0; while (i0 < d.length && Math.abs(d[i0]) < peak * 0.03) i0++;
    return { buf, offset: Math.max(0, i0 / buf.sampleRate - 0.002) };
  }
  const ready = Promise.all(Object.keys(roles).map(async (r) => {
    for (const name of cfg[r] || []) {
      try { roles[r].push(await load(name)); } catch (e) { console.warn("click sound:", e.message); }
    }
  }));
  const big = (name) => name === "Space" || name === "Return" || name === "Enter" || /^Shift/.test(name);
  const rnd = (k) => 1 + (Math.random() * 2 - 1) * k;
  function play(role, fallback, rate = 1, gain = 1) {
    let list = roles[role];
    if (!list.length && fallback) { list = roles[fallback]; }
    if (!list.length) return;
    let i = Math.floor(Math.random() * list.length);
    if (list.length > 1 && i === last[role]) i = (i + 1) % list.length;
    last[role] = i;
    const c = list[i], src = ctx.createBufferSource(), g = ctx.createGain();
    src.buffer = c.buf;
    src.playbackRate.value = rate * rnd(0.03);
    g.gain.value = gain * rnd(0.12);
    src.connect(g).connect(bus);
    src.start(0, c.offset);
  }
  return {
    ready,
    keyDown(name) { if (big(name)) play(roles.bigDown.length ? "bigDown" : "keyDown", null, roles.bigDown.length ? 1 : 0.84, roles.bigDown.length ? 1 : 1.15); else play("keyDown"); },
    keyUp(name) { if (big(name)) play(roles.bigUp.length ? "bigUp" : "keyUp", null, roles.bigUp.length ? 1 : 0.84); else play("keyUp"); },
    mouseDown() { play("mouseDown"); },
    mouseUp() { play("mouseUp"); },
    bus,
  };
}
