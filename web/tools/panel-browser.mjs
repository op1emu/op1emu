#!/usr/bin/env node
// Exercise the real built SVG/PNG frontend with a recording Worker, no firmware.
// node tools/panel-browser.mjs dist node_modules/playwright-core [CHROME]
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { serveDist } from "./serve-dist.mjs";

const [site, playwrightDir, chrome] = process.argv.slice(2);
if (!site || !playwrightDir) throw new Error("usage: panel-browser.mjs DIST PLAYWRIGHT_CORE [CHROME]");
const { chromium } = await import(pathToFileURL(path.join(playwrightDir, "index.mjs")).href);
const svg = JSON.parse(fs.readFileSync(new URL("../panel/layout.json", import.meta.url)));
const png = JSON.parse(fs.readFileSync(new URL("../../gui/ui.json", import.meta.url)));
const artwork = fs.readFileSync(new URL("../panel/op1.svg", import.meta.url), "utf8");
assert.equal((artwork.match(/<filter\b/g) ?? []).length, 217);
assert.deepEqual(Object.keys(svg.buttons).sort(), Object.keys(png.buttons).sort());
for (const name in svg.buttons) assert.deepEqual(svg.buttons[name].gpio, png.buttons[name].gpio);

function standIn() {
  globalThis.messages = [];
  globalThis.workerCount = 0;
  globalThis.startCount = 0;
  globalThis.Worker = class {
    constructor() { workerCount++; globalThis.testWorker = this; }
    postMessage(message) {
      messages.push(message);
      if (message.type === "start") startCount++;
      if (message.type === "files") setTimeout(() => this.onmessage({ data: {
        type: "files", syncAccess: true, files: { nand: { modified: 0 }, otp: { modified: 0 } },
      } }), 0);
    }
  };
  globalThis.sendFrame = () => {
    const pixels = new Uint8ClampedArray(400 * 200 * 4);
    for (let y = 0; y < 200; y++) for (let x = 0; x < 400; x++) {
      const color = y < 100 ? (x < 200 ? [255, 0, 0, 255] : [0, 255, 0, 255])
        : (x < 200 ? [0, 0, 255, 255] : [255, 255, 0, 255]);
      pixels.set(color, (y * 400 + x) * 4);
    }
    testWorker.onmessage({ data: { type: "frame", width: 400, height: 200, pixels } });
  };
}

