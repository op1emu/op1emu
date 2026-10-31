// The emulator's tier Worker (web/README.md), nested in worker.js, or a
// Node worker thread (tools/run-node.mjs): the emulator's promotions
// (bcore's Core::set_remote_tier) compiled with LLVM by another instance of
// the emulator's module (tier_worker.cpp, op1_tier_compile), each object
// made a module and sent back for the emulator to instantiate
// (compile-helpers.js), its functions called once here first: V8 compiles a
// function the first time it is called, on the calling thread, and keeps
// the code with the module for every Worker that has it. Called with a
// null CPU state against a memory of no pages, it traps at its first load,
// compiled, having run nothing else.

const node = typeof process === "object" && process.versions?.node && typeof WorkerGlobalScope === "undefined";
const port = node ? (await import("node:worker_threads")).parentPort : self;
const post = (message) => port.postMessage(message);
const listen = (handler) => (node ? port.on("message", handler) : (port.onmessage = (event) => handler(event.data)));

Error.stackTraceLimit = 0;  // a trap's stack, unread

// What a block's module may import (bcore's code_loader_wasm.cpp), for
// warm(): a memory with no pages, a table with no slots, a stack pointer and
// helpers that throw.
const memory = new WebAssembly.Memory({ initial: 0 });
const table = new WebAssembly.Table({ initial: 0, element: "anyfunc" });
const stackPointer = new WebAssembly.Global({ value: "i32", mutable: true }, 0);
function helper() {
  throw new Error("a helper was called while warming");
}

// Each function of the module called once, its trap caught.
function warm(module) {
  const env = Object.create(null);
  env.__linear_memory = memory;
  env.__indirect_function_table = table;
  env.__stack_pointer = stackPointer;
  for (const { name, kind } of WebAssembly.Module.imports(module)) if (kind === "function") env[name] = helper;
  const exports = new WebAssembly.Instance(module, { env }).exports;
  for (const fn of Object.values(exports)) {
    if (typeof fn !== "function") continue;
    try {
      fn(0, 0);
    } catch {}
  }
}

// The tier Worker's instance, once "init" made it (dead: it failed, or
// LLVM aborted), and its jobs in order, those sent meanwhile too.
let op1 = null, dead = false;
const jobs = [];
let busy = false;

async function init({ module, js, deferred }) {
  try {
    const deferredModule = node
      ? await WebAssembly.compile((await import("node:fs")).readFileSync(new URL(deferred)))
      : await WebAssembly.compileStreaming(fetch(deferred));
    const { default: createOp1 } = await import(js);
    op1 = await createOp1({
      print: () => {},
      printErr: (s) => {
        // SPLIT_MODULE's notes as the deferred module (LLVM) loads.
        if (!s.startsWith("placeholder function called") && !s.startsWith("instantiated deferred module")) console.error(s);
      },
      instantiateWasm: (imports, done) => {
        // A failure here would leave createOp1 unsettled: no instance (too
        // little memory, say), no jobs.
        WebAssembly.instantiate(module, imports).then((instance) => done(instance, module),
                                                      (error) => die(String(error)));
        return {};
      },
      locateFile: (name) => name,
      // LLVM's code, compiled above (instead of the glue's synchronous fetch).
      loadSplitModule: (file, imports) => [new WebAssembly.Instance(deferredModule, imports), deferredModule],
    });
    next();
  } catch (error) {
    die(String(error));
  }
}

function die(why) {
  if (dead) return;
  dead = true;
  op1 = null;
  jobs.length = 0;
  post({ type: "dead", why });
}

// The job compiled, its code posted; or "fail" for it. What LLVM throws
// (an abort) leaves the instance unusable: that goes to the caller.
function compile({ ticket, job }) {
  const bytes = op1._malloc(job.length);
  op1.HEAPU8.set(job, bytes);
  let answer;
  try {
    answer = op1._op1_tier_compile(bytes, job.length) >>> 2;
  } finally {
    op1._free(bytes);
  }
  const word = (i) => op1.HEAPU32[answer + i] >>> 0;
  if (!word(0)) return post({ type: "fail", ticket, why: op1.UTF8ToString(word(5)) });
  try {
    const module = new WebAssembly.Module(op1.HEAPU8.slice(word(1), word(1) + word(2)));
    warm(module);
    post({ type: "code", ticket, module, name: op1.UTF8ToString(word(3)), fallthrough: word(4) });
  } catch (error) {
    post({ type: "fail", ticket, why: String(error) });
  }
}

// One job a task: messages that come meanwhile are taken in between.
function next() {
  if (busy || !op1 || !jobs.length) return;
  busy = true;
  setTimeout(() => {
    const job = jobs.shift();
    try {
      compile(job);
    } catch (error) {
      die(String(error));
    }
    busy = false;
    next();
  }, 0);
}

listen((message) => {
  switch (message.type) {
    case "init":
      init(message);
      break;
    case "job":
      if (dead) return post({ type: "fail", ticket: message.ticket, why: "no instance" });
      jobs.push(message);
      next();
      break;
  }
});
