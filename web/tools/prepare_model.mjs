// Convert the 3D Mac for the web: "Macintosh 128K Computer (1984)" by Daz (Sketchfab, CC BY-NC 4.0),
// Devon's glTF export in web/vendor/mac128k_gltf/ (scene.gltf + scene.bin + small PNG textures).
//
//   node web/tools/prepare_model.mjs [in.gltf] [out.glb]
//
// What it does (the page does the rest at load time, see src/view3d.js):
//   * packs everything into one binary .glb (textures embedded; they are tiny flat-colour PNGs)
//   * removes duplicate data, then meshopt-compresses the geometry (EXT_meshopt_compression with
//     quantised attributes); the page decodes it with three.js's own meshopt decoder (no wasm fetch)
//   * keeps every node and its name: each keyboard key is its own node ('Space', 'Return', 'A', ...),
//     which the page animates; nothing is merged or flattened
// The mouse button is not a separate part in the source: the page splits it off the mouse's
// dark-plastic mesh at load time (connected components; see splitMouseButton in view3d.js).
import path from "node:path";
import fs from "node:fs";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

const WEB = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const require = createRequire(path.join(WEB, "build", "package.json"));
const { NodeIO } = require("@gltf-transform/core");
const { ALL_EXTENSIONS } = require("@gltf-transform/extensions");
const { dedup, prune, meshopt } = require("@gltf-transform/functions");
const { MeshoptEncoder, MeshoptDecoder } = require("meshoptimizer");

const inPath = process.argv[2] || path.join(WEB, "vendor", "mac128k_gltf", "scene.gltf");
const outPath = process.argv[3] || path.join(WEB, "build", "model", "mac128k.glb");

await MeshoptEncoder.ready;
await MeshoptDecoder.ready;
const io = new NodeIO().registerExtensions(ALL_EXTENSIONS).registerDependencies({
  "meshopt.encoder": MeshoptEncoder, "meshopt.decoder": MeshoptDecoder,
});
const doc = await io.read(inPath);
const before = doc.getRoot().listNodes().map((n) => n.getName());
await doc.transform(
  dedup(),
  prune({ keepLeaves: true, keepAttributes: true }),
  meshopt({ encoder: MeshoptEncoder, level: "medium" }),
);
const after = new Set(doc.getRoot().listNodes().map((n) => n.getName()));
const lost = before.filter((n) => !after.has(n));
if (lost.length) { console.log("nodes lost:", lost); process.exit(1); }
fs.mkdirSync(path.dirname(outPath), { recursive: true });
await io.write(outPath, doc);
const tris = doc.getRoot().listMeshes().reduce((s, m) => s + m.listPrimitives().reduce((t, p) => t + (p.getIndices() ? p.getIndices().getCount() / 3 : 0), 0), 0);
console.log(`${outPath}: ${(fs.statSync(outPath).size / 1048576).toFixed(2)} MiB, ${after.size} nodes, ${Math.round(tris)} triangles`);
