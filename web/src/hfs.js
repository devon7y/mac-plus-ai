// A small HFS volume builder: files and folders (both forks, type, creator, Finder flags) in, a raw
// HFS disk image out, for Mini vMac to mount. The structures a Mac Plus needs: boot blocks (empty),
// the master directory block (and its spare copy), the volume bitmap, an empty extents B-tree and a
// catalog B-tree (folder, file and thread records, index nodes when it needs more than one leaf),
// every fork stored contiguously. Catalog names are ordered as the Mac orders them; NAME_ORDER was
// read off hfsutils (the order its catalog keeps one-character names in, case pairs equal).

const NAME_ORDER = [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,39,40,41,42,43,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,0,64,65,66,67,68,69,70,80,81,83,84,89,90,91,92,97,98,99,100,101,103,112,113,114,115,117,118,123,124,125,126,128,129,130,131,132,133,76,70,80,81,83,84,89,90,91,92,97,98,99,100,101,103,112,113,114,115,117,118,123,124,125,126,128,134,135,136,137,138,72,74,82,85,102,104,119,77,71,78,72,73,74,82,85,86,87,88,93,94,95,96,102,108,109,110,104,105,120,121,122,119,139,140,141,142,143,144,145,116,146,147,148,149,150,151,75,106,152,153,154,155,156,157,158,159,160,161,162,79,111,163,75,106,164,165,166,167,168,169,170,37,38,171,32,71,73,105,107,107,172,173,35,36,44,45,174,175,127,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214];

// Mac Roman, bytes 0x80-0xFF.
const MAC_HIGH = "ÄÅÇÉÑÖÜáàâäãåçéèêëíìîïñóòôöõúùûü†°¢£§•¶ß®©™´¨≠ÆØ∞±≤≥¥µ∂∑∏π∫ªºΩæø¿¡¬√ƒ≈∆«»… ÀÃÕŒœ–—“”‘’÷◊ÿŸ⁄€‹›ﬁﬂ‡·‚„‰ÂÊÁËÈÍÎÏÌÓÔÒÚÛÙıˆ˜¯˘˙˚¸˝˛ˇ";
const TO_MAC = new Map([...MAC_HIGH].map((ch, i) => [ch, 0x80 + i]));
export function macRoman(str) {
  const out = [];
  for (const ch of str.normalize("NFC")) {
    const c = ch.codePointAt(0);
    out.push(c < 0x80 ? c : TO_MAC.get(ch) ?? 0x3F);
  }
  return Uint8Array.from(out);
}
export function fromMacRoman(bytes) { let s = ""; for (const b of bytes) s += b < 0x80 ? String.fromCharCode(b) : MAC_HIGH[b - 0x80]; return s; }

function compareNames(a, b) {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i++) { const d = NAME_ORDER[a[i]] - NAME_ORDER[b[i]]; if (d) return d; }
  return a.length - b.length;
}
const cleanName = (bytes, max) => { const b = Uint8Array.from(bytes.slice(0, max)).map((c) => (c === 0x3A ? 0x2F : c)); return b.length ? b : Uint8Array.of(0x3F); };

