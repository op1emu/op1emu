// The emulator's Worker (web/README.md): the images in OPFS, the module, and
// op1_step() a slice at a time, yielding to this Worker's event loop between
// slices for keys and the audio worklet's reports. Frames go to the page,
// audio straight to the worklet over the port the page hands in.
//
// From the page: {type: "files"}, {type: "import", name, file},
// {type: "export", name}, {type: "delete"}, {type: "start", build, site,
// snapshot, audioMs, audioPort, cached}, {type: "selftest", build, site, cached,
// program}, {type: "key", bank, pin, pressed}, {type: "volume", value},
// {type: "frame-done"} (cached: the page's Service Worker serves the .wasm
// from its cache).
// build is virtual:op1emu (tools/emulator-assets.js), whose files are at the
// URL site. To it: {type: "files", files, syncAccess}, {type: "progress",
// text}, {type: "exported", name, blob}, {type: "frame", width, height,
// pixels}, {type: "stats", guestS, frames, audio}, {type: "running"} (the
// module is the page's to cache), {type: "selftest", result}, {type:
// "error", text}, {type: "stopped"}.

import { BOOT_ROM_SIZE, parseBootRom, sha256 } from "./bootrom.js";
import { opfsFs } from "./opfs-fs.js";

const NAMES = { nand: "nand.bin", otp: "otp.bin" };
const BOOT_ROM = "bootrom.bin";
const SIZES = { nand: 553648128, otp: 8192 };  // MT29F4G08's pages and OOBs; the OTP's
const SLICE_MS = 8;
const FLUSH_MS = 2000;

let op1 = null;          // the module, once started
let handles = null;      // open sync access handles, while it runs
let dataFs = null;
let audioPort = null;
let frameBusy = false;   // the page has not drawn the last frame yet
let pendingFrame = null;
let audio = null;        // the worklet's last report

const syncAccess = Boolean(globalThis.FileSystemFileHandle?.prototype.createSyncAccessHandle);
const post = (message, transfer = []) => self.postMessage(message, transfer);
const status = (text) => post({ type: "progress", text });

async function directory() {
  return navigator.storage.getDirectory();
}

async function files() {
  const dir = await directory();
  const out = {};
  for (const [key, name] of Object.entries(NAMES)) {
    try {
      const file = handles ? null : await (await dir.getFileHandle(name)).getFile();
      // One of another size is the rest of an import that did not finish.
      if (file && file.size !== SIZES[key]) throw new Error("incomplete");
      out[key] = file ? { size: file.size, modified: file.lastModified } : { size: SIZES[key], open: true };
    } catch {
      out[key] = null;
    }
  }
  return out;
}

// A picked file into OPFS, written in chunks through a sync handle (which
// Safari has, unlike createWritable).
async function importFile(key, file) {
  if (handles) throw new Error("the emulator is running: reload the page to change images");
  if (file.size !== SIZES[key])
    throw new Error(`${file.name} is ${file.size} bytes; a${key === "nand" ? " NAND image" : "n OTP"} is ${SIZES[key]}`);
  const dir = await directory();
  const handle = await (await dir.getFileHandle(NAMES[key], { create: true })).createSyncAccessHandle();
  try {
    handle.truncate(0);
    const chunk = 16 << 20;
    for (let at = 0; at < file.size; at += chunk) {
      const bytes = new Uint8Array(await file.slice(at, at + chunk).arrayBuffer());
      handle.write(bytes, { at });
      status(`Storing ${file.name}: ${Math.floor((100 * (at + bytes.length)) / file.size)}%`);
    }
    handle.flush();
  } finally {
    handle.close();
  }
}

// An image out of OPFS: while the emulator runs, read through its handle.
async function exportFile(key) {
  if (!handles) return (await (await directory()).getFileHandle(NAMES[key])).getFile();
  dataFs.flush();
  const handle = handles[key], parts = [], chunk = 16 << 20;
  for (let at = 0, size = handle.getSize(); at < size; at += chunk) {
    const part = new Uint8Array(Math.min(chunk, size - at));
    handle.read(part, { at });
    parts.push(part);
  }
  return new Blob(parts);
}

