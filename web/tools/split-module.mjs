#!/usr/bin/env node
// LLVM out of the browser's module (README.md): bcore's LLVM compiles a block
// only when copy-and-patch takes none (none of a boot of the OP-1's
// firmware), so its code goes to a second module, op1emu-web.deferred.wasm,
// which the glue loads the first time one of its functions is called
// (Emscripten's SPLIT_MODULE).
//
//   split-module.mjs DIR WASM_SPLIT
//
// DIR holds the link's output with -sSPLIT_MODULE: op1emu-web.wasm.orig (with
// the functions' names, --profiling-funcs), and op1emu-web.mjs and
// op1emu-web.wasm, instrumented to record which functions run. That runs an
// LDR of one RTS (no firmware) under Node, which records the functions the
// module runs to start: LLVM's static constructors (its options) among them,
// which stay. Every other function in LLVM's namespace (or named LLVM...)
// moves; op1emu-web.wasm is written again, the module without them and with
// no names.

import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const [dir, wasmSplit] = process.argv.slice(2);
if (!dir || !wasmSplit) throw new Error("usage: split-module.mjs DIR WASM_SPLIT");
const here = path.dirname(new URL(import.meta.url).pathname);
const orig = path.join(dir, "op1emu-web.wasm.orig");
const features = ["--enable-tail-call", "--enable-exception-handling", "--enable-bulk-memory",
                  "--enable-nontrapping-float-to-int", "--enable-sign-ext", "--enable-mutable-globals"];
const work = fs.mkdtempSync(path.join(os.tmpdir(), "op1-split-"));
try {
  // The functions an LDR of one RTS runs (tests/ldr_smoke.py's, as the
  // page's self-test has it).
  const ldr = new Uint8Array(18), view = new DataView(ldr.buffer);
  view.setUint32(0, 0x4000 | 0x8000, true);  // the first and final block
  view.setUint32(4, 0xffa00000, true);       // at L1 instruction memory
  view.setUint32(8, 2, true);
  ldr.set([0x10, 0x00], 16);                 // RTS
  fs.writeFileSync(path.join(work, "rts.ldr"), ldr);
  const profile = path.join(work, "start.prof");
  execFileSync(process.execPath, ["--import", path.join(here, "write-profile.mjs"), path.join(here, "run-node.mjs"),
                                  "ldr", path.join(dir, "op1emu-web.mjs"), "--ldr", path.join(work, "rts.ldr")],
               { env: { ...process.env, OP1_PROFILE: profile }, stdio: ["ignore", "ignore", "inherit"] });
  // Every function's name as wasm-split has it ("+" kept: it ran).
  const printed = execFileSync(wasmSplit, [...features, `--print-profile=${profile}`, orig],
                               { encoding: "utf8", maxBuffer: 1 << 30 });
  const unescape = (name) => name.replace(/\\([0-9a-f]{2})/g, (_, hex) => String.fromCharCode(parseInt(hex, 16)));
  const moved = [];
  let ran = 0;
  for (const line of printed.split("\n")) {
    const kept = line.startsWith("+ ");
    if (!kept && !line.startsWith("- ")) continue;
    const name = line.slice(2);
    if (!/\bllvm::|^llvm|LLVM/.test(unescape(name))) continue;
    if (kept) ++ran;
    else moved.push(name);
  }
  if (moved.length < 1000) throw new Error(`only ${moved.length} of LLVM's functions found to move`);
  const list = path.join(work, "moved.txt");
  fs.writeFileSync(list, moved.join("\n") + "\n");
  execFileSync(wasmSplit, [...features, orig, `--split-funcs=@${list}`, "-o1", path.join(dir, "op1emu-web.wasm"),
                           "-o2", path.join(dir, "op1emu-web.deferred.wasm")], { stdio: "inherit" });
  console.log(`split-module: ${moved.length} of LLVM's functions to op1emu-web.deferred.wasm, ${ran} that ran ` +
              "to start kept");
} finally {
  fs.rmSync(work, { recursive: true, force: true });
}
