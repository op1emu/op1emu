// The site (README.md): the page and its Worker bundled by Vite, the
// emulator's module beside them (tools/emulator-assets.js), from the CMake
// build in OP1_WASM_BUILD (../build-wasm by default).

import path from "node:path";
import { defineConfig } from "vite";
import { emulatorAssets } from "./tools/emulator-assets.js";

const root = path.dirname(new URL(import.meta.url).pathname);

export default defineConfig({
  // Relative, so that the site works at any path (GitHub Pages: /op1emu/).
  base: "./",
  build: {
    // npm dependencies' licenses; the rest is in public/THIRD_PARTY.md.
    license: { fileName: "licenses.md" },
  },
  worker: { format: "es" },
  // brotli-dec-wasm finds its .wasm by new URL(..., import.meta.url), which
  // pre-bundling would break (vitejs/vite#8427).
  optimizeDeps: { exclude: ["brotli-dec-wasm"] },
  // gui/ui.json and ui.png, the desktop frontend's.
  server: { fs: { allow: [path.join(root, "..")] } },
  plugins: [
    emulatorAssets({
      build: path.resolve(root, process.env.OP1_WASM_BUILD ?? "../build-wasm"),
      bootRomHeader: path.join(root, "../ext/bfin_sim/bf526-0.2.h"),
    }),
  ],
});