async function deleteFiles() {
  if (handles) throw new Error("the emulator is running: reload the page first");
  const dir = await directory();
  for (const name of [...Object.values(NAMES), BOOT_ROM]) await dir.removeEntry(name).catch(() => {});
}

// The boot ROM in OPFS as bootrom.bin (bootrom.js): fetched from the header
// build names when there is none, or another, which the sha256 tells.
async function ensureBootRom({ url, sha256: expected }) {
  const dir = await directory();
  try {
    const kept = new Uint8Array(await (await (await dir.getFileHandle(BOOT_ROM)).getFile()).arrayBuffer());
    if (kept.length === BOOT_ROM_SIZE && (await sha256(kept)) === expected) return;
  } catch {}
  status("Fetching the boot ROM");
  const response = await fetch(url);
  if (!response.ok) throw new Error(`the boot ROM, ${url}: ${response.status} ${response.statusText}`);
  const rom = parseBootRom(await response.text());
  if ((await sha256(rom)) !== expected) throw new Error(`the boot ROM at ${url} is not the one this build expects`);
  const handle = await (await dir.getFileHandle(BOOT_ROM, { create: true })).createSyncAccessHandle();
  try {
    handle.truncate(0);
    handle.write(rom, { at: 0 });
    handle.flush();
  } finally {
    handle.close();
  }
}

// The module from build's files at site: the .wasm.br, decompressed as it
// streams in, by DecompressionStream where it has brotli (not Chrome yet),
// else by brotli-dec-wasm; the .wasm (which GitHub Pages sends gzipped) if
// that fails, or with `vite`, which serves no .br. Where the page's Service
// Worker has the .wasm (cached), the .wasm, from it as it came: Chrome then
// keeps the code it compiled for the next time, and this time takes what it
// kept of the last (a module made of a stream of our own it keeps nothing of).
let moduleSource = null;  // where compileModule took it from: "cache", "br" or "wasm"
async function compileModule(build, site, cached) {
  const counted = (response, total) => {
    let loaded = 0;
    return response.body.pipeThrough(new TransformStream({
      transform(chunk, controller) {
        loaded += chunk.length;
        status(`Downloading the emulator: ${Math.min(100, Math.floor((100 * loaded) / total))}%`);
        controller.enqueue(chunk);
      },
    }));
  };
  const fetched = async (file) => {
    const response = await fetch(new URL(file, site));
    if (!response.ok) throw new Error(`${file}: ${response.status} ${response.statusText}`);
    return response;
  };
  const compile = (stream) =>
    WebAssembly.compileStreaming(new Response(stream, { headers: { "content-type": "application/wasm" } }));
  if (cached) {
    try {
      status("Loading the emulator");
      const module = await WebAssembly.compileStreaming(fetched(build.wasm.file));
      moduleSource = "cache";
      return module;
    } catch (e) {
      console.warn("the cached module failed, downloading it:", e);
    }
  }
  if (build.wasmBr) {
    try {
      const stream = counted(await fetched(build.wasmBr.file), build.wasmBr.size);
      const module = await compile(stream.pipeThrough(await brotliStream()));
      moduleSource = "br";
      return module;
    } catch (e) {
      console.warn("the brotli module failed, fetching the .wasm:", e);
    }
  }
  const module = await compile(counted(await fetched(build.wasm.file), build.wasm.size));
  moduleSource = "wasm";
  return module;
}

// A brotli decompressing TransformStream: the browser's, or brotli-dec-wasm's
// (its README's, with larger output chunks).
async function brotliStream() {
  try {
    return new DecompressionStream("brotli");
  } catch {}
  const brotli = await (await import("brotli-dec-wasm")).default;
  const decoder = new brotli.DecompressStream();
  const { NeedsMoreInput, NeedsMoreOutput, ResultSuccess } = brotli.BrotliStreamResultCode;
  return new TransformStream({
    transform(chunk, controller) {
      let code, offset = 0;
      do {
        const result = decoder.decompress(chunk.subarray(offset), 1 << 20);
        controller.enqueue(result.buf);
        code = result.code;
        offset += result.input_offset;
      } while (code === NeedsMoreOutput);
      if (code !== NeedsMoreInput && code !== ResultSuccess) throw new Error(`brotli: ${code}`);
    },
  });
}