const server = await serveDist(site);
const browser = await chromium.launch({ headless: true, ...(chrome ? { executablePath: chrome } : { channel: "chrome" }) });
const errors = [];
try {
  const context = await browser.newContext({ viewport: { width: 1280, height: 900 }, deviceScaleFactor: 2, hasTouch: true });
  await context.addInitScript(standIn);
  const page = await context.newPage();
  page.on("pageerror", (error) => errors.push(error.message));
  await page.goto(server.url);
  await page.locator("#start").click();
  await page.evaluate(() => sendFrame());
  const sent = () => page.evaluate(() => messages.filter((m) => m.type === "key")
    .map((m) => `${m.bank}.${m.pin}${m.pressed ? "+" : "-"}`));
  const clear = () => page.evaluate(() => { messages.length = 0; });
  const ready = () => page.waitForFunction(() => {
    const image = document.getElementById("faceplate");
    return image.complete && image.naturalWidth > 0;
  });
  const change = async (selector, value) => {
    const before = await page.locator("#faceplate").getAttribute("src");
    await page.selectOption(selector, value);
    await page.waitForFunction((before) => document.getElementById("faceplate").src !== before, before);
    await ready();
  };
  const position = async (rectangle, layout) => {
    // Clicking Start or changing selectors can scroll the longer setup form.
    // Mouse coordinates do not auto-scroll like locator clicks do.
    await page.locator("#device").scrollIntoViewIfNeeded();
    const box = await page.locator("#device").boundingBox();
    const image = await page.locator("#faceplate").evaluate((img) => [img.naturalWidth, img.naturalHeight]);
    const [x, y, w, h] = rectangle;
    const point = [box.x + (x + w / 2) * box.width / (layout.width ?? image[0]),
      box.y + (y + h / 2) * box.height / (layout.height ?? image[1])];
    const viewport = page.viewportSize();
    assert.ok(point[0] >= 0 && point[0] < viewport.width && point[1] >= 0 && point[1] < viewport.height,
      `panel control outside viewport: ${point}`);
    return point;
  };
  const checkFrame = async () => {
    const colors = await page.locator("#screen").evaluate((canvas) => {
      const ctx = canvas.getContext("2d");
      return [[10, 10], [390, 10], [10, 190], [390, 190]]
        .map(([x, y]) => [...ctx.getImageData(x, y, 1, 1).data]);
    });
    assert.deepEqual(colors, [[255, 0, 0, 255], [0, 255, 0, 255], [0, 0, 255, 255], [255, 255, 0, 255]]);
    assert.equal(await page.evaluate(() => workerCount), 1);
    assert.equal(await page.evaluate(() => startCount), 1);
  };
  // All 58 controls retain their GPIOs at desktop and narrow widths, for both layouts.
  for (const panel of ["svg", "png"]) {
    if (panel === "png") await change("#panel-style", panel);
    const layout = panel === "svg" ? svg : png;
    for (const width of [1280, 390]) {
      await page.setViewportSize({ width, height: 900 });
      for (const [name, button] of Object.entries(layout.buttons)) {
        await clear();
        await page.mouse.click(...await position(button.position, layout));
        assert.deepEqual(await sent(), [`${button.gpio.join(".")}+`, `${button.gpio.join(".")}-`], `${panel} ${width} ${name}`);
      }
      const screenBox = await page.locator("#screen").boundingBox();
      const deviceBox = await page.locator("#device").boundingBox();
      const [left, top, w, h] = layout.display.quad
        ? [295.5, 32, 231, 115.5] : Object.values(layout.display);
      const [iw, ih] = await page.locator("#faceplate").evaluate((img) => [img.naturalWidth, img.naturalHeight]);
      for (const [actual, expected] of [[screenBox.x, deviceBox.x + left / iw * deviceBox.width],
        [screenBox.y, deviceBox.y + top / ih * deviceBox.height],
        [screenBox.width, w / iw * deviceBox.width], [screenBox.height, h / ih * deviceBox.height]])
        assert.ok(Math.abs(actual - expected) < 1, `${panel} screen calibration`);
    }
    // Each encoder's forward/reverse click and wheel sends the quadrature sequence.
    for (const encoder of Object.values(layout.encoders)) {
      await clear();
      await page.mouse.click(...await position(encoder.clockwise, layout));
      await page.mouse.click(...await position(encoder.counter_clockwise, layout));
      const [a, b] = [encoder.clockwise_gpio.join("."), encoder.counter_clockwise_gpio.join(".")];
      assert.deepEqual(await sent(), [`${a}+`, `${b}-`, `${a}-`, `${b}-`]);
      await clear();
      await page.mouse.move(...await position(encoder.clockwise, layout));
      await page.mouse.wheel(0, -20);
      await page.waitForFunction(() => messages.some((m) => m.type === "key"));
      await page.mouse.wheel(0, 20);
      await page.waitForFunction(() => messages.filter((m) => m.type === "key").length === 4);
      assert.deepEqual(await sent(), [`${a}+`, `${b}-`, `${a}-`, `${b}-`]);
    }
    await clear();
    await page.mouse.click(...await position(layout.volume_knob.clockwise, layout));
    await page.mouse.click(...await position(layout.volume_knob.counter_clockwise, layout));
    assert.deepEqual(await page.evaluate(() => messages.filter((m) => m.type === "volume").map((m) => m.value)), [133, 128]);
    await clear();
    await page.touchscreen.tap(...await position(layout.buttons.c4.position, layout));
    assert.deepEqual(await sent(), ["4.12+", "4.12-"]);
    await clear();
    await page.mouse.move(...await position(layout.buttons.c4.position, layout));
    await page.mouse.down();
    await page.mouse.move(0, 0);
    await page.mouse.up();
    assert.deepEqual(await sent(), ["4.12+", "4.12-"]);
    assert.equal(await page.locator(".down").count(), 0);
  }
  await change("#panel-style", "svg");
  await page.setViewportSize({ width: 1280, height: 900 });
  const samples = [];
  const protectedSamples = [];
  for (const theme of ["slate", "silver", "yellow", "green", "purple", "orange"]) {
    await change("#theme", theme);
    const paint = await page.locator("#faceplate").evaluate((image) => {
      const c = document.createElement("canvas"); c.width = 1204; c.height = 434;
      const ctx = c.getContext("2d"); ctx.drawImage(image, 0, 0);
      const pixel = (x, y) => [...ctx.getImageData(x, y, 1, 1).data].join(",");
      return { body: pixel(1160, 200), fixed: [[607, 84], [738, 84], [869, 84], [1000, 84], [411, 88], [0, 0]]
        .map(([x, y]) => pixel(x, y)) };
    });
    samples.push(paint.body);
    protectedSamples.push(paint.fixed);
    await page.evaluate(() => sendFrame());
    await checkFrame();
    if (process.env.OP1_PANEL_CAPTURE && theme === "silver") {
      const box = await page.locator("#device").boundingBox();
      await page.screenshot({ path: process.env.OP1_PANEL_CAPTURE,
        clip: { x: 0, y: 0, width: 1280, height: Math.ceil(box.y + box.height + 16) } });
    }
  }
  assert.equal(new Set(samples).size, 6, "all six themes have distinct paint");
  for (const fixed of protectedSamples) assert.deepEqual(fixed, protectedSamples[0], "caps, screen and silhouette keep their paint");
  await clear();
  await page.focus("#export-otp");
  await page.keyboard.down("q");
  await change("#theme", "silver");
  assert.deepEqual(await sent(), ["5.7+"], "theme changes preserve held notes");
  await page.keyboard.up("q");
  assert.deepEqual(await sent(), ["5.7+", "5.7-"]);
  // Selecting themes with a musical-key letter must not press an instrument key.
  await clear();
  await page.focus("#theme");
  await page.keyboard.press("g");
  assert.deepEqual(await sent(), []);
  // A panel change releases held keys before changing coordinate systems.
  await page.focus("#export-otp");
  await page.keyboard.down("q");
  await change("#panel-style", "png");
  await page.keyboard.up("q");
  assert.deepEqual(await sent(), ["5.7+", "5.7-"]);
  assert.equal(await page.locator(".down").count(), 0);
  await checkFrame();
  assert.equal(await page.locator("#theme").isDisabled(), true);

  // Rapid changes commit only the newest request, and keep the same Worker.
  await change("#panel-style", "svg");
  await page.evaluate(() => {
    const select = document.getElementById("theme");
    for (const value of ["green", "slate", "orange"]) {
      select.value = value; select.dispatchEvent(new Event("change"));
    }
  });
  await page.waitForFunction(async () => {
    const text = await (await fetch(document.getElementById("faceplate").src)).text();
    return text.includes("fill: #9A6429;");
  });

  const fallback = await context.newPage();
  fallback.on("pageerror", (error) => errors.push(error.message));
  await fallback.route("**/*.svg", (route) => route.abort());
  await fallback.goto(server.url);
  await fallback.waitForFunction(() => document.getElementById("device").dataset.panel === "png");
  assert.match(await fallback.locator("#panel-status").textContent(), /SVG unavailable/);
  assert.equal(await fallback.locator("#theme").isDisabled(), true);
  await fallback.locator("#start").click();
  await fallback.locator("#key-c4").click({ force: true });
  assert.deepEqual(await fallback.evaluate(() => messages.filter((m) => m.type === "key").map((m) => m.pressed)), [true, false]);
  const original = await context.newPage();
  await original.goto(`${server.url}?ui=png`);
  await original.waitForFunction(() => document.getElementById("device").dataset.panel === "png");
  assert.deepEqual(errors, []);
  console.log("panel-browser: SVG/PNG, 232 button mappings, encoders, volume, touch, release, themes, frames and fallback: ok");
} finally {
  await browser.close();
  server.close();
}
