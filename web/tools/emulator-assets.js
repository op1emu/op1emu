// The emulator's module in the site (README.md). Vite leaves it out of the
// bundle, as projects with a large Emscripten module do (ffmpeg.wasm,
// Pyodide): bundled, its glue's new URL("op1emu-web.wasm") would copy the
// 35 MiB .wasm the Worker never fetches, and its Node code would be
// resolved. This plugin puts op1emu-web.mjs and .wasm from the CMake build
// in the site as they are, named by their content, and the .wasm also
// brotli-compressed, which GitHub Pages does not do (it sends gzip): the
// Worker decompresses it. `virtual:op1emu` is their names, for the page to
// hand the Worker. `vite` serves the files from the build as they are, with
// no .br.

import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";

const VIRTUAL = "virtual:op1emu";

export function emulatorAssets({ build }) {
  let serving = false;
  const files = () => {
    const js = fs.readFileSync(path.join(build, "web", "op1emu-web.mjs"));
    const wasm = fs.readFileSync(path.join(build, "web", "op1emu-web.wasm"));
    const hash = crypto.createHash("sha256").update(wasm).update(js).digest("hex").slice(0, 12);
    return { js, wasm, names: { js: `op1emu-web.${hash}.mjs`, wasm: `op1emu-web.${hash}.wasm` } };
  };
  let built = null;  // the build's: the files, and the .wasm compressed

  async function manifest() {
    const { names, wasm } = built ?? files();
    return {
      js: names.js,
      wasm: { file: names.wasm, size: wasm.length },
      wasmBr: built ? { file: `${names.wasm}.br`, size: built.br.length } : null,
    };
  }

  return {
    name: "op1emu-emulator",
    configResolved(config) {
      serving = config.command === "serve";
    },
    buildStart() {
      if (serving) return;
      const read = files();
      const br = zlib.brotliCompressSync(read.wasm, {
        params: { [zlib.constants.BROTLI_PARAM_QUALITY]: 11, [zlib.constants.BROTLI_PARAM_SIZE_HINT]: read.wasm.length },
      });
      built = { ...read, br };
    },
    resolveId: (id) => (id === VIRTUAL ? `\0${VIRTUAL}` : null),
    async load(id) {
      if (id === `\0${VIRTUAL}`) return `export default ${JSON.stringify(await manifest())};`;
      return null;
    },
    generateBundle() {
      const { names, js, wasm, br } = built;
      this.emitFile({ type: "asset", fileName: names.js, source: js });
      this.emitFile({ type: "asset", fileName: names.wasm, source: wasm });
      this.emitFile({ type: "asset", fileName: `${names.wasm}.br`, source: br });
    },
    configureServer(server) {
      server.middlewares.use((request, response, next) => {
        const name = path.basename(new URL(request.url, "http://host").pathname);
        const match = /^op1emu-web\.[0-9a-f]{12}\.(mjs|wasm)$/.exec(name);
        if (!match) return next();
        const read = files();
        response.setHeader("content-type", match[1] === "mjs" ? "text/javascript" : "application/wasm");
        response.end(match[1] === "mjs" ? read.js : read.wasm);
      });
    },
  };
}
