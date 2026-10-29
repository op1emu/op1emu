// The page (web/README.md): the OP-1's faceplate from gui/ui.png with the
// panel on it and gui/ui.json's keys, encoders and volume knob as the
// desktop frontend has them (gui/glfw_display.cpp); the images the user
// brings, kept in this browser's origin private file system (OPFS); and
// the emulator in a Worker (worker.js), its audio in an AudioWorklet.

import build from "virtual:op1emu";
import { exceptions, tailCall } from "wasm-feature-detect";
import ui from "../gui/ui.json";
import faceplate from "../gui/ui.png";
// A file of its own (Vite would inline one this small as a data: URL).
import audioWorklet from "./audio-worklet.js?worker&url";
import { KEYMAP } from "./keymap.js";

const params = new URLSearchParams(location.search);
const AUDIO_MS = Number(params.get("audio") ?? 100);
// Where the emulator's files are (tools/emulator-assets.js): beside the page.
const SITE = new URL(import.meta.env.BASE_URL, location.href).href;
const ENCODER_STEPS = [0, 2, 3, 1];  // the quadrature sequence, as glfw_display.cpp steps it

const $ = (id) => document.getElementById(id);
const worker = new Worker(new URL("./worker.js", import.meta.url), { type: "module" });
let size = null;  // ui.png's
let running = false;
let volume = 128;
const encoderIndex = {};

function setStatus(text, error = false) {
  $("status").textContent = text;
  $("status").classList.toggle("error", error);
}

