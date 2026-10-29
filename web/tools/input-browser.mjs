#!/usr/bin/env node
// CI's check of the page's input in Chrome (README.md), with a stand-in for
// its Worker that has both images and records what the page sends: every
// OP-1 key pressed is released, whatever has the focus when the computer's
// key comes up, and when the window loses focus with keys held on the
// keyboard or by the mouse.
//
//   node web/tools/input-browser.mjs DIST_DIR PLAYWRIGHT_CORE_DIR [CHROME]

import path from "node:path";
import { pathToFileURL } from "node:url";
import { serveDist } from "./serve-dist.mjs";

const [site, playwrightDir, chrome] = process.argv.slice(2);
if (!site || !playwrightDir) {
  console.error("usage: input-browser.mjs DIST_DIR PLAYWRIGHT_CORE_DIR [CHROME]");
  process.exit(2);
}
const { chromium } = await import(pathToFileURL(path.join(playwrightDir, "index.mjs")).href);

// The Worker: both images stored, and the keys the page sends, in order.
function standIn() {
  globalThis.keys = [];
  globalThis.Worker = class {
    postMessage(message) {
      if (message.type === "key") keys.push(`${message.bank}.${message.pin}${message.pressed ? "+" : "-"}`);
      if (message.type === "files") {
        const files = { nand: { size: 553648128, modified: 0 }, otp: { size: 8192, modified: 0 } };
        setTimeout(() => this.onmessage({ data: { type: "files", syncAccess: true, files } }), 0);
      }
    }
  };
}

const server = await serveDist(site);
const browser = await chromium.launch({ headless: true, ...(chrome ? { executablePath: chrome } : { channel: "chrome" }) });
const failures = [];
try {
  const page = await browser.newPage();
  page.on("pageerror", (error) => failures.push(`page: ${error.message}`));
  await page.addInitScript(standIn);
  await page.goto(server.url);
  await page.locator("#start").click();
  await page.waitForFunction(() => document.getElementById("start").disabled);

  // Each check: what it does, then every key it pressed released and none
  // shown down.
  const check = async (name, steps) => {
    await page.evaluate(() => (keys.length = 0));
    await steps();
    const { sent, down } = await page.evaluate(() => ({
      sent: [...keys],
      down: [...document.querySelectorAll(".down")].map((element) => element.id),
    }));
    const level = new Map();
    for (const event of sent) level.set(event.slice(0, -1), event.at(-1));
    const stuck = [...level].filter(([, last]) => last === "+").map(([gpio]) => gpio);
    const ok = sent.length > 0 && stuck.length === 0 && down.length === 0;
    console.log(`${ok ? "ok" : "FAILED"}: ${name}: sent ${sent.join(" ") || "nothing"}`);
    if (!ok) failures.push(`${name}: sent [${sent}], stuck [${stuck}], shown down [${down}]`);
  };
  const button = async (name) => {
    const box = await page.locator(`#key-${name}`).boundingBox();
    await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  };

  await check("a key with a button focused", async () => {
    await page.focus("#export-otp");
    await page.keyboard.press("q");
  });
  await check("a key released after the focus moved", async () => {
    await page.evaluate(() => document.activeElement.blur());
    await page.keyboard.down("q");
    await page.focus("#export-otp");
    await page.keyboard.up("q");
  });
  await check("keys held when the window loses focus", async () => {
    await page.keyboard.down("q");
    await button("c4");
    await page.mouse.down();
    await page.evaluate(() => dispatchEvent(new Event("blur")));
  });
  // The releases that come after it send nothing more.
  await page.evaluate(() => (keys.length = 0));
  await page.mouse.up();
  await page.keyboard.up("q");
  const late = await page.evaluate(() => keys);
  if (late.length) failures.push(`after the blur: ${late}`);
} finally {
  await browser.close();
  server.close();
}
if (failures.length) {
  console.error(failures.join("\n"));
  process.exit(1);
}
console.log("input-browser: ok");
