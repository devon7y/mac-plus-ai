// Machine sounds (power, ambience, floppy) for the page, cut from the recordings in sfx.json:
//   node web/tools/make_sfx.mjs web/sfx.json web/build/sfx
// Each clip: decoded with ffmpeg (mono, 48 kHz, optional high-pass to drop the recording's own hum),
// trimmed, a 5 ms fade in, an optional fade out, levelled (a fixed gain, a peak or an RMS level) and exported as Ogg Opus and MP3. A "loop" clip is made seamless: its first `loop`
// seconds are an equal-power crossfade with the audio just past its end, so the last sample runs
// straight into the first. Writes manifest.json (clip names, durations, the timing marks).
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";

const [cfgFile, outDir] = process.argv.slice(2);
const cfg = JSON.parse(fs.readFileSync(cfgFile, "utf8"));
const base = path.dirname(path.resolve(cfgFile));
fs.mkdirSync(outDir, { recursive: true });
const SR = 48000;
const man = { clips: {}, timing: { ...(cfg.timing || {}) } };
for (const [name, c] of Object.entries(cfg.clips)) {
  const file = path.resolve(base, c.file);
  if (!fs.existsSync(file)) { console.log(`${name}: ${c.file} missing, skipped`); continue; }
  const extra = c.loop ? c.loop : 0;
  const af = [c.highpass ? `highpass=f=${c.highpass}` : null].filter(Boolean);
  const cut = (from, to) => {
    const raw = execFileSync("ffmpeg", ["-v", "error", "-ss", String(from), "-to", String(to), "-i", file, ...(af.length ? ["-af", af.join(",")] : []),
      "-ac", "1", "-ar", String(SR), "-f", "f32le", "-"], { maxBuffer: 1 << 30 });
    return new Float32Array(raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.length));
  };
  let x;
  if (c.segments) {                                // pieces joined with equal-power crossfades
    const X = Math.round((c.xfade || 0.02) * SR);
    x = cut(...c.segments[0]);
    for (const seg of c.segments.slice(1)) {
      const y = cut(...seg), out = new Float32Array(x.length + y.length - X);
      out.set(x.subarray(0, x.length - X));
      for (let i = 0; i < X; i++) { const t = (i + 0.5) / X; out[x.length - X + i] = x[x.length - X + i] * Math.cos(t * Math.PI / 2) + y[i] * Math.sin(t * Math.PI / 2); }
      out.set(y.subarray(X), x.length);
      x = out;
    }
  } else x = cut(c.from, c.to + extra);
  if (c.loop) {
    const L = Math.round((c.to - c.from) * SR), X = Math.round(c.loop * SR), y = new Float32Array(L);
    for (let i = 0; i < L; i++) {
      if (i < X && L + i < x.length) { const t = i / X; y[i] = x[i] * Math.sin(t * Math.PI / 2) + x[L + i] * Math.cos(t * Math.PI / 2); }
      else y[i] = x[i];
    }
    x = y;
  } else {
    const fi = Math.round(0.005 * SR);
    for (let i = 0; i < fi && i < x.length; i++) x[i] *= i / fi;
    if (c.fadeOut) { const fo = Math.min(x.length, Math.round(c.fadeOut * SR)); for (let i = 0; i < fo; i++) x[x.length - 1 - i] *= Math.pow(i / fo, 1.5); }
  }
  if (c.boost) {                                   // [from s, to s, dB]: lift a quiet part (50 ms ramps)
    const [b0, b1, dB] = c.boost, g = Math.pow(10, dB / 20), r = 0.05 * SR;
    for (let i = 0; i < x.length; i++) {
      const t = i, a0 = b0 * SR, a1 = b1 * SR;
      const w = t < a0 - r || t > a1 + r ? 0 : t < a0 ? (t - (a0 - r)) / r : t > a1 ? 1 - (t - a1) / r : 1;
      x[i] *= 1 + (g - 1) * Math.max(0, Math.min(1, w));
    }
  }
  let peak = 0, ss = 0; for (const v of x) { peak = Math.max(peak, Math.abs(v)); ss += v * v; }
  const rms = Math.sqrt(ss / x.length);
  // A fixed gain keeps the hum of the power clips at the ambience's level; otherwise a peak or RMS level.
  const g = c.gain !== undefined ? Math.pow(10, c.gain / 20) : c.rms !== undefined ? Math.pow(10, c.rms / 20) / rms : Math.pow(10, (c.peak ?? -3) / 20) / peak;
  for (let i = 0; i < x.length; i++) x[i] = Math.max(-1, Math.min(1, x[i] * g));
  // trimLead: start at the first clearly audible sound (a 10 ms window peaking within trimLead dB of
  // the clip's peak), less 15 ms; the clip's timing marks move with it.
  if (c.trimLead !== undefined) {
    let pk = 0; for (const v of x) pk = Math.max(pk, Math.abs(v));
    const thr = pk * Math.pow(10, c.trimLead / 20), win = Math.round(0.01 * SR);
    let at = 0;
    for (let o = 0; o + win <= x.length; o += win) { let m = 0; for (let k = 0; k < win; k++) m = Math.max(m, Math.abs(x[o + k])); if (m >= thr) { at = Math.max(0, o - Math.round(0.015 * SR)); break; } }
    if (at > 0) {
      x = x.slice(at);
      const fi = Math.round(0.004 * SR); for (let i = 0; i < fi; i++) x[i] *= i / fi;
      for (const k of Object.keys(man.timing)) if (k.startsWith(name)) man.timing[k] = +(man.timing[k] - at / SR).toFixed(3);
      console.log(`${name}: the first clearly audible sound is ${(at / SR + 0.015).toFixed(3)} s in; playback starts at ${(at / SR).toFixed(3)} s`);
    }
  }
  const pcm = Buffer.from(x.buffer, x.byteOffset, x.byteLength);
  const stem = `${name}.${crypto.createHash("sha256").update(pcm).digest("hex").slice(0, 8)}`;
  const inArgs = ["-v", "error", "-y", "-f", "f32le", "-ar", String(SR), "-ac", "1", "-i", "pipe:0"];
  const br = c.loop ? "32k" : "48k";
  execFileSync("ffmpeg", [...inArgs, "-c:a", "libopus", "-b:a", br, path.join(outDir, `${stem}.ogg`)], { input: pcm });
  execFileSync("ffmpeg", [...inArgs, "-c:a", "libmp3lame", "-b:a", c.loop ? "64k" : "80k", path.join(outDir, `${stem}.mp3`)], { input: pcm });
  man.clips[name] = { file: stem, seconds: +(x.length / SR).toFixed(3), loop: !!c.loop };
  console.log(`${name}: ${(x.length / SR).toFixed(2)} s, gain ${(20 * Math.log10(g)).toFixed(1)} dB -> ${stem} (${(fs.statSync(path.join(outDir, `${stem}.ogg`)).size / 1024).toFixed(0)} KB ogg)`);
}
fs.writeFileSync(path.join(outDir, "manifest.json"), JSON.stringify(man, null, 1));