// What keeps this browser from running it, if anything. Firefox's
// SpiderMonkey keeps every WebAssembly module's code in 128 KiB of a 2 GiB
// executable pool, about 16,000 modules a process, and bcore makes a
// module of each of the firmware's blocks: a boot needs about 20,000.
// Chromium before 133 (V8 13.3) copies the whole function table into every
// module it instantiates, so memory grows as the square of the blocks.
function browserProblem() {
  if (/\bFirefox\//.test(navigator.userAgent))
    return "Firefox is not supported yet: it runs out of memory for the emulator's compiled code " +
           "(about 16,000 WebAssembly modules a process; a boot makes about 20,000). Use Chrome or Edge.";
  const chromium = navigator.userAgentData?.brands?.find((b) => b.brand === "Chromium");
  if (chromium && Number(chromium.version) < 133)
    return `This is Chromium ${chromium.version}; the emulator needs 133 or later.`;
  return null;
}

async function unsupported() {
  const missing = [];
  if (!(await tailCall())) missing.push("WebAssembly tail calls");
  if (!(await exceptions())) missing.push("WebAssembly exception handling");
  if (!navigator.storage?.getDirectory) missing.push("the origin private file system");
  if (!globalThis.AudioWorkletNode) missing.push("AudioWorklet");
  return missing;
}

// ---- the faceplate ----

function place(element, [left, top, width, height]) {
  const { width: w, height: h } = size;
  Object.assign(element.style, {
    left: `${(100 * left) / w}%`, top: `${(100 * top) / h}%`,
    width: `${(100 * width) / w}%`, height: `${(100 * height) / h}%`,
  });
}

async function loadFaceplate() {
  const image = $("faceplate");
  image.src = faceplate;
  await image.decode();
  size = { width: image.naturalWidth, height: image.naturalHeight };
  const { left, top, width, height } = ui.display;
  place($("screen"), [left, top, width, height]);
  for (const [name, button] of Object.entries(ui.buttons)) {
    const mark = document.createElement("div");
    mark.className = "pressed";
    mark.id = `key-${name}`;
    place(mark, button.position);
    $("device").append(mark);
  }
}

// Where on the image (ui.json's coordinates) a pointer is.
function imagePoint(event) {
  const rect = $("device").getBoundingClientRect();
  return [((event.clientX - rect.left) * size.width) / rect.width,
          ((event.clientY - rect.top) * size.height) / rect.height];
}
const inside = ([x, y], [left, top, width, height]) => x >= left && x < left + width && y >= top && y < top + height;

// ---- input, to the worker ----

function key(name, pressed) {
  const button = ui.buttons[name];
  if (!button || !running) return;
  const [bank, pin] = button.gpio;
  worker.postMessage({ type: "key", bank, pin, pressed });
  $(`key-${name}`)?.classList.toggle("down", pressed);
}

function turnEncoder(name, clockwise) {
  const encoder = ui.encoders[name];
  const index = ((encoderIndex[name] ?? 0) + (clockwise ? 1 : 3)) % 4;
  encoderIndex[name] = index;
  const value = ENCODER_STEPS[index];
  for (const [gpio, bit] of [[encoder.clockwise_gpio, 2], [encoder.counter_clockwise_gpio, 1]])
    worker.postMessage({ type: "key", bank: gpio[0], pin: gpio[1], pressed: Boolean(value & bit) });
}

function turnVolume(up) {
  volume = Math.max(0, Math.min(255, volume + (up ? 5 : -5)));
  worker.postMessage({ type: "volume", value: volume });
}

// The encoder or knob at a point: how to turn it, and which way a click
// there turns it (each has a clockwise and a counter-clockwise half).
function turnerAt(point) {
  for (const [name, encoder] of Object.entries(ui.encoders)) {
    const clockwise = inside(point, encoder.clockwise);
    if (clockwise || inside(point, encoder.counter_clockwise))
      return { turn: (cw) => turnEncoder(name, cw), clockwise };
  }
  const knob = ui.volume_knob, clockwise = inside(point, knob.clockwise);
  if (clockwise || inside(point, knob.counter_clockwise)) return { turn: turnVolume, clockwise };
  return null;
}

const held = new Map();  // pointer id -> button name
const keysDown = new Set();  // KeyboardEvent.code, pressed and not yet released
function bindInput() {
  const device = $("device");
  device.addEventListener("pointerdown", (event) => {
    if (!running) return;
    const point = imagePoint(event);
    const name = Object.keys(ui.buttons).find((n) => inside(point, ui.buttons[n].position));
    if (name) {
      device.setPointerCapture(event.pointerId);
      held.set(event.pointerId, name);
      key(name, true);
    } else {
      const turner = turnerAt(point);
      turner?.turn(turner.clockwise);
    }
    event.preventDefault();
  });
  const release = (event) => {
    const name = held.get(event.pointerId);
    if (name) key(name, false);
    held.delete(event.pointerId);
  };
  device.addEventListener("pointerup", release);
  device.addEventListener("pointercancel", release);
  device.addEventListener("wheel", (event) => {
    const turner = running && turnerAt(imagePoint(event));
    if (!turner) return;
    event.preventDefault();
    turner.turn(event.deltaY < 0);
  }, { passive: false });
  // Whichever element has the focus (a button just clicked keeps it): the
  // page has no text to type, and KEYMAP no key a button or checkbox takes.
  // A key is released as it was pressed, whatever has the focus by then.
  addEventListener("keydown", (event) => {
    const name = KEYMAP[event.code];
    if (!name || !running) return;
    event.preventDefault();
    if (event.repeat || keysDown.has(event.code)) return;
    keysDown.add(event.code);
    key(name, true);
  });
  addEventListener("keyup", (event) => {
    if (!keysDown.delete(event.code)) return;
    event.preventDefault();
    key(KEYMAP[event.code], false);
  });
  // Keys held when the window loses focus, on the keyboard or by a pointer,
  // would stay down: their releases may never come.
  addEventListener("blur", () => {
    for (const code of keysDown) key(KEYMAP[code], false);
    keysDown.clear();
    for (const name of held.values()) key(name, false);
    held.clear();
  });
}

// ---- the images ----

let files = {};
function showFiles(next) {
  files = next;
  const describe = (file) =>
    file ? (file.open ? "in use" : `stored ${new Date(file.modified).toLocaleString()}`) : "not stored";
  $("nand-state").textContent = describe(files.nand);
  $("otp-state").textContent = describe(files.otp);
  $("start").disabled = running || !files.nand || !files.otp;
  for (const id of ["export-nand", "export-otp"]) $(id).disabled = !files[id.slice(7)];
}

function importFile(file) {
  // By size: the NAND image is 528 MiB, the OTP 8 KiB.
  const name = file.size > 1 << 20 ? "nand" : "otp";
  worker.postMessage({ type: "import", name, file });
}

function bindFiles() {
  $("pick").addEventListener("change", (event) => {
    for (const file of event.target.files) importFile(file);
    event.target.value = "";
  });
  addEventListener("dragover", (event) => event.preventDefault());
  addEventListener("drop", (event) => {
    event.preventDefault();
    if (!running) for (const file of event.dataTransfer.files) importFile(file);
  });
  $("export-nand").addEventListener("click", () => worker.postMessage({ type: "export", name: "nand" }));
  $("export-otp").addEventListener("click", () => worker.postMessage({ type: "export", name: "otp" }));
  $("delete").addEventListener("click", () => {
    if (confirm("Delete the NAND image and otp.bin stored in this browser?")) worker.postMessage({ type: "delete" });
  });
  $("start").addEventListener("click", start);
}

async function start() {
  running = true;
  $("start").disabled = true;
  $("panel").classList.add("running");
  navigator.storage.persist?.();
  // The audio needs the click: an AudioContext made without one is suspended.
  let audioPort = null;
  try {
    const context = new AudioContext({ sampleRate: 44100, latencyHint: "interactive" });
    await context.audioWorklet.addModule(audioWorklet);
    const node = new AudioWorkletNode(context, "op1-output", {
      numberOfInputs: 0, outputChannelCount: [2],
      processorOptions: { targetFrames: Math.round((44100 * AUDIO_MS) / 1000) },
    });
    node.connect(context.destination);
    const channel = new MessageChannel();
    node.port.postMessage({ port: channel.port1 }, [channel.port1]);
    audioPort = channel.port2;
  } catch (e) {
    console.warn("no audio:", e);
  }
  worker.postMessage({ type: "start", build, site: SITE, snapshot: $("snapshot").checked, audioMs: AUDIO_MS,
                       audioPort }, audioPort ? [audioPort] : []);
}

// ---- from the worker ----

const screen = $("screen").getContext("2d");
worker.onmessage = ({ data }) => {
  switch (data.type) {
    case "files":
      // Worker-only, so the worker says.
      if (!data.syncAccess) {
        setStatus("This browser lacks the origin private file system's synchronous access. " +
                  "Use a current Chrome or Edge.", true);
        return;
      }
      showFiles(data.files);
      break;
    case "progress":
      setStatus(data.text);
      break;
    case "stats":
      globalThis.op1Stats = data;  // for a look from the console
      setStatus(data.frames ? "" : `Booting: ${data.guestS.toFixed(1)} s of the OP-1's time` +
                " (it compiles the firmware's code as it goes: about a minute)");
      break;
    case "frame": {
      const canvas = $("screen");
      if (canvas.width !== data.width) [canvas.width, canvas.height] = [data.width, data.height];
      screen.putImageData(new ImageData(data.pixels, data.width, data.height), 0, 0);
      worker.postMessage({ type: "frame-done" });
      break;
    }
    case "exported": {
      const link = document.createElement("a");
      link.href = URL.createObjectURL(data.blob);
      link.download = data.name;
      link.click();
      setTimeout(() => URL.revokeObjectURL(link.href), 60000);
      break;
    }
    case "selftest":
      globalThis.op1Selftest = data.result;
      setStatus(`Self-test: ${JSON.stringify(data.result)}`);
      break;
    case "error":
      if (params.has("selftest")) globalThis.op1Selftest = { error: data.text };
      setStatus(data.text, true);
      break;
    case "stopped":
      running = false;
      setStatus("The emulator stopped (see the browser's console).", true);
      break;
  }
};

(async () => {
  const problem = browserProblem();
  const missing = await unsupported();
  if (problem || missing.length) {
    setStatus(problem ?? `This browser lacks ${missing.join(", ")}. Use a current Chrome or Edge.`, true);
    return;
  }
  await loadFaceplate();
  bindInput();
  bindFiles();
  worker.postMessage({ type: "files" });
  // The module and the boot ROM with no firmware (tools/smoke-browser.mjs).
  if (params.has("selftest")) worker.postMessage({ type: "selftest", build, site: SITE });
})();
