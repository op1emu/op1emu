#!/usr/bin/env node
// CI's look at the built site in Chrome (README.md), with no firmware: the
// page starts and its Worker answers, and its self-test (?selftest: the
// boot ROM fetched from GitHub and checked, and the module, brotli-
// decompressed as the Worker gets it, running an LDR of one RTS to its end)
// passes; then, the module cached by the Service Worker (public/sw.js), a
// second visit's self-test takes it from there. Neither loads LLVM's module
// (op1emu-web.<hash>.deferred.wasm); a third visit's (?selftest=llvm) runs a
// packet only LLVM compiles, which loads it.
//
//   node web/tools/smoke-browser.mjs DIST_DIR PLAYWRIGHT_CORE_DIR [CHROME]

import path from "node:path";
import { pathToFileURL } from "node:url";
import { serveDist } from "./serve-dist.mjs";

const [site, playwrightDir, chrome] = process.argv.slice(2);
if (!site || !playwrightDir) {
  console.error("usage: smoke-browser.mjs DIST_DIR PLAYWRIGHT_CORE_DIR [CHROME]");
  process.exit(2);
}
const { chromium } = await import(pathToFileURL(path.join(playwrightDir, "index.mjs")).href);

const server = await serveDist(site);

const browser = await chromium.launch({ headless: true, ...(chrome ? { executablePath: chrome } : { channel: "chrome" }) });
const context = await browser.newContext();
const failures = [];
// A visit's self-test of `program`: its result, which should have taken the
// module from `source`.
async function visit(name, source, program = "rts") {
  const page = await context.newPage();
  page.on("pageerror", (error) => failures.push(`${name}: ${error.message}`));
  page.on("console", (message) => console.log(`[${message.type()}] ${message.text()}`));
  await page.goto(`${server.url}?selftest=${program}`);
  await page.waitForFunction(() => document.getElementById("nand-state").textContent !== "…", null, { timeout: 30000 });
  const state = await page.textContent("#nand-state");
  console.log(`${name}: NAND ${state}`);
  if (state !== "not stored") failures.push(`${name}: NAND "${state}"`);
  await page.waitForFunction(() => globalThis.op1Selftest, null, { timeout: 300000 });
  const result = await page.evaluate(() => globalThis.op1Selftest);
  console.log(`${name} selftest: ${JSON.stringify(result)}`);
  if (result.error || result.program !== program || result.started !== 0 || !result.finished ||
      result.module !== source)
    failures.push(`${name} selftest (the module from ${source}): ${JSON.stringify(result)}`);
  return page;
}
try {
  const first = await visit("first visit", "br");
  // The Service Worker has the module once it says so (it downloads it).
  await first.waitForFunction(() => globalThis.op1ModuleCached?.(), null, { timeout: 120000, polling: 500 })
    .catch(() => failures.push("the Service Worker did not cache the module"));
  await first.close();
  if (!failures.length) await (await visit("second visit", "cache")).close();
  const deferred = () => server.requests.filter((p) => p.endsWith(".deferred.wasm"));
  if (deferred().length) failures.push(`LLVM's module was loaded: ${deferred().join(", ")}`);
  if (!failures.length) {
    await visit("LLVM's visit", "cache", "llvm");
    if (deferred().length !== 1) failures.push(`LLVM's module loaded ${deferred().length} times, not once`);
  }
} finally {
  await browser.close();
  server.close();
}
if (failures.length) {
  console.error(failures.join("\n"));
  process.exit(1);
}
console.log("smoke-browser: ok");
