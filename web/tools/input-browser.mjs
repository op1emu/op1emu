#!/usr/bin/env node
// CI's check of the page's input in Chrome (README.md), with a stand-in for
// its Worker that has both images and records what the page sends: every
// OP-1 key pressed is released, whatever has the focus when the computer's
// key comes up, and when the window loses focus with keys held on the
// keyboard or by the mouse.
//
//   node web/tools/input-browser.mjs DIST_DIR PLAYWRIGHT_CORE_DIR [CHROME]

import path from "node:path";
import assert from "node:assert/strict";
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
  globalThis.messages = [];
  globalThis.Worker = class {
    constructor() { globalThis.testWorker = this; }
    postMessage(message) {
      messages.push(message);
      if (message.type === "key" || message.type === "com")
        keys.push(`${message.bank}.${message.pin}${message.pressed ? "+" : "-"}`);
      if (message.type === "files") {
        const files = { kind: "manual", ready: true, nand: { size: 553648128, modified: 0 }, otp: { size: 8192, modified: 0 } };
        setTimeout(() => this.onmessage({ data: { type: "files", syncAccess: true, files } }), 0);
      }
    }
  };
}

const server = await serveDist(site);
const browser = await chromium.launch({ headless: true,
  args: ["--autoplay-policy=document-user-activation-required",
    "--disable-features=PreloadMediaEngagementData,MediaEngagementBypassAutoplayPolicies"],
  ...(chrome ? { executablePath: chrome } : { channel: "chrome" }) });
