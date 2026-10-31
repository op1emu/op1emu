#!/usr/bin/env node
// op1emu-web.mjs under Node (web/README.md), for the tests and measurements:
//
//   run-node.mjs gate MODULE --nand NAND --otp OTP --marks MARKS [--until MARK]
//                [--cold THRESHOLD,BATCH,AGE] [--tier THRESHOLD,SIZE_REF]
//                [--compile worker]
//       Boots with chaining off and prints [mark]/[digest] lines at each
//       phase mark, as a native profiling build's --profile-mark-digest run
//       does (tools/op1prof gate) with the same NAND, OTP and sensors.
//   run-node.mjs boot MODULE --nand NAND --otp OTP [--cycles N] [--frames N]
//                [--script FILE] [--slice MS] [--tier THRESHOLD,SIZE_REF]
//                [--cold THRESHOLD,BATCH,AGE] [--compile worker] [--light 0|1]
//       Boots as the browser does (chained), up to N guest cycles or until N
//       frames, and prints the time it took and the memory it used, each
//       guest second's wall time and the longest op1_step (a block's
//       compilation cannot be cut short), steps of --slice ms (default 100).
//       --cold: the cold tier's threshold, batch and age (op1_cold).
//       --compile worker: promotions (with --tier) compiled by the tier
//       Worker in a worker thread, as the browser has it
//       (compile-helpers.js), the loop yielding between steps.
//       --light: promotions with bcore's light pipeline (op1_light_tier1).
//       --script: a headless input script whose keys are all guest-timed
//       (`at`), such as tools/op1prof/scripts/synth-keys.txt.
//   run-node.mjs ldr MODULE --ldr LDR
//       Runs an LDR on a missing NAND (snapshot: read as erased) to its end.
//
// Every run is deterministic, with the sensors of tools/op1prof's default
// script (accel 100 -100 500, volume 128); the OTP is copied, the NAND
// opened read-only (--nand-snapshot).

import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { parseBootRom } from "../bootrom.js";
import { attachTierWorker } from "../compile-helpers.js";

const DETERMINISTIC = 1, NAND_SNAPSHOT = 2, NO_CHAIN = 4;

function parse(argv) {
  const [command, module, ...rest] = argv;
  const options = {};
  for (let i = 0; i < rest.length; i += 2) {
    if (!rest[i].startsWith("--") || i + 1 >= rest.length) throw new Error(`bad argument ${rest[i]}`);
    options[rest[i].slice(2)] = rest[i + 1];
  }
  if (!["gate", "boot", "ldr"].includes(command) || !module) throw new Error("usage: see run-node.mjs");
  return { command, module, options };
}

// The module with its files: the host's file system at /host, and a scratch
// directory with the OTP (and the LDR) at /data, the working directory.
async function load(modulePath, options) {
  const { default: createOp1 } = await import(pathToFileURL(path.resolve(modulePath)).href);
  // --compile: the module compiled here, for the tier-1 Worker to have too.
  const compiled = options.compile
    ? await WebAssembly.compile(fs.readFileSync(path.resolve(modulePath).replace(/\.mjs$/, ".wasm")))
    : null;
  const instantiate = compiled && {
    instantiateWasm: (imports, done) => {
      WebAssembly.instantiate(compiled, imports).then((instance) => done(instance, compiled));
      return {};
    },
  };
  const op1 = await createOp1({ print: (s) => console.log(s), printErr: (s) => console.error(s), ...instantiate });
  op1.compiledModule = compiled;
  const work = fs.mkdtempSync(path.join(os.tmpdir(), "op1-web-"));
  if (options.otp) fs.copyFileSync(options.otp, path.join(work, "otp.bin"));
  // The boot ROM the browser's build reads (src/cpu/bootrom.cpp), from the
  // submodule's header, as the page fetches it.
  const romHeader = new URL("../../ext/bfin_sim/bf526-0.2.h", import.meta.url);
  fs.writeFileSync(path.join(work, "bootrom.bin"), parseBootRom(fs.readFileSync(romHeader, "utf8")));
  if (options.ldr) fs.copyFileSync(options.ldr, path.join(work, "prog.ldr"));
  const { FS } = op1;
  FS.mkdir("/host");
  FS.mount(FS.filesystems.NODEFS, { root: "/" }, "/host");
  FS.mkdir("/data");
  FS.mount(FS.filesystems.NODEFS, { root: work }, "/data");
  FS.chdir("/data");
  const host = (file) => `/host${path.resolve(file)}`;
  return { op1, work, host, nandPath: options.nand ? host(options.nand) : "/data/missing.nand" };
}

function start(op1, nandPath, ldrPath, flags) {
  const nand = op1.stringToNewUTF8(nandPath), ldr = op1.stringToNewUTF8(ldrPath);
  const result = op1._op1_start(nand, ldr, flags, 0, 0);
  op1._free(nand);
  op1._free(ldr);
  if (result !== 0) throw new Error("op1_start failed");
  op1._op1_accel(100, -100, 500);
  op1._op1_volume(128);
}

// A headless script's sensors and guest-timed keys (gui/headless.cpp),
// queued before the run starts, as the native frontend queues them.
function queueScript(op1, file) {
  const keys = JSON.parse(fs.readFileSync(new URL("../../gui/ui.json", import.meta.url))).buttons;
  let at = null;
  for (const line of fs.readFileSync(file, "utf8").split("\n")) {
    const [command, ...args] = line.trim().split(/\s+/);
    if (!command || command.startsWith("#") || command === "wait") continue;
    if (command === "accel") op1._op1_accel(...args.map(Number));
    else if (command === "volume") op1._op1_volume(Number(args[0]));
    else if (command === "at") at = Number(args[0]);
    else if (["tap", "press", "release"].includes(command) && at !== null && keys[args[0]]) {
      const [bank, pin] = keys[args[0]].gpio;
      op1._op1_key(bank, pin, command !== "release", at);
      if (command === "tap") op1._op1_key(bank, pin, 0, at + Number(args[1] ?? 100));
    } else throw new Error(`${file}: not supported here: ${line}`);
  }
}