// entries: [{path: [Uint8Array (Mac Roman) per level], folder: bool, type, creator: 4-byte arrays,
//            flags: Finder flags, data: Uint8Array, rsrc: Uint8Array}]
export function buildHfs(volName, entries, { minBytes = 819200, slack = 1.3, extra = 262144 } = {}) {
  const now = Math.floor(Date.now() / 1000) + 2082844800;           // seconds since 1904
  // The tree of folders (root = 2), deduplicated by name (case-insensitively, as HFS does).
  let nextId = 16;
  const root = { id: 2, parent: 1, name: cleanName(volName, 27), kids: [], folder: true };
  const find = (dir, name) => dir.kids.find((k) => compareNames(k.name, name) === 0 && k.name.length === name.length);
  for (const e of entries) {
    let dir = root;
    for (let i = 0; i < e.path.length; i++) {
      const name = cleanName(e.path[i], 31), last = i === e.path.length - 1;
      let k = find(dir, name);
      if (!k) {
        k = last && !e.folder ? { name, folder: false, e } : { name, folder: true, kids: [] };
        k.id = nextId++; k.parent = dir.id; dir.kids.push(k);
      } else if (last && !e.folder) {                // a second file with the same name: keep both
        let j = 2, nm; do { nm = Uint8Array.from([...name.slice(0, 28), ...macRoman(" " + j++)]); } while (find(dir, nm));
        k = { name: nm, folder: false, e, id: nextId++, parent: dir.id }; dir.kids.push(k);
      }
      if (k.folder) dir = k;
    }
  }
  const all = [];
  (function walk(d) { all.push(d); for (const k of d.kids || []) k.folder ? walk(k) : all.push(k); })(root);
  const files = all.filter((x) => !x.folder), folders = all.filter((x) => x.folder);

  // Sizes: allocation blocks of 512 bytes per 32 MB (HFS counts blocks in 16 bits).
  const forkBytes = files.reduce((s, f) => s + f.e.data.length + f.e.rsrc.length + 1024, 0);
  const catRecords = folders.length * 2 + files.length;
  const leafEstimate = Math.ceil((catRecords * 150) / 480) + 1;
  const catNodes = Math.max(32, 2 * (leafEstimate + Math.ceil(leafEstimate / 10) + 2) + 16);
  const xtNodes = 8;
  let bytes = Math.max(minBytes, Math.ceil(forkBytes * slack) + (catNodes + xtNodes) * 512 + extra);
  bytes = Math.ceil(bytes / 1024) * 1024;
  const sectors = bytes / 512;
  const alBlkSiz = 512 * Math.ceil(sectors / 65000);
  const bitmapSectors = Math.ceil(Math.ceil(sectors / (alBlkSiz / 512)) / 4096);
  const alBlSt = 3 + bitmapSectors;
  const nmAlBlks = Math.floor(((sectors - alBlSt - 2) * 512) / alBlkSiz);
  const img = new Uint8Array(bytes), dv = new DataView(img.buffer);
  const blkOff = (b) => alBlSt * 512 + b * alBlkSiz;
  let nextBlk = 0;
  const alloc = (len) => { const n = Math.ceil(len / alBlkSiz); const s = nextBlk; nextBlk += n; if (nextBlk > nmAlBlks) throw new Error("the files don't fit"); return [s, n]; };
  const [xtStart, xtCount] = alloc(xtNodes * 512), [ctStart, ctCount] = alloc(catNodes * 512);
  for (const f of files) {
    f.d = f.e.data.length ? alloc(f.e.data.length) : [0, 0];
    if (f.e.data.length) img.set(f.e.data, blkOff(f.d[0]));
    f.r = f.e.rsrc.length ? alloc(f.e.rsrc.length) : [0, 0];
    if (f.e.rsrc.length) img.set(f.e.rsrc, blkOff(f.r[0]));
  }

  // Catalog records.
  const key = (parent, name) => { const k = new Uint8Array(1 + 6 + name.length); k[0] = 6 + name.length; new DataView(k.buffer).setUint32(2, parent); k[6] = name.length; k.set(name, 7); return k; };
  const recs = [];
  for (const d of folders) {
    const r = new Uint8Array(70), v = new DataView(r.buffer);
    r[0] = 1; v.setUint16(4, d.kids.length); v.setUint32(6, d.id); v.setUint32(10, now); v.setUint32(14, now);
    // DInfo: window rect (top, left, bottom, right), flags (not inited: the Finder places the icon), view
    v.setInt16(22, 60); v.setInt16(24, 40); v.setInt16(26, 300); v.setInt16(28, 440);
    recs.push({ parent: d.parent, name: d.name, data: r });
    const t = new Uint8Array(46), tv = new DataView(t.buffer);
    t[0] = 3; tv.setUint32(10, d.parent); t[14] = d.name.length; t.set(d.name, 15);
    recs.push({ parent: d.id, name: new Uint8Array(0), data: t });
  }
  for (const f of files) {
    const r = new Uint8Array(102), v = new DataView(r.buffer), e = f.e;
    r[0] = 2;
    r.set(e.type, 4); r.set(e.creator, 8);
    v.setUint16(12, (e.flags || 0) & ~(0x0100 | 0x0001 | 0x4000 * 0));    // not inited, not on the desktop
    v.setUint32(20, f.id);
    v.setUint16(24, f.d[0]); v.setUint32(26, e.data.length); v.setUint32(30, f.d[1] * alBlkSiz);
    v.setUint16(34, f.r[0]); v.setUint32(36, e.rsrc.length); v.setUint32(40, f.r[1] * alBlkSiz);
    v.setUint32(44, now); v.setUint32(48, now);
    v.setUint16(74, f.d[0]); v.setUint16(76, f.d[1]);                   // filExtRec
    v.setUint16(86, f.r[0]); v.setUint16(88, f.r[1]);                   // filRExtRec
    recs.push({ parent: f.parent, name: f.name, data: r });
  }
  recs.sort((a, b) => a.parent - b.parent || compareNames(a.name, b.name));

  // Leaf nodes, then index levels; node 0 is the header.
  const NODE = 512, nodes = [];
  const leafRec = (r) => { const k = key(r.parent, r.name), pad = k.length & 1; const out = new Uint8Array(k.length + pad + r.data.length); out.set(k); out.set(r.data, k.length + pad); return out; };
  const pack = (items, type, height) => {               // fill nodes in order; returns [{recs, first}]
    const out = []; let cur = null, used = 0;
    for (const it of items) {
      if (!cur || used + it.bytes.length + 2 > NODE - 14 - 2) { cur = { type, height, recs: [], first: it }; out.push(cur); used = 0; }
      cur.recs.push(it.bytes); used += it.bytes.length + 2;
    }
    return out;
  };
  let level = pack(recs.map((r) => ({ bytes: leafRec(r), parent: r.parent, name: r.name })), 0xFF, 1);
  const leaves = level;
  const levels = [level];
  while (level.length > 1) {
    const idx = level.map((n) => {
      const k = new Uint8Array(38 + 4), v = new DataView(k.buffer);
      k[0] = 37; v.setUint32(2, n.first.parent); k[6] = n.first.name.length; k.set(n.first.name, 7);
      n.ptrSlot = k;                                       // the child's number goes in at 38 later
      return { bytes: k, parent: n.first.parent, name: n.first.name, child: n };
    });
    level = pack(idx, 0x00, levels.length + 1);
    level.forEach((n) => { n.children = idx.filter((x) => n.recs.includes(x.bytes)).map((x) => x.child); });
    levels.push(level);
  }
  let num = 1;
  for (const lv of levels) for (const n of lv) n.num = num++;
  const used = num;
  if (used > catNodes) throw new Error("too many files for the catalog");
  for (const lv of levels) for (const n of lv) for (const c of n.children || []) new DataView(c.ptrSlot.buffer).setUint32(38, c.num);
  const ctBase = blkOff(ctStart);
  const writeNode = (n, flink, blink) => {
    const o = ctBase + n.num * NODE;
    dv.setUint32(o, flink); dv.setUint32(o + 4, blink); img[o + 8] = n.type; img[o + 9] = n.height; dv.setUint16(o + 10, n.recs.length);
    let p = 14;
    n.recs.forEach((r, i) => { img.set(r, o + p); dv.setUint16(o + NODE - 2 - 2 * i, p); p += r.length; });
    dv.setUint16(o + NODE - 2 - 2 * n.recs.length, p);
  };
  for (const lv of levels) lv.forEach((n, i) => writeNode(n, lv[i + 1] ? lv[i + 1].num : 0, lv[i - 1] ? lv[i - 1].num : 0));
  const header = (base, depth, rootNode, nrecs, fnode, lnode, keyLen, total, inUse) => {
    const o = base;
    img[o + 8] = 1; img[o + 9] = 0; dv.setUint16(o + 10, 3);
    dv.setUint16(o + 14, depth); dv.setUint32(o + 16, rootNode); dv.setUint32(o + 20, nrecs); dv.setUint32(o + 24, fnode); dv.setUint32(o + 28, lnode);
    dv.setUint16(o + 32, NODE); dv.setUint16(o + 34, keyLen); dv.setUint32(o + 36, total); dv.setUint32(o + 40, total - inUse);
    for (let i = 0; i < inUse; i++) img[o + 248 + (i >> 3)] |= 0x80 >> (i & 7);   // the node map
    dv.setUint16(o + 510, 14); dv.setUint16(o + 508, 120); dv.setUint16(o + 506, 248); dv.setUint16(o + 504, 504);
  };
  const top = levels[levels.length - 1][0];
  header(ctBase, levels.length, top.num, recs.length, leaves[0].num, leaves[leaves.length - 1].num, 37, catNodes, used);
  header(blkOff(xtStart), 0, 0, 0, 0, 0, 7, xtNodes, 1);

  // The volume bitmap and the master directory block (and its spare copy near the end).
  for (let b = 0; b < nextBlk; b++) img[3 * 512 + (b >> 3)] |= 0x80 >> (b & 7);
  const mdb = new Uint8Array(162), m = new DataView(mdb.buffer);
  mdb[0] = 0x42; mdb[1] = 0x44;
  m.setUint32(2, now); m.setUint32(6, now); m.setUint16(10, 0x0100);
  m.setUint16(12, root.kids.filter((k) => !k.folder).length); m.setUint16(14, 3); m.setUint16(16, nextBlk);
  m.setUint16(18, nmAlBlks); m.setUint32(20, alBlkSiz); m.setUint32(24, alBlkSiz * 4); m.setUint16(28, alBlSt);
  m.setUint32(30, nextId); m.setUint16(34, nmAlBlks - nextBlk);
  mdb[36] = root.name.length; mdb.set(root.name, 37);
  m.setUint32(74, xtCount * alBlkSiz); m.setUint32(78, ctCount * alBlkSiz);
  m.setUint16(82, root.kids.filter((k) => k.folder).length);
  m.setUint32(84, files.length); m.setUint32(88, folders.length - 1);
  m.setUint32(130, xtCount * alBlkSiz); m.setUint16(134, xtStart); m.setUint16(136, xtCount);
  m.setUint32(146, ctCount * alBlkSiz); m.setUint16(150, ctStart); m.setUint16(152, ctCount);
  img.set(mdb, 1024);
  img.set(mdb, (sectors - 2) * 512);
  return img;
}
