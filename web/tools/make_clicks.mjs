// Click sounds for the 3D keyboard and mouse, from real recordings.
//   node web/tools/make_clicks.mjs web/clicks.json web/build/clicks
// For each source in clicks.json (a recording, the part of it to use, and what it is: "keys" or
// "mouse"), the script decodes it with ffmpeg (mono, 48 kHz), finds the clicks (onsets of the peak
// envelope), keeps the clean ones (a press with nothing just before it, its release 40-220 ms later
// and quieter), trims each to the click itself (3 ms before the onset to just before the next sound,
// at most 90 ms for a press and 80 ms for a release), fades the ends, normalises loudness (presses to
// one RMS level; a release keeps its level relative to its press) and exports every clip twice:
// Opus in Ogg (most browsers) and MP3 (Safari). It writes manifest.json listing the clips by role:
// keyDown, keyUp, mouseDown, mouseUp (bigDown/bigUp when a source is marked "big": Space, Return),
// and the sources with their licences (for the credits).
// Recordings that hold single presses (Devon's own, say) work the same way; give "start"/"end" in
// seconds to use part of a file.
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";

const [cfgFile, outDir] = process.argv.slice(2);
if (!cfgFile || !outDir) { console.error("usage: node make_clicks.mjs <clicks.json> <out dir>"); process.exit(2); }
const cfg = JSON.parse(fs.readFileSync(cfgFile, "utf8"));
const base = path.dirname(path.resolve(cfgFile));
fs.mkdirSync(outDir, { recursive: true });
const SR = 48000, ms = (t) => Math.round(t * SR / 1000);

function decode(file, start, end) {
  const args = ["-v", "error"];
  if (start) args.push("-ss", String(start));
  if (end) args.push("-to", String(end));
  args.push("-i", file, "-ac", "1", "-ar", String(SR), "-f", "f32le", "-");
  const raw = execFileSync("ffmpeg", args, { maxBuffer: 1 << 30 });
  return new Float32Array(raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.length));
}

// Onsets of the 1 ms peak envelope crossing a threshold well above the noise floor.
function onsets(x) {
  const hop = ms(1), n = Math.floor(x.length / hop), env = new Float32Array(n);
  for (let i = 0; i < n; i++) { let m = 0; for (let k = 0; k < hop; k++) m = Math.max(m, Math.abs(x[i * hop + k])); env[i] = m; }
  const s = Float32Array.from(env).sort();
  const floor = s[Math.floor(n * 0.3)], peak = s[Math.floor(n * 0.999)];
  const thr = Math.max(floor * 6, floor + 0.12 * (peak - floor));
  const out = [];
  let last = -1e9;
  for (let i = 1; i < n; i++) {
    if (env[i] >= thr && env[i - 1] < thr && i - last > 35) {
      let p = 0; for (let j = i; j < Math.min(n, i + 25); j++) p = Math.max(p, env[j]);
      out.push({ t: i, peak: p }); last = i;
    }
  }
  return { list: out, floor, peak, n };
}

const rms = (a) => Math.sqrt(a.reduce((s, v) => s + v * v, 0) / a.length);
function cut(x, startMs, lenMs) {
  const a = x.slice(Math.max(0, ms(startMs)), Math.min(x.length, ms(startMs + lenMs)));
  const fi = ms(1), fo = Math.min(ms(8), Math.floor(a.length / 3));
  for (let i = 0; i < fi && i < a.length; i++) a[i] *= i / fi;
  for (let i = 0; i < fo; i++) a[a.length - 1 - i] *= i / fo;
  return a;
}
function encode(samples, name) {
  const pcm = Buffer.from(samples.buffer, samples.byteOffset, samples.byteLength);
  const hash = crypto.createHash("sha256").update(pcm).digest("hex").slice(0, 8);
  const stem = `${name}.${hash}`;
  const inArgs = ["-v", "error", "-y", "-f", "f32le", "-ar", String(SR), "-ac", "1", "-i", "pipe:0"];
  execFileSync("ffmpeg", [...inArgs, "-c:a", "libopus", "-b:a", "64k", path.join(outDir, `${stem}.ogg`)], { input: pcm });
  execFileSync("ffmpeg", [...inArgs, "-c:a", "libmp3lame", "-b:a", "96k", path.join(outDir, `${stem}.mp3`)], { input: pcm });
  return stem;
}

const manifest = { keyDown: [], keyUp: [], bigDown: [], bigUp: [], mouseDown: [], mouseUp: [], sources: [] };
const TARGET = 0.08;                     // RMS of a press clip
for (const src of cfg.sources) {
  const file = path.resolve(base, src.file);
  if (!fs.existsSync(file)) { console.log(`${src.id}: ${src.file} is missing; skipped`); continue; }
  const x = decode(file, src.start, src.end);
  const { list } = onsets(x);
  const maxPeak = Math.max(...list.map((o) => o.peak));
  const picks = [];
  for (let i = 0; i < list.length; i++) {
    const o = list[i], prev = list[i - 1], next = list[i + 1], after = list[i + 2];
    if (prev && o.t - prev.t < (src.gapBefore ?? 120)) continue;          // something just before it
    if (o.peak < maxPeak * (src.minPeak ?? 0.25)) continue;               // too quiet to be a press
    let rel = null;
    if (next && next.t - o.t >= 40 && next.t - o.t <= 220 && next.peak < o.peak * 0.9 && (!after || after.t - next.t >= 50)) rel = next;
    picks.push({ press: o, rel, pressLen: Math.min(90, (rel || next || { t: o.t + 1e9 }).t - o.t - 3), relLen: rel ? Math.min(80, (after || { t: rel.t + 1e9 }).t - rel.t - 3) : 0 });
  }
  // Best first: a clean release, then loud and well separated.
  picks.sort((a, b) => (!!b.rel - !!a.rel) || (b.press.peak - a.press.peak));
  const take = picks.filter((p) => p.pressLen >= 25).slice(0, src.count ?? 8);
  const role = src.role === "mouse" ? ["mouseDown", "mouseUp"] : src.big ? ["bigDown", "bigUp"] : ["keyDown", "keyUp"];
  let k = 0;
  for (const p of take) {
    const press = cut(x, p.press.t - 3, p.pressLen + 3);
    const g = TARGET / Math.max(1e-4, rms(press.subarray(0, Math.min(press.length, ms(40)))));
    for (let i = 0; i < press.length; i++) press[i] = Math.max(-1, Math.min(1, press[i] * g));
    const id = `${src.id}-${k++}`;
    manifest[role[0]].push(encode(press, `${role[0]}-${id}`));
    if (p.rel && p.relLen >= 15) {
      const rel = cut(x, p.rel.t - 3, p.relLen + 3);
      for (let i = 0; i < rel.length; i++) rel[i] = Math.max(-1, Math.min(1, rel[i] * g));
      manifest[role[1]].push(encode(rel, `${role[1]}-${id}`));
    }
  }
  manifest.sources.push({ id: src.id, title: src.title, author: src.author, url: src.url, licence: src.licence, what: src.what, clips: take.length });
  console.log(`${src.id}: ${list.length} sounds found, ${picks.length} clean presses, ${take.length} kept (${take.filter((p) => p.rel).length} with their release)`);
}
fs.writeFileSync(path.join(outDir, "manifest.json"), JSON.stringify(manifest, null, 1));
console.log(`${outDir}: ${["keyDown", "keyUp", "bigDown", "bigUp", "mouseDown", "mouseUp"].map((r) => `${r} ${manifest[r].length}`).join(", ")}`);