// --cold THRESHOLD,BATCH,AGE (op1_cold), if given.
function cold(op1, options) {
  if (options.cold && op1._op1_cold(...options.cold.split(",").map(Number)) !== 0)
    throw new Error("--cold: this build has no cold tier");
}

// The tier Worker in a worker thread (--compile worker).
async function tierWorker(op1, modulePath, mode) {
  if (mode !== "worker") throw new Error(`--compile ${mode}: only worker`);
  const { Worker } = await import("node:worker_threads");
  const spawn = () => {
    const worker = new Worker(new URL("../compile-worker.js", import.meta.url));
    return {
      post: (message, transfer) => worker.postMessage(message, transfer),
      onMessage: (handler) => worker.on("message", handler),
      onError: (handler) => worker.on("error", handler),
      terminate: () => worker.terminate(),
    };
  };
  const file = path.resolve(modulePath);
  return attachTierWorker(op1, {
    spawn,
    module: op1.compiledModule,
    js: pathToFileURL(file).href,
    deferred: pathToFileURL(file.replace(/\.mjs$/, ".deferred.wasm")).href,
  });
}

function memory(op1) {
  return `rss_mb=${(process.memoryUsage().rss / 2 ** 20).toFixed(0)} heap_mb=${(op1.HEAPU8.length / 2 ** 20).toFixed(0)}`;
}

async function main() {
  const { command, module, options } = parse(process.argv.slice(2));
  const { op1, work, host, nandPath } = await load(module, options);
  let status = 0;
  try {
    if (command === "gate") {
      start(op1, nandPath, "", DETERMINISTIC | NAND_SNAPSHOT | NO_CHAIN);
      if (options.tier) op1._op1_tiering(...options.tier.split(",").map(Number));
      cold(op1, options);
      const marks = op1.stringToNewUTF8(host(options.marks));
      const until = op1.stringToNewUTF8(options.until ?? "main-display");
      if (options.compile) {
        // In steps, the tier Worker's answers taken between them: whenever they
        // come, the guest must do the same (the digests say).
        const helped = await tierWorker(op1, module, options.compile);
        let step = op1._op1_gate_open(marks, until) === 0 ? 0 : -1;
        while (step === 0) {
          step = op1._op1_gate_step(20000);
          await new Promise(setImmediate);
        }
        status = step === 1 ? 0 : 1;
        console.log(`[node] tier1 installed=${op1._op1_remote_stat(2)}`);
        helped.stop();
      } else {
        status = op1._op1_gate(marks, until) === 0 ? 0 : 1;
      }
      console.log(`[node] ${memory(op1)}`);
    } else if (command === "boot") {
      start(op1, nandPath, "", DETERMINISTIC | NAND_SNAPSHOT);
      if (options.script) queueScript(op1, options.script);
      if (options.tier) op1._op1_tiering(...options.tier.split(",").map(Number));
      if (options.light) op1._op1_light_tier1(Number(options.light));
      cold(op1, options);
      const helped = options.compile ? await tierWorker(op1, module, options.compile) : null;
      const cycles = Number(options.cycles ?? Infinity), frames = Number(options.frames ?? Infinity);
      const slice = Number(options.slice ?? 100);
      const begin = performance.now();
      let second = begin, nextNs = 1e9, longest = 0, longestAt = 0;
      while (op1._op1_cycles() < cycles && op1._op1_frames() < frames) {
        const before = performance.now();
        if (op1._op1_step(slice) < 0) throw new Error("the emulation stopped");
        const now = performance.now();
        // The tier Worker's answers, between steps, as the browser's Worker takes them.
        if (helped) await new Promise(setImmediate);
        if (now - before > longest) [longest, longestAt] = [now - before, op1._op1_guest_ns() / 1e9];
        if (op1._op1_guest_ns() >= nextNs) {
          console.log(`[node] guest_s=${nextNs / 1e9} wall_s=${((now - second) / 1e3).toFixed(2)} ` +
                      `longest_step_ms=${longest.toFixed(1)} at_guest_s=${longestAt.toFixed(2)} ${memory(op1)}`);
          [second, nextNs, longest] = [now, nextNs + 1e9, 0];
        }
      }
      console.log(`[node] boot cycles=${op1._op1_cycles()} packets=${op1._op1_packets()} frames=${op1._op1_frames()} ` +
                  `wall_s=${((performance.now() - begin) / 1e3).toFixed(2)} ${memory(op1)}`);
      if (helped) {
        const stat = (i) => op1._op1_remote_stat(i);
        console.log(`[node] tier1 submitted=${stat(0)} refused=${stat(1)} ` +
                    `installed=${stat(2)} stale=${stat(3)} failed=${stat(4)}${helped.counts.dead ? " dead" : ""}`);
        helped.stop();
      }
    } else {
      start(op1, nandPath, "/data/prog.ldr", DETERMINISTIC | NAND_SNAPSHOT);
      while (op1._op1_step(100) >= 0) {}
      status = op1._op1_finished() ? 0 : 1;
      console.log(status ? "[node] ldr: did not finish" : "[node] ldr: ok");
    }
  } finally {
    fs.rmSync(work, { recursive: true, force: true });
  }
  process.exitCode = status;
}

main().catch((e) => {
  console.error(e.message ?? e);
  process.exitCode = 1;
});
