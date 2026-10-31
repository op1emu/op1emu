// A --pre-js of op1emu-web's (CMakeLists.txt). SPLIT_MODULE's glue finds the
// deferred module (LLVM's) beside wasmBinaryFile, which only the glue's own
// loader sets: with Module.instantiateWasm, as the Worker has it, createWasm
// returns before it, and the first call into the deferred module threw
// "Cannot read properties of undefined (reading 'slice')" (Emscripten 4.0.9,
// and its main as of 2026-10). So the module's address is set before the
// host's instantiateWasm runs, as the glue's loader would set it
// (findWasmBinary: Module.locateFile's "op1emu-web.wasm").
if (Module["instantiateWasm"]) {
  var hostInstantiateWasm = Module["instantiateWasm"];
  Module["instantiateWasm"] = (imports, done) => {
    wasmBinaryFile ??= findWasmBinary();
    return hostInstantiateWasm(imports, done);
  };
}
