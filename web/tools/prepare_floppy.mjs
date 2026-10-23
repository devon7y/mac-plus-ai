// The 3D floppy disk for Insert Disk: "Floppy disk" by drumdorf (Sketchfab), CC BY-SA 4.0
// (web/vendor/floppy/license.txt). Modified here, and the result is CC BY-SA 4.0 as well:
//   * specular-glossiness material converted to metal-roughness (three.js reads that natively)
//   * its 2048² PNG textures resized to 512² and stored as WebP (about 5.6 MB -> about 0.1 MB)
//   * packed into one .glb
// The page scales it to a real 3.5" disk and orients it (src/view3d.js).
//   node web/tools/prepare_floppy.mjs [in.gltf] [out.glb]
import path from "node:path";
import fs from "node:fs";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

const WEB = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const require = createRequire(path.join(WEB, "build", "package.json"));
const { NodeIO } = require("@gltf-transform/core");
const { ALL_EXTENSIONS } = require("@gltf-transform/extensions");
const { metalRough, textureCompress, dedup, prune } = require("@gltf-transform/functions");
const sharp = require("sharp");

const inPath = process.argv[2] || path.join(WEB, "vendor", "floppy", "scene.gltf");
const outPath = process.argv[3] || path.join(WEB, "build", "model", "floppy.glb");
const io = new NodeIO().registerExtensions(ALL_EXTENSIONS);
const doc = await io.read(inPath);
await doc.transform(
  metalRough(),
  dedup(),
  prune(),
  textureCompress({ encoder: sharp, targetFormat: "webp", resize: [512, 512], quality: 82 }),
);
fs.mkdirSync(path.dirname(outPath), { recursive: true });
await io.write(outPath, doc);
console.log(`${outPath}: ${(fs.statSync(outPath).size / 1024).toFixed(0)} KB`);
