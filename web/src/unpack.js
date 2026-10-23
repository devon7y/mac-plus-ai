// Unpacking classic Mac downloads in the page, for Insert Disk: BinHex 4.0 (.hqx), MacBinary I/II/III
// (.bin), StuffIt 1.x-5.x including Arsenic (.sit, and the archive inside a self-extracting .sea;
// decoded by the `stuffit` crate compiled to WebAssembly, sit.wasm), .zip (fflate, with Mac
// resource forks from __MACOSX AppleDouble files put back), recursively. A "file" here is
//   { name: Uint8Array (Mac Roman), type, creator: 4-byte arrays, flags, data, rsrc, path: [names] }.
import { macRoman, fromMacRoman } from "./hfs.js";

export class UnpackError extends Error {}
export class NotPacked extends Error {}       // the file wasn't an archive or disk at all
const latin1 = new TextDecoder("latin1");
const ascii = (u, o, n) => latin1.decode(u.subarray(o, o + n));
const be32 = (u, o) => ((u[o] << 24) | (u[o + 1] << 16) | (u[o + 2] << 8) | u[o + 3]) >>> 0;
const be16 = (u, o) => (u[o] << 8) | u[o + 1];
const bytes4 = (s) => Uint8Array.from(s, (c) => c.charCodeAt(0) & 255);

// ---------------------------------------------------------------- BinHex 4.0
const HQX = "!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr";
export function isBinHex(u) {
  const head = ascii(u, 0, Math.min(u.length, 4096));
  return head.includes("(This file must be converted with BinHex");
}
export function decodeBinHex(u) {
  const text = ascii(u, 0, u.length);
  const start = text.indexOf(":", text.indexOf("(This file must be converted with BinHex"));
  if (start < 0) throw new UnpackError("damaged BinHex file");
  const map = new Int16Array(256).fill(-1);
  for (let i = 0; i < HQX.length; i++) map[HQX.charCodeAt(i)] = i;
  const six = [];
  for (let i = start + 1; i < text.length; i++) {
    const c = text.charCodeAt(i);
    if (c === 0x3A) break;
    const v = map[c]; if (v >= 0) six.push(v);
  }
  const packed = new Uint8Array(Math.floor((six.length * 6) / 8));
  for (let i = 0, bits = 0, acc = 0, o = 0; i < six.length; i++) {
    acc = (acc << 6) | six[i]; bits += 6;
    if (bits >= 8) { bits -= 8; packed[o++] = (acc >> bits) & 255; }
  }
  const out = [];                                        // run-length decoding (0x90 n: repeat)
  for (let i = 0, last = 0; i < packed.length; i++) {
    const b = packed[i];
    if (b === 0x90) { const n = packed[++i]; if (n === 0) { out.push(0x90); last = 0x90; } else for (let k = 1; k < n; k++) out.push(last); }
    else { out.push(b); last = b; }
  }
  const d = Uint8Array.from(out);
  const nl = d[0], o = 1 + nl + 1;
  const dataLen = be32(d, o + 10), rsrcLen = be32(d, o + 14), p = o + 20;
  if (p + dataLen + 2 + rsrcLen > d.length + 2) throw new UnpackError("damaged BinHex file");
  return { name: d.slice(1, 1 + nl), type: d.slice(o, o + 4), creator: d.slice(o + 4, o + 8), flags: be16(d, o + 8),
    data: d.slice(p, p + dataLen), rsrc: d.slice(p + dataLen + 2, p + dataLen + 2 + rsrcLen) };
}