const failures = [];
try {
  const page = await browser.newPage();
  page.on("pageerror", (error) => failures.push(`page: ${error.message}`));
  await page.addInitScript(standIn);
  await page.goto(server.url);
  const preBootUsb = await page.locator("#key-usb").boundingBox();
  await page.mouse.click(preBootUsb.x + preBootUsb.width / 2, preBootUsb.y + preBootUsb.height / 2);
  assert.equal(await page.evaluate(() => messages.some((m) => m.type === "com" || m.type === "start")), false,
    "the panel USB key does not send input or start the CPU before boot");
  assert.equal(await page.locator("#key-usb").evaluate((key) => key.classList.contains("down")), false);
  await page.locator("#start").click();
  await page.waitForFunction(() => messages.some((m) => m.type === "start"));
  assert.equal(await page.evaluate(() => messages.some((m) => m.type === "com")), false,
    "ordinary Start does not hold COM");

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
    await page.locator("#device").scrollIntoViewIfNeeded();
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

  // COM is momentary, including while startup awaits the audio/module cache.
  const boot = await browser.newPage({ hasTouch: true });
  boot.on("pageerror", (error) => failures.push(`COM page: ${error.message}`));
  await boot.addInitScript(standIn);
  await boot.goto(server.url);
  const com = boot.locator("#te-boot");
  await com.waitFor({ state: "visible" });
  await boot.waitForFunction(() => !document.getElementById("te-boot").disabled);
  const levels = () => boot.evaluate(() => messages.filter((m) => m.type === "com").map((m) => m.pressed));
  const clearCom = () => boot.evaluate(() => { messages.length = 0; });
  await boot.evaluate(() => scrollTo(0, document.documentElement.scrollHeight));
  await com.hover();
  const originalBox = await com.boundingBox();
  await boot.mouse.down();
  await boot.waitForFunction(() => messages.some((m) => m.type === "start"));
  assert.deepEqual(await levels(), [true], "holding COM starts with COM down");
  assert.equal(await com.isVisible(), true, "COM remains available during startup");
  assert.deepEqual(await com.boundingBox(), originalBox, "starting does not move COM");
  assert.equal(await boot.locator("#start").isVisible(), false, "Start is hidden during the session");
  assert.equal(await com.textContent(), "COM (hold)");
  await boot.mouse.move(0, 0);
  await boot.mouse.up();
  assert.deepEqual(await levels(), [true, false], "releasing outside the button releases COM");
  assert.equal(await boot.evaluate(() => messages.filter((m) => m.type === "start").length), 1);

  await clearCom();
  await com.focus();
  await boot.keyboard.down("Space");
  await boot.keyboard.down("Space");
  await com.hover();
  await boot.mouse.down();
  await boot.mouse.up();
  assert.deepEqual(await levels(), [true], "pointer release preserves a keyboard hold");
  await boot.focus("#export-otp");
  await boot.keyboard.up("Space");
  assert.deepEqual(await levels(), [true, false], "key release follows focus changes");

  await clearCom();
  await com.focus();
  await boot.keyboard.down("Space");
  const pointAtUsb = async () => {
    // Setup height varies (including the firmware preparation form). A bounding
    // box can be outside the viewport; focus can also scroll between gestures.
    await boot.locator('#device').scrollIntoViewIfNeeded();
    const usb = await boot.locator('#key-usb').boundingBox();
    await boot.mouse.move(usb.x + usb.width / 2, usb.y + usb.height / 2);
  };
  await pointAtUsb();
  await boot.mouse.down();
  await boot.mouse.up();
  assert.deepEqual(await levels(), [true], "panel USB release preserves a COM button hold");
  await boot.keyboard.up("Space");
  assert.deepEqual(await levels(), [true, false]);
  await clearCom();
  await pointAtUsb();
  await boot.mouse.down();
  assert.deepEqual(await levels(), [true], 'panel USB pointer hold was acquired');
  await com.evaluate(button => button.focus({ preventScroll: true }));
  await boot.keyboard.press("Space");
  assert.deepEqual(await levels(), [true], "COM button release preserves a panel USB hold");
  await boot.mouse.up();
  assert.deepEqual(await levels(), [true, false]);

  await clearCom();
  await com.hover();
  await boot.mouse.down();
  await boot.evaluate(() => dispatchEvent(new Event("blur")));
  await boot.mouse.up();
  assert.deepEqual(await levels(), [true, false], "blur releases COM exactly once");
  assert.equal(await com.evaluate((button) => button.classList.contains("down")), false);

  for (const type of ["pointercancel", "lostpointercapture"]) {
    await clearCom();
    await com.hover();
    await boot.mouse.down();
    await com.dispatchEvent(type, { pointerId: 1 });
    await boot.mouse.up();
    assert.deepEqual(await levels(), [true, false], `${type} releases COM`);
  }
  await clearCom();
  await com.tap();
  assert.deepEqual(await levels(), [true, false], "touch releases COM");
  assert.equal(await boot.evaluate(() => messages.filter((m) => m.type === "start").length), 0,
    "COM does not restart a running emulator");
  await boot.reload();
  await boot.waitForFunction(() => !document.getElementById("te-boot").disabled);
  await com.focus();
  await boot.keyboard.press("Enter");
  await boot.waitForFunction(() => messages.some((m) => m.type === "start"));
  assert.deepEqual(await levels(), [true, false], "release during loading cancels the hold");
  for (const command of ["import", "export", "delete"]) {
    await boot.evaluate((command) => testWorker.onmessage({ data: {
      type: "error", command, text: `${command} failed`,
    } }), command);
    assert.equal(await com.isDisabled(), false, `${command} failure leaves the session running`);
    assert.equal(await boot.locator("#status").textContent(), `${command} failed`);
  }
  await boot.evaluate(() => testWorker.onmessage({ data: { type: "stopped" } }));
  assert.equal(await com.isDisabled(), true, "COM is disabled after stop");
  const starts = await boot.evaluate(() => messages.filter((m) => m.type === "start").length);
  await com.dispatchEvent("pointerdown", { button: 0, pointerId: 1 });
  await com.dispatchEvent("keydown", { code: "Space" });
  await boot.evaluate(() => testWorker.onmessage({ data: {
    type: "files", syncAccess: true, files: { nand: { modified: 0 }, otp: { modified: 0 } },
  } }));
  await boot.evaluate(() => testWorker.onmessage({ data: { type: "stats", frames: 10, guestS: 1 } }));
  assert.equal(await com.isDisabled(), true, "file updates cannot re-enable a stopped session");
  assert.equal(await boot.evaluate(() => messages.filter((m) => m.type === "start").length), starts);
  assert.match(await boot.locator("#status").textContent(), /Reload the page/);
  await boot.close();
  console.log("ok: COM startup, pointer, touch, keyboard, cancellation and blur");

  // A fresh document with no previous activation, real AudioContext/worklet,
  // and trusted touch events. Read through CDP with userGesture:false:
  // Playwright evaluate() itself can grant activation and mask this bug.
  for (const mode of ["touch-com", "touch-start", "mouse-com"]) {
    const context = await browser.newContext({ hasTouch: true, viewport: { width: 1280, height: 1100 } });
    await context.addInitScript(standIn);
    await context.addInitScript(() => {
      Object.defineProperty(navigator, "serviceWorker", { value: undefined });
      globalThis.audioContexts = [];
      const RealAudioContext = AudioContext;
      globalThis.AudioContext = class extends RealAudioContext {
        constructor(...args) { super(...args); audioContexts.push(this); }
      };
    });
    const audioPage = await context.newPage();
    audioPage.on("pageerror", (error) => failures.push(`${mode}: ${error.message}`));
    await audioPage.goto(server.url, { waitUntil: "networkidle" });
    const cdp = await context.newCDPSession(audioPage);
    const read = async (expression) => {
      const result = await cdp.send("Runtime.evaluate", {
        expression, userGesture: false, returnByValue: true,
      });
      if (result.exceptionDetails)
        throw new Error(result.exceptionDetails.exception?.description ?? result.exceptionDetails.text);
      return result.result.value;
    };
    const until = async (expression) => {
      const end = Date.now() + 5000;
      while (!(await read(expression))) {
        if (Date.now() >= end) {
          const states = await read("audioContexts.map(c => c.state)");
          throw new Error(`${mode}: timed out: ${expression}; audio states: ${states}`);
        }
        await new Promise((resolve) => setTimeout(resolve, 20));
      }
    };
    assert.equal(await read("navigator.userActivation.hasBeenActive"), false);
    const selector = mode === "touch-start" ? "#start" : "#te-boot";
    const point = await read(`(() => {
      const r = document.querySelector('${selector}').getBoundingClientRect();
      return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
    })()`);
    if (mode === "mouse-com")
      await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", ...point, button: "left", clickCount: 1 });
    else await cdp.send("Input.dispatchTouchEvent", { type: "touchStart", touchPoints: [{ ...point, id: 1 }] });
    if (mode === "touch-start") assert.equal(await read("audioContexts.length"), 0);
    else {
      await until('messages.some((m) => m.type === "start")');
      assert.equal(await read("audioContexts[0].state"), mode === "touch-com" ? "suspended" : "running",
        "COM starts the CPU while held, without waiting for touch audio activation");
    }
    if (mode === "mouse-com")
      await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", ...point, button: "left", clickCount: 1 });
    else await cdp.send("Input.dispatchTouchEvent", { type: "touchEnd", touchPoints: [] });
    await until('audioContexts.length === 1 && audioContexts[0].state === "running"');
    if (mode === "mouse-com") {
      await read('testWorker.onmessage({data:{type:"error",command:"export",text:"export failed"}})');
      assert.equal(await read("audioContexts[0].state"), "running", "ordinary errors keep audio running");
      assert.equal(await read('document.getElementById("te-boot").disabled'), false);
      await read('testWorker.onmessage({data:{type:"error",command:"start",text:"start failed"}})');
      await until('audioContexts[0].state === "closed"');
      assert.equal(await read('document.getElementById("te-boot").disabled'), true);
      await read('testWorker.onmessage({data:{type:"progress",text:"late download progress"}})');
      assert.match(await read('document.getElementById("status").textContent'), /start failed.*Reload the page/);
      assert.equal(await read('messages.filter(m => m.type === "start").length'), 1);
    }
    await context.close();
    console.log(`ok: ${mode}: real AudioContext running after release`);
  }
} finally {
  await browser.close();
  server.close();
}
if (failures.length) {
  console.error(failures.join("\n"));
  process.exit(1);
}
console.log("input-browser: ok");
