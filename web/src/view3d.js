// The 3D view: the emulated Mac Plus's screen on a 3D Macintosh 128K in a dark photo studio.
// Model: "Macintosh 128K Computer (1984)" by Daz (Sketchfab), CC BY-NC 4.0; converted for the web by
// tools/prepare_model.mjs. Units are centimetres, y is up, the Mac faces +z.
//
// The screen is drawn in WebGL: the emulator's canvas is the texture of a mesh that hugs the
// curved CRT glass, through a CRT shader (crisp pixels, scanlines when they are big enough to see,
// the Mac Plus's blue-white phosphor, glow, edge falloff), its light added onto the polished glass,
// which reflects a studio. Pointer input is mapped back by raycasting that same mesh (its UV is the Mac
// pixel), so what you click is exactly the pixel drawn under the pointer.
//
// Also pickable: every key of the 3D keyboard (click to type; Shift, Option and Command latch until
// the next key; Caps Lock locks), the 3D mouse's button (clicks at the Mac cursor) and the mouse's
// body (drag it to move the Mac cursor). Anything else: drag to turn the view; it holds for 0.5 s,
// then eases back to the front. Wheel or pinch zooms along the line to the screen's centre. It opens
// head-on on the whole desk and dollies in to the working zoom.
import * as THREE from "three";
import { GLTFLoader } from "three/addons/loaders/GLTFLoader.js";
import { MeshoptDecoder } from "three/addons/libs/meshopt_decoder.module.js";

// Browser key (KeyboardEvent.code) -> key node of the 128K's keyboard (Apple M0110: no arrows, Esc,
// Control or keypad). The model names its key nodes by legend: bottom row Option.001, Start
// (Command), Space, Enter, Option; Shift.001 Z X C V B N M < > ? Shift; CapLock A S D F G H J K L ; '
// Return; Tab Q W E R T Y U I O P [ ] / (the backslash key); ~ (the ` key) 1 2 3 4 5 6 7 8 9 0 - +
// Backspace. Missing keys map to the key doing that job here: Esc -> ` (our games read ` as Esc),
// Ctrl and Cmd -> Command, keypad digits -> number row, keypad Enter -> Enter. Arrows: none.
const KEY_NODES = {
  Backquote: "~", Escape: "~", IntlBackslash: "~",
  Digit1: "1", Digit2: "2", Digit3: "3", Digit4: "4", Digit5: "5", Digit6: "6", Digit7: "7",
  Digit8: "8", Digit9: "9", Digit0: "0", Minus: "-", Equal: "+", Backspace: "Backspace",
  Tab: "Tab", KeyQ: "Q", KeyW: "W", KeyE: "E", KeyR: "R", KeyT: "T", KeyY: "Y", KeyU: "U",
  KeyI: "I", KeyO: "O", KeyP: "P", BracketLeft: "[", BracketRight: "]", Backslash: "/",
  CapsLock: "CapLock", KeyA: "A", KeyS: "S", KeyD: "D", KeyF: "F", KeyG: "G", KeyH: "H",
  KeyJ: "J", KeyK: "K", KeyL: "L", Semicolon: ";", Quote: "'", Enter: "Return",
  ShiftLeft: "Shift.001", KeyZ: "Z", KeyX: "X", KeyC: "C", KeyV: "V", KeyB: "B", KeyN: "N",
  KeyM: "M", Comma: "<", Period: ">", Slash: "?", ShiftRight: "Shift",
  AltLeft: "Option.001", AltRight: "Option", MetaLeft: "Start", MetaRight: "Start",
  ControlLeft: "Start", ControlRight: "Start", Space: "Space", NumpadEnter: "Enter",
  Numpad0: "0", Numpad1: "1", Numpad2: "2", Numpad3: "3", Numpad4: "4", Numpad5: "5",
  Numpad6: "6", Numpad7: "7", Numpad8: "8", Numpad9: "9",
};
// Key node -> Mac key code, for clicks on the 3D keyboard.
const NODE_MAC = {
  A: 0x00, S: 0x01, D: 0x02, F: 0x03, H: 0x04, G: 0x05, Z: 0x06, X: 0x07, C: 0x08, V: 0x09, B: 0x0b,
  Q: 0x0c, W: 0x0d, E: 0x0e, R: 0x0f, Y: 0x10, T: 0x11, 1: 0x12, 2: 0x13, 3: 0x14, 4: 0x15, 6: 0x16,
  5: 0x17, "+": 0x18, 9: 0x19, 7: 0x1a, "-": 0x1b, 8: 0x1c, 0: 0x1d, "]": 0x1e, O: 0x1f, U: 0x20,
  "[": 0x21, I: 0x22, P: 0x23, Return: 0x24, L: 0x25, J: 0x26, "'": 0x27, K: 0x28, ";": 0x29,
  "/": 0x2a, "<": 0x2b, "?": 0x2c, N: 0x2d, M: 0x2e, ">": 0x2f, Tab: 0x30, Space: 0x31, "~": 0x32,
  Backspace: 0x33, Enter: 0x34, Start: 0x37, "Shift.001": 0x38, Shift: 0x38, CapLock: 0x39,
  "Option.001": 0x3a, Option: 0x3a,
};
const LATCHING = new Set(["Shift.001", "Shift", "Option.001", "Option", "Start"]);   // like the SE's on-screen keyboards
const LOCKING = new Set(["CapLock"]);                                               // mechanically locking on the M0110

const KEY_TRAVEL = 0.25;        // cm: a 2.5 mm press
const KEY_TIME = 40;            // ms each way
const BUTTON_TRAVEL = 0.12;     // cm
const SCREEN_W = 512, SCREEN_H = 342;
const IMAGE_OF_GLASS = 0.905;   // the picture's width as a share of the glass's width
const ELEVATION = 0.2;          // rad: the front view looks down this much more than the screen's normal
const FOV = 30;                 // degrees, vertical, of the whole viewport
const HOLD_MS = 500, RETURN_MS = 1500;   // snap-back: hold where the user left it, then ease home

// Quality levels. "auto" picks one from the GPU and steps down if frames are slow.
export const QUALITY = {
  high: { dpr: 2, shadow: 2048, contact: 512, physical: true, clearcoat: true, antialias: true, peel: 2048, crt: 2 },
  medium: { dpr: 1.5, shadow: 1024, contact: 256, physical: true, clearcoat: false, antialias: true, peel: 1024, crt: 1 },
  low: { dpr: 1, shadow: 0, contact: 256, physical: false, clearcoat: false, antialias: false, peel: 512, crt: 0 },
};
export function detectQuality() {
  try {
    const c = document.createElement("canvas");
    const gl = c.getContext("webgl2") || c.getContext("webgl");
    if (!gl) return "low";
    const info = gl.getExtension("WEBGL_debug_renderer_info");
    const name = info ? String(gl.getParameter(info.UNMASKED_RENDERER_WEBGL)) : "";
    gl.getExtension("WEBGL_lose_context")?.loseContext();
    if (/swiftshader|llvmpipe|software|basic render/i.test(name)) return "low";
    const small = Math.min(screen.width, screen.height) < 600;
    if (small || /mali-[gt][0-9]|adreno \(tm\) [3-5]|powervr|intel\(r\) (hd|uhd) graphics [2-5]/i.test(name)) return "medium";
    return "high";
  } catch { return "medium"; }
}

const easeInOut = (k) => (k < 0.5 ? 4 * k * k * k : 1 - Math.pow(-2 * k + 2, 3) / 2);
const lerp = (a, b, t) => a + (b - a) * t;

// The CRT. vUv (0,0) is the bottom left of the Mac screen.
const CRT_VERTEX = /* glsl */`
varying vec2 vUv;
void main() { vUv = uv; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }`;
const CRT_FRAGMENT = /* glsl */`
uniform sampler2D map;
uniform vec2 texSize;
uniform vec3 tint;
uniform float glow, scan, time, noise;
uniform float fx;               // 0: a still picture; 1: the live CRT effects below
uniform sampler2D after;        // phosphor afterglow: the last frames, decayed (high quality only)
uniform float afterAmt;
varying vec2 vUv;
float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
void main() {
  // Crisp pixels at any size ("sharp bilinear"): each Mac pixel is flat, with a one-screen-pixel
  // soft edge, so text stays sharp up close and doesn't shimmer when small.
  vec2 texel = vUv * texSize;
  vec2 scale = 1.0 / max(fwidth(texel), vec2(1e-4));
  vec2 cd = fract(texel) - 0.5;
  vec2 range = 0.5 - 0.5 / max(scale * 1.6, vec2(1.0));
  vec2 f = clamp((cd - clamp(cd, -range, range)) * scale * 1.6 + 0.5, 0.0, 1.0);   // ~0.6 px edges: dark strokes stay dark
  float v = texture2D(map, (floor(texel) + f) / texSize).r;
  // Phosphor glow: a blurred copy (a smaller mipmap level) of the screen added on top.
  float g = texture2D(map, vUv, 3.0).r;
  // Scanlines, only once a Mac row is big enough on screen to show them without moire.
  float row = fract(texel.y) - 0.5;
  float k = scan * smoothstep(1.8, 3.2, scale.y);
  float s = mix(1.0, 0.6 + 0.4 * cos(6.2831853 * row), k);
  // Edge falloff of the raster and a soft vignette.
  vec2 c = vUv * 2.0 - 1.0;
  float vig = 1.0 - 0.17 * dot(c * vec2(0.8, 1.0), c * vec2(0.8, 1.0));
  vec2 e = smoothstep(vec2(0.0), vec2(0.013, 0.02), vUv) * smoothstep(vec2(0.0), vec2(0.013, 0.02), 1.0 - vUv);
  // Light emitted on top of the glass (the material blends additively): a black pixel is unlit
  // phosphor and adds nothing, so it looks exactly like the bare glass around the raster, with the
  // same shading and reflections. Faint flicker on the lit phosphor only.
  // Live CRT (fx = 0 turns it off): a steady flicker of about ±4% (a 7.5 Hz ripple on a slow 0.4 Hz
  // swell, like a camera seeing a 60.15 Hz tube), a soft brighter band rolling down the screen every
  // 6.5 s (+8%), animated grain (±3%), the glow breathing (±15% of the glow), and the phosphor's
  // afterglow: what was lit a moment ago fades over a few frames.
  float tt = time;
  float flick = 1.0 + fx * (0.024 * sin(6.2831853 * 7.5 * tt) + 0.016 * sin(6.2831853 * 0.4 * tt));
  float bandY = fract(tt / 6.5) * 1.3 - 0.15;
  float band = 1.0 + fx * 0.08 * exp(-pow(((1.0 - vUv.y) - bandY) / 0.09, 2.0));
  float n = (hash(floor(texel) + fract(tt * 0.37)) - 0.5) * noise * fx;
  float gl = glow * (1.0 + fx * 0.15 * sin(6.2831853 * 0.23 * tt));
  if (afterAmt > 0.0) v = max(v, afterAmt * texture2D(after, (floor(texel) + f) / texSize).r);
  vec3 col = tint * ((v * s + gl * g) * vig * e.x * e.y * flick * band * (1.0 + n));
  gl_FragColor = vec4(max(col, 0.0), 1.0);
  #include <colorspace_fragment>
}`;
// Debug encoding of the Mac pixel under each screen pixel, for the alignment test.
const UVCODE_FRAGMENT = /* glsl */`
varying vec2 vUv;
void main() {
  float x = clamp(floor(vUv.x * 512.0), 0.0, 511.0), y = clamp(floor((1.0 - vUv.y) * 342.0), 0.0, 341.0);
  gl_FragColor = vec4(mod(x, 256.0) / 255.0, (floor(x / 256.0) + 16.0 * floor(y / 256.0)) / 255.0, mod(y, 256.0) / 255.0, 1.0);
}`;
const BLUR_VERTEX = /* glsl */`varying vec2 vUv; void main() { vUv = uv; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }`;
const BLUR_FRAGMENT = /* glsl */`
uniform sampler2D tDiffuse; uniform vec2 step; varying vec2 vUv;
void main() {
  vec4 s = texture2D(tDiffuse, vUv) * 0.1633;
  s += (texture2D(tDiffuse, vUv - step) + texture2D(tDiffuse, vUv + step)) * 0.1531;
  s += (texture2D(tDiffuse, vUv - 2.0 * step) + texture2D(tDiffuse, vUv + 2.0 * step)) * 0.12245;
  s += (texture2D(tDiffuse, vUv - 3.0 * step) + texture2D(tDiffuse, vUv + 3.0 * step)) * 0.0918;
  s += (texture2D(tDiffuse, vUv - 4.0 * step) + texture2D(tDiffuse, vUv + 4.0 * step)) * 0.051;
  gl_FragColor = s;
}`;

