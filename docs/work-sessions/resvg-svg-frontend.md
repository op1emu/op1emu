# Work session: resvg-backed SVG front panel for op1emu

**Status: user-approved for upstream review on 2026-10-29.**
The user approved integrating the six themes, upper-right dropdown and latest
`op-skew.svg` artwork into [upstream PR #77](https://github.com/op1emu/op1emu/pull/77)
and marking it ready for review. The SVG's redistribution and isolated firmware-246
testing were explicitly authorized. Broader firmware and desktop compatibility
remain review/validation limits; no merge was authorized. See
[verification and visual comparisons](../visual-comparisons/resvg-frontend/README.md)
and the README's SVG build/run instructions. The specification below is retained
as the original implementation record; subsequent explicit user approvals supersede
its original draft/publication restrictions. Implementation continues on
`feature/resvg-svg-ui` in `GosuBoy/op1emu`; do not modify `main` or merge the PR.

## Original implementation specification

## Goal

Render the user-supplied **`op-skew(1).svg`** as the OP-1 *hardware/front-panel* illustration using **resvg's C API**, with the original firmware-driven display overlaid independently. The design uses Gaussian blur plus other complex SVG filters, so the renderer must preserve those effects. Support sharp rendering on high-DPI displays while keeping the current PNG layout fully usable as a fallback.

**Input asset:** The SVG was supplied in the ChatGPT conversation; it is **not yet part of this repository or this planning PR**. The work session should obtain that exact original asset from the user/session attachment, not generate an approximation. Previous review described a roughly 1377 x 496 SVG with extensive `feGaussianBlur`, `feTurbulence`, `feDisplacementMap`, blend and gradient use; **verify** actual `viewBox`, dimensions, linked resources and effective filter output from the file, rather than treating those estimates as a specification. Confirm the artwork can be redistributed in this public repository before committing it. Never commit the original firmware, OTP, NAND image, or private research files.

## Current architecture (read these first)

- `gui/glfw_display.cpp/.h`: `GLFWDisplay` creates GLFW/OpenGL context, loads `gui/ui.json`, decodes `gui/ui.png` via stb_image and draws it as a textured quad. `RenderFramebuffer()` composites a **separate, 90-degree-rotated RGB565** firmware display texture over that background.
- `gui/ui.json`: `background`, `scale`, the display rectangle, button/encoder/volume-knob hit rectangles, and GPIO wiring. Existing positions are in the PNG's native coordinate system and are scaled in `LoadUIConfig()`.
- `gui/main.cpp`: creates `GLFWDisplay` or `HeadlessFrontend`, attaches it to `BlackFinCpu` as both a display and keyboard.
- `src/cpu/ppi.cpp` and `src/cpu/cpu.cpp`: DMA/PPI delivers already-rendered pixels to `Display::UpdateRowBuffer()`. The original OP-1 UI is drawn **inside the emulated firmware**; that pipeline must remain unchanged.
- `CMakeLists.txt`, `.github/workflows/ci.yml`: existing GLFW/OpenGL/C++17 build, Linux/macOS build-and-test jobs, firmware-free CTest.
- `gui/headless.cpp` and `tests/headless_test.cpp`: headless input and screenshot behavior must remain unaffected.

## Locked technical choice

Use **resvg 0.49.0**, pinned to a reproducible release/commit; its C API requires **Rust >= 1.85** and exists under upstream `crates/c-api/`. Do **not** use the Rust-only `resvg` crate as if it directly exported a C ABI: the build target is **`resvg-capi`**.

Reference API: https://github.com/linebender/resvg/blob/v0.49.0/crates/c-api/resvg.h

- `resvg_options_create/destroy`
- `resvg_parse_tree_from_file` or `resvg_parse_tree_from_data`; check the error and pointer
- `resvg_get_image_size`, `resvg_transform_identity`
- `resvg_render` into a caller-allocated **premultiplied RGBA8888** buffer (`width * height * 4` bytes)
- `resvg_tree_destroy`

The key OpenGL correctness detail: **premultiplied alpha** must use the corresponding blend equation, e.g. `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`, rather than treating resvg's bytes as straight alpha. Preserve/restore GL state so the old PNG path and the firmware display are unaffected. Check GL unpack alignment and texture lifetime; clean up both the display texture and background texture.

The renderer is static SVG-to-pixels, not animated DOM. Do not introduce Chromium/Electron, a webview, or a new frontend framework.

## Execution plan for the Work session

### 1. Establish a visual baseline before changing code

1. Read the current rendering, input and CMake files listed above. Confirm clean working tree and branch.
2. Obtain the exact SVG attachment; inspect its geometry, screen artwork, transforms, clipping, filters, external references and license. It may be skewed/perspective-styled. **Do not assume the interactive screen is an axis-aligned rectangle.**
3. With **resvg 0.49.0 CLI**, render the exact SVG at 1x and 2x and compare to the user's existing Inkscape/Figma reference. Inspect especially the knobs, textured/blurred shadows, displaced surfaces and labels. Record any visible discrepancy; do not silently replace the effects with a flattened PNG.
4. Determine which artwork represents the static display content. Remove/hide that content in a nondestructive source derivative, while retaining the device bezel. Identify the correct screen quad, rotation, cropping, color space and alpha for the live firmware texture.

Stop and document a rendering incompatibility if the complex filter output is materially wrong. A fallback to librsvg or a narrowly justified asset adjustment needs explicit explanation in the PR, not an unannounced substitution.

### 2. Integrate a pinned C-compatible resvg build

1. Add reproducible build integration for `resvg-capi` 0.49.0 using CMake plus Cargo (`--locked`) or an equivalently pinned, documented system/package workflow. A Cargo command from the upstream workspace is typically `cargo build --release --locked -p resvg-capi`; confirm generated library path/name and platform linker requirements instead of guessing.
2. Keep Cargo output in the build directory (not tracked source). Link only the frontend executable/helper target, not the core `emulator` library or headless-specific tests. Account for macOS arm64 and Linux x86_64. Handle runtime packaging if using a dynamic library.
3. Make dependency choice and failure modes explicit. Keep the original PNG path buildable and usable when SVG is disabled, resvg is unavailable, or an SVG cannot be loaded. Avoid requiring Rust to run existing headless-only workflows where practical.
4. Update GitHub CI to install/pin a compatible Rust toolchain for SVG-enabled builds and cache Cargo artifacts sensibly, without weakening current Linux profiling ON/OFF and macOS checks. Note that a Markdown-only planning PR will not exercise CI until code changes are pushed.

### 3. Add a testable SVG rasterization adapter

1. Make a small, RAII-managed C++ adapter (e.g. `gui/svg_background_renderer.h/.cpp`) that parses an SVG **once** into a tree, validates intrinsic size, and renders at an explicit pixel size into a bounded premultiplied RGBA buffer.
2. Reject invalid/oversized dimensions before allocating `width * height * 4`; use overflow-safe arithmetic. Surface useful parse/load errors and clean up on all paths.
3. Compute **uniform aspect-preserving transforms** and preserve the SVG's viewBox and clipping. Avoid cropping shadows or stretching geometry; use clear viewport/letterbox rules.
4. Rasterize on initial load and when necessary after framebuffer-size/content-scale changes, **not** on every `PollEvents()` or firmware frame. Reuse the existing GL texture and avoid expensive repeated filter evaluation while continuously dragging a window edge (e.g. coalesced/deferred resizing).
5. Add tiny synthetic SVG test fixtures covering opaque paint, alpha, one `feGaussianBlur`, and preferably turbulence/displacement. These tests should run without an OpenGL window, original firmware, or user proprietary asset.

### 4. Integrate into GLFW, preserving interaction

1. Add an **opt-in vector UI configuration** (for example `gui/ui-svg.json` + `gui/ui.svg`, selectable via `--ui=svg` or `--ui-config`). Preserve `gui/ui.png` and `gui/ui.json` as working defaults until visual/input acceptance passes. Document the final choice and invocation.
2. Render the background in the existing GLFW/OpenGL window with the *actual* framebuffer pixel size (`glfwGetFramebufferSize`) and correct content scale. Maintain a consistent mapping among SVG/viewBox coordinates, **logical window coordinates** for mouse clicks, and physical framebuffer pixels; do not accidentally multiply hitbox positions by Retina scaling twice.
3. Preserve firmware `GL_RGB / GL_UNSIGNED_SHORT_5_6_5` display upload, screen rotation, and interaction callbacks. If the new artwork skews the screen, use the correct textured quad/transform and mapping rather than overlaying a mismatched rectangular screen.
4. Provide an **independent** set of correct click regions for the new layout, preserving existing GPIO bank/pin mappings for every button, four encoder rotation regions, encoder pushes and volume control. A polygon or inverse-transform hit test is acceptable when the visual geometry is skewed. Releasing a button outside its original region should not leave it stuck; ensure releases work correctly under the new mapping.
5. Preserve existing PNG UI behavior and headless mode. Do not change the Blackfin, DMA/PPI, NAND, audio, or firmware asset semantics.

### 5. Verify and document

- Automated: parser/error paths, nonempty pixel outputs and alpha handling, render dimensions/overflow, scale/coordinate transforms, representative filter rendering; existing headless/input CTests still pass.
- Platform: build and run on Linux and macOS arm64. At minimum, all existing CI jobs remain green and the new SVG tests run in CI with resvg enabled.
- Visual: capture before/after screenshots at **1x and 2x** pixel density, including full front panel, screen inset and shadows. Compare resvg against Inkscape/Figma reference. Check no black/white halos around blurred transparent edges.
- Input: manually test Play, Shift, Synth, Tape, all four encoder directions and push-buttons, volume knob, plus press-inside/release-outside. Mouse positions must stay aligned after resize or display-scale changes.
- Overlay: display area contains the **live emulator framebuffer** with correct orientation, not a baked-in screenshot. If there is no firmware, the shell still renders and the screen area shows an intentional placeholder/blank state.
- Behavior: PNG UI still works; headless and `--help` unaffected. Error paths for missing/malformed SVG must not crash or leave GL objects allocated.

Typical validation from the repository root, using this repository's current LLVM/CMake toolchain setup:

```sh
git submodule update --init --recursive
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release \
  -DOP1_BUILD_HEADLESS_TESTS=ON -DOP1_BUILD_DMA_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/op1emu --help
```

Document any new CMake flags and how to select SVG/PNG, and ensure CI covers the SVG-enabled configuration.

## Deliverables and definition of done

- Pinned resvg C API dependency with reproducible Linux/macOS build instructions.
- A standalone, testable SVG rasterizer wrapper, correct premultiplied-alpha OpenGL integration and resize/Retina behavior.
- The actual user-provided vector artwork (only after redistribution rights confirmed), with static screen content separated from live screen.
- Opt-in working vector UI config with accurate display overlay and all hitboxes.
- Firmware-free tests, updated CI, README/usage notes, and captured visual comparisons.
- CI passing on Linux and macOS, original PNG and headless regressions preserved.

Keep implementation commits on the PR branch and record validation and known
limits in the PR. The upstream submission and ready-for-review transition were
subsequently authorized by the user; merging still requires separate authorization.
