// Insert Disk: turn a file the visitor chose (or dropped) into a disk image Mini vMac can mount.
//   raw HFS or MFS volumes (.dsk, .img, .image, .hfv, any size), DiskCopy 4.2 images (84-byte
//   header, found by its fields, not the name), a disk inside an Apple partition map (its HFS
//   partition is taken out), and a .zip holding exactly one of those (unzipped here with fflate,
//   self-hosted). Anything else gets a plain explanation of what to do.
// Nothing leaves the browser.

const be32 = (u, o) => ((u[o] << 24) | (u[o + 1] << 16) | (u[o + 2] << 8) | u[o + 3]) >>> 0;
const be16 = (u, o) => (u[o] << 8) | u[o + 1];
const ascii = (u, o, n) => String.fromCharCode(...u.subarray(o, o + n));

export class DiskError extends Error {}

// The volume signature 1024 bytes in: HFS "BD", MFS 0xD2D7, HFS+ "H+" / HFSX "HX".
function volumeKind(u) {
  if (u.length < 1536) return null;
  const sig = be16(u, 1024);
  return sig === 0x4244 ? "HFS" : sig === 0xD2D7 ? "MFS" : sig === 0x482B || sig === 0x4858 ? "HFS+" : null;
}

// One disk image (not an archive): the volume's bytes, or a DiskError saying what it is instead.
export function diskFromBytes(u, fileName) {
  const n = u.length, lower = fileName.toLowerCase();
  // DiskCopy 4.2: name (Pascal string, < 64), data size and tag size at $40/$44, 0x0100 at $52.
  if (n > 84 + 512 && u[0] > 0 && u[0] < 64 && u[0x52] === 0x01 && u[0x53] === 0x00) {
    const dataSize = be32(u, 0x40), tagSize = be32(u, 0x44);
    if (dataSize > 0 && dataSize % 512 === 0 && 84 + dataSize <= n && 84 + dataSize + tagSize <= n + 512) {
      const data = u.subarray(84, 84 + dataSize);
      if (volumeKind(data) === "HFS+") throw new DiskError(hfsPlus(fileName));
      if (volumeKind(data)) return { data, format: "DiskCopy 4.2" };
    }
  }
  const kind = volumeKind(u);
  if (kind === "HFS+") throw new DiskError(hfsPlus(fileName));
  if (kind) return { data: u, format: `raw ${kind}` };
  // An Apple partition map ("ER", then "PM" entries): take the HFS partition.
  if (n > 2048 && u[0] === 0x45 && u[1] === 0x52) {
    const block = be16(u, 2) || 512;
    for (let i = 1; i < 64 && (i + 1) * block <= n; i++) {
      const o = i * block;
      if (u[o] !== 0x50 || u[o + 1] !== 0x4D) break;
      const type = ascii(u, o + 48, 32).replace(/\0.*$/, "");
      if (type === "Apple_HFS") {
        const start = be32(u, o + 8) * block, count = be32(u, o + 12) * block;
        const data = u.subarray(start, Math.min(n, start + count));
        if (volumeKind(data) === "HFS") return { data, format: "HFS partition of a hard disk image" };
      }
    }
  }
  throw new DiskError(notADisk(u, fileName, lower));
}

const hfsPlus = (name) => `“${name}” is a Mac OS Extended (HFS+) disk, made for Mac OS 8.1 or later. This Mac runs System 6, which can only read older HFS and MFS disks.`;

function notADisk(u, name, lower) {
  const head = ascii(u, 0, Math.min(u.length, 120));
  if (head.startsWith("SIT!") || head.startsWith("StuffIt") || /\.(sit|sitx|sea)$/.test(lower))
    return `“${name}” is a StuffIt archive. Unpack it first (for example with The Unarchiver), then insert the disk image that was inside.`;
  if (head.includes("(This file must be converted with BinHex") || /\.hqx$/.test(lower))
    return `“${name}” is BinHex text. Unpack it first (for example with The Unarchiver), then insert the disk image that was inside.`;
  if (u.length > 1024 && ascii(u, u.length - 512, 4) === "koly" || /\.dmg$/.test(lower))
    return `“${name}” is a modern Mac .dmg, not a classic disk image. Open it on a Mac (or unpack it with The Unarchiver) and look for a .dsk, .img or .image inside.`;
  if (/\.(bin|macbin)$/.test(lower) || (u.length > 128 && u[0] === 0 && u[1] > 0 && u[1] < 64 && u[74] === 0 && u[82] === 0))
    return `“${name}” is a MacBinary file (one Mac file, not a disk). If it holds a disk image, unpack it first (for example with The Unarchiver); a single app needs to be put on a disk image.`;
  if (/\.(7z|rar|tar|gz|tgz|bz2|xz)$/.test(lower))
    return `“${name}” is an archive. Unpack it first and insert the disk image that was inside (.dsk, .img or .image).`;
  return `“${name}” doesn't look like a Mac disk image. Disk images usually end in .dsk, .img, .image or .hfv; archives (.sit, .hqx, .bin, .zip with several files) need unpacking first.`;
}