export async function createView3D(opts) {
  const { container, canvas: emuCanvas, modelUrl, onProgress, mac, insets, onPlaced } = opts;
  const sound = opts.sound || (() => {});      // click sounds: 3D keys and mouse button only
  const reduceMotion = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  let qualityName = opts.quality && QUALITY[opts.quality] ? opts.quality : detectQuality();
  let Q = QUALITY[qualityName];
  const autoQuality = !(opts.quality && QUALITY[opts.quality]);

  // ---------------------------------------------------------------- renderer
  const renderer = new THREE.WebGLRenderer({ antialias: Q.antialias, powerPreference: "high-performance" });
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, Q.dpr));
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  renderer.toneMapping = THREE.AgXToneMapping;
  renderer.toneMappingExposure = 0.92;
  renderer.shadowMap.enabled = Q.shadow > 0;
  renderer.shadowMap.type = THREE.PCFShadowMap;      // (VSM bled light and banded on the bezel's shallow recess faces)
  // Softer PCF: three.js filters with 5 rotated taps; 16 give a wide penumbra without visible grain,
  // so shadow edges stay soft even close up. The shadow map itself is drawn once (see frame()).
  if (!THREE.ShaderChunk.shadowmap_pars_fragment.includes("SOFT16")) {
    const before = THREE.ShaderChunk.shadowmap_pars_fragment;
    THREE.ShaderChunk.shadowmap_pars_fragment = before.replace(
      /shadow = \(\s*texture\( shadowMap, vec3\( shadowCoord\.xy \+ vogelDiskSample\( 0, 5, phi \) \* radius, shadowCoord\.z \) \) \+[\s\S]*?\) \* 0\.2;/,
      "shadow = 0.0; /* SOFT16 */ for ( int i = 0; i < 16; i ++ ) shadow += texture( shadowMap, vec3( shadowCoord.xy + vogelDiskSample( i, 16, phi ) * radius, shadowCoord.z ) ); shadow /= 16.0;");
    if (THREE.ShaderChunk.shadowmap_pars_fragment === before) console.warn("soft shadows: three.js PCF code not found");
  }
  renderer.shadowMap.autoUpdate = false;
  const canvas = renderer.domElement;
  canvas.className = "gl";
  container.prepend(canvas);

  const scene = new THREE.Scene();
  const BG = new THREE.Color(0x0b0b0c);
  scene.background = BG;
  scene.fog = new THREE.Fog(BG, 150, 380);

  // Image-based lighting: a dark studio with softboxes, built here (no download, so nothing
  // cross-origin under COEP) and prefiltered with PMREM.
  const pmrem = new THREE.PMREMGenerator(renderer);
  scene.environment = pmrem.fromScene(studioScene(), 0.03).texture;
  scene.environmentIntensity = 0.85;

  // ---------------------------------------------------------------- the model
  const loader = new GLTFLoader().setMeshoptDecoder(MeshoptDecoder);
  const gltf = await new Promise((resolve, reject) =>
    loader.load(modelUrl, resolve, (e) => onProgress && e.total && onProgress(e.loaded / e.total), reject));
  const model = gltf.scene;
  scene.add(model);
  model.updateMatrixWorld(true);
  let glass = null, mouseNode = null, mouseDark = null, mouseLight = null;
  model.traverse((o) => {
    if (o.isMesh) {
      if (o.material && o.material.name === "Screen") glass = o;
      if (o.name === "Mouse_Darkplastic_0") mouseDark = o;
      if (o.name === "Mouse_Lightplastic_0") mouseLight = o;
    }
    if (o.name === "Mouse") mouseNode = o;
  });
  if (!glass) throw new Error("the model has no Screen mesh");
  const peel = makePeelTexture(Q.peel);
  peel.anisotropy = Math.min(8, renderer.capabilities.getMaxAnisotropy());   // grazing faces (the recess walls) stay even
  dressMaterials(model, Q, peel);

  const box = new THREE.Box3().setFromObject(model);
  const mid = box.getCenter(new THREE.Vector3());
  const floorY = box.min.y;

  // ---------------------------------------------------------------- lights
  // The key: a broad overhead source (like a ceiling softbox), a little in front of the Mac and angled
  // down, so the top of the case catches the most light and the front falls off gently; no spotlight
  // pool. Its shadows fall mostly downward, softened (PCF) and widened by the contact shadow.
  const key = new THREE.DirectionalLight(0xfff4e8, 1.9);
  key.position.copy(mid).add(new THREE.Vector3(-14, 130, 48));
  key.target.position.copy(mid);
  PEEL_KEY.value.copy(key.position).sub(key.target.position).normalize();
  key.shadow.bias = -0.0005;
  key.shadow.normalBias = 0.12;                     // cm: keeps steep and shallow faces (the bezel recess) free of acne
  key.shadow.radius = 14;                           // texels: a penumbra of about 1 cm
  { const c = key.shadow.camera; c.left = -75; c.right = 75; c.top = 75; c.bottom = -75; c.near = 40; c.far = 260; }
  const fill = new THREE.DirectionalLight(0xdde6ff, 0.42);
  fill.position.copy(mid).add(new THREE.Vector3(80, 30, 70)); fill.target.position.copy(mid);
  const rim = new THREE.DirectionalLight(0xeaf2ff, 1.2);
  rim.position.copy(mid).add(new THREE.Vector3(30, 80, -110)); rim.target.position.copy(mid);
  scene.add(key, key.target, fill, fill.target, rim, rim.target);

  // The floor: dark, catching the overhead light and the shadows, fading into the fog.
  const floor = new THREE.Mesh(new THREE.CircleGeometry(600, 64), new THREE.MeshStandardMaterial({ color: 0x2b2926, roughness: 0.9, metalness: 0 }));
  floor.rotation.x = -Math.PI / 2;
  floor.position.set(mid.x, floorY, mid.z);
  floor.receiveShadow = true;
  floor.layers.set(1);
  scene.add(floor);

  // ---------------------------------------------------------------- the "Macintosh Plus" badge
  let badge = null;
  try { badge = await makeBadge(model, Q, peel, opts.badgeFont); if (badge) scene.add(badge); }
  catch (err) { console.warn("badge:", err); }

  // ---------------------------------------------------------------- the screen's geometry
  const gpos = glass.geometry.attributes.position;
  const pts = [];
  for (let i = 0; i < gpos.count; i++) pts.push(new THREE.Vector3().fromBufferAttribute(gpos, i).applyMatrix4(glass.matrixWorld));
  const corner = (sx, sy) => pts.reduce((best, p) => (sx * p.x + sy * p.y > sx * best.x + sy * best.y ? p : best), pts[0]);
  const BL = corner(-1, -1), BR = corner(1, -1), TL = corner(-1, 1), TR = corner(1, 1);
  const right = new THREE.Vector3().addVectors(TR, BR).sub(TL).sub(BL).normalize();
  let up = new THREE.Vector3().addVectors(TL, TR).sub(BL).sub(BR).normalize();
  const normal = new THREE.Vector3().crossVectors(right, up).normalize();
  up = new THREE.Vector3().crossVectors(normal, right).normalize();
  const glassCenter = new THREE.Vector3().add(BL).add(BR).add(TL).add(TR).multiplyScalar(0.25);
  const glassW = (TR.distanceTo(TL) + BR.distanceTo(BL)) / 2, glassH = (TL.distanceTo(BL) + TR.distanceTo(BR)) / 2;
  const imageW = glassW * IMAGE_OF_GLASS, imageH = imageW * SCREEN_H / SCREEN_W;
  const basis = new THREE.Matrix4().makeBasis(right, up, normal);
  const qScreen = new THREE.Quaternion().setFromRotationMatrix(basis);
  // The glass in the screen's basis and a smooth fit of its dome; the glass mesh takes the fit's
  // normals, so its reflections have clean, continuous edges instead of the low-poly facets' steps.
  const local = (p) => { const v = p.clone().sub(glassCenter); return [v.dot(right), v.dot(up), v.dot(normal)]; };
  const glassPts = pts.map(local);
  const glassFit = fitParaboloid(glassPts);
  {
    glass.geometry = glass.geometry.clone();
    const gn = glass.geometry.attributes.normal, toLocal = new THREE.Matrix4().copy(glass.matrixWorld).invert();
    for (let i = 0; i < gn.count; i++) {
      const [x, y] = glassPts[i], f = glassFit.normal(x, y);
      const w = new THREE.Vector3().addScaledVector(right, f.x).addScaledVector(up, f.y).addScaledVector(normal, f.z);
      w.transformDirection(toLocal); gn.setXYZ(i, w.x, w.y, w.z);
    }
    gn.needsUpdate = true;
  }

  // The front view's direction (from the Mac's centre to the camera): the screen's normal, raised.
  const dir0Of = (n) => n.clone().multiplyScalar(Math.cos(ELEVATION)).addScaledVector(new THREE.Vector3(0, 1, 0), Math.sin(ELEVATION)).normalize();
  // A grid in the screen's plane, each vertex cast onto the curved glass along the normal and lifted.
  const ray = new THREE.Raycaster();
  function castGrid(w, h, sx, sy, lift, miss = lift) {
    const g = new THREE.PlaneGeometry(w, h, sx, sy);
    const p = g.attributes.position;
    for (let i = 0; i < p.count; i++) {
      const wpt = glassCenter.clone().addScaledVector(right, p.getX(i)).addScaledVector(up, p.getY(i));
      ray.set(wpt.clone().addScaledVector(normal, 10), normal.clone().negate());
      const hit = ray.intersectObject(glass, false)[0];
      p.setZ(i, hit ? hit.point.clone().sub(glassCenter).dot(normal) + lift : typeof miss === "function" ? miss(p.getX(i), p.getY(i)) : miss);
    }
    g.computeVertexNormals();
    g.computeBoundingSphere();
    return g;
  }
  const SEG_X = 48, SEG_Y = 32;
  const tex = new THREE.CanvasTexture(emuCanvas);
  tex.colorSpace = THREE.SRGBColorSpace;
  tex.magFilter = THREE.LinearFilter;
  tex.minFilter = THREE.LinearMipmapLinearFilter;
  tex.generateMipmaps = true;
  tex.anisotropy = Math.min(8, renderer.capabilities.getMaxAnisotropy());
  const crt = new THREE.ShaderMaterial({
    uniforms: {
      map: { value: tex }, texSize: { value: new THREE.Vector2(SCREEN_W, SCREEN_H) },
      tint: { value: new THREE.Color(0.86, 0.92, 1.0) },      // the Plus's slightly blue-white phosphor (linear)
      glow: { value: 0.2 }, scan: { value: 0.8 }, time: { value: 0 }, noise: { value: 0.06 },
      fx: { value: 0 }, after: { value: null }, afterAmt: { value: 0 },
    },
    vertexShader: CRT_VERTEX, fragmentShader: CRT_FRAGMENT, toneMapped: false,
    transparent: true, depthWrite: false, blending: THREE.AdditiveBlending,
  });
  // The phosphor afterglow (high quality): a 512×342 history, H = max(H × decay, screen), ping-ponged
  // in two small render targets; the CRT shows max(screen, 0.45 × H from the frame before).
  const afterglow = (() => {
    const mk = () => new THREE.WebGLRenderTarget(SCREEN_W, SCREEN_H, { depthBuffer: false, magFilter: THREE.LinearFilter, minFilter: THREE.LinearFilter });
    let a = mk(), b = mk();
    const mat = new THREE.ShaderMaterial({
      uniforms: { cur: { value: tex }, hist: { value: a.texture }, decay: { value: 0 } },
      vertexShader: "varying vec2 vUv; void main() { vUv = uv; gl_Position = vec4(position.xy, 0.0, 1.0); }",
      fragmentShader: "uniform sampler2D cur, hist; uniform float decay; varying vec2 vUv; void main() { gl_FragColor = vec4(vec3(max(texture2D(hist, vUv).r * decay, texture2D(cur, vUv).r)), 1.0); }",
      depthTest: false, depthWrite: false, toneMapped: false,
    });
    const quad = new THREE.Mesh(new THREE.PlaneGeometry(2, 2), mat), sc = new THREE.Scene(), cam = new THREE.Camera();
    sc.add(quad);
    return {
      texture: () => a.texture,
      update(dt) {                                         // after the frame: b = max(a × decay, screen); swap
        mat.uniforms.hist.value = a.texture; mat.uniforms.decay.value = Math.exp(-dt / 42.5);
        const rt = renderer.getRenderTarget();
        renderer.setRenderTarget(b); renderer.render(sc, cam); renderer.setRenderTarget(rt);
        [a, b] = [b, a];
      },
    };
  })();
  const picture = new THREE.Mesh(castGrid(imageW, imageH, SEG_X, SEG_Y, 0.06), crt);
  picture.position.copy(glassCenter); picture.quaternion.copy(qScreen);
  picture.layers.set(1); picture.layers.enable(2);
  scene.add(picture);
  picture.renderOrder = 1;
  // The glass itself: polished CRT glass (roughness 0.05, Fresnel rising at grazing angles) over a
  // near-black tube, reflecting its own crisp studio. Everything is the glass mesh's own material, so
  // no reflection can reach past the glass's silhouette onto the bezel or case. The picture's light is
  // added on top (additive), so reflections show over the raster exactly as over the bare glass; over
  // lit phosphor they are simply lost in its light. Its normals are the smooth fit set above (no
  // facets, no stepped highlights). Lights add no specular dot here: the softbox is the key light.
  // Where the bezel's opening meets the glass (on a real Plus the opening overlaps the tube, which
  // carries on under the plastic): found by looking at the front, where the first thing hit along
  // the screen's normal changes from glass to case (half width, half height, corner radius). The
  // glass is shaded darker along it (below); no geometry is added, so the edge stays clean.
  const opening = (() => {
    const caseMeshes = []; model.traverse((o) => { if (o.isMesh && o !== glass && o.name.startsWith("Computer_")) caseMeshes.push(o); });
    const all = caseMeshes.concat([glass]);
    const hitsGlass = (x, y) => {
      const at = glassCenter.clone().addScaledVector(right, x).addScaledVector(up, y);
      ray.set(at.addScaledVector(normal, 30), normal.clone().negate()); ray.layers.enableAll();
      const h = ray.intersectObjects(all, false)[0];
      return !!h && h.object === glass;
    };
    const edge = (dx, dy, max) => { let t = 0; while (t < max && hitsGlass(dx * t, dy * t)) t += 0.01; return t; };
    const hw = edge(1, 0, 15), hh = edge(0, 1, 15);
    const k = edge(hw, hh, 1.2);                         // along the diagonal, as a share of (hw, hh)
    const a = hw * (1 - k), b = hh * (1 - k);
    const r = Math.min(Math.min(hw, hh), Math.max(0, a + b + Math.sqrt(2 * a * b)));
    return { hw, hh, r };
  })();
  const glassMat = new THREE.MeshStandardMaterial({ color: 0x2a2e2c, metalness: 0, roughness: 0.05, envMapIntensity: 1.0 });
  // The seam and the bezel's shadow on the glass: a thin dark line right at the opening's edge and a
  // soft darkening fading over a few millimetres inside it, stronger under the top edge (the light is
  // overhead), on everything the glass shows (its shading and its reflections).
  glassMat.onBeforeCompile = (sh) => {
    sh.uniforms.gC = { value: glassCenter }; sh.uniforms.gR = { value: right }; sh.uniforms.gU = { value: up };
    sh.uniforms.gOpen = { value: new THREE.Vector3(opening.hw, opening.hh, opening.r) };
    sh.vertexShader = "varying vec3 vGlassW;\n" + sh.vertexShader.replace("#include <project_vertex>", "#include <project_vertex>\n  vGlassW = (modelMatrix * vec4(transformed, 1.0)).xyz;");
    sh.fragmentShader = "varying vec3 vGlassW;\nuniform vec3 gC, gR, gU, gOpen;\n" + sh.fragmentShader
      .replace("#include <aomap_fragment>", "#include <aomap_fragment>\n  reflectedLight.directSpecular = vec3(0.0);")
      .replace("#include <opaque_fragment>", `{
    vec3 rel = vGlassW - gC;
    vec2 p = vec2(dot(rel, gR), dot(rel, gU));
    vec2 q = abs(p) - (gOpen.xy - gOpen.z);
    float inside = -(length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - gOpen.z);   // cm from the edge, inwards
    // the bezel's shadow: 2.5 mm deep at the bottom, 5.5 mm under the top edge (the light is
    // overhead), a little more along the sides; darkest at the edge, damping the reflections too
    float d = max(inside, 0.0);
    float top = smoothstep(-0.2, 0.8, p.y / max(gOpen.y, 0.01));
    float side = smoothstep(0.6, 1.0, abs(p.x) / max(gOpen.x, 0.01));
    float width = mix(0.25, 0.55, top) + 0.1 * side;
    float shade = 0.85 * exp(-d / (0.45 * width));
    float seam = 1.0 - smoothstep(0.03, 0.07, d);                              // a ~0.5 mm near-black line
    outgoingLight *= max((1.0 - shade) * (1.0 - 0.95 * seam), 0.02);
  }
  #include <opaque_fragment>`);
  };
  glassMat.customProgramCacheKey = () => "crt-glass";
  glass.material = glassMat;
  // The key softbox's reflection: a fixed spot on the glass (the top border, left of centre, clear of
  // the picture) in every front view, whatever the zoom; the glass's environment is turned a little
  // with the zoom to keep it there (glassEnvForZoom, called from updateCamera). Turning the view
  // doesn't touch it, so the highlight slides over the curved glass as the view turns.
  const softSpot = { point: null, normal: null, dir: null };
  {
    const cam = glassCenter.clone().addScaledVector(dir0Of(normal), imageH / 2 / (0.62 * Math.tan(THREE.MathUtils.degToRad(FOV / 2))));
    ray.layers.enableAll();
    const glassAt = (x, y) => {
      const at = glassCenter.clone().addScaledVector(right, x).addScaledVector(up, y);
      ray.set(at.clone().addScaledVector(normal, 10), normal.clone().negate());
      const hit = ray.intersectObject(glass, false)[0];
      if (!hit) return null;
      const f = glassFit.normal(x, y);
      return { point: hit.point.clone(), normal: new THREE.Vector3().addScaledVector(right, f.x).addScaledVector(up, f.y).addScaledVector(normal, f.z).normalize() };
    };
    const reflAt = (x, y, fallback) => {
      const g = glassAt(x, y);
      return g ? g.point.clone().sub(cam).normalize().reflect(g.normal) : fallback;
    };
    const border = (glassH - imageH) / 2;
    const sx = -0.1 * glassW, sy = imageH / 2 + border * 0.5;      // near the top, just left of centre: the overhead light
    const g = glassAt(sx, sy);
    if (g) { softSpot.point = g.point; softSpot.normal = g.normal; }
    // A direction from yaw (degrees towards +x, the viewer's right) and elevation (degrees).
    const dirOf = (yaw, el) => { const y = THREE.MathUtils.degToRad(yaw), e = THREE.MathUtils.degToRad(el); return new THREE.Vector3(Math.sin(y) * Math.cos(e), Math.sin(e), Math.cos(y) * Math.cos(e)); };
    softSpot.dir = reflAt(sx, sy, new THREE.Vector3(-0.3, 0.45, 0.85));
    const lights = [
      { dir: softSpot.dir, w: 8.5, h: 2.6, r: 0.32, i: 15, color: 0xfff4e8, roll: 0.05, hot: true },   // the key softbox
      { dir: dirOf(-41, -6), w: 20, h: 16, r: 0.1, i: 3.2, color: 0xfff3e6, hot: true },   // a big softbox on the viewer's left (seen when the view turns right)
      { dir: dirOf(41, -8), w: 16, h: 13, r: 0.1, i: 1.8, color: 0xeef3ff, hot: true },    // a dimmer one on the right (seen when the view turns left)
      { dir: new THREE.Vector3(0, 22, -36), w: 60, h: 7, r: 0.3, i: 3, color: 0xffffff },                  // the rim strip behind
      { dir: new THREE.Vector3(0, 1, 0.3), w: 24, h: 4, r: 0.4, i: 1.2, color: 0xffffff },                // an overhead bar
    ];
    glassMat.envMap = pmrem.fromScene(glassStudio(lights), 0, 0.1, 100, { size: Q.dpr >= 2 ? 512 : 256 }).texture;
  }
  const qEnv = new THREE.Quaternion(), vEnv = new THREE.Vector3();
  function glassEnvForZoom(frontCam) {
    if (!softSpot.point) return;
    vEnv.copy(softSpot.point).sub(frontCam).normalize().reflect(softSpot.normal);
    qEnv.setFromUnitVectors(softSpot.dir, vEnv);         // environment turned so the softbox is where this view looks
    glassMat.envMapRotation.setFromQuaternion(qEnv);
  }


  picture.updateMatrixWorld(true);
  const picPos = picture.geometry.attributes.position;

  // ---------------------------------------------------------------- keys and the mouse
  const keys = new Map();               // node name -> {node, base, t, down (real keys held), held (clicked/latched), meshes}
  const keyByMesh = new Map();
  const keyNames = new Set([...Object.values(KEY_NODES), ...Object.keys(NODE_MAC)]);
  // Key nodes by their names in the file (GLTFLoader strips ".", "[", "]" and "/" from node names, so
  // "Shift.001", "[" or "/" are only in userData.name).
  model.traverse((o) => {
    const nm = (o.userData && o.userData.name) || o.name;
    if (keyNames.has(nm) && !keys.has(nm)) {
      const k = { name: nm, node: o, base: o.position.y, t: 0, down: 0, held: false, meshes: [] };
      o.traverse((m) => { if (m.isMesh) { m.material = m.material.clone(); k.meshes.push(m); keyByMesh.set(m, k); } });
      keys.set(nm, k);
    }
  });
  const button = splitMouseButton(mouseDark);
  if (button) button.material = button.material.clone();
  const mouse = { node: mouseNode, base: mouseNode ? mouseNode.position.clone() : null, button, t: 0, down: false,
                  target: new THREE.Vector2(), slide: new THREE.Vector2() };
  const mouseBody = [mouseLight, mouseDark].filter(Boolean);
  // The floppy slot: an invisible target over the slot on the case's front (found once, by depth,
  // in the model: a deep slit right of the badge), lit faintly on hover; a click opens the file picker.
  const slot = (() => {
    let cm = null; model.traverse((o) => { if (o.name === "Computer") cm = o; });
    if (!cm) return null;
    const b = new THREE.Box3().setFromObject(cm), w = b.max.x - b.min.x, h = b.max.y - b.min.y;
    const x0 = b.min.x + 0.483 * w, x1 = b.min.x + 0.921 * w, y0 = b.min.y + 0.277 * h, y1 = b.min.y + 0.346 * h;
    const m = new THREE.Mesh(new THREE.BoxGeometry(x1 - x0, y1 - y0, 0.5),
      new THREE.MeshBasicMaterial({ color: 0xfff6e0, transparent: true, opacity: 0.09, depthWrite: false, visible: false }));
    m.position.set((x0 + x1) / 2, (y0 + y1) / 2, b.max.z - 0.15);
    m.name = "FloppySlot"; m.renderOrder = 3;
    m.layers.set(3);                                     // drawn by the main camera only (not the contact shadow's)
    scene.add(m);
    return m;
  })();
  const pickables = [picture, ...keyByMesh.keys(), ...(button ? [button] : []), ...mouseBody, ...(slot ? [slot] : [])];

  // ---------------------------------------------------------------- the 3D floppy disk
  // "Floppy disk" by drumdorf (CC BY-SA 4.0; resized and compressed by tools/prepare_floppy.mjs).
  // Its mesh is in inches, the insertion axis along +x with the shutter at -x and the label side +z;
  // here it becomes a real 3.5" disk (94 × 90 × 3 mm) lying flat, shutter first into the slot.
  // Inserting: it slides in from just in front of the slot, stops, drops onto the spindle (the
  // drive's clunk) and stays there, seated 4 mm down with its back edge 7.5 mm inside the slot, until it is ejected. Ejecting: it pops up, slides out a third of the way, rests, then
  // glides towards the viewer and fades. Timed to the drive's sounds (sfx.json "timing").
  const floppy = { group: null, mat: null, label: null, state: "out", id: null, anim: null, slotX: 0, slotY: 0, slotZ: 0 };
  if (slot) {
    floppy.slotX = slot.position.x; floppy.slotZ = slot.position.z + 0.15;
    // The slit's height: the deepest part of the slot box, looking straight in.
    const meshes = []; model.traverse((o) => { if (o.isMesh && o.name.startsWith("Computer_")) meshes.push(o); });
    const rc = new THREE.Raycaster(); rc.layers.enableAll();
    const hy = slot.geometry.parameters.height, ys = [];
    for (let k = 0; k <= 40; k++) {
      const y = slot.position.y - hy / 2 + (hy * k) / 40;
      rc.set(new THREE.Vector3(floppy.slotX, y, floppy.slotZ + 5), new THREE.Vector3(0, 0, -1));
      const h = rc.intersectObjects(meshes, false)[0];
      if (!h || h.point.z < floppy.slotZ - 1.2) ys.push(y);
    }
    floppy.slotY = ys.length ? ys.reduce((a, b) => a + b, 0) / ys.length : slot.position.y;
  }
  if (opts.floppyUrl && slot) {
    loader.load(opts.floppyUrl, (g) => {
      let src = null; g.scene.traverse((o) => { if (o.isMesh && !src) src = o; });
      if (!src) return;
      const geo = src.geometry.clone();
      geo.computeBoundingBox();
      const c = geo.boundingBox.getCenter(new THREE.Vector3()), hz = geo.boundingBox.max.z - c.z;
      geo.translate(-c.x, -c.y, -c.z);
      const m0 = src.material;
      const mat = new THREE.MeshStandardMaterial({ map: m0.map, normalMap: m0.normalMap, roughnessMap: m0.roughnessMap || null,
        roughness: 0.8, metalness: 0, envMapIntensity: 0.8, transparent: true, opacity: 1 });
      const disk = new THREE.Mesh(geo, mat);
      const inner = new THREE.Group();
      // local x -> world +z (shutter end, -x, goes in first), local y -> world +x, local z -> world +y
      inner.matrix.makeBasis(new THREE.Vector3(0, 0, 1), new THREE.Vector3(1, 0, 0), new THREE.Vector3(0, 1, 0)).scale(new THREE.Vector3(2.54, 2.54, 2.54));
      inner.matrixAutoUpdate = false;
      inner.add(disk);
      // The paper label with the disk image's name, on the top near the back end.
      const cv = document.createElement("canvas"); cv.width = 512; cv.height = 340;
      const ltex = new THREE.CanvasTexture(cv); ltex.colorSpace = THREE.SRGBColorSpace; ltex.anisotropy = 4;
      const lmat = new THREE.MeshStandardMaterial({ map: ltex, roughness: 0.9, transparent: true, opacity: 1, polygonOffset: true, polygonOffsetFactor: -2 });
      const lab = new THREE.Mesh(new THREE.PlaneGeometry(2.3, 1.55), lmat);
      lab.matrix.makeBasis(new THREE.Vector3(0, 1, 0), new THREE.Vector3(-1, 0, 0), new THREE.Vector3(0, 0, 1)).setPosition(0.95, 0, hz + 0.004);
      lab.matrixAutoUpdate = false;
      inner.add(lab);
      const grp = new THREE.Group(); grp.add(inner); grp.visible = false;
      scene.add(grp);
      floppy.group = grp; floppy.mat = mat; floppy.label = { cv, ltex, lmat };
    }, undefined, (e) => console.warn("floppy:", e));
  }
  function drawLabel(name) {
    const { cv, ltex } = floppy.label, x = cv.getContext("2d");
    x.clearRect(0, 0, cv.width, cv.height);
    x.fillStyle = "#efeadf"; x.beginPath(); x.roundRect(6, 6, cv.width - 12, cv.height - 12, 18); x.fill();
    x.strokeStyle = "rgba(90, 110, 150, 0.35)"; x.lineWidth = 3;
    for (let yy = 150; yy < cv.height - 30; yy += 52) { x.beginPath(); x.moveTo(30, yy); x.lineTo(cv.width - 30, yy); x.stroke(); }
    const text = name.replace(/(\.(dsk|img|image|hfv|zip|dc42|diskcopy|sit|sea|hqx|bin|cpt))+$/i, "");
    x.fillStyle = "#1d2a4d"; x.textAlign = "center"; x.textBaseline = "alphabetic";
    let size = 62; x.font = `600 ${size}px "Marker Felt", "Comic Sans MS", "Segoe Print", cursive`;
    while (x.measureText(text).width > cv.width - 60 && size > 22) { size -= 4; x.font = `600 ${size}px "Marker Felt", "Comic Sans MS", "Segoe Print", cursive`; }
    x.fillText(text, cv.width / 2, 135);
    ltex.needsUpdate = true;
  }
  const SEAT = -0.4;                                    // cm: the drop onto the spindle, seated
  // Seated: the back edge 7.5 mm inside the slot, clear of its lip and bevel.
  const FL = 9.4, IN_Z = () => floppy.slotZ - FL / 2 - 0.75, OUT_Z = () => floppy.slotZ + FL / 2 + 2.5, THIRD_Z = () => floppy.slotZ - FL / 2 + FL / 3;
  function floppyPose(z, dy, op) {
    floppy.group.position.set(floppy.slotX, floppy.slotY + dy, z);
    floppy.mat.opacity = op; floppy.label.lmat.opacity = op;
    floppy.group.visible = op > 0.001;
  }
  const ease = (k) => (k < 0.5 ? 4 * k * k * k : 1 - Math.pow(-2 * k + 2, 3) / 2), clamp01 = (k) => Math.max(0, Math.min(1, k));
  function floppyStep(now) {
    const a = floppy.anim, t = now - a.t0, T = a.timing;
    if (a.kind === "in") {
      const slide = clamp01(t / (T.insertClick * 1000)), drop = clamp01((t - T.insertClunk * 1000 + 60) / 140);
      floppyPose(OUT_Z() + (IN_Z() - OUT_Z()) * (1 - Math.pow(1 - slide, 2)), SEAT * drop * drop, clamp01(t / 100));
      if (t > T.insertClunk * 1000 + 300) { floppyPose(IN_Z(), SEAT, 1); floppy.anim = null; }   // stays in the drive
    } else if (a.kind === "out") {
      const pop = T.ejectPop * 1000, pull = T.ejectPull * 1000;
      if (t < pop) floppyPose(IN_Z(), SEAT, 1);
      else if (t < pull) {
        const up = clamp01((t - pop) / 80), out = clamp01((t - pop) / 420);
        floppyPose(IN_Z() + (THIRD_Z() - IN_Z()) * (1 - Math.pow(1 - out, 3)), SEAT - SEAT * up, 1);
      } else {
        const k = clamp01((t - pull) / 700);
        floppyPose(THIRD_Z() + 12 * ease(k), 0.6 * k, 1 - k);
        if (k >= 1) { floppyPose(OUT_Z(), 0, 0); floppy.anim = null; floppy.state = "out"; }
      }
    } else if (a.kind === "fade") {                     // reduced motion: a short fade in the drive
      const k = clamp01(t / 400), shown = a.after === "in" ? k : 1 - k;
      floppyPose(IN_Z(), SEAT, shown);
      if (k >= 1) { floppy.anim = null; if (a.after) floppy.state = a.after; }
    }
  }
  const floppyTiming = Object.assign({ insertClick: 0.87, insertClunk: 1.59, ejectPop: 0.15, ejectPull: 1.78 }, opts.floppyTiming || {});
  const occluders = [];
  model.traverse((o) => { if (o.isMesh && o.name.startsWith("Computer_")) occluders.push(o); });
  model.traverse((o) => { if (o.isMesh) { o.castShadow = true; o.receiveShadow = true; } });
  if (mouseNode) mouseNode.traverse((o) => { if (o.isMesh) o.castShadow = false; });

  // ---------------------------------------------------------------- contact shadow
  const contact = makeContactShadow();
  scene.add(contact.plane);

  function makeContactShadow() {
    const b = new THREE.Box3();
    model.traverse((o) => { if (["Computer", "Keyboard", "Mouse", "Keyboardchord", "Mousechord", "Mouseplug"].includes(o.name)) b.expandByObject(o); });
    b.expandByVector(new THREE.Vector3(14, 0, 14));
    const w = b.max.x - b.min.x, d = b.max.z - b.min.z;
    const res = Q.contact;
    const rt = new THREE.WebGLRenderTarget(res, Math.round(res * d / w)); rt.texture.generateMipmaps = false;
    const rtBlur = new THREE.WebGLRenderTarget(rt.width, rt.height); rtBlur.texture.generateMipmaps = false;
    const geo = new THREE.PlaneGeometry(w, d).rotateX(Math.PI / 2);
    const plane = new THREE.Mesh(geo, new THREE.MeshBasicMaterial({ map: rt.texture, transparent: true, opacity: 0.9, depthWrite: false, toneMapped: false }));
    plane.scale.y = -1;
    plane.position.set((b.min.x + b.max.x) / 2, floorY + 0.03, (b.min.z + b.max.z) / 2);
    plane.renderOrder = 1;
    plane.layers.set(1);
    const cam = new THREE.OrthographicCamera(-w / 2, w / 2, d / 2, -d / 2, 0, 9);
    cam.rotation.x = Math.PI / 2;
    cam.position.copy(plane.position).setY(floorY);
    cam.updateMatrixWorld(true);
    const depth = new THREE.MeshDepthMaterial();
    depth.onBeforeCompile = (sh) => {
      sh.fragmentShader = sh.fragmentShader.replace("gl_FragColor = vec4( vec3( 1.0 - fragCoordZ ), opacity );",
        "gl_FragColor = vec4( vec3( 0.0 ), ( 1.0 - fragCoordZ ) * 1.15 );");
    };
    depth.depthTest = false; depth.depthWrite = false;
    const blur = new THREE.ShaderMaterial({ uniforms: { tDiffuse: { value: null }, step: { value: new THREE.Vector2() } }, vertexShader: BLUR_VERTEX, fragmentShader: BLUR_FRAGMENT });
    const blurPlane = new THREE.Mesh(geo, blur);
    const blurScene = new THREE.Scene(); blurScene.add(blurPlane); blurPlane.position.copy(plane.position).setY(floorY + 1);
    function pass(src, dst, sx, sy) { blur.uniforms.tDiffuse.value = src.texture; blur.uniforms.step.value.set(sx, sy); renderer.setRenderTarget(dst); renderer.render(blurScene, cam); }
    function update() {
      const bg = scene.background, fog = scene.fog, cc = renderer.getClearAlpha();
      scene.background = null; scene.fog = null; scene.overrideMaterial = depth;
      renderer.setClearAlpha(0);
      renderer.setRenderTarget(rt); renderer.clear(); renderer.render(scene, cam);
      scene.overrideMaterial = null;
      for (const a of [2.6, 1.2]) { pass(rt, rtBlur, a / rt.width, 0); pass(rtBlur, rt, 0, a / rt.height); }
      renderer.setRenderTarget(null); renderer.setClearAlpha(cc);
      scene.background = bg; scene.fog = fog;
    }
    return { plane, update, dirty: true };
  }

  // ---------------------------------------------------------------- camera
  const camera = new THREE.PerspectiveCamera(FOV, 1, 2, 900);
  camera.layers.enable(1); camera.layers.enable(3);
  const T = glassCenter.clone();                       // the Mac's centre: the camera always looks here
  const worldUp = new THREE.Vector3(0, 1, 0);
  const dir0 = normal.clone().multiplyScalar(Math.cos(ELEVATION)).addScaledVector(worldUp, Math.sin(ELEVATION)).normalize();
  const fitBox = new THREE.Box3();
  model.traverse((o) => { if (["Computer", "Keyboard", "Mouse"].includes(o.name)) fitBox.expandByObject(o); });
  fitBox.expandByVector(new THREE.Vector3(3, 0, 3));
  const view = { yaw: 0, pitch: 0, zoom: 0, zoomTarget: 0, zoomTau: 90, spring: null, dragging: false, dolly: null };
  let W = 1, H = 1, safeTop = 0, safeBottom = 0, dMin = 40, dMax = 120;

  function frustum() {
    const sh = Math.max(120, H - safeTop - safeBottom);
    const tanV = Math.tan(THREE.MathUtils.degToRad(FOV / 2)) * sh / H;
    return { tanV, tanH: tanV * W / sh, sh };
  }
  function computeDistances() {
    const { tanV, tanH } = frustum();
    dMin = Math.max(imageH / 2 / (0.86 * tanV), imageW / 2 / (0.86 * tanH));
    const f = dir0.clone().negate(), r = new THREE.Vector3().crossVectors(f, worldUp).normalize(), u = new THREE.Vector3().crossVectors(r, f);
    let d = dMin * 1.2;
    for (let i = 0; i < 8; i++) {
      const p = new THREE.Vector3(i & 1 ? fitBox.max.x : fitBox.min.x, i & 2 ? fitBox.max.y : fitBox.min.y, i & 4 ? fitBox.max.z : fitBox.min.z);
      const v = p.sub(T), x = v.dot(r), y = v.dot(u), z = v.dot(dir0);
      d = Math.max(d, z + Math.abs(x) / (tanH * 0.92), z + Math.abs(y) / (tanV * 0.9));
    }
    dMax = d;
  }
  const distanceAt = (z) => Math.exp(lerp(Math.log(dMax), Math.log(dMin), z));
  // The zoom at which the picture fills a share of the area between the bars (height or width).
  function zoomForScreen(share) {
    const { tanV, tanH } = frustum();
    const d = Math.max(imageH / 2 / (share * tanV), imageW / 2 / (share * tanH));
    return Math.max(0, Math.min(1, Math.log(dMax / d) / Math.log(dMax / dMin)));
  }
  const vCam = new THREE.Vector3();
  function updateCamera() {
    const d = distanceAt(view.zoom);
    const dir = dir0.clone().applyAxisAngle(worldUp, view.yaw);
    const elev = Math.asin(THREE.MathUtils.clamp(dir.y, -1, 1));
    const target = THREE.MathUtils.clamp(elev + view.pitch, -0.05, 1.15);
    const horiz = new THREE.Vector3(dir.x, 0, dir.z).normalize();
    const dd = horiz.multiplyScalar(Math.cos(target)).addScaledVector(worldUp, Math.sin(target));
    camera.position.copy(T).addScaledVector(dd, d);
    glassEnvForZoom(vCam.copy(T).addScaledVector(dir0, d));
    camera.up.set(0, 1, 0);
    camera.lookAt(T);
    // Centre the Mac between the top and bottom bars (a vertical offset of the whole frustum).
    camera.aspect = W / H;
    camera.setViewOffset(W, H, 0, -(safeTop - safeBottom) / 2, W, H);
    camera.updateMatrixWorld(true);
  }

  // ---------------------------------------------------------------- picking and mapping
  const ndc = new THREE.Vector2(), tmpV = new THREE.Vector3();
  function rayAt(cx, cy) {
    const r = canvas.getBoundingClientRect();
    ndc.set(((cx - r.left) / r.width) * 2 - 1, -((cy - r.top) / r.height) * 2 + 1);
    ray.setFromCamera(ndc, camera);
    return ray;
  }
  const isFront = () => !view.dragging && !view.spring && view.yaw === 0 && view.pitch === 0 && !gesture;
  function pick(cx, cy) {
    rayAt(cx, cy);
    ray.layers.enableAll();
    const front = isFront();
    const hits = ray.intersectObjects(front ? pickables : pickables.concat(occluders), false);
    const h = hits[0];
    if (!h) return null;
    if (h.object === picture) return front ? { kind: "screen", mac: uvToMac(h.uv) } : null;
    if (keyByMesh.has(h.object)) return { kind: "key", key: keyByMesh.get(h.object), mesh: h.object };
    if (button && h.object === button) return { kind: "button", mesh: button };
    if (mouseBody.includes(h.object)) return { kind: "body", mesh: h.object };
    if (slot && h.object === slot) return { kind: "slot" };
    return null;
  }
  const uvToMac = (uv) => [Math.max(0, Math.min(SCREEN_W - 1, Math.floor(uv.x * SCREEN_W))), Math.max(0, Math.min(SCREEN_H - 1, Math.floor((1 - uv.y) * SCREEN_H)))];
  // For drags that leave the screen: the ray's crossing with the screen's plane, clamped to the edge.
  function macFromPlane(cx, cy) {
    rayAt(cx, cy);
    const pl = new THREE.Plane().setFromNormalAndCoplanarPoint(normal, glassCenter);
    const p = ray.ray.intersectPlane(pl, tmpV);
    if (!p) return null;
    const v = p.clone().sub(glassCenter);
    return uvToMac({ x: v.dot(right) / imageW + 0.5, y: v.dot(up) / imageH + 0.5 });
  }
  // The point of the picture's surface showing Mac pixel (x, y), exactly as the GPU interpolates it.
  function pictureWorld(u, v) {
    const gx = Math.min(SEG_X - 1e-6, u * SEG_X), gy = Math.min(SEG_Y - 1e-6, (1 - v) * SEG_Y);
    const ix = Math.floor(gx), iy = Math.floor(gy), fx = gx - ix, fy = gy - iy;
    const idx = (i, j) => j * (SEG_X + 1) + i;
    const P = (i) => new THREE.Vector3().fromBufferAttribute(picPos, i);
    const a = P(idx(ix, iy)), b = P(idx(ix, iy + 1)), c = P(idx(ix + 1, iy + 1)), d = P(idx(ix + 1, iy));
    const loc = fx + fy <= 1
      ? a.clone().addScaledVector(d.clone().sub(a), fx).addScaledVector(b.clone().sub(a), fy)
      : c.clone().addScaledVector(b.clone().sub(c), 1 - fx).addScaledVector(d.clone().sub(c), 1 - fy);
    return picture.localToWorld(loc);
  }
  function toClient(world) {
    const r = canvas.getBoundingClientRect();
    tmpV.copy(world).project(camera);
    return [r.left + (tmpV.x + 1) / 2 * r.width, r.top + (1 - tmpV.y) / 2 * r.height];
  }
  const clientFromMac = (x, y) => toClient(pictureWorld((x + 0.5) / SCREEN_W, 1 - (y + 0.5) / SCREEN_H));

  // ---------------------------------------------------------------- input
  const pointers = new Map();
  let gesture = null;                    // orbit | multi | mac | key | button | body
  let hover = null, hoverMesh = null;
  const ORBIT_SPEED = 0.006;
  let started = false, locked = false;
  const latched = new Set();
  let caps = false;
  function setHover(h) {
    const mesh = h && (h.kind === "key" || h.kind === "button") ? h.mesh : null;
    if (mesh !== hoverMesh) {
      if (hoverMesh) hoverMesh.material.emissive?.setHex(0x000000);
      hoverMesh = mesh;
      if (hoverMesh) hoverMesh.material.emissive?.setHex(0x2a2416);
      request();
    }
    hover = h;
    let cur = "grab";
    if (gesture && (gesture.kind === "orbit" || gesture.kind === "multi")) cur = "grabbing";
    else if (h && h.kind === "screen" && started) cur = "none";            // the Mac draws its own arrow
    else if (h && (h.kind === "key" || h.kind === "button" || h.kind === "slot")) cur = "pointer";
    if (slot && slot.material.visible !== !!(h && h.kind === "slot")) { slot.material.visible = !!(h && h.kind === "slot"); request(); }
    else if (h && h.kind === "body") cur = "move";
    if (gesture && gesture.kind === "mac") cur = "none";
    canvas.style.cursor = cur;
  }
  function startSpring() {
    view.dragging = false;
    if (view.yaw === 0 && view.pitch === 0) { view.spring = null; request(); return; }
    view.spring = { yaw: view.yaw, pitch: view.pitch, t0: performance.now() + (reduceMotion ? 0 : HOLD_MS), ms: reduceMotion ? 150 : RETURN_MS };
    request();
  }
  function setZoom(z) { view.zoomTarget = Math.max(0, Math.min(1, z)); request(); }
  // Clicks on the 3D keyboard: Shift, Option and Command latch until the next other key; Caps Lock locks.
  function clickKey(k, down) {
    const code = NODE_MAC[k.name];
    if (code === undefined) return;
    if (LOCKING.has(k.name)) {
      if (!down) return;
      caps = !caps; k.held = caps; mac.key(code, caps); request(); return;
    }
    if (LATCHING.has(k.name)) {
      if (!down) return;
      if (latched.has(k.name)) { latched.delete(k.name); k.held = false; mac.key(code, false); }
      else { latched.add(k.name); k.held = true; mac.key(code, true); }
      request(); return;
    }
    k.held = down;
    mac.key(code, down);
    if (!down) {
      for (const n of latched) { const lk = keys.get(n); lk.held = false; mac.key(NODE_MAC[n], false); }
      latched.clear();
    }
    request();
  }

  // Any input takes over from the opening dolly at the zoom it has reached.
  function stopDolly() { if (view.dolly) { view.dolly = null; view.zoomTarget = view.zoom; } }
  canvas.addEventListener("pointerdown", (ev) => {
    stopDolly();
    pointers.set(ev.pointerId, { x: ev.clientX, y: ev.clientY });
    if (pointers.size >= 2) {                 // two fingers: turn and pinch
      if (gesture && gesture.kind === "mac") mac.up();
      if (gesture && gesture.kind === "key") clickKey(gesture.key, false);
      const [a, b] = [...pointers.values()];
      gesture = { kind: "multi", dist: Math.hypot(a.x - b.x, a.y - b.y), zoom: view.zoomTarget, cx: (a.x + b.x) / 2, cy: (a.y + b.y) / 2 };
      view.spring = null; view.dragging = true;
      ev.preventDefault(); request(); return;
    }
    if (locked) { if (ev.button === 0) { sound("mousedown"); mac.down(); setButton(true); gesture = { kind: "mac" }; } return; }
    // Relative-mouse programs (Blockworld): the first click on the screen captures the mouse.
    if (ev.button === 0 && started && mac.relative && mac.relative()) {
      const h0 = pick(ev.clientX, ev.clientY);
      if (h0 && h0.kind === "screen" && mac.capture(canvas)) { ev.preventDefault(); return; }
    }
    if (ev.button !== 0) return;
    const h = pick(ev.clientX, ev.clientY);
    try { canvas.setPointerCapture(ev.pointerId); } catch { /* ignore */ }
    ev.preventDefault();
    if (h && h.kind === "screen" && started) {
      if (!(mac.relative && mac.relative())) mac.move(...h.mac);
      sound("mousedown");
      mac.down(); setButton(true);
      gesture = { kind: "mac" };
    } else if (h && h.kind === "key") {
      gesture = { kind: "key", key: h.key };
      sound("keydown", h.key.name);
      clickKey(h.key, true);
    } else if (h && h.kind === "button") {
      gesture = { kind: "button" };
      sound("mousedown");
      mac.down(); setButton(true);
    } else if (h && h.kind === "slot") {                // the floppy slot: Insert Disk's file picker
      gesture = { kind: "slot" };
      if (opts.onSlot) opts.onSlot();
    } else if (h && h.kind === "body" && started) {
      gesture = { kind: "body", x: ev.clientX, y: ev.clientY, fx: 0, fy: 0 };
    } else {
      gesture = { kind: "orbit", x: ev.clientX, y: ev.clientY };
      view.spring = null; view.dragging = true;
    }
    setHover(h);
    request();
  });
  // Movement for mouse look without a captured mouse: from the pointer's own positions (movementX
  // isn't filled in by every browser for every event); with a captured mouse, movementX.
  let lastX = null, lastY = null;
  canvas.addEventListener("pointermove", (ev) => {
    const p = pointers.get(ev.pointerId);
    if (p) { p.x = ev.clientX; p.y = ev.clientY; }
    const mx = lastX === null ? 0 : ev.clientX - lastX, my = lastY === null ? 0 : ev.clientY - lastY;
    lastX = ev.clientX; lastY = ev.clientY;
    const relative = started && mac.relative && mac.relative();
    if (locked) {                            // captured mouse: movement only
      const r = pictureClientRect(), scale = r ? SCREEN_W / Math.max(1, r.right - r.left) : 1;
      mac.look(ev.movementX, ev.movementY, scale);
      return;
    }
    if (!gesture) {
      const h = pick(ev.clientX, ev.clientY);
      if (h && h.kind === "screen" && started) {
        if (relative) mac.look(mx, my);                             // Blockworld: movement, never positions
        else { mac.move(...h.mac); mouseAt(...h.mac); }
      }
      setHover(h);
      return;
    }
    if (gesture.kind === "mac") {
      if (relative) { mac.look(mx, my); return; }
      const m = macFromPlane(ev.clientX, ev.clientY);
      if (m) { mac.move(...m); mouseAt(...m); }
    } else if (gesture.kind === "body") {
      // Slide the 3D mouse: the Mac cursor moves with it (one CSS pixel = one Mac pixel).
      const dx = ev.clientX - gesture.x, dy = ev.clientY - gesture.y;
      gesture.x = ev.clientX; gesture.y = ev.clientY;
      mac.delta(dx, dy);
      mouse.slide.x = THREE.MathUtils.clamp(mouse.slide.x + dx * 0.02, -3, 3);
      mouse.slide.y = THREE.MathUtils.clamp(mouse.slide.y + dy * 0.02, -2, 2);
      mouse.target.copy(mouse.slide);
      request();
    } else if (gesture.kind === "orbit") {
      view.yaw -= (ev.clientX - gesture.x) * ORBIT_SPEED;
      view.pitch += (ev.clientY - gesture.y) * ORBIT_SPEED;
      gesture.x = ev.clientX; gesture.y = ev.clientY;
      clampView(); request();
    } else if (gesture.kind === "multi") {
      const [a, b] = [...pointers.values()];
      if (!a || !b) return;
      const cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2;
      view.yaw -= (cx - gesture.cx) * ORBIT_SPEED; view.pitch += (cy - gesture.cy) * ORBIT_SPEED;
      gesture.cx = cx; gesture.cy = cy;
      const d = Math.hypot(a.x - b.x, a.y - b.y);
      if (gesture.dist > 10) setZoom(gesture.zoom + Math.log(d / gesture.dist) * 0.9);
      clampView(); request();
    }
  });
  const endPointer = (ev) => {
    if (!pointers.has(ev.pointerId)) return;
    pointers.delete(ev.pointerId);
    const g = gesture;
    if (!g) return;
    if (g.kind === "multi") {
      if (pointers.size === 1) { const [p] = [...pointers.values()]; gesture = { kind: "orbit", x: p.x, y: p.y }; return; }
      gesture = null; startSpring(); return;
    }
    gesture = null;
    if (g.kind === "mac" || g.kind === "button") { mac.up(); setButton(false); sound("mouseup"); }
    else if (g.kind === "key") { clickKey(g.key, false); sound("keyup", g.key.name); }
    else if (g.kind === "orbit") startSpring();
    setHover(pick(ev.clientX, ev.clientY));
  };
  canvas.addEventListener("pointerup", endPointer);
  canvas.addEventListener("pointercancel", endPointer);
  canvas.addEventListener("pointerleave", () => { lastX = lastY = null; if (!gesture) setHover(null); });
  canvas.addEventListener("contextmenu", (ev) => ev.preventDefault());
  canvas.addEventListener("wheel", (ev) => {
    ev.preventDefault();
    stopDolly();
    const dy = ev.deltaMode === 1 ? ev.deltaY * 16 : ev.deltaMode === 2 ? ev.deltaY * 400 : ev.deltaY;
    setZoom(view.zoomTarget - dy * 0.0016);
  }, { passive: false });
  let gStart = 0;
  canvas.addEventListener("gesturestart", (ev) => { ev.preventDefault(); stopDolly(); gStart = view.zoomTarget; });
  canvas.addEventListener("gesturechange", (ev) => { ev.preventDefault(); setZoom(gStart + Math.log(ev.scale) * 0.9); });
  document.addEventListener("pointerlockchange", () => { locked = document.pointerLockElement === canvas; });
  const clampView = () => { view.yaw = Math.max(-2.6, Math.min(2.6, view.yaw)); view.pitch = Math.max(-0.3, Math.min(0.9, view.pitch)); };

  function setButton(down) { mouse.down = down; request(); }
  function mouseAt(x, y) {
    if (!mouse.node) return;
    mouse.target.set((x / SCREEN_W - 0.5) * 5, (y / SCREEN_H - 0.5) * 3.4);
    mouse.slide.copy(mouse.target);
    request();
  }
  function pictureClientRect() {
    const cs = [[0, 0], [SCREEN_W - 1, 0], [0, SCREEN_H - 1], [SCREEN_W - 1, SCREEN_H - 1]].map(([x, y]) => clientFromMac(x, y));
    return { left: Math.min(...cs.map((c) => c[0])), right: Math.max(...cs.map((c) => c[0])), top: Math.min(...cs.map((c) => c[1])), bottom: Math.max(...cs.map((c) => c[1])) };
  }

  // ---------------------------------------------------------------- render loop (on demand)
  let raf = 0, last = performance.now(), texDirty = true, destroyed = false, shadowsDirty = true;
  const frameTimes = [];
  function request() { if (!raf && !destroyed) raf = requestAnimationFrame(frame); }
  // The live CRT effects: quality high = all (with afterglow), medium = no afterglow, low = off; off
  // with reduced motion and before the Mac starts.
  const liveCrt = () => (started && !reduceMotion ? Q.crt : 0);
  let idleFx = false, lastFxFrame = 0;
  function frame(now) {
    raf = 0;
    // Only the CRT is animating: 30 frames a second are plenty for it.
    if (idleFx && now - lastFxFrame < 31 && !texDirty) { request(); return; }
    idleFx = false; lastFxFrame = now;
    const dt = Math.min(100, now - last); last = now;
    let busy = false;
    if (view.dolly) {                                   // the opening dolly (eased, 2.6 s)
      const k = Math.min(1, (now - view.dolly.t0) / view.dolly.ms), e = k < 0.5 ? 4 * k * k * k : 1 - Math.pow(-2 * k + 2, 3) / 2;
      view.zoom = view.zoomTarget = view.dolly.from + (view.dolly.to - view.dolly.from) * e;
      if (k >= 1) view.dolly = null;
      busy = true;
    } else if (Math.abs(view.zoomTarget - view.zoom) > 0.0005) {
      view.zoom += (view.zoomTarget - view.zoom) * (1 - Math.exp(-dt / (reduceMotion ? 20 : view.zoomTau)));
      busy = true;
    } else { view.zoom = view.zoomTarget; view.zoomTau = 90; }
    if (view.spring) {
      const k = (now - view.spring.t0) / view.spring.ms;
      if (k >= 1) { view.yaw = 0; view.pitch = 0; view.spring = null; }
      else {
        const e = k <= 0 ? 0 : easeInOut(k);
        view.yaw = view.spring.yaw * (1 - e); view.pitch = view.spring.pitch * (1 - e);
        busy = true;
      }
    }
    for (const k of keys.values()) {
      const target = k.down > 0 || k.held ? 1 : 0;
      if (k.t !== target) {
        k.t = target > k.t ? Math.min(1, k.t + dt / KEY_TIME) : Math.max(0, k.t - dt / KEY_TIME);
        k.node.position.y = k.base - KEY_TRAVEL * k.t;
        busy = true;
      }
    }
    if (mouse.button) {
      const target = mouse.down ? 1 : 0;
      if (mouse.t !== target) {
        mouse.t = target > mouse.t ? Math.min(1, mouse.t + dt / KEY_TIME) : Math.max(0, mouse.t - dt / KEY_TIME);
        mouse.button.position.y = mouse.button.userData.baseY - BUTTON_TRAVEL * mouse.t;
        busy = true;
      }
    }
    if (mouse.node) {
      const p = mouse.node.position, tx = mouse.base.x + mouse.target.x, tz = mouse.base.z + mouse.target.y;
      if (Math.abs(p.x - tx) + Math.abs(p.z - tz) > 0.01) {
        p.x += (tx - p.x) * Math.min(1, dt / 60); p.z += (tz - p.z) * Math.min(1, dt / 60);
        busy = true; contact.dirty = true;
      }
    }
    if (floppy.anim) { floppyStep(now); busy = true; }
    updateCamera();
    if (opts.onView) opts.onView(view.yaw, view.pitch, view.zoom);
    if (texDirty) { tex.needsUpdate = true; texDirty = false; }
    crt.uniforms.time.value = (now / 1000) % 1000;
    const fxLevel = liveCrt();
    crt.uniforms.fx.value = fxLevel > 0 ? 1 : 0;
    crt.uniforms.afterAmt.value = fxLevel > 1 ? 0.45 : 0;
    crt.uniforms.after.value = afterglow.texture();
    // The key light's shadow map is drawn only when the static scene changes (start, quality): the
    // 3D keys' 2.5 mm travel doesn't change it, and the mouse casts no shadow into it (its own soft
    // contact shadow follows it). Its softness is filtering done every frame (PCF), so it never
    // hardens, whatever moves.
    if (shadowsDirty) { renderer.shadowMap.needsUpdate = true; shadowsDirty = false; }
    if (contact.dirty) { contact.update(); contact.dirty = false; }
    renderer.render(scene, camera);
    if (fxLevel > 1) afterglow.update(dt);
    if (onPlaced) onPlaced();
    // Auto quality: if the first animated frames are slow, step down.
    if (autoQuality && busy && frameTimes.length < 40) {
      frameTimes.push(dt);
      if (frameTimes.length === 40) {
        const avg = frameTimes.slice(5).reduce((s, v) => s + v, 0) / 35;
        if (avg > 30 && qualityName !== "low") setQuality(qualityName === "high" ? "medium" : "low");
      }
    }
    if (busy || view.dragging || view.spring) request();
    else if (fxLevel > 0) { idleFx = true; request(); }        // the live CRT keeps drawing (at about 30 fps)
  }

  function setQuality(name) {
    if (!QUALITY[name]) return;
    qualityName = name; Q = QUALITY[name];
    renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, Q.dpr));
    renderer.shadowMap.enabled = Q.shadow > 0;
    key.castShadow = Q.shadow > 0;
    if (Q.shadow > 0) {
      key.shadow.mapSize.set(Q.shadow, Q.shadow);
      if (key.shadow.map) { key.shadow.map.dispose(); key.shadow.map = null; }
    }
    shadowsDirty = true;
    resize();
  }

  function resize() {
    const w = container.clientWidth, h = container.clientHeight;
    if (!w || !h) return;
    W = w; H = h;
    const ins = insets ? insets() : { top: 0, bottom: 0 };
    safeTop = ins.top; safeBottom = ins.bottom;
    renderer.setSize(w, h, false);
    canvas.style.width = `${w}px`; canvas.style.height = `${h}px`;
    computeDistances();
    request();
  }
  setQuality(qualityName);
  const ro = new ResizeObserver(resize);
  ro.observe(container);
  resize();

  // ---------------------------------------------------------------- the opening
  // Head-on from the front, the whole desk in view (zoom 0); dollyIn() then moves straight in along
  // the front axis to the working zoom. Any input stops it where it is.
  request();

  return {
    renderer, scene, camera, view, picture, glass, badge,
    // For tests: the glass's reflections on or off (its base shading stays).
    setGlare(on) { glassMat.envMapIntensity = on ? 1 : 0; request(); },
    screenChanged() { texDirty = true; request(); },
    setStarted(b) { started = b; request(); },
    key(code, down) {                          // real keyboard: animate only
      const k = keys.get(KEY_NODES[code]);
      if (!k) return;
      k.down = Math.max(0, k.down + (down ? 1 : -1));
      request();
    },
    releaseKeys() { for (const k of keys.values()) { k.down = 0; } request(); },
    mouseButton(down) { setButton(down); },
    mouseAt,
    resetView(share = 0.62) { view.dolly = null; view.zoomTarget = zoomForScreen(share); view.zoomTau = 260; startSpring(); },
    glideTo(z, tau = 380) { view.zoomTarget = Math.max(0, Math.min(1, z)); view.zoomTau = tau; request(); },
    // The opening: head-on, from the whole desk in to the working zoom along the front axis.
    dollyIn(z, ms = 2600) {
      z = Math.max(0, Math.min(1, z));
      if (reduceMotion) { view.zoom = view.zoomTarget = z; request(); return; }
      view.dolly = { from: view.zoom, to: z, t0: performance.now(), ms }; request();
    },
    stopDolly() { stopDolly(); request(); },
    // The 3D floppy: true when this disk is the one shown going in (the first in the drive).
    floppyIn(id, name) {
      if (!floppy.group || floppy.state !== "out") return false;
      drawLabel(name || "");
      floppy.state = "in"; floppy.id = id;
      floppy.anim = reduceMotion ? { kind: "fade", t0: performance.now(), timing: floppyTiming, after: "in" } : { kind: "in", t0: performance.now(), timing: floppyTiming };
      request(); return true;
    },
    floppyOut(id) {
      if (!floppy.group || floppy.state !== "in" || floppy.id !== id) return false;
      floppy.state = "ejecting";
      floppy.anim = reduceMotion ? { kind: "fade", t0: performance.now(), timing: floppyTiming, after: "out" } : { kind: "out", t0: performance.now(), timing: floppyTiming };
      request(); return true;
    },
    // The disk going in before the emulator has numbered it (the animation starts when the image is
    // ready to send): give it its number when the emulator reports it.
    floppySetId(id) { if (floppy.state === "in" && floppy.id === -1) floppy.id = id; },
    floppyReset() { if (!floppy.group) return; floppy.anim = null; floppy.state = "out"; floppy.id = null; floppyPose(OUT_Z(), 0, 0); request(); },
    floppyInfo: () => ({ state: floppy.state, loaded: !!floppy.group, slot: [floppy.slotX, floppy.slotY, floppy.slotZ], anim: floppy.anim && floppy.anim.kind }),
    zoomForScreen,
    jumpTo(z) { view.dolly = null; view.zoom = view.zoomTarget = z; view.yaw = view.pitch = 0; view.spring = null; request(); },
    setZoom: (z) => { stopDolly(); setZoom(z); }, isFront, relayout: resize,
    quality: () => qualityName, setQuality,
    keyNames: () => [...keys.keys()],
    hasMouseButton: () => !!mouse.button,
    keyState: (code) => { const k = keys.get(KEY_NODES[code]); return k ? { t: k.t, y: k.node.position.y, base: k.base } : null; },
    keyStateNode: (name) => { const k = keys.get(name); return k ? { t: k.t, held: k.held, y: k.node.position.y, base: k.base } : null; },
    latched: () => [...latched],
    buttonState: () => (mouse.button ? { t: mouse.t, y: mouse.button.position.y, base: mouse.button.userData.baseY } : null),
    clientFromMac,
    clientFromNode(name) {                     // the top of a key, for tests
      const k = keys.get(name); if (!k) return null;
      const b = new THREE.Box3().setFromObject(k.node); const c = b.getCenter(new THREE.Vector3()); c.y = b.max.y;
      return toClient(c);
    },
    clientFromBadge() { if (!badge) return null; return toClient(badge.getWorldPosition(new THREE.Vector3())); },
    clientFromLogo() { if (!badge) return null; return toClient(badge.userData.logoCenter); },
    clientFromButton() {
      if (!button) return null;
      const b = new THREE.Box3().setFromObject(button); const c = b.getCenter(new THREE.Vector3()); c.y = b.max.y;
      return toClient(c);
    },
    pictureClientRect,
    // For tests: where a point of the glass lands on the page, given in cm from the glass's centre
    // along the screen's right and up (the picture is imageSize, the glass glassSize).
    clientFromGlass(x, y) {
      const at = glassCenter.clone().addScaledVector(right, x).addScaledVector(up, y);
      ray.set(at.clone().addScaledVector(normal, 10), normal.clone().negate());
      ray.layers.enableAll();
      const hit = ray.intersectObject(glass, false)[0];
      return hit ? toClient(hit.point) : null;
    },
    imageSize: [imageW, imageH], glassSize: [glassW, glassH],
    glassOpening: opening,
    // Where the camera's target (the Mac's centre) lands, and the centre of the area between the bars.
    targetClient: () => toClient(T),
    safeCenter: () => { const r = canvas.getBoundingClientRect(); return [r.left + W / 2, r.top + safeTop + (H - safeTop - safeBottom) / 2]; },
    // For the alignment test: the Mac pixel the GPU draws under a client point (debug render of the
    // picture alone with its UV encoded), or null where the picture isn't.
    renderedMacPixelAt(cx, cy) {
      const r = canvas.getBoundingClientRect(), pr = renderer.getPixelRatio();
      const bw = Math.round(W * pr), bh = Math.round(H * pr);
      const rt = new THREE.WebGLRenderTarget(bw, bh, { depthBuffer: true });
      const mat = picture.material;
      picture.material = new THREE.ShaderMaterial({ vertexShader: CRT_VERTEX, fragmentShader: UVCODE_FRAGMENT, toneMapped: false });
      const bg = scene.background; scene.background = null;            // cleared to alpha 0: no picture there
      const fog = scene.fog; scene.fog = null;
      camera.layers.set(2);
      renderer.setRenderTarget(rt); renderer.setClearColor(0x000000, 0); renderer.clear(); renderer.render(scene, camera);
      const px = new Uint8Array(4);
      const x = Math.floor((cx - r.left) * pr), y = Math.floor((cy - r.top) * pr);
      renderer.readRenderTargetPixels(rt, x, bh - 1 - y, 1, 1, px);
      renderer.setRenderTarget(null); renderer.setClearColor(0x000000, 1);
      camera.layers.set(0); camera.layers.enable(1); camera.layers.enable(3);
      scene.background = bg; scene.fog = fog;
      picture.material.dispose(); picture.material = mat; rt.dispose();
      request();
      if (px[3] < 200) return null;
      return [px[0] + 256 * (px[1] & 15), px[2] + 256 * (px[1] >> 4)];
    },
    destroy() { destroyed = true; ro.disconnect(); renderer.dispose(); canvas.remove(); },
  };
}

