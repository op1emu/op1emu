# op1emu in the browser

`op1emu-web` is the emulator built for WebAssembly: one module that a Web
Worker or Node imports. bcore compiles the guest's blocks to WebAssembly
modules that it loads as it goes (`ext/bcore/tools/wasm/README.md`).
bcore's WebAssembly build has no threads, so neither has this one: the host
calls `op1_step()` a slice at a time, and between the slices it takes keys,
draws frames and plays audio. Native code is the same as the desktop build's
except for `BlackFinCpu::Pace()`, which instead of sleeping tells the host how
long to wait (`PacedUntilNs`).

`src/op1_web.cpp` sets the emulator up as `gui/main.cpp` does, less what needs
threads (USB/IP, the promotion worker). Blocks are made of bcore's
copy-and-patch stencils, WebAssembly's own (`ext/bcore/tools/wasm/README.md`):
a boot to the main screen took 11 s in Node, against 44 to 49 s when LLVM
compiled every block in the module, and 21 s in Chrome 133 against 67 s. The
stencils are 43 MiB of data in memory (`-sINITIAL_MEMORY=128MB`). A block's
first 10 starts make it no module of its own (bcore's cold tier), and those
that start more often are compiled 32 to a module (`kColdThreshold`,
`kColdBatch`): 21,400 modules of a boot to 5,900, 1.2 GB of memory to 0.7, the
boot in Node 9.9 s to 8.1. LLVM compiles a block only where copy-and-patch
takes none, which no block of a boot is: its code is in a module of its own,
`op1emu-web.deferred.wasm`, which the glue loads the first time it is called
(Emscripten's `SPLIT_MODULE`; `tools/split-module.mjs` moves every function
of LLVM's but those an LDR of one RTS runs to start, after the link).
Its files are whatever the host mounted: the NAND image at the path it gives
`op1_start()`, `otp.bin` and `bootrom.bin` in the working directory. The
desktop build has the BF526's boot ROM compiled in (`ext/bfin_sim`'s
`bf526-0.2.h`); this one does not carry it, and reads `bootrom.bin` instead
(`src/cpu/bootrom.cpp`).

## The page

- `index.html`, `main.js`, `faceplate.js`: the default faceplate is a native
  browser-rendered SVG (`panel/op1.svg`), with a separate canvas for the live
  firmware display. The Panel selector also offers the original `gui/ui.png`
  with `gui/ui.json`'s calibration. Keys, encoders and volume send the same
  GPIOs in both layouts (an encoder steps its quadrature sequence a click or
  a scroll at a time). The computer keyboard's musical keys are op1kenobi's
  layout (`keymap.js`).
- `worker.js`: the emulator's Worker. The user's NAND image and `otp.bin` are
  imported into the origin private file system (OPFS), and exported from it.
  The emulator opens them through sync access handles, mounted with
  `opfs-fs.js` (WORKERFS's shape, with writes), so the image is never in
  memory and the guest's writes stay in it. Reloading keeps the images.
  `op1_step()` runs 8 ms at a time; between slices the Worker takes keys and
  the worklet's reports, and waits as long as `Pace()` asked.
- `audio-worklet.js`: the SPORT's frames come straight from the Worker over a
  MessagePort, into a ring whose target level (`?audio=MS`, default 100 ms)
  the emulator's servo holds (`src/web_audio.h`, the desktop's `LevelServo`).
- The boot ROM: on the first start the Worker fetches `bf526-0.2.h` from
  GitHub (op1emu/bfin_sim, at the submodule's commit, which the build
  names with the ROM's sha256), checks it, and keeps the ROM in OPFS as
  `bootrom.bin` (`bootrom.js`), while the emulator downloads.
- `public/sw.js`: a Service Worker that serves the `.wasm` from Cache
  Storage once it has it, so that Chrome keeps the code it compiled of the
  module (V8's code cache keeps nothing of a module compiled from a stream
  the page made, as the decompressed `.wasm.br` is). The first start
  downloads the `.wasm.br`, and once the emulator runs the page has the
  Service Worker fetch the `.wasm`; later starts compile that.
- No SharedArrayBuffer, so no COOP/COEP headers are needed: GitHub Pages
  serves it as it is.

The site is built with Vite (`vite.config.js`), with npm packages from
`package-lock.json` (wasm-feature-detect, brotli-dec-wasm), as projects with a
large Emscripten module have it (ffmpeg.wasm, Pyodide, Ruffle). Vite bundles
the page, the Worker and the worklet, with hashed names, and lists the npm
packages' licenses in `licenses.md`. The emulator's module stays out of the
bundle (`tools/emulator-assets.js`): its glue, `.wasm` and `.deferred.wasm`
are copied as they are, named by their content, and the Worker imports the
glue by URL. GitHub Pages compresses with gzip only, so the `.wasm` (43 MiB,
39 of them the stencils) is also brotli-compressed, 2.0 MiB, which the Worker
decompresses as it streams in: with `DecompressionStream("brotli")` where the
browser has it (not Chrome yet), else with brotli-dec-wasm, and with the
`.wasm` only if that fails. LLVM's `.deferred.wasm` (28 MiB, 6.0 compressed)
goes as gzip, if ever. `public/THIRD_PARTY.md` has what Vite does not list.

Browsers: Chrome or Edge 133 or later. The table must be shared between the
modules that import it (V8 13.3); before that, memory grows as the square of
the blocks. Firefox is not supported yet. SpiderMonkey gives each
WebAssembly module's code at least 128 KiB of a 2 GiB executable pool, about
16,000 modules a process (measured, whatever the module's size); a boot now
makes about 6,000, but it is untried. Safari is untried.

## SVG panel and themes

![Browser SVG panel in Silver with a synthetic four-color display](../docs/web-svg-panel-retina.png)

The Theme selector offers Slate, Silver (the supplied artwork's colors), Yellow,
Green, Purple and Orange. It uses a native HTML select, including its keyboard
and touch behavior. SVG filters, gradients and outlines are rendered by the
browser at the current display density; there is no resvg, Rust, native-window
or new npm dependency. The display stays a separate 400 × 200 landscape canvas,
with smooth scaling on the SVG and the original pixel scaling on the PNG.

`?ui=png` starts with the original panel; `?theme=green` chooses an SVG preset.
Selections last for this page session. If the SVG cannot be fetched or decoded,
the page reports it and loads the PNG with its matching controls. Theme changes
replace only the background image, retaining the Worker, framebuffer, audio and
held notes. Switching between SVG and PNG releases held keys before changing
coordinates. Both layouts resize with the page; theme controls sit outside the
instrument's input area, and typing in their selectors sends no instrument keys.

The artwork, six palettes and calibrated controls come from
[PR #77 at `523eac7`](https://github.com/op1emu/op1emu/pull/77).
`panel/op1.svg` is the exact class-only derivative from that revision
(`gui/op1-themeable.svg`); all 217 filters are retained. Its source SVG was
provided with redistribution authorization, as recorded in that PR. The layout
uses the same 1204 × 434 viewport, 58 button GPIO mappings, encoder regions and
231 × 115.5 screen rectangle. The four surface classes (`op1-body`, `op1-panel`,
`op1-keycap`, `op1-encoder-ring`) take the palette's paint; labels, black keys,
colored encoder caps and firmware pixels keep their own colors. The trusted,
bundled SVG is fetched once, styled in an SVG document and decoded as an image;
it is never inserted into the page DOM. Superseded Blob URLs are revoked.
No firmware, NAND or OTP data is included.

`tools/panel-browser.mjs` runs against the production bundle with a recording
Worker, without firmware. It checks all 58 button mappings for each panel at
desktop and phone widths, encoder clicks/wheels, volume, touch, release outside,
screen placement and four-color frames, all six themes, held-key handling,
rapid selections, PNG startup and failed-SVG fallback. CI also runs the existing
real-Worker LDR self-test and input checks. Set `OP1_PANEL_CAPTURE` to an absolute
PNG path for a Retina-density screenshot with a synthetic four-color screen.

## Building

With bcore's WebAssembly tools (`ext/bcore/tools/wasm/setup.sh`: Emscripten
4.0.9 and emscripten-forge's LLVM 23) and Node 24:

```sh
ext/bcore/tools/wasm/setup.sh
node=$(command -v node)   # Node 24, before emsdk's goes on PATH
source ~/.cache/bcore-wasm/emsdk-4.0.9/emsdk_env.sh
emcmake cmake -S . -B build-wasm -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR=$HOME/.cache/bcore-wasm/llvm-23.1.2/lib/cmake/llvm \
  -DCMAKE_CROSSCOMPILING_EMULATOR=$node
cmake --build build-wasm --target op1emu-web
```

This makes `build-wasm/web/op1emu-web.mjs`, `op1emu-web.wasm` and
`op1emu-web.deferred.wasm` (and `op1emu-web.wasm.orig`, the link's, with
names). Then the site,
from `web/` (`OP1_WASM_BUILD` names another build directory):

```sh
npm ci
npm run dev        # http://localhost:5173/, the module from build-wasm as it is
npm run build      # dist/, as GitHub Pages serves it (the brotli takes a minute)
npm run preview    # dist/ at http://localhost:4173/
node tools/smoke-browser.mjs dist node_modules/playwright-core /usr/bin/google-chrome
node tools/input-browser.mjs dist node_modules/playwright-core /usr/bin/google-chrome
node tools/panel-browser.mjs dist node_modules/playwright-core /usr/bin/google-chrome
```

## Node

`tools/run-node.mjs` runs the module under Node, deterministically, with the
sensors of `tools/op1prof`'s default script and the boot ROM from the
submodule's header:

```sh
# The phase marks' guest state, as a native profiling build prints it
# (tools/op1prof gate, --profile-mark-digest): the lines must be the same.
node web/tools/run-node.mjs gate build-wasm/web/op1emu-web.mjs \
  --nand nand.bin --otp otp.bin --marks marks.json --until main-display
# A boot as the browser runs it: wall time per guest second, the longest
# step, memory. With a script of guest-timed keys, the synth phase.
node web/tools/run-node.mjs boot build-wasm/web/op1emu-web.mjs \
  --nand nand.bin --otp otp.bin --cycles 4000000000 --slice 5 \
  --script tools/op1prof/scripts/synth-keys.txt
# --cold THRESHOLD,BATCH,AGE: another cold tier (0,1,0 for none).
# An LDR to its end, with no NAND or OTP (as tests/ldr_smoke.py).
node web/tools/run-node.mjs ldr build-wasm/web/op1emu-web.mjs --ldr tiny.ldr
```

The gate runs with chaining off, so that every block's start is seen: the
marks fire as the profiler fires them.
