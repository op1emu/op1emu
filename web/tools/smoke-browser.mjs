#!/usr/bin/env node
// CI's look at the built site in Chrome (README.md), with no firmware: the
// page starts and its Worker answers, and its self-test (?selftest: the
// module, brotli-decompressed as the Worker gets it, running an LDR of one
// RTS to its end) passes.
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
const failures = [];
try {
  const page = await browser.newPage();
  page.on("pageerror", (error) => failures.push(`page: ${error.message}`));
  page.on("console", (message) => console.log(`[${message.type()}] ${message.text()}`));
  await page.goto(`${server.url}?selftest`);
  await page.waitForFunction(() => document.getElementById("nand-state").textContent !== "…", null, { timeout: 30000 });
  const state = await page.textContent("#nand-state");
  console.log(`page: NAND ${state}`);
  if (state !== "not stored") failures.push(`page: NAND "${state}"`);
  await page.waitForFunction(() => globalThis.op1Selftest, null, { timeout: 300000 });
  const result = await page.evaluate(() => globalThis.op1Selftest);
  console.log(`selftest: ${JSON.stringify(result)}`);
  if (result.error || result.started !== 0 || !result.finished) failures.push(`selftest: ${JSON.stringify(result)}`);
} finally {
  await browser.close();
  server.close();
}
if (failures.length) {
  console.error(failures.join("\n"));
  process.exit(1);
}
console.log("smoke-browser: ok");