// A dark photo studio for image-based lighting: a near-black room with a big warm overhead softbox
// a little in front, a dim wall of light in front, a dim fill on the right and a cool rim strip behind.
function studioScene() {
  const s = new THREE.Scene();
  const room = new THREE.SphereGeometry(45, 32, 16);
  const col = [];
  const p = room.attributes.position;
  for (let i = 0; i < p.count; i++) { const y = p.getY(i) / 45; const v = 0.012 + 0.03 * Math.max(0, y); col.push(v, v, v * 1.05); }
  room.setAttribute("color", new THREE.Float32BufferAttribute(col, 3));
  s.add(new THREE.Mesh(room, new THREE.MeshBasicMaterial({ vertexColors: true, side: THREE.BackSide })));
  const panel = (w, h, pos, intensity, color) => {
    const m = new THREE.Mesh(new THREE.PlaneGeometry(w, h), new THREE.MeshBasicMaterial({ color: new THREE.Color(color).multiplyScalar(intensity), side: THREE.DoubleSide }));
    m.position.set(...pos); m.lookAt(0, 0, 0); s.add(m);
  };
  panel(34, 26, [-4, 40, 12], 6, 0xfff3e6);     // key: the big overhead softbox, a little in front
  panel(30, 14, [0, 6, 40], 0.7, 0xf4f1ec);     // a dim wall of light in front, so the front stays readable
  panel(18, 14, [34, 8, 22], 1.6, 0xeef3ff);    // fill
  panel(44, 5, [0, 22, -36], 5, 0xffffff);      // rim strip
  return s;
}