// compileModule begun, while the boot ROM is fetched (ensureBootRom): its
// failure is instantiate's to report.
function compileAhead(build, site, cached = false) {
  const compiled = compileModule(build, site, cached);
  compiled.catch(() => {});
  return compiled;
}

// The module instantiated: compiled (compileAhead's) from build's files at
// site, with the glue (op1emu-web.mjs), which Vite leaves alone.
async function instantiate(build, site, compiled) {
  const module = await compiled;
  status("Starting");
  const { default: createOp1 } = await import(/* @vite-ignore */ new URL(build.js, site).href);
  return createOp1({
    print: (s) => console.log(s),
    printErr: (s) => console.warn(s),
    // The .wasm by the name the site gives it, for the glue to find the
    // deferred module (LLVM's) beside it: op1emu-web.<hash>.deferred.wasm.
    locateFile: (file) => new URL(file === "op1emu-web.wasm" ? build.wasm.file : file, site).href,
    instantiateWasm(imports, done) {
      WebAssembly.instantiate(module, imports).then((instance) => done(instance, module));
      return {};
    },
  });
}

// CI's look at the site in a browser with no firmware
// (tools/smoke-browser.mjs; the page's ?selftest): the boot ROM fetched and
// checked, and the module, as start() gets it, running an LDR of one RTS
// (tests/ldr_smoke.py's) to its end in memory; where it took the module from.
// ?selftest=llvm runs a packet copy-and-patch has no stencils for first, so
// that LLVM compiles it, from the deferred module.
const SELFTEST_PROGRAMS = {
  rts: [0x0010],                                   // RTS
  llvm: [0xcc0d, 0x4210, 0x0000, 0x0000, 0x0010],  // (R0, R1) = SEARCH R2 (GE) || NOP || NOP; RTS
};
async function selftest({ build, site, cached, program = "rts" }) {
  const code = SELFTEST_PROGRAMS[program];
  if (!code) throw new Error(`no self-test program ${program}`);
  const compiled = compileAhead(build, site, cached);
  await ensureBootRom(build.bootRom);
  const op1 = await instantiate(build, site, compiled);
  const ldr = new Uint8Array(16 + 2 * code.length), view = new DataView(ldr.buffer);
  view.setUint32(0, 0x4000 | 0x8000, true);  // the first and final block
  view.setUint32(4, 0xffa00000, true);       // at L1 instruction memory
  view.setUint32(8, 2 * code.length, true);
  code.forEach((halfword, i) => view.setUint16(16 + 2 * i, halfword, true));
  op1.FS.writeFile("/prog.ldr", ldr);
  const DETERMINISTIC = 1, NAND_SNAPSHOT = 2;
  const started = op1._op1_start(op1.stringToNewUTF8("/missing.nand"), op1.stringToNewUTF8("/prog.ldr"),
                                 DETERMINISTIC | NAND_SNAPSHOT, 0, 0);
  while (started === 0 && op1._op1_step(100) >= 0) {}
  return { program, started, finished: Boolean(op1._op1_finished()), packets: op1._op1_packets(),
           module: moduleSource };
}