// ---------------------------------------------------------------- MacBinary I/II/III
export function isMacBinary(u) {
  if (u.length < 128 || u[0] !== 0 || u[74] !== 0 || u[82] !== 0 || u[1] < 1 || u[1] > 63) return false;
  const dl = be32(u, 83), rl = be32(u, 87);
  if (dl > 0x7FFFFF || rl > 0x7FFFFF) return false;
  const need = 128 + Math.ceil(dl / 128) * 128 + rl;
  return need <= u.length + 128 && (dl + rl > 0);
}
export function decodeMacBinary(u) {
  const dl = be32(u, 83), rl = be32(u, 87), second = be16(u, 120);            // MacBinary II's secondary header
  const dataStart = 128 + Math.ceil(second / 128) * 128, rsrcStart = dataStart + Math.ceil(dl / 128) * 128;
  return { name: u.slice(2, 2 + u[1]), type: u.slice(65, 69), creator: u.slice(69, 73), flags: (u[73] << 8) | u[101],
    data: u.slice(dataStart, dataStart + dl), rsrc: u.slice(rsrcStart, rsrcStart + rl) };
}

// ---------------------------------------------------------------- StuffIt (sit.wasm)
const SIT_SIGS = ["SIT!", "STi4", "StuffIt (c)"];
export function stuffItOffset(u, scan = false) {
  if (u.length > 14 && (ascii(u, 0, 4) === "SIT!" && ascii(u, 10, 4) === "rLau" || ascii(u, 0, 11) === "StuffIt (c)")) return 0;
  if (!scan) return -1;
  // A self-extracting archive: the StuffIt archive follows the extractor in the data fork.
  for (let i = 1; i + 14 < u.length; i++) {
    if (u[i] === 0x53 && (ascii(u, i, 4) === "SIT!" && ascii(u, i + 10, 4) === "rLau" || ascii(u, i, 11) === "StuffIt (c)")) return i;
  }
  return -1;
}
export const isStuffItX = (u) => ascii(u, 0, 8) === "StuffIt!" || ascii(u, 0, 8) === "StuffIt?";
let sitModule = null;
async function sitUnpack(u, wasmUrl) {
  if (!sitModule) {
    try { sitModule = await WebAssembly.compileStreaming(fetch(wasmUrl)); }
    catch { sitModule = await WebAssembly.compile(await (await fetch(wasmUrl)).arrayBuffer()); }
  }
  const { exports: e } = await WebAssembly.instantiate(sitModule, {});
  const p = e.sit_alloc(u.length);
  new Uint8Array(e.memory.buffer, p, u.length).set(u);
  const k = e.sit_unpack(p, u.length);
  if (k < 0) throw new UnpackError(k === -1 ? "this StuffIt archive couldn't be read (damaged, or a kind not supported)" : "a file in this StuffIt archive uses a compression method that isn't supported");
  const out = new Uint8Array(e.memory.buffer, e.sit_out_ptr(), e.sit_out_len()).slice();
  const dv = new DataView(out.buffer), files = [];
  for (let i = 0, o = 0; i < k; i++) {
    const folder = out[o] === 1; o += 1;
    const nl = dv.getUint16(o); o += 2;
    const name = new TextDecoder().decode(out.subarray(o, o + nl)); o += nl;
    const type = out.slice(o, o + 4), creator = out.slice(o + 4, o + 8); o += 8;
    const flags = dv.getUint16(o); o += 2;
    const dl = dv.getUint32(o); o += 4; const data = out.slice(o, o + dl); o += dl;
    const rl = dv.getUint32(o); o += 4; const rsrc = out.slice(o, o + rl); o += rl;
    files.push({ folder, path: name.split("/").filter(Boolean).map((s) => macRoman(s)), type, creator, flags, data, rsrc });
  }
  return files;
}