// A smooth surface fitted to points given in the screen's basis (x right, y up, z out): an elliptic
// paraboloid, z = c0 + c1 x² + c2 y², least squares. Its curvature is the same all over, as on a
// real CRT's glass, so reflections keep their shape as they slide across. The model's glass is a
// low-poly dome (flat facets) that would break reflections into steps; its fit is smooth.
function fitParaboloid(pts) {
  const N = 3, B = (x, y) => [1, x * x, y * y];
  const A = Array.from({ length: N }, () => new Array(N + 1).fill(0));
  for (const [x, y, z] of pts) {
    const b = B(x, y);
    for (let r = 0; r < N; r++) { for (let c = 0; c < N; c++) A[r][c] += b[r] * b[c]; A[r][N] += b[r] * z; }
  }
  for (let c = 0; c < N; c++) {                // Gauss-Jordan with partial pivoting
    let piv = c; for (let r = c + 1; r < N; r++) if (Math.abs(A[r][c]) > Math.abs(A[piv][c])) piv = r;
    [A[c], A[piv]] = [A[piv], A[c]];
    for (let r = 0; r < N; r++) if (r !== c) { const f = A[r][c] / A[c][c]; for (let k = c; k <= N; k++) A[r][k] -= f * A[c][k]; }
  }
  const k = A.map((row, i) => row[N] / row[i]);
  return {
    k,
    z: (x, y) => k[0] + k[1] * x * x + k[2] * y * y,
    // The normal is linear in x and y before normalising, so per-vertex normals interpolate exactly.
    normal: (x, y) => new THREE.Vector3(-2 * k[1] * x, -2 * k[2] * y, 1).normalize(),
  };
}

