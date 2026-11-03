# Firmware importer draft validation

## Upstream startup integration (2026-11-02)

Merged upstream `64a093c95a40f3f88f38bd7028d054d1420437c2` without rewriting
feature history. The browser retains COM held-level startup, release during
loading, source-aware key handling and trusted-event audio activation. A stopped
or failed start releases input and closes audio but remains terminal until reload;
ordinary storage errors do not stop a healthy session. Both Start and COM use
the active configuration and are disabled during preparation. Local OTP-only
preparation and commit-last selection are unchanged. OTP extraction remains out
of scope; no device access is performed.

Local integration tests: 45 passed, 1 optional oracle skipped, 0 failed; independent
review approved. Syntax and diff checks pass. Integration checks cover
prepared-only COM readiness, preparation/terminal
controls, command-tagged failures, late progress and COM state before the first
CPU slice. Browser/native/WASM execution is delegated to exact-head CI because
this fresh cloud checkout has no installed build dependencies. No installation
was performed; real-input/mobile limitations below still apply.

## Single active configuration revision (earlier 2026-11-02)

- Synthetic tests exercise prepared-only Export, both input transitions,
  shared UI/Start/Export selection, persistence, failed initial commits,
  repeated preparation, queued/in-flight/final-handle cancellation, and native
  session lifecycle. No firmware or OTP values are used in these tests.
- Selection records commit last; manual file imports also stage replacements.
  Failed preparation preserves the active configuration. The downloaded OTP
  integration is removed; only one-way deletion of an obsolete cache remains.
- Local Node tests: 42 passed, 1 optional oracle skipped, 0 failed. Syntax
  checks and `git diff --check` pass. The optional
  independent mkyaffs2 oracle is unavailable and skipped.
- This fresh cloud workspace lacks Vite/Playwright dependencies, pytest and the
  native/WASM build toolchain. None were installed. Exact-head CI must provide
  those build/test checks; its existing workflow includes the updated synthetic
  firmware browser harness and native emulator browser smoke tests.
- Real-firmware preparation/boot, private-input browser transition tests and
  low-memory/mobile qualification remain unverified for this revision. Keep
  the PR draft; synthetic and historical evidence are not those claims.

## Original draft evidence

Historical cloud validation of the original draft, 2026-11-01. No physical-device
action. These results predate the single-active-configuration revision and are
not an exact-head validation claim for that revision.

- Upstream base: `a2ff78e3ec866384225545caa0e77343d754f199`.
- 24 tests pass with the optional independently built mkyaffs2 oracle enabled.
  Default tests skip that one external-oracle case; they never download tools.
- Actual `lzma@2.3.2` JavaScript decompression of a privately supplied stock 246
  package, followed by OTP text parsing and loader decryption,
  produces the canonical 2,171,624-byte plaintext SHA-256
  `1e806672dc440d495987c6b1f33b300876e6a3bb1d58254aac5fe60c960e906a`.
  800 records; 45 encrypted records; 3,358 cipher units including 30 tail units.
- Actual factory content produces 11,810 pages and 115 non-root objects.
  All full data pages compare against independently compiled yaffs2utils
  `1edc1688b7f8db140d2b85448dd68f9e6318dc95` after normalizing parent IDs,
  atime/ctime, and directory permission bits. The host needed directory execute
  permission for traversal; the browser image retains original TAR permissions.
  Sequence/chunk/length tags agree. Object IDs and their ECC differ because
  reference filesystem enumeration is not lexical; this is not a claim of
  identical whole-image bytes. Separate nested synthetic fixtures compare every
  data and translated OOB/ECC byte, including a 64-page boundary.
- Historical source-level lifecycle regressions covered queued cancellation,
  repeated native start, terminal stop, and generic storage errors. Download/cache
  code and its tests have since been removed; preparation now requires local
  authorized OTP text for encrypted input.
- Vite bundling with a test-only emulator manifest succeeds. This is NOT a full
  native/WASM build. The stub is confined to `tools/firmware-browser.mjs` and is
  never used by the production build.

Still pending: exact-head CI build, browser UI execution, complete original
firmware boot through the new path, and low-memory/mobile qualification. Local
Chromium execution is blocked by workspace sandbox socket restrictions; no
sandbox workaround was used. Keep the PR draft until these are reviewed.

`node tools/firmware-browser.mjs` runs the source-only UI/worker test in Chrome
without firmware or OTP. Optional private `OP1_FIRMWARE` and `OP1_OTP_TEXT`
variables enable local end-to-end preparation/persistence checks; never supply
those inputs to public CI or upload their artifacts.