async function start({ build, site, snapshot, audioMs, audioPort: port, cached }) {
  const compiled = compileAhead(build, site, cached);
  await ensureBootRom(build.bootRom);
  const dir = await directory();
  handles = {};
  try {
    for (const [key, name] of Object.entries({ ...NAMES, bootRom: BOOT_ROM }))
      handles[key] = await (await dir.getFileHandle(name)).createSyncAccessHandle();
  } catch (e) {
    for (const handle of Object.values(handles)) handle.close();
    handles = null;
    throw new Error(`the images are not stored, or another tab has them open (${e.message})`);
  }
  op1 = await instantiate(build, site, compiled);
  const { FS } = op1;
  dataFs = opfsFs(FS);
  FS.mkdir("/data");
  FS.mount(dataFs, { files: { [NAMES.nand]: handles.nand, [NAMES.otp]: handles.otp, [BOOT_ROM]: handles.bootRom } },
           "/data");
  FS.chdir("/data");
  op1.onFrame = frame;
  if (port) {
    audioPort = port;
    audioPort.onmessage = ({ data }) => {
      audio = data;
      op1._op1_audio_report(data.consumed, data.playing ? 1 : 0);
    };
    op1.onAudio = (pointer, count) => {
      const frames = op1.HEAPF32.slice(pointer / 4, pointer / 4 + count * 2);
      audioPort.postMessage(frames, [frames.buffer]);
    };
  }
  const path = op1.stringToNewUTF8(`/data/${NAMES.nand}`), none = op1.stringToNewUTF8("");
  const NAND_SNAPSHOT = 2;
  if (op1._op1_start(path, none, snapshot ? NAND_SNAPSHOT : 0, 0, port ? audioMs : 0) !== 0)
    throw new Error("the emulator did not start (see the console)");
  status("Booting");
  post({ type: "running" });
  setInterval(() => dataFs.flush(), FLUSH_MS);
  setInterval(() => post({ type: "stats", guestS: op1._op1_guest_ns() / 1e9, frames: op1._op1_frames(), audio }), 500);
  run();
}

// The panel, rotated into the landscape the faceplate shows (as the
// headless screenshot is): RGBA for the page's canvas.
function frame(pointer, width, height) {
  const pixels = op1.HEAPU16.subarray(pointer / 2, pointer / 2 + width * height);
  const rgba = new Uint8ClampedArray(width * height * 4);
  for (let yd = 0; yd < width; ++yd) {
    for (let xd = 0; xd < height; ++xd) {
      const p = pixels[(height - 1 - xd) * width + yd], at = (yd * height + xd) * 4;
      const r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
      rgba[at] = (r << 3) | (r >> 2);
      rgba[at + 1] = (g << 2) | (g >> 4);
      rgba[at + 2] = (b << 3) | (b >> 2);
      rgba[at + 3] = 255;
    }
  }
  const message = { type: "frame", width: height, height: width, pixels: rgba };
  if (frameBusy) {
    pendingFrame = message;  // the latest only
    return;
  }
  frameBusy = true;
  post(message, [rgba.buffer]);
}

// op1_step() again after the wait it asks for; at once through a message,
// which, unlike setTimeout(0), is not held to 4 ms when nested.
const channel = new MessageChannel();
channel.port1.onmessage = run;
function run() {
  let wait;
  try {
    wait = op1._op1_step(SLICE_MS);
  } catch (e) {
    wait = -1;
    console.error(e);
  }
  if (wait < 0) {
    dataFs.flush();
    post({ type: "stopped" });
    return;
  }
  if (wait > 0.5) setTimeout(run, wait);
  else channel.port2.postMessage(0);
}

// Input at once; the rest in order, one at a time (an import must end
// before the next starts, or before the emulator opens the file).
let queue = Promise.resolve();
self.onmessage = ({ data }) => {
  switch (data.type) {
    case "key":
      op1?._op1_key(data.bank, data.pin, data.pressed ? 1 : 0, 0);
      return;
    case "volume":
      op1?._op1_volume(data.value);
      return;
    case "frame-done":
      frameBusy = false;
      if (pendingFrame) {
        const message = pendingFrame;
        pendingFrame = null;
        frameBusy = true;
        post(message, [message.pixels.buffer]);
      }
      return;
  }
  queue = queue.then(() => handle(data)).catch((e) => post({ type: "error", text: e.message ?? String(e) }));
};

async function handle(data) {
  switch (data.type) {
    case "files":
      post({ type: "files", files: await files(), syncAccess });
      break;
    case "import":
      try {
        await importFile(data.name, data.file);
      } finally {
        post({ type: "files", files: await files(), syncAccess });
      }
      status("");
      break;
    case "export":
      post({ type: "exported", name: NAMES[data.name], blob: await exportFile(data.name) });
      break;
    case "delete":
      await deleteFiles();
      post({ type: "files", files: await files(), syncAccess });
      break;
    case "start":
      await start(data);
      break;
    case "selftest":
      post({ type: "selftest", result: await selftest(data) });
      break;
  }
}