// The CRT glass's own environment (only the glass uses it): the same dark studio drawn crisp, with
// hard-edged rounded softboxes, so the polished glass shows defined reflections instead of a haze.
// "soft" is the direction (from the glass) of the big key softbox whose reflection sits near the top
// left corner of the glass in the front view; it was worked out from the glass's own curvature.
function glassStudio(lights) {
  const s = new THREE.Scene();
  const room = new THREE.SphereGeometry(45, 32, 16);
  const col = [];
  const p = room.attributes.position;
  for (let i = 0; i < p.count; i++) { const y = p.getY(i) / 45; const v = 0.004 + 0.012 * Math.max(0, y); col.push(v, v, v * 1.05); }
  room.setAttribute("color", new THREE.Float32BufferAttribute(col, 3));
  s.add(new THREE.Mesh(room, new THREE.MeshBasicMaterial({ vertexColors: true, side: THREE.BackSide })));
  // Each light: a direction, an angular width and height (degrees), corner radius (share of the
  // smaller side), brightness and colour. Each is a quad whose texture holds the rounded shape with
  // an anti-aliased edge (about 0.15 degrees: crisp, but no stair steps when the reflection is
  // magnified), and for softboxes a diffuser a little hotter in the middle.
  const D = 30, size = (deg) => 2 * D * Math.tan(THREE.MathUtils.degToRad(deg) / 2);
  for (const l of lights) {
    const pxDeg = Math.min(48, 512 / Math.max(l.w, l.h));
    const cw = Math.max(8, Math.round(l.w * pxDeg) + 8), ch = Math.max(8, Math.round(l.h * pxDeg) + 8);
    const img = new ImageData(cw, ch), d = img.data;
    const hw = l.w * pxDeg / 2, hh = l.h * pxDeg / 2, rr = Math.min(hw, hh) * 2 * (l.r ?? 0.2), feather = Math.max(1, 0.15 * pxDeg);
    for (let j = 0; j < ch; j++) for (let i = 0; i < cw; i++) {
      const px = Math.abs(i + 0.5 - cw / 2) - (hw - rr), py = Math.abs(j + 0.5 - ch / 2) - (hh - rr);
      const sd = Math.hypot(Math.max(px, 0), Math.max(py, 0)) + Math.min(Math.max(px, py), 0) - rr;   // signed distance to the rounded rect
      const a = Math.min(1, Math.max(0, 0.5 - sd / feather));
      const q = l.hot ? 1 - 0.38 * Math.min(1, ((i + 0.5 - cw / 2) / hw) ** 2 * 0.6 + ((j + 0.5 - ch / 2) / hh) ** 2 * 0.6) : 1;
      const k = (j * cw + i) * 4; d[k] = d[k + 1] = d[k + 2] = Math.round(255 * q); d[k + 3] = Math.round(255 * a);
    }
    const tex = new THREE.DataTexture(img.data, cw, ch); tex.needsUpdate = true;
    tex.magFilter = THREE.LinearFilter; tex.minFilter = THREE.LinearFilter; tex.flipY = false;
    const m = new THREE.Mesh(new THREE.PlaneGeometry(size(l.w) * cw / (l.w * pxDeg), size(l.h) * ch / (l.h * pxDeg)),
      new THREE.MeshBasicMaterial({ map: tex, color: new THREE.Color(l.color ?? 0xffffff).multiplyScalar(l.i), transparent: true, depthWrite: false, side: THREE.DoubleSide }));
    m.position.copy(l.dir).normalize().multiplyScalar(D); m.lookAt(0, 0, 0); m.rotateZ(l.roll ?? 0); s.add(m);
  }
  return s;
}

