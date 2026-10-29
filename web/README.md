# op1emu in the browser

`op1emu-web` is the emulator built for WebAssembly: one module that a Web
Worker or Node imports. bcore compiles the guest's blocks with LLVM inside it,
to WebAssembly modules that it loads as it goes (`ext/bcore/tools/wasm/README.md`).
bcore's WebAssembly build has no threads, so neither has this one: the host
calls `op1_step()` a slice at a time, and between the slices it takes keys,
draws frames and plays audio. Native code is the same as the desktop build's
except for `BlackFinCpu::Pace()`, which instead of sleeping tells the host how
long to wait (`PacedUntilNs`).

`src/op1_web.cpp` sets the emulator up as `gui/main.cpp` does, less what needs
threads (USB/IP, the promotion worker; there is no copy-and-patch tier either).
Its files are whatever the host mounted: the NAND image at the path it gives
`op1_start()`, `otp.bin` in the working directory.

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

This makes `build-wasm/web/op1emu-web.mjs` and `op1emu-web.wasm`.

## Node

`tools/run-node.mjs` runs the module under Node, deterministically, with the
sensors of `tools/op1prof`'s default script:

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
# An LDR to its end, with no NAND or OTP (as tests/ldr_smoke.py).
node web/tools/run-node.mjs ldr build-wasm/web/op1emu-web.mjs --ldr tiny.ldr
```

The gate runs with chaining off, so that every block's start is seen: the
marks fire as the profiler fires them.