// A chosen or dropped file -> the disks to insert: a disk image as it is; an archive or encoded
// file (.sit, .sea, .hqx, .bin, .zip, stacked any way) unpacked, and either the disk images inside
// it, or a new HFS disk holding its files and folders (named after it, with room for saves).
export async function disksFromFile(file, { fflateUrl, wasmUrl, progress = () => {} }) {
  if (file.size > 64 * 1048576) throw new DiskError(`“${file.name}” is ${(file.size / 1048576).toFixed(0)} MB. Files for this Mac can be at most 64 MB.`);
  const u = new Uint8Array(await file.arrayBuffer());
  const diskOf = (b, name) => { try { return diskFromBytes(b, name); } catch { return null; } };
  const direct = diskOf(u, file.name);
  if (direct) return [{ name: file.name, data: direct.data, format: direct.format }];
  const { unpackAll, UnpackError, NotPacked } = await import("./unpack.js");
  const { buildHfs, macRoman, fromMacRoman } = await import("./hfs.js");
  let res;
  try {
    res = await unpackAll({ name: macRoman(file.name), data: u, rsrc: new Uint8Array(0), type: new Uint8Array(4), creator: new Uint8Array(4), flags: 0 },
      { diskOf, wasmUrl, fflateUrl, progress });
  } catch (e) {
    if (e instanceof NotPacked) diskFromBytes(u, file.name);      // throws the explanation
    if (e instanceof UnpackError) throw new DiskError(e.message);
    throw new DiskError(`“${file.name}” couldn't be unpacked (${e.message || e}).`);
  }
  if (res.disks.length) return res.disks.map((d) => ({ name: d.name, data: d.data, format: `${d.format}, from ${file.name}` }));
  const items = res.files.filter((f) => f.folder || f.data.length || f.rsrc.length);
  if (!items.some((f) => !f.folder)) throw new DiskError(`“${file.name}” has no files in it.`);
  // One folder holding everything: its contents go on the disk, which takes the folder's name.
  let vol = file.name.replace(/(\.(sit|sea|hqx|bin|zip|cpt|macbin))+$/i, "");
  const tops = new Set(items.map((f) => fromMacRoman(f.path[0])));
  if (tops.size === 1 && items.every((f) => f.path.length > 1 || f.folder)) {
    vol = [...tops][0];
    for (const f of items) f.path = f.path.slice(1);
  }
  const entries = items.filter((f) => f.path.length).map((f) => ({ path: f.path, folder: !!f.folder, type: f.type, creator: f.creator, flags: f.flags || 0, data: f.data, rsrc: f.rsrc }));
  progress(`Making a disk for ${vol}…`);
  const data = buildHfs(macRoman(vol.slice(0, 27)), entries);
  const nf = entries.filter((e) => !e.folder).length;
  return [{ name: file.name, data, format: `a new disk “${vol}” with ${nf} file${nf === 1 ? "" : "s"} from ${file.name}` }];
}

// (Kept for disk images only.)
export async function diskFromFile(file, fflateUrl) {
  if (file.size > 64 * 1048576) throw new DiskError(`“${file.name}” is ${(file.size / 1048576).toFixed(0)} MB. Disk images for this Mac are at most 64 MB.`);
  const u = new Uint8Array(await file.arrayBuffer());
  if (u.length >= 4 && u[0] === 0x50 && u[1] === 0x4B && u[2] === 0x03 && u[3] === 0x04) {
    const { unzipSync } = await import(fflateUrl);
    let files;
    try { files = unzipSync(u, { filter: (f) => !/(^|\/)(__MACOSX|\._)/.test(f.name) && !f.name.endsWith("/") }); }
    catch { throw new DiskError(`“${file.name}” couldn't be unzipped.`); }
    const found = [];
    const problems = [];
    for (const [name, bytes] of Object.entries(files)) {
      try { found.push({ name: name.split("/").pop(), ...diskFromBytes(bytes, name.split("/").pop()) }); }
      catch (e) { problems.push(e.message); }
    }
    if (found.length === 1) return { name: found[0].name, data: found[0].data, format: `${found[0].format}, from ${file.name}` };
    if (found.length > 1) throw new DiskError(`“${file.name}” holds ${found.length} disk images. Unzip it and insert them one at a time.`);
    throw new DiskError(problems.length === 1 ? problems[0] : `“${file.name}” has no Mac disk image in it.`);
  }
  const d = diskFromBytes(u, file.name);
  return { name: file.name, data: d.data, format: d.format };
}