// Plastics: the source colours are flat textures; make them read as slightly glossy beige ABS, with
// the Mac's fine pebbled "orange peel" finish on the case, keyboard and mouse (normals and roughness
// only, the colour is untouched), finer and weaker on the keycaps.
function dressMaterials(model, Q, peel) {
  const make = (m, rough, peelOpts, extra = {}) => {
    const P = Q.physical ? THREE.MeshPhysicalMaterial : THREE.MeshStandardMaterial;
    const n = new P({ map: m.map, color: m.color, roughness: rough, metalness: 0, envMapIntensity: 1.0,
      ...(Q.physical ? { specularIntensity: 0.55, ...(Q.clearcoat ? { clearcoat: 0.18, clearcoatRoughness: 0.42 } : {}) } : {}), ...extra });
    n.name = m.name;
    if (peelOpts) addPeel(n, peel, peelOpts);
    return n;
  };
  model.traverse((o) => {
    if (!o.isMesh || !o.material) return;
    const m = o.material;
    switch (m.name) {
      case "Computer": case "Lightplastic": case "Whiteplastic":
        o.material = make(m, 0.44, { scale: 1 / 5, strength: 0.3, rough: 0.45 });
        break;
      case "Keys": o.material = make(m, 0.52, { scale: 1 / 3.5, strength: 0.12, rough: 0.2 }); break;
      case "Blackplastic": o.material = make(m, 0.55); break;
      case "Darkplastic": o.material = make(m, 0.5); break;
      default: break;                            // Metal, Screen
    }
  });
}

