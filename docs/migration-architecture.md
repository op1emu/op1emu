# Headless inspection migration architecture

This branch starts at upstream commit `3b926b129ec37cc00b8b33ef379d4a5e31a957a7`.
The existing project records the same upstream repository as `origin` and
`upstream` at `https://github.com/op1emu/op1emu.git`; upstream history and the existing GUI
and execution-oriented emulator remain retained.

`OP1EMU_BUILD_GUI=OFF` selects a distinct build path containing only
`op1inspection` (safe firmware-container and BF52x LDR metadata inspection),
the `op1inspect` CLI, and synthetic parser tests. It does not configure or link
GLFW, OpenGL, GUI assets, Bcore, `ext/bfin_sim`, the embedded Boot ROM header,
or upstream CPU/device execution code. The upstream LDR parser remains isolated
in the existing emulator build; parsed metadata is not connected to execution
or memory loading. Extraction is an explicit CLI operation and uses the
container parser's path validation and staging behavior.

Bcore is reference-only for future research and is not a dependency of this
inspection path. GNU Blackfin `bfin-elf-run` is a separate external process
oracle built by `tools/gnu-bfin-oracle`; GPL simulator source is neither linked
into an emulator target nor copied into it. Oracle sources, downloads, binaries,
and smoke outputs default to the ignored `.cache/gnu-bfin-oracle/` directory.

Boot ROM data is excluded. Its provenance and licensing remain unresolved.
Upstream peripheral behavior remains unverified by this migration; no claim of
hardware completeness follows from compiling either build path. The preserved
original research project is `/home/chris/Projects/op-1_emu` and is read-only
source material for this migration.

## Headless validation

From a clean build directory:

```sh
cmake -S . -B build-headless -DOP1EMU_BUILD_GUI=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-headless --parallel
ctest --test-dir build-headless --output-on-failure
cmake -S . -B build-headless-sanitize -DOP1EMU_BUILD_GUI=OFF \
  -DOP1EMU_ENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-headless-sanitize --parallel
ctest --test-dir build-headless-sanitize --output-on-failure
```

The tests use synthetic redistributable inputs and include malformed container
and truncated LDR cases. Firmware-dependent regression checks are optional and
skip when their local image is absent.