// ---------------------------------------------------------------- zip (with AppleDouble forks)
async function zipUnpack(u, fflateUrl) {
  const { unzipSync } = await import(fflateUrl);
  let all;
  try { all = unzipSync(u); } catch { throw new UnpackError("this .zip couldn't be unzipped"); }
  const doubles = new Map(), files = [];
  for (const [path, data] of Object.entries(all)) {
    if (path.endsWith("/")) continue;
    const parts = path.split("/");
    if (parts[0] === "__MACOSX" && parts[parts.length - 1].startsWith("._")) {
      doubles.set([...parts.slice(1, -1), parts[parts.length - 1].slice(2)].join("/"), data);
      continue;
    }
    if (parts.some((p) => p.startsWith("._") || p === ".DS_Store")) continue;
    files.push({ path: parts.map((s) => macRoman(s)), key: path, type: bytes4("????"), creator: bytes4("????"), flags: 0, data, rsrc: new Uint8Array(0) });
  }
  for (const f of files) {                               // AppleDouble: resource fork (2), Finder info (9)
    const ad = doubles.get(f.key);
    if (!ad || be32(ad, 0) !== 0x00051607) continue;
    const n = be16(ad, 24);
    for (let i = 0; i < n; i++) {
      const id = be32(ad, 26 + 12 * i), off = be32(ad, 30 + 12 * i), len = be32(ad, 34 + 12 * i);
      if (id === 2) f.rsrc = ad.slice(off, off + len);
      if (id === 9 && len >= 10) { f.type = ad.slice(off, off + 4); f.creator = ad.slice(off + 4, off + 8); f.flags = be16(ad, off + 8); }
    }
  }
  return files;
}

// ---------------------------------------------------------------- recursion
// What a file turns into: disk images (raw HFS/MFS or DiskCopy, by content), and Mac files.
// diskOf(bytes, name) returns {data, format} for a disk image, or null.
export async function unpackAll(top, { diskOf, wasmUrl, fflateUrl, progress = () => {} }) {
  const disks = [], files = [];
  async function visit(f, path, depth) {
    if (depth > 6) { files.push({ ...f, path }); return; }
    const u = f.data, label = fromMacRoman(f.name || path[path.length - 1] || new Uint8Array(0));
    const disk = u.length ? diskOf(u, label) : null;
    if (disk) { disks.push({ name: label, ...disk }); return; }
    if (u.length >= 4 && u[0] === 0x50 && u[1] === 0x4B && u[2] === 0x03 && u[3] === 0x04) {
      progress(`Unzipping ${label}…`);
      for (const g of await zipUnpack(u, fflateUrl)) await visit({ ...g, name: g.path[g.path.length - 1] }, [...path.slice(0, -1), ...g.path], depth + 1);
      return;
    }
    if (isBinHex(u)) { progress(`Decoding ${label}…`); const g = decodeBinHex(u); await visit(g, [...path.slice(0, -1), g.name], depth + 1); return; }
    if (depth === 0 && isMacBinary(u) || isMacBinary(u) && /\.(bin|macbin)$/i.test(label)) {
      const g = decodeMacBinary(u); await visit(g, [...path.slice(0, -1), g.name], depth + 1); return;
    }
    if (isStuffItX(u)) throw new UnpackError(`“${label}” is a StuffIt X (.sitx) archive, which this page can't unpack. Unpack it with StuffIt Expander or The Unarchiver, then insert what was inside.`);
    const typ = ascii(f.type || new Uint8Array(4), 0, 4), cre = ascii(f.creator || new Uint8Array(4), 0, 4);
    const sea = typ === "APPL" && (cre === "aust" || /\.sea$/i.test(label)) || /\.sea$/i.test(label);
    const at = stuffItOffset(u, sea || typ === "SIT!" || typ === "SITD" || typ === "SIT5");
    if (at >= 0) {
      progress(`Unstuffing ${label}…`);
      const inner = await sitUnpack(at ? u.subarray(at) : u, wasmUrl);
      const base = path.slice(0, -1);
      for (const g of inner) {
        if (g.folder) { files.push({ ...g, path: [...base, ...g.path] }); continue; }
        await visit({ ...g, name: g.path[g.path.length - 1] }, [...base, ...g.path], depth + 1);
      }
      return;
    }
    if (typ === "PACT" || /\.cpt$/i.test(label)) throw new UnpackError(`“${label}” is a Compact Pro archive, which this page can't unpack. Unpack it with The Unarchiver, then insert what was inside.`);
    if (depth === 0) throw new NotPacked();
    files.push({ ...f, path });
  }
  await visit(top, [top.name], 0);
  return { disks, files };
}