// The orange-peel surface: a tileable height field of rounded pebbles (two octaves of blurred
// noise, shaped), stored as a normal map (RG) plus height (B). Generated here, no download.
function makePeelTexture(size) {
  // The pebble pattern is made at 512² (same seed, so the same layout and pebble size at every
  // resolution), then enlarged to the map's size with a smooth (cubic B-spline) filter and shaped with
  // a steep curve, so each pebble gets more texels and crisp edges instead of a soft blur.
  const B = 512;
  let seed = 0x9e3779b9;
  const rnd = () => { seed = (seed + 0x6d2b79f5) | 0; let t = Math.imul(seed ^ (seed >>> 15), 1 | seed); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
  const NB = B * B;
  const blur = (a, r) => {                                 // box blur with wrap-around, horizontal then vertical
    const b = new Float32Array(NB), n = 2 * r + 1;
    for (let y = 0; y < B; y++) for (let x = 0; x < B; x++) {
      let s = 0; for (let k = -r; k <= r; k++) s += a[y * B + ((x + k + B) % B)]; b[y * B + x] = s / n;
    }
    for (let y = 0; y < B; y++) for (let x = 0; x < B; x++) {
      let s = 0; for (let k = -r; k <= r; k++) s += b[((y + k + B) % B) * B + x]; a[y * B + x] = s / n;
    }
    return a;
  };
  const norm = (a) => { let lo = 1e9, hi = -1e9; for (const v of a) { lo = Math.min(lo, v); hi = Math.max(hi, v); } return a.map((v) => (v - lo) / (hi - lo)); };
  let fine = new Float32Array(NB).map(rnd); blur(fine, 1); blur(fine, 1); fine = norm(fine);
  let coarse = new Float32Array(NB).map(rnd); blur(coarse, 3); blur(coarse, 3); coarse = norm(coarse);
  const base = new Float32Array(NB);
  for (let i = 0; i < NB; i++) base[i] = 0.7 * fine[i] + 0.3 * coarse[i];
  // Enlarge (separable cubic B-spline, wrapping), rows then columns.
  const S = size, f = B / S;
  // Cubic B-spline: smooth to the second derivative, so the normals (slopes) have no kinks on the
  // 512² grid (Catmull-Rom left faint grid lines in the relief).
  const cr = (p0, p1, p2, p3, t) => { const t2 = t * t, t3 = t2 * t, u = 1 - t;
    return (u * u * u * p0 + (3 * t3 - 6 * t2 + 4) * p1 + (-3 * t3 + 3 * t2 + 3 * t + 1) * p2 + t3 * p3) / 6; };
  let v = base;
  if (S !== B) {
    const rows = new Float32Array(B * S);
    for (let y = 0; y < B; y++) for (let x = 0; x < S; x++) {
      const u = (x + 0.5) * f - 0.5, i = Math.floor(u), t = u - i, at = (k) => base[y * B + ((i + k + B) % B)];
      rows[y * S + x] = cr(at(-1), at(0), at(1), at(2), t);
    }
    v = new Float32Array(S * S);
    for (let y = 0; y < S; y++) {
      const u = (y + 0.5) * f - 0.5, i = Math.floor(u), t = u - i;
      const r0 = ((i - 1 + B) % B) * S, r1 = ((i + B) % B) * S, r2 = ((i + 1) % B) * S, r3 = ((i + 2) % B) * S;
      for (let x = 0; x < S; x++) v[y * S + x] = cr(rows[r0 + x], rows[r1 + x], rows[r2 + x], rows[r3 + x], t);
    }
  }
  const N = S * S, h = new Float32Array(N);
  for (let i = 0; i < N; i++) {
    const t = Math.min(1, Math.max(0, (v[i] - 0.375) / 0.3));   // same midpoint as before, steeper edges
    h[i] = t * t * (3 - 2 * t);                                 // rounded tops, flat-ish valleys
  }
  const data = new Uint8Array(N * 4), k = 2.2 * S / B;          // slope per world distance as at 512²
  for (let y = 0; y < S; y++) for (let x = 0; x < S; x++) {
    const i = y * S + x;
    const dx = h[y * S + (x + 1) % S] - h[y * S + (x - 1 + S) % S];
    const dy = h[((y + 1) % S) * S + x] - h[((y - 1 + S) % S) * S + x];
    let nx = -dx * k, ny = -dy * k, nz = 1; const l = Math.hypot(nx, ny, nz); nx /= l; ny /= l;
    data[i * 4] = Math.round((nx * 0.5 + 0.5) * 255); data[i * 4 + 1] = Math.round((ny * 0.5 + 0.5) * 255);
    data[i * 4 + 2] = Math.round(h[i] * 255); data[i * 4 + 3] = 255;
  }
  const tex = new THREE.DataTexture(data, S, S, THREE.RGBAFormat);
  tex.wrapS = tex.wrapT = THREE.RepeatWrapping;
  tex.magFilter = THREE.LinearFilter; tex.minFilter = THREE.LinearMipmapLinearFilter; tex.generateMipmaps = true;
  tex.colorSpace = THREE.NoColorSpace;
  tex.needsUpdate = true;
  return tex;
}

// Triplanar (world-space) orange peel for a standard/physical material: the model's UVs don't
// matter, the scale is physical (o.scale = tiles per cm), no seams. Tops glossier, valleys matte.
// With o.ink, the material's alpha thins slightly on the peaks (pad-printed ink).
// The key light's direction for the orange peel (set when the light is made).
const PEEL_KEY = { value: new THREE.Vector3(0, 1, 0) };
function addPeel(mat, peel, o) {
  mat.onBeforeCompile = (sh) => {
    sh.uniforms.peelMap = { value: peel };
    sh.uniforms.peelScale = { value: o.scale };
    sh.uniforms.peelStrength = { value: o.strength };
    sh.uniforms.peelRough = { value: o.rough };
    sh.uniforms.peelKey = PEEL_KEY;
    sh.vertexShader = sh.vertexShader
      .replace("#include <common>", "#include <common>\nvarying vec3 vPeelPos;\nvarying vec3 vPeelNrm;")
      .replace("#include <project_vertex>", "#include <project_vertex>\nvPeelPos = (modelMatrix * vec4(transformed, 1.0)).xyz;\nvPeelNrm = normalize(mat3(modelMatrix) * objectNormal);");
    sh.fragmentShader = sh.fragmentShader
      .replace("#include <common>", "#include <common>\nvarying vec3 vPeelPos;\nvarying vec3 vPeelNrm;\nuniform sampler2D peelMap;\nuniform float peelScale, peelStrength, peelRough;\nuniform vec3 peelKey;")
      .replace("#include <roughnessmap_fragment>", `
        vec3 pN = normalize(vPeelNrm);
        vec3 pW = pow(abs(pN), vec3(4.0)); pW /= (pW.x + pW.y + pW.z);
        vec4 pX = texture2D(peelMap, vPeelPos.zy * peelScale);
        vec4 pY = texture2D(peelMap, vPeelPos.xz * peelScale);
        vec4 pZ = texture2D(peelMap, vPeelPos.xy * peelScale);
        float peelH = pW.x * pX.z + pW.y * pY.z + pW.z * pZ.z;
        #include <roughnessmap_fragment>
        roughnessFactor = clamp(roughnessFactor * (1.0 + peelRough * (0.5 - peelH) * 2.0), 0.05, 1.0);
        ${o.ink ? "diffuseColor.a *= mix(1.0, 0.72, smoothstep(0.55, 0.95, peelH));" : ""}`)
      .replace("#include <normal_fragment_maps>", `#include <normal_fragment_maps>
        {
          vec3 pert = pW.x * vec3(0.0, pX.y * 2.0 - 1.0, pX.x * 2.0 - 1.0)
                    + pW.y * vec3(pY.x * 2.0 - 1.0, 0.0, pY.y * 2.0 - 1.0)
                    + pW.z * vec3(pZ.x * 2.0 - 1.0, pZ.y * 2.0 - 1.0, 0.0);
          // On faces turned away from the overhead key (in its shade) the bumps are toned down, so
          // they can't flicker in and out of its light and shadow (no mottling in the bezel recess).
          float keyFacing = smoothstep(-0.05, 0.35, dot(pN, peelKey));
          vec3 nW = normalize(pN + pert * peelStrength * mix(0.35, 1.0, keyFacing));
          normal = normalize(normal + (viewMatrix * vec4(nW - pN, 0.0)).xyz);
          // Specular anti-aliasing: where the bumps change faster than a pixel, widen the highlight
          // instead of letting it sparkle (no bright speckles at grazing angles or far away).
          roughnessFactor = min(1.0, max(roughnessFactor, 0.28) + 0.7 * length(fwidth(nW)));
        }`);
  };
  mat.customProgramCacheKey = () => `peel-${o.scale}-${o.strength}-${o.rough}-${o.ink ? 1 : 0}`;
}

// The "Macintosh Plus" badge, pad-printed to the right of the rainbow Apple logo as on a real
// Plus. The logo is found by its colours in the case texture, then on the mesh by UV, which gives
// its place and size in the world. The text is EB Garamond (OFL, self-hosted), condensed and tight
// like Apple Garamond, in charcoal-navy ink: cap height 0.42 of the logo's height, the capitals'
// middle level with the logo's middle, 0.6 logo widths from the logo.
async function makeBadge(model, Q, peel, fontUrl) {
  let mesh = null;
  model.traverse((o) => { if (o.isMesh && o.name === "Computer_Computer_0") mesh = o; });
  if (!mesh || !mesh.material.map || !mesh.material.map.image) return null;
  const img = mesh.material.map.image;
  const tw = img.width, th = img.height;
  const c = document.createElement("canvas"); c.width = tw; c.height = th;
  const cx = c.getContext("2d", { willReadFrequently: true }); cx.drawImage(img, 0, 0);
  const px = cx.getImageData(0, 0, tw, th).data;
  let x0 = 1e9, y0 = 1e9, x1 = -1, y1 = -1;
  for (let y = 0; y < th; y++) for (let x = 0; x < tw; x++) {
    const i = (y * tw + x) * 4, mx = Math.max(px[i], px[i + 1], px[i + 2]), mn = Math.min(px[i], px[i + 1], px[i + 2]);
    if (mx - mn > 60) { x0 = Math.min(x0, x); x1 = Math.max(x1, x); y0 = Math.min(y0, y); y1 = Math.max(y1, y); }
  }
  if (x1 < 0) return null;
  // UV box of the logo (glTF textures: v down). The logo is painted inside large triangles, so
  // map UV points to the surface through the triangle that contains them (barycentric in UV, the
  // same weights in 3D).
  const map = mesh.material.map; map.updateMatrix();
  const uv = mesh.geometry.attributes.uv, pos = mesh.geometry.attributes.position, index = mesh.geometry.index;
  mesh.updateMatrixWorld(true);
  const ua = new THREE.Vector2(), ub = new THREE.Vector2(), uc = new THREE.Vector2();
  const uvAt = (i, out) => out.fromBufferAttribute(uv, i).applyMatrix3(map.matrix);
  function surfaceAt(u, v) {
    const n = index ? index.count : pos.count;
    for (let t = 0; t < n; t += 3) {
      const i0 = index ? index.getX(t) : t, i1 = index ? index.getX(t + 1) : t + 1, i2 = index ? index.getX(t + 2) : t + 2;
      uvAt(i0, ua); uvAt(i1, ub); uvAt(i2, uc);
      const d = (ub.y - uc.y) * (ua.x - uc.x) + (uc.x - ub.x) * (ua.y - uc.y);
      if (Math.abs(d) < 1e-12) continue;
      const w0 = ((ub.y - uc.y) * (u - uc.x) + (uc.x - ub.x) * (v - uc.y)) / d;
      const w1 = ((uc.y - ua.y) * (u - uc.x) + (ua.x - uc.x) * (v - uc.y)) / d;
      const w2 = 1 - w0 - w1;
      if (w0 < -1e-4 || w1 < -1e-4 || w2 < -1e-4) continue;
      const A = new THREE.Vector3().fromBufferAttribute(pos, i0), B = new THREE.Vector3().fromBufferAttribute(pos, i1), C = new THREE.Vector3().fromBufferAttribute(pos, i2);
      const p = A.clone().multiplyScalar(w0).addScaledVector(B, w1).addScaledVector(C, w2).applyMatrix4(mesh.matrixWorld);
      const nn = new THREE.Vector3().crossVectors(B.clone().sub(A), C.clone().sub(A)).transformDirection(mesh.matrixWorld).normalize();
      return { p, n: nn };
    }
    return null;
  }
  const u0 = x0 / tw, u1 = (x1 + 1) / tw, v0 = y0 / th, v1 = (y1 + 1) / th;
  const mid = surfaceAt((u0 + u1) / 2, (v0 + v1) / 2);
  const cs = [[u0, v0], [u1, v0], [u0, v1], [u1, v1]].map(([u, v]) => surfaceAt(u, v));
  if (!mid || cs.some((c) => !c)) return null;
  const n = mid.n.z < 0 ? mid.n.negate() : mid.n;          // facing the viewer (+z)
  const rightV = new THREE.Vector3().crossVectors(new THREE.Vector3(0, 1, 0), n).normalize();
  const upV = new THREE.Vector3().crossVectors(n, rightV).normalize();
  // The texture is rotated on this quad (u runs up the case), so measure the logo's extent from all
  // four corners, whatever the mapping's orientation.
  const along = (vec) => cs.map((c) => c.p.clone().sub(mid.p).dot(vec));
  const extR = along(rightV), extU = along(upV);
  const logoW = Math.max(...extR) - Math.min(...extR), logoH = Math.max(...extU) - Math.min(...extU);
  const logoC = mid.p.clone().addScaledVector(rightV, (Math.max(...extR) + Math.min(...extR)) / 2).addScaledVector(upV, (Math.max(...extU) + Math.min(...extU)) / 2);

  // The text, drawn big for close zoom.
  const family = "BadgeGaramond";
  let loaded = false;
  if (fontUrl) {                                        // (document.fonts.check() is true for unknown families)
    try { const ff = new FontFace(family, `url(${fontUrl})`, { weight: "500" }); await ff.load(); document.fonts.add(ff); loaded = true; }
    catch (err) { console.warn("badge font:", err); }
  }
  const H = 512, fontPx = 360, condense = 0.86;
  const font = `500 ${fontPx}px ${loaded ? family : "Georgia, serif"}`;
  const cv = document.createElement("canvas");
  const g = cv.getContext("2d");
  const setFont = () => { g.font = font; try { g.letterSpacing = "-6px"; } catch { /* older browsers */ } };
  setFont();
  const text = "Macintosh Plus", m = g.measureText(text);
  const capPx = g.measureText("M").actualBoundingBoxAscent || fontPx * 0.66;
  cv.width = Math.ceil(m.width * condense + 40); cv.height = H;      // (resizing resets the context)
  setFont();
  const base = H * 0.72;
  g.fillStyle = "#2a2f3d";
  g.setTransform(condense, 0, 0, 1, 20, 0);
  g.fillText(text, 0, base);
  const tex = new THREE.CanvasTexture(cv);
  tex.colorSpace = THREE.SRGBColorSpace; tex.anisotropy = 8;
  tex.minFilter = THREE.LinearMipmapLinearFilter; tex.generateMipmaps = true;

  // World size and place.
  const capW = 0.42 * logoH, perPx = capW / capPx;
  const w = cv.width * perPx, h = H * perPx;
  const P = THREE.MeshPhysicalMaterial && Q.physical ? THREE.MeshPhysicalMaterial : THREE.MeshStandardMaterial;
  const mat = new P({ map: tex, transparent: true, roughness: 0.7, metalness: 0, depthWrite: false,
    polygonOffset: true, polygonOffsetFactor: -2, polygonOffsetUnits: -2 });
  addPeel(mat, peel, { scale: 1 / 5, strength: 0.3, rough: 0.2, ink: true });
  const plane = new THREE.Mesh(new THREE.PlaneGeometry(w, h), mat);
  plane.name = "Badge";
  const leftEdge = logoC.clone().addScaledVector(rightV, logoW / 2 + 0.6 * logoW - 20 * perPx);
  // Level with the logo: the middle of the capitals (baseline to cap height) on the logo's middle.
  const baseline = logoC.clone().addScaledVector(upV, -capW / 2);
  const center = leftEdge.clone().addScaledVector(rightV, w / 2);
  // The canvas's baseline row (base px from the top) sits at the wanted baseline.
  center.addScaledVector(upV, baseline.clone().sub(logoC).dot(upV) + (base - H / 2) * perPx);
  // The logo sits in a recess: put the badge on the case's real surface there. Rays along the
  // logo's normal at the text's ends, middle and top and bottom find the surface; the plane, facing
  // along that normal, sits just above the highest of them, so no part of the text dips into the case.
  const meshes = []; model.traverse((o) => { if (o.isMesh) meshes.push(o); });
  const nn = n.clone().normalize();
  let lift = null;
  for (const [fx, fy] of [[0, 0], [-0.45, 0], [0.45, 0], [0, 0.3], [0, -0.3], [-0.45, 0.3], [0.45, -0.3], [0.45, 0.3], [-0.45, -0.3]]) {
    const p = center.clone().addScaledVector(rightV, fx * w).addScaledVector(upV, fy * h);
    const hit = new THREE.Raycaster(p.clone().addScaledVector(nn, 4), nn.clone().negate()).intersectObjects(meshes, false)[0];
    if (hit) { const d = hit.point.clone().sub(center).dot(nn); lift = lift === null ? d : Math.max(lift, d); }
  }
  const r2 = new THREE.Vector3().crossVectors(new THREE.Vector3(0, 1, 0), nn).normalize(), u2 = new THREE.Vector3().crossVectors(nn, r2).normalize();
  center.addScaledVector(nn, (lift ?? 0) + 0.012);
  plane.position.copy(center);
  plane.quaternion.setFromRotationMatrix(new THREE.Matrix4().makeBasis(r2, u2, nn));
  plane.receiveShadow = true;
  plane.renderOrder = 1;
  plane.userData.logoCenter = logoC;
  plane.userData.logo = { w: logoW, h: logoH };
  return plane;
}

// The mouse's button is part of one dark-plastic mesh in the model (with the cord's strain relief
// and the ball). Split it off at load time: find the connected pieces of that mesh and take the
// ones on top (the button), as their own mesh that can move.
function splitMouseButton(mesh) {
  if (!mesh) return null;
  const g = mesh.geometry;
  const pos = g.attributes.position, idx = g.index;
  if (!idx) return null;
  const n = pos.count;
  const parent = new Int32Array(n).map((_, i) => i);
  const find = (a) => { while (parent[a] !== a) { parent[a] = parent[parent[a]]; a = parent[a]; } return a; };
  const unite = (a, b) => { a = find(a); b = find(b); if (a !== b) parent[a] = b; };
  const key = (i) => `${pos.getX(i).toFixed(3)},${pos.getY(i).toFixed(3)},${pos.getZ(i).toFixed(3)}`;
  const seen = new Map();
  for (let i = 0; i < n; i++) { const k = key(i); if (seen.has(k)) unite(i, seen.get(k)); else seen.set(k, i); }
  for (let t = 0; t < idx.count; t += 3) { unite(idx.getX(t), idx.getX(t + 1)); unite(idx.getX(t), idx.getX(t + 2)); }
  mesh.updateMatrix();                         // measure in the parent's space (meshopt adds a node scale)
  const comps = new Map();
  for (let i = 0; i < n; i++) {
    const r = find(i);
    if (!comps.has(r)) comps.set(r, { box: new THREE.Box3(), verts: 0 });
    const c = comps.get(r);
    c.box.expandByPoint(new THREE.Vector3(pos.getX(i), pos.getY(i), pos.getZ(i)).applyMatrix4(mesh.matrix));
    c.verts++;
  }
  const list = [...comps.entries()].map(([r, c]) => ({ r, ...c }));
  const topY = Math.max(...list.map((c) => c.box.max.y));
  const buttonRoots = new Set(list.filter((c) => c.box.max.y > topY - 0.2 && c.box.max.y - c.box.min.y < 1.5).map((c) => c.r));
  if (!buttonRoots.size || buttonRoots.size === list.length) return null;
  const keepIdx = [], btnIdx = [];
  for (let t = 0; t < idx.count; t += 3) {
    const a = idx.getX(t), b = idx.getX(t + 1), c = idx.getX(t + 2);
    (buttonRoots.has(find(a)) ? btnIdx : keepIdx).push(a, b, c);
  }
  const body = g.clone(); body.setIndex(keepIdx);
  const btn = g.clone(); btn.setIndex(btnIdx);
  // Its bounds from its own triangles (a clone keeps the whole part's box, which is wrong for picking
  // and for finding the button on screen).
  const bb = new THREE.Box3(), v = new THREE.Vector3();
  for (const i of btnIdx) bb.expandByPoint(v.fromBufferAttribute(pos, i));
  btn.boundingBox = bb; btn.boundingSphere = bb.getBoundingSphere(new THREE.Sphere());
  mesh.geometry = body;
  const button = new THREE.Mesh(btn, mesh.material);
  button.name = "MouseButton";
  button.castShadow = false; button.receiveShadow = true;
  button.position.copy(mesh.position); button.quaternion.copy(mesh.quaternion); button.scale.copy(mesh.scale);
  button.userData.baseY = button.position.y;
  mesh.parent.add(button);
  return button;
}
