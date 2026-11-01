# Firmware importer draft validation

Cloud validation, 2026-11-01. No physical-device action.

- Upstream base: `a2ff78e3ec866384225545caa0e77343d754f199`.
- 24 tests pass with the optional independently built mkyaffs2 oracle enabled.
  Default tests skip that one external-oracle case; they never download tools.
- Actual `lzma@2.3.2` JavaScript decompression of a privately supplied stock 246
  package, followed by pinned-reference key parsing and loader decryption,
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
- Source-level lifecycle regressions cover queued cancellation, failed cache
  deletion, repeated native start, terminal stop, and generic storage errors.
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
