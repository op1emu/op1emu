// A Node --import preload for split-module.mjs: the profile of the module
// wasm-split --instrument made (its __write_profile export), written to
// $OP1_PROFILE when the process exits.

import fs from "node:fs";

const instantiate = WebAssembly.instantiate, Instance = WebAssembly.Instance;
let instrumented = null;
const take = (instance) => {
  if (!instrumented && instance?.exports?.__write_profile) instrumented = instance.exports;
  return instance;
};
WebAssembly.instantiate = async (...args) => {
  const result = await instantiate(...args);
  take(result.instance ?? result);
  return result;
};
WebAssembly.Instance = function (...args) {
  return take(new Instance(...args));
};
WebAssembly.Instance.prototype = Instance.prototype;

process.on("exit", () => {
  if (!instrumented) throw new Error("write-profile: no instrumented module ran");
  const size = 64 << 20, at = instrumented.malloc(size);
  const written = instrumented.__write_profile(at, size);
  fs.writeFileSync(process.env.OP1_PROFILE, new Uint8Array(instrumented.memory.buffer, at, written));
});
