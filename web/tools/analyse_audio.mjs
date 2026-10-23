// A timeline of a recording, for choosing where to cut sound effects without listening:
//   node web/tools/analyse_audio.mjs <file> [step seconds, default 0.25] [from] [to]
// For each step: loudness (RMS, dBFS), peak, spectral centroid (Hz), spectral flatness (0 tonal ..
// 1 noise-like), low-band share (< 300 Hz: motors, hum) and the onsets (sharp rises of the 5 ms
// envelope: clicks, clunks, seeks) inside it. Steady stretches (low spread of RMS and centroid over
// a few steps) are marked "~".
import { execFileSync } from "node:child_process";

const [file, stepArg = "0.25", fromArg = "0", toArg = "1e9"] = process.argv.slice(2);
const SR = 22050, step = +stepArg;
const raw = execFileSync("ffmpeg", ["-v", "error", "-i", file, "-ac", "1", "-ar", String(SR), "-f", "f32le", "-"], { maxBuffer: 1 << 30 });
const x = new Float32Array(raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.length));
const N = 1024;
const win = Float32Array.from({ length: N }, (_, i) => 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / N));
function spectrum(o) {                                   // magnitude spectrum of a Hann-windowed frame
  const re = new Float64Array(N), im = new Float64Array(N);
  for (let i = 0; i < N; i++) re[i] = (x[o + i] || 0) * win[i];
  for (let i = 1, j = 0; i < N; i++) {                   // radix-2 FFT
    let bit = N >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit;
    if (i < j) { [re[i], re[j]] = [re[j], re[i]]; [im[i], im[j]] = [im[j], im[i]]; }
  }
  for (let len = 2; len <= N; len <<= 1) {
    const a = (-2 * Math.PI) / len, wr = Math.cos(a), wi = Math.sin(a);
    for (let i = 0; i < N; i += len) {
      let cr = 1, ci = 0;
      for (let k = 0; k < len / 2; k++) {
        const ur = re[i + k], ui = im[i + k], vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci, vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
        re[i + k] = ur + vr; im[i + k] = ui + vi; re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
        [cr, ci] = [cr * wr - ci * wi, cr * wi + ci * wr];
      }
    }
  }
  const m = new Float64Array(N / 2);
  for (let k = 0; k < N / 2; k++) m[k] = Math.hypot(re[k], im[k]) + 1e-12;
  return m;
}
// 5 ms envelope and onsets.
const hop = Math.round(SR * 0.005), env = [];
for (let o = 0; o + hop <= x.length; o += hop) { let p = 0; for (let k = 0; k < hop; k++) p = Math.max(p, Math.abs(x[o + k])); env.push(p); }
const onsets = [];
for (let i = 4; i < env.length; i++) {
  const before = Math.max(...env.slice(i - 4, i)) + 1e-4;
  if (env[i] > 0.02 && env[i] / before > 2.5 && (!onsets.length || i - onsets[onsets.length - 1].i > 8)) onsets.push({ i, t: (i * hop) / SR, p: env[i] });
}
const rows = [];
const db = (v) => (20 * Math.log10(v + 1e-9)).toFixed(0).padStart(4);
for (let t = +fromArg; t < Math.min(x.length / SR, +toArg); t += step) {
  const a = Math.floor(t * SR), b = Math.min(x.length, Math.floor((t + step) * SR));
  let s = 0, pk = 0; for (let i = a; i < b; i++) { s += x[i] * x[i]; pk = Math.max(pk, Math.abs(x[i])); }
  const rms = Math.sqrt(s / Math.max(1, b - a));
  // average spectrum over the step
  const acc = new Float64Array(N / 2); let frames = 0;
  for (let o = a; o + N <= b || (frames === 0 && o < b); o += N / 2) { const m = spectrum(o); for (let k = 0; k < N / 2; k++) acc[k] += m[k]; frames++; }
  let num = 0, den = 0, lg = 0, low = 0;
  for (let k = 1; k < N / 2; k++) { const f = (k * SR) / N, p = acc[k] * acc[k]; num += f * p; den += p; lg += Math.log(acc[k]); if (f < 300) low += p; }
  const centroid = num / den, flat = Math.exp(lg / (N / 2 - 1)) / (acc.slice(1).reduce((u, v) => u + v, 0) / (N / 2 - 1));
  const ons = onsets.filter((o) => o.t >= t && o.t < t + step).map((o) => `${o.t.toFixed(2)}(${o.p.toFixed(2)})`);
  rows.push({ t, rms, pk, centroid, flat, low: low / den, ons });
}
for (let i = 0; i < rows.length; i++) {
  const r = rows[i], nb = rows.slice(Math.max(0, i - 3), i + 4);
  const sd = (f) => { const v = nb.map(f), m = v.reduce((u, w) => u + w, 0) / v.length; return Math.sqrt(v.reduce((u, w) => u + (w - m) ** 2, 0) / v.length); };
  const steady = sd((q) => 20 * Math.log10(q.rms + 1e-9)) < 1.2 && sd((q) => q.centroid) < 150;
  console.log(`${r.t.toFixed(2).padStart(6)} s ${db(r.rms)} dB pk ${db(r.pk)}  centroid ${r.centroid.toFixed(0).padStart(5)} Hz  flat ${r.flat.toFixed(2)}  low ${(100 * r.low).toFixed(0).padStart(3)}%  ${steady ? "~" : " "} ${r.ons.join(" ")}`);
}
