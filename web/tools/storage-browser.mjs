#!/usr/bin/env node
// Browser sidebar against a scripted storage Worker; no firmware or real USB.
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { serveDist } from "./serve-dist.mjs";
import { createStorageWorker } from "../storage-worker.js";

const [site, playwrightDir, chrome] = process.argv.slice(2);
if (!site || !playwrightDir) throw new Error("usage: storage-browser.mjs DIST PLAYWRIGHT_CORE [CHROME]");
const { chromium } = await import(pathToFileURL(path.join(playwrightDir, "index.mjs")).href);

// A real bus-loss transition tears down the old mount even before the next
// command: returning to Upload firmware must always require re-enumeration.
{
  let active = true, mounted = true, disconnects = 0;
  const events = [];
  const storage = createStorageWorker({
    _op1_usb_bus_active: () => Number(active),
    _op1_msc_connected: () => Number(mounted),
    _op1_msc_disconnect: () => { ++disconnects; mounted = false; },
  }, (data) => events.push(data));
  storage.pollBus();
  events.length = 0;
  active = false;
  storage.pollBus();
  assert.equal(disconnects, 1, "bus loss disconnects a mounted MSC session");
  assert.ok(events.some((data) => data.type === "usb-bus-state" && !data.active));
  assert.ok(events.some((data) => data.type === "msc-state" && !data.connected));
}

function standIn() {
  globalThis.messages = [];
  globalThis.uploads = [];
  globalThis.saved = [];
  globalThis.previewText = "OP-1\n<b>not markup</b> π\n";
  globalThis.failConnect = false;
  globalThis.deferUpload = false;
  globalThis.pendingUpload = null;
  globalThis.deferDownload = false;
  globalThis.pendingDownload = null;
  globalThis.deferDisconnect = false;
  globalThis.createdObjectUrls = [];
  globalThis.revokedObjectUrls = [];
  globalThis.audioPauses = 0;
  const createObjectURL = URL.createObjectURL.bind(URL);
  const revokeObjectURL = URL.revokeObjectURL.bind(URL);
  URL.createObjectURL = (blob) => {
    const url = createObjectURL(blob);
    createdObjectUrls.push(url);
    return url;
  };
  URL.revokeObjectURL = (url) => {
    revokedObjectUrls.push(url);
    revokeObjectURL(url);
  };
  const pause = HTMLMediaElement.prototype.pause;
  HTMLMediaElement.prototype.pause = function (...args) {
    if (this.tagName === "AUDIO") ++audioPauses;
    return pause.apply(this, args);
  };
  globalThis.showSaveFilePicker = undefined;  // exercise the Blob fallback first
  const stamp = { modifiedDate: ((2026 - 1980) << 9) | (11 << 5) | 3, modifiedTime: (12 << 11) | (34 << 5) | 28 };
  const listings = {
    "/": [
      { name: "samples", size: 0, directory: true, ...stamp },
      { name: "other", size: 0, directory: true, ...stamp },
      { name: "many", size: 0, directory: true, ...stamp },
      { name: "root.txt", size: new TextEncoder().encode(previewText).length, directory: false, ...stamp },
      { name: "<img src=x onerror=alert(1)>", size: 1, directory: false, ...stamp },
    ],
    "/samples": [
      { name: "demo.bin", size: 150000, directory: false, ...stamp },
      { name: "pixel.png", size: 150000, directory: false, ...stamp },
      { name: "tone.wav", size: 150000, directory: false, ...stamp },
      { name: "huge.bin", size: 128 * 1024 * 1024 + 1, directory: false, ...stamp },
      { name: "oversize-stream.bin", size: 1, directory: false, ...stamp },
    ],
    "/other": [],
    "/many": Array.from({ length: 80 }, (_, i) => ({
      name: `file-${String(i).padStart(2, "0")}.txt`, size: i, directory: false, ...stamp,
    })),
  };
  const downloads = new Map();
  globalThis.Worker = class {
    constructor() { globalThis.testWorker = this; }
    postMessage(message) {
      messages.push(message);
      const answer = (data) => setTimeout(() => this.onmessage({ data }), 0);
      if (message.type === "files") answer({ type: "files", syncAccess: true,
        files: { nand: { modified: 0 }, otp: { modified: 0 } } });
      if (message.type === "msc-connect") {
        if (failConnect) return answer({ type: "error", command: message.type, id: message.id,
          text: "USB device has no mass storage interface" });
        answer({ type: "msc-state", connected: true });
        answer({ type: "msc-result", id: message.id, command: message.type });
      }
      if (message.type === "msc-disconnect") {
        if (deferDisconnect) {
          globalThis.releaseDisconnect = () => {
            globalThis.releaseDisconnect = null;
            this.onmessage({ data: { type: "msc-state", connected: false } });
            answer({ type: "msc-result", id: message.id, command: message.type });
          };
        } else {
          answer({ type: "msc-state", connected: false });
          answer({ type: "msc-result", id: message.id, command: message.type });
        }
      }
      if (message.type === "msc-list") answer({ type: "msc-result", id: message.id,
        command: message.type, path: message.path, entries: listings[message.path] ?? [] });
      if (message.type === "msc-upload") {
        uploads.push(message);
        if (deferUpload) {
          pendingUpload = { message, worker: this };
          globalThis.releaseUpload = () => {
            const pending = pendingUpload;
            pendingUpload = null;
            pending.worker.finishUpload(pending.message);
          };
        } else this.finishUpload(message);
      }
      if (message.type === "msc-download") {
        const content = message.path === "/root.txt" ? new TextEncoder().encode(previewText) : null;
        const size = content?.length ?? (message.path === "/samples/oversize-stream.bin" ? 16 * 1024 * 1024 + 1 : 150000);
        downloads.set(message.id, { at: 0, size, content, name: message.name });
        answer({ type: "msc-progress", id: message.id, done: 0, total: size });
        if (deferDownload) {
          pendingDownload = { id: message.id, worker: this };
          globalThis.releaseDownload = () => {
            const pending = pendingDownload;
            pendingDownload = null;
            pending.worker.sendChunk(pending.id);
          };
        } else this.sendChunk(message.id);
      }
      if (message.type === "msc-data-done") {
        if (message.text) {
          downloads.delete(message.id);
          answer({ type: "error", command: "msc-download", id: message.id, text: message.text });
        } else this.sendChunk(message.id, true);
      }
    }
    finishUpload(message) {
      const parent = message.path.slice(0, message.path.lastIndexOf("/")) || "/";
      const entries = listings[parent] ??= [];
      const entry = { name: message.file.name, size: message.file.size, directory: false, ...stamp };
      const at = entries.findIndex((item) => item.name === entry.name);
      if (at < 0) entries.push(entry); else entries[at] = entry;
      this.onmessage({ data: { type: "msc-progress", id: message.id, done: message.file.size, total: message.file.size } });
      setTimeout(() => this.onmessage({ data: { type: "msc-result", id: message.id,
        command: message.type, path: message.path } }), 0);
    }
    sendChunk(id, acknowledged = false) {
      const download = downloads.get(id);
      if (!download) return;
      if (acknowledged) download.at += 65536;
      if (download.at >= download.size) {
        downloads.delete(id);
        setTimeout(() => this.onmessage({ data: { type: "msc-result", id, command: "msc-download",
          name: download.name, size: download.size } }), 0);
        return;
      }
      const count = Math.min(65536, download.size - download.at);
      const bytes = download.content ? download.content.slice(download.at, download.at + count) : new Uint8Array(count);
      if (!download.content) {
        for (let i = 0; i < count; ++i) bytes[i] = (download.at + i) & 255;
      }
      setTimeout(() => this.onmessage({ data: { type: "msc-data", id, bytes } }), 0);
    }
  };
}

const server = await serveDist(site);
const browser = await chromium.launch({ headless: true, ...(chrome ? { executablePath: chrome } : { channel: "chrome" }) });
const errors = [];
try {
  const context = await browser.newContext({ viewport: { width: 1280, height: 900 }, acceptDownloads: true });
  await context.addInitScript(standIn);
  const page = await context.newPage();
  await page.emulateMedia({ reducedMotion: "reduce" });
  page.on("pageerror", (error) => errors.push(error.message));
  await page.goto(server.url);
  const toggle = page.locator("#storage-toggle");
  const storageRow = (path) => page.locator(`#storage-tree [data-storage-path="${path}"]`);
  async function openRowMenu(path) {
    await storageRow(path).click({ button: "right" });
    const menu = page.getByRole("menu", { name: "Storage actions" });
    await menu.waitFor({ state: "visible" });
    return menu;
  }
  async function chooseUpload(path, files) {
    await openRowMenu(path);
    const chooser = page.waitForEvent("filechooser");
    await page.getByRole("menuitem", { name: "Upload", exact: true }).click();
    await (await chooser).setFiles(files);
  }
  async function dropFiles(path, files) {
    const transfer = await page.evaluateHandle((descriptors) => {
      const data = new DataTransfer();
      for (const file of descriptors)
        data.items.add(new File([Uint8Array.from(file.bytes)], file.name, { type: file.mimeType }));
      return data;
    }, files);
    const target = path === "/" ? page.locator("#storage-tree") : storageRow(path);
    await target.dispatchEvent("dragenter", { dataTransfer: transfer });
    await target.dispatchEvent("dragover", { dataTransfer: transfer });
    if (path !== "/")
      assert.equal(await target.evaluate((element) => element.classList.contains("storage-drop-target")), true);
    await target.dispatchEvent("drop", { dataTransfer: transfer });
    await transfer.dispose();
  }
  assert.equal(await toggle.getAttribute("aria-expanded"), "false", "the left drawer starts hidden");
  assert.equal(await page.locator("#storage").evaluate((element) => element.inert), true);

  await page.locator("#start").click();
  await page.locator("#panel-style").focus();
  const closedGeometry = await page.evaluate(() => ({
    device: document.getElementById("device").getBoundingClientRect().toJSON(),
    paddingLeft: getComputedStyle(document.body).paddingLeft,
  }));
  await page.evaluate(() => testWorker.onmessage({ data: { type: "usb-bus-state", active: true } }));
  await page.waitForFunction(() => document.getElementById("storage-status").textContent.includes("USB bus active"));
  assert.match(await page.locator("#storage-announcement").textContent(), /USB bus active/,
    "activation is announced outside the inert drawer");
  assert.equal(await toggle.getAttribute("aria-expanded"), "false", "bus activation leaves the drawer closed");
  assert.equal(await page.locator("#storage").evaluate((element) => element.inert), true);
  assert.equal(await page.locator("#storage").getAttribute("aria-hidden"), "true");
  assert.match(await toggle.textContent(), /USB ready/, "the closed toggle announces activation");
  assert.equal(await page.evaluate(() => document.activeElement.id), "panel-style", "activation does not steal focus");
  assert.equal(await page.evaluate(() => messages.some((message) => message.type === "msc-connect")), false,
    "bus activation never mounts storage automatically");

  await toggle.click();
  await page.waitForFunction(() => document.getElementById("storage").getBoundingClientRect().x === 0);
  const openedGeometry = await page.evaluate(() => ({
    device: document.getElementById("device").getBoundingClientRect().toJSON(),
    paddingLeft: getComputedStyle(document.body).paddingLeft,
  }));
  assert.deepEqual(openedGeometry, closedGeometry, "the overlay never moves the device or changes body padding");
  await toggle.click();
  assert.equal(await toggle.getAttribute("aria-expanded"), "false");
  assert.deepEqual(await page.evaluate(() => ({
    device: document.getElementById("device").getBoundingClientRect().toJSON(),
    paddingLeft: getComputedStyle(document.body).paddingLeft,
  })), closedGeometry, "closing the overlay also leaves the layout untouched");
  await page.evaluate(() => testWorker.onmessage({ data: { type: "usb-bus-state", active: true } }));
  await page.waitForTimeout(30);
  assert.equal(await toggle.getAttribute("aria-expanded"), "false", "a duplicate active state does not open the drawer");
  await page.evaluate(() => {
    testWorker.onmessage({ data: { type: "usb-bus-state", active: false } });
    testWorker.onmessage({ data: { type: "usb-bus-state", active: true } });
  });
  await page.waitForTimeout(30);
  assert.equal(await toggle.getAttribute("aria-expanded"), "false", "reactivation still does not open the drawer");
  await toggle.click();
  await page.waitForFunction(() => document.getElementById("storage").getBoundingClientRect().x === 0);

  await page.locator("#storage-connect").click();
  await page.waitForFunction(() => document.querySelectorAll("#storage-tree .storage-file").length === 2 &&
    document.getElementById("storage").getAttribute("aria-busy") === "false");
  assert.equal(await page.evaluate(() => document.activeElement.id), "storage-connect",
    "a successful connection keeps focus on the still-usable Connect button");
  assert.equal(await page.locator("#storage-tree img").count(), 0, "FAT names never become HTML");
  assert.equal(await page.locator("#storage-tree").textContent().then((s) => s.includes("<img src=x")), true);
  assert.deepEqual(await page.locator("#storage-tree .wb-header .wb-col-title").allTextContents(),
    ["Name", "Size", "Modified"], "Finder list shows name, size and modification time");
  assert.equal(await page.locator("#storage-tree .wb-row").evaluateAll((rows) =>
    rows.every((row) => row.querySelectorAll(":scope > .wb-col").length === 3)), true, "rows have three aligned columns");
  assert.equal(await storageRow("/root.txt").locator(".storage-modified").textContent(), "2026-11-03 12:34");
  assert.match(await storageRow("/root.txt").locator(".storage-size").textContent(), /^[0-9]+ B$/);
  assert.equal(await storageRow("/root.txt").evaluate((row) => {
    const icon = row.querySelector(".storage-document").getBoundingClientRect();
    const title = row.querySelector(".wb-title").getBoundingClientRect();
    return Math.abs((icon.y + icon.height / 2) - (title.y + title.height / 2)) <= 1;
  }), true, "file icons and labels share the same vertical center");
  assert.equal(await page.locator("#storage-upload, #storage-path").count(), 0, "the old Upload to toolbar is removed");
  assert.equal(await page.locator("#storage-pick").count(), 1, "menu uploads retain the hidden file input");
  assert.equal(await page.locator('#storage-status[role="status"], #storage-status[aria-live]').count(), 0,
    "the visible status is not a duplicate live region");
  assert.equal(await page.locator("#storage-tree .storage-download").count(), 0, "inline Download buttons are gone");

  await openRowMenu("/samples");
  assert.equal(await page.getByRole("menuitem", { name: "Upload", exact: true }).count(), 1);
  assert.equal(await page.getByRole("menuitem", { name: "Preview", exact: true }).count(), 0);
  assert.equal(await page.getByRole("menuitem", { name: "Download", exact: true }).count(), 0);
  await page.keyboard.press("Escape");
  assert.equal(await page.getByRole("menu", { name: "Storage actions" }).count(), 0);
  assert.equal(await storageRow("/samples").evaluate((row) => row.classList.contains("wb-active")), false,
    "opening a directory menu does not activate the row");

  await page.locator('.storage-directory[data-storage-path="/other"] .wb-title').click();
  await page.waitForFunction(() => document.getElementById("storage").getAttribute("aria-busy") === "false");
  await page.keyboard.press("Shift+F10");
  assert.equal(await page.getByRole("menu", { name: "Storage actions" }).count(), 1,
    "Shift+F10 opens the focused row's menu");
  assert.equal(await page.evaluate(() => document.activeElement.getAttribute("role")), "menuitem");
  await page.keyboard.press("Escape");
  await page.locator('.storage-directory[data-storage-path="/other"] .storage-row-actions').click({ force: true });
  assert.equal(await page.getByRole("menu", { name: "Storage actions" }).count(), 1,
    "the row overflow button exposes the same actions on touch");
  await page.keyboard.press("Escape");

  await toggle.click();
  await toggle.click();
  assert.equal(await page.locator("#storage-tree .storage-file").count(), 2, "hiding the drawer preserves the loaded tree");

  await page.locator('.storage-directory[data-storage-path="/many"] .wb-title').click();
  await page.waitForFunction(() => document.querySelector('[data-storage-path="/many/file-00.txt"]'));
  const scrolling = await page.locator("#storage-tree.wunderbaum").evaluate((element) => ({
    clientHeight: element.clientHeight, scrollHeight: element.scrollHeight, overflowY: getComputedStyle(element).overflowY,
    clientWidth: element.clientWidth, scrollWidth: element.scrollWidth, overflowX: getComputedStyle(element).overflowX,
  }));
  assert.ok(scrolling.scrollHeight > scrolling.clientHeight, "a long directory has internal overflow");
  assert.ok(["auto", "scroll"].includes(scrolling.overflowY), "the tree itself owns the vertical scrollbar");
  assert.equal(scrolling.overflowX, "hidden", "the Finder list never shows a horizontal scrollbar");
  assert.ok(scrolling.scrollWidth <= scrolling.clientWidth + 1,
    `all three columns fit without horizontal overflow: ${JSON.stringify(scrolling)}`);
  await page.locator("#storage-tree.wunderbaum").evaluate((element) => { element.scrollTop = element.scrollHeight; });
  await page.waitForFunction(() => document.querySelector('[data-storage-path="/many/file-79.txt"]'),
    null, { timeout: 5000 });

  await page.locator('.storage-directory[data-storage-path="/samples"] .wb-title').click();
  await page.waitForFunction(() => document.querySelector('[data-storage-path="/samples/demo.bin"] .wb-title')?.textContent === "demo.bin");
  assert.equal(await storageRow("/samples/demo.bin").locator(".storage-size").textContent(), "146.5 KiB");
  assert.equal(await storageRow("/samples/demo.bin").locator(".storage-modified").textContent(), "2026-11-03 12:34");
  const highlight = await storageRow("/samples").evaluate((row) => {
    const rowColor = getComputedStyle(row).backgroundColor;
    return {
      active: row.classList.contains("wb-active"),
      rowColor,
      columns: [...row.querySelectorAll(":scope > .wb-col")].map((column) => {
        const color = getComputedStyle(column).backgroundColor;
        return color === "rgba(0, 0, 0, 0)" || color === "transparent" ? rowColor : color;
      }),
    };
  });
  assert.ok(highlight.active, "the clicked directory is the active row");
  assert.ok(!["rgba(0, 0, 0, 0)", "transparent"].includes(highlight.rowColor),
    "the active row has a visible highlight");
  assert.equal(new Set(highlight.columns).size, 1, "the active row uses one highlight across all three columns");

  await openRowMenu("/root.txt");
  assert.equal(await page.getByRole("menuitem", { name: "Preview", exact: true }).count(), 1);
  assert.equal(await page.getByRole("menuitem", { name: "Download", exact: true }).count(), 1);
  assert.equal(await page.getByRole("menuitem", { name: "Upload", exact: true }).count(), 0);
  await page.getByRole("menuitem", { name: "Preview", exact: true }).click();
  const preview = page.getByRole("dialog", { name: "root.txt" });
  await preview.waitFor({ state: "visible" });
  assert.equal(await preview.locator("pre").textContent(), await page.evaluate(() => previewText));
  assert.equal(await preview.locator("b").count(), 0, "preview text is never interpreted as markup");
  await page.getByRole("button", { name: "Close preview" }).click();
  await preview.waitFor({ state: "hidden" });
  assert.equal(await page.evaluate(() => document.getElementById("storage-tree").contains(document.activeElement)), true,
    "closing preview restores focus to the tree");
  assert.equal(await storageRow("/samples").evaluate((row) => row.classList.contains("wb-active")), true,
    "file preview keeps the selected directory");

  await page.evaluate(() => { deferDownload = true; });
  const treeBeforeProgress = await page.locator("#storage-tree").evaluate((element) => element.getBoundingClientRect().toJSON());
  await openRowMenu("/root.txt");
  await page.getByRole("menuitem", { name: "Preview", exact: true }).click();
  await page.waitForFunction(() => globalThis.pendingDownload);
  await page.locator("#storage-progress").waitFor({ state: "visible" });
  const progressLayout = await page.locator("#storage-progress").evaluate((element) => {
    const rect = element.getBoundingClientRect();
    const drawer = document.getElementById("storage");
    const drawerRect = drawer.getBoundingClientRect();
    const statusRect = document.getElementById("storage-status").getBoundingClientRect();
    const style = getComputedStyle(element);
    const drawerColor = getComputedStyle(drawer).backgroundColor;
    const luminance = (color) => {
      const channels = color.match(/[\d.]+/g).slice(0, 3).map(Number).map((value) => {
        const srgb = value / 255;
        return srgb <= 0.04045 ? srgb / 12.92 : ((srgb + 0.055) / 1.055) ** 2.4;
      });
      return channels[0] * 0.2126 + channels[1] * 0.7152 + channels[2] * 0.0722;
    };
    const colors = [luminance(style.borderTopColor), luminance(drawerColor)].sort((a, b) => b - a);
    return {
      height: rect.height,
      progressTop: rect.top,
      statusBottom: statusRect.bottom,
      withinDrawer: rect.top >= drawerRect.top && rect.bottom <= drawerRect.bottom,
      directlyBelowStatus: Math.abs(rect.top - statusRect.bottom) <= 1,
      trackColor: getComputedStyle(element, "::-webkit-progress-bar").backgroundColor,
      drawerColor,
      borderWidth: parseFloat(style.borderTopWidth),
      borderContrast: (colors[0] + 0.05) / (colors[1] + 0.05),
    };
  });
  assert.ok(progressLayout.height >= 6, `the overlaid progress track is easy to see: ${JSON.stringify(progressLayout)}`);
  assert.ok(progressLayout.withinDrawer, "the progress track is not clipped by the drawer");
  assert.ok(progressLayout.directlyBelowStatus,
    `progress stays directly below the operation status text: ${JSON.stringify(progressLayout)}`);
  assert.ok(!["rgba(0, 0, 0, 0)", "transparent"].includes(progressLayout.trackColor),
    "an empty progress track is still visible before the first chunk arrives");
  assert.notEqual(progressLayout.trackColor, progressLayout.drawerColor,
    "the progress track contrasts with the drawer background");
  assert.ok(progressLayout.borderWidth >= 1 && progressLayout.borderContrast >= 3,
    `the progress boundary has at least 3:1 contrast in the active theme: ${JSON.stringify(progressLayout)}`);
  if (process.env.OP1_STORAGE_PROGRESS_CAPTURE)
    await page.screenshot({ path: process.env.OP1_STORAGE_PROGRESS_CAPTURE });
  assert.deepEqual(await page.locator("#storage-tree").evaluate((element) => element.getBoundingClientRect().toJSON()),
    treeBeforeProgress, "preview progress overlays the drawer without moving or resizing the tree");
  await toggle.click();
  await page.evaluate(() => { deferDownload = false; releaseDownload(); });
  await page.waitForFunction(() => document.getElementById("storage").getAttribute("aria-busy") === "false");
  assert.equal(await page.locator("#storage-preview").evaluate((dialog) => dialog.open), false,
    "a pending preview cannot reopen after the drawer closes");
  assert.equal(await page.locator("#storage-status").textContent(), "Preview cancelled",
    "cancelled previews do not leave a stale loading status");
  await toggle.click();

  for (const [name, selector] of [["pixel.png", "img"], ["tone.wav", "audio"]]) {
    const pausesBefore = await page.evaluate(() => audioPauses);
    await openRowMenu(`/samples/${name}`);
    await page.getByRole("menuitem", { name: "Preview", exact: true }).click();
    const mediaPreview = page.getByRole("dialog", { name });
    await mediaPreview.waitFor({ state: "visible" });
    const url = await mediaPreview.locator(selector).getAttribute("src");
    assert.ok(await page.evaluate((value) => createdObjectUrls.includes(value), url));
    await page.getByRole("button", { name: "Close preview" }).click();
    await page.waitForFunction((value) => revokedObjectUrls.includes(value), url);
    if (selector === "audio")
      assert.ok(await page.evaluate((before) => audioPauses > before, pausesBefore), "closing audio stops playback");
  }

  await openRowMenu("/samples/demo.bin");
  await page.getByRole("menuitem", { name: "Preview", exact: true }).click();
  const hexPreview = page.getByRole("dialog", { name: "demo.bin" });
  await hexPreview.waitFor({ state: "visible" });
  assert.equal(await hexPreview.locator(".storage-hex").textContent().then((text) => text.split("\n").length), 4096,
    "hex preview contains exactly the first 64 KiB");
  assert.match(await hexPreview.locator(".storage-preview-note").textContent(), /first 64 KiB/);
  await page.getByRole("button", { name: "Close preview" }).click();

  if (process.env.OP1_STORAGE_CAPTURE) {
    await openRowMenu("/samples/demo.bin");
    await page.screenshot({ path: process.env.OP1_STORAGE_CAPTURE });
    await page.keyboard.press("Escape");
  }

  // Bus loss invalidates the mount, not just the Connect button. A later bus
  // activation requires an explicit fresh enumeration before file access.
  await openRowMenu("/samples/demo.bin");
  await page.evaluate(() => testWorker.onmessage({ data: { type: "usb-bus-state", active: false } }));
  await page.waitForFunction(() => document.querySelectorAll("#storage-tree .storage-file").length === 0);
  assert.equal(await page.evaluate(() => document.activeElement.id), "storage-toggle",
    "bus loss moves focus out of the dismissed menu");
  assert.equal(await page.locator("#storage-connect").textContent(), "Connect USB");
  assert.equal(await page.locator("#storage-connect").isDisabled(), true);
  assert.equal(await page.locator("#storage-refresh").isDisabled(), true);
  assert.equal(await page.locator("#storage-pick").isDisabled(), true);
  await page.waitForFunction(() => document.querySelectorAll("#storage-tree .wb-node-list .wb-row.wb-active").length === 0,
    null, { timeout: 5000 });
  await page.evaluate(() => testWorker.onmessage({ data: { type: "usb-bus-state", active: true } }));
  await page.locator("#storage-connect").click();
  await page.waitForFunction(() => document.querySelectorAll("#storage-tree .storage-file").length === 2 &&
    document.getElementById("storage").getAttribute("aria-busy") === "false");
  assert.equal(await page.evaluate(() => document.activeElement.id), "storage-connect",
    "a successful connection keeps focus on the still-usable Connect button");
  await page.locator('.storage-directory[data-storage-path="/samples"] .wb-title').click();
  await page.waitForFunction(() => document.querySelector('[data-storage-path="/samples/demo.bin"]'));

  const listCount = await page.evaluate(() => messages.filter((m) => m.type === "msc-list").length);
  await page.locator("#storage-refresh").click();
  await page.waitForFunction((count) => messages.filter((m) => m.type === "msc-list").length === count + 1 &&
    document.getElementById("storage").getAttribute("aria-busy") === "false", listCount);
  assert.equal(await page.evaluate(() => messages.filter((m) => m.type === "msc-list").at(-1).path), "/samples",
    "Refresh lists the selected directory instead of the operation token");

  const payload = new Uint8Array(150000);
  for (let i = 0; i < payload.length; ++i) payload[i] = i & 255;
  const downloadEvent = page.waitForEvent("download");
  await openRowMenu("/samples/demo.bin");
  await page.getByRole("menuitem", { name: "Download", exact: true }).click();
  const downloaded = await downloadEvent;
  assert.equal(downloaded.suggestedFilename(), "demo.bin");
  const bytes = fs.readFileSync(await downloaded.path());
  assert.deepEqual(new Uint8Array(bytes), payload, "Blob fallback preserves every chunk");

  const upload = Buffer.from(Array.from({ length: 150000 }, (_, i) => (i * 3) & 255));
  await chooseUpload("/samples", { name: "new file.op1", mimeType: "application/octet-stream", buffer: upload });
  await page.waitForFunction(() => globalThis.uploads.length === 1 &&
    document.getElementById("storage").getAttribute("aria-busy") === "false");
  await storageRow("/samples/new file.op1").waitFor({ state: "visible" });
  assert.equal(await page.locator("#storage-status").textContent(), "Upload complete",
    "refreshing an expanded directory after upload succeeds");
  for (const name of ["demo.bin", "pixel.png", "tone.wav", "huge.bin", "oversize-stream.bin", "new file.op1"])
    assert.equal(await storageRow(`/samples/${name}`).count(), 1, `${name} occurs exactly once after refresh`);
  assert.equal(await storageRow("/samples/new file.op1").isVisible(), true,
    "the uploaded file is visible in the refreshed directory");
  assert.equal(await storageRow("/samples").evaluate((row) => row.classList.contains("wb-expanded")), true,
    "refresh preserves directory expansion");
  const first = await page.evaluate(async () => {
    const item = uploads[0];
    return { path: item.path, overwrite: item.overwrite, size: item.file.size,
      bytes: [...new Uint8Array(await item.file.arrayBuffer()).slice(0, 8)] };
  });
  assert.deepEqual(first, { path: "/samples/new file.op1", overwrite: false, size: 150000,
    bytes: [...upload.subarray(0, 8)] });

  page.once("dialog", (dialog) => dialog.accept());
  await chooseUpload("/samples", { name: "new file.op1", mimeType: "application/octet-stream", buffer: upload });
  await page.waitForFunction(() => globalThis.uploads.length === 2);
  assert.equal(await page.evaluate(() => uploads[1].overwrite), true);
  assert.equal(await page.evaluate(() => messages.some((m) => m.type === "import")), false,
    "storage uploads never enter NAND/OTP import");

  // A deferred first file must not let a directory click split the batch.
  await page.evaluate(() => { deferUpload = true; });
  await chooseUpload("/samples", [
    { name: "batch-1.bin", mimeType: "application/octet-stream", buffer: Buffer.from([1]) },
    { name: "batch-2.bin", mimeType: "application/octet-stream", buffer: Buffer.from([2]) },
  ]);
  await page.waitForFunction(() => globalThis.pendingUpload);
  await page.locator('.storage-directory[data-storage-path="/other"] .wb-title').dispatchEvent("click");
  assert.equal(await storageRow("/samples").evaluate((row) => row.classList.contains("wb-active")), true,
    "busy directories cannot change the selected destination");
  await page.evaluate(() => { deferUpload = false; releaseUpload(); });
  await page.waitForFunction(() => globalThis.uploads.length === 4);
  assert.deepEqual(await page.evaluate(() => uploads.slice(2).map((m) => m.path)),
    ["/samples/batch-1.bin", "/samples/batch-2.bin"]);
  await page.waitForFunction(() => document.getElementById("storage").getAttribute("aria-busy") === "false");

  await dropFiles("/other", [{ name: "drag folder.bin", mimeType: "application/octet-stream", bytes: [9, 8, 7] }]);
  await page.waitForFunction(() => globalThis.uploads.length === 5);
  await page.waitForFunction(() => document.getElementById("storage").getAttribute("aria-busy") === "false");
  assert.equal(await storageRow("/other").evaluate((element) => element.classList.contains("storage-drop-target")), false,
    "drop highlighting is cleaned up");
  assert.deepEqual(await page.evaluate(async () => ({
    path: uploads[4].path,
    bytes: [...new Uint8Array(await uploads[4].file.arrayBuffer())],
  })), { path: "/other/drag folder.bin", bytes: [9, 8, 7] }, "a folder drop overrides the selected destination");

  await dropFiles("/", [{ name: "drag root.bin", mimeType: "application/octet-stream", bytes: [6, 5] }]);
  await page.waitForFunction(() => globalThis.uploads.length === 6);
  await page.waitForFunction(() => document.getElementById("storage").getAttribute("aria-busy") === "false");
  assert.equal(await page.evaluate(() => uploads[5].path), "/drag root.bin", "tree whitespace drops target the root");
  assert.equal(await page.evaluate(() => messages.filter((m) => m.type === "msc-upload").length), 6,
    "each drop is uploaded exactly once");

  await page.locator("#storage-tree").dispatchEvent("contextmenu", { clientX: 220, clientY: 700, button: 2 });
  const blankChooser = page.waitForEvent("filechooser");
  await page.getByRole("menuitem", { name: "Upload", exact: true }).click();
  await (await blankChooser).setFiles({ name: "blank-upload.txt", mimeType: "text/plain", buffer: Buffer.from("blank") });
  await page.waitForFunction(() => globalThis.uploads.length === 7);
  await page.waitForFunction(() => document.getElementById("storage").getAttribute("aria-busy") === "false");
  assert.equal(await page.evaluate(() => uploads[6].path), "/blank-upload.txt", "blank-tree context menu uploads to the root");
  assert.equal(await page.evaluate(() => messages.some((m) => m.type === "import")), false,
    "dropped storage files never reach NAND/OTP import");

  // Focus must leave any drawer control that bus loss is about to disable.
  await page.locator("#storage-refresh").focus();
  await page.evaluate(() => testWorker.onmessage({ data: { type: "usb-bus-state", active: false } }));
  assert.equal(await page.evaluate(() => document.activeElement.id), "storage-toggle",
    "bus loss moves focus from drawer controls to the surviving toggle");
  await page.evaluate(() => testWorker.onmessage({ data: { type: "usb-bus-state", active: true } }));
  await page.locator("#storage-connect").click();
  await page.waitForFunction(() => document.querySelectorAll("#storage-tree .storage-file").length === 4);
  await page.locator('.storage-directory[data-storage-path="/samples"] .wb-title').click();
  await page.waitForFunction(() => document.querySelector('[data-storage-path="/samples/demo.bin"]'));

  // A bus loss also cancels an in-flight batch. Releasing its late Worker
  // response cannot resurrect the old destination or mounted state.
  await page.evaluate(() => { deferUpload = true; });
  await chooseUpload("/samples", {
    name: "interrupted.bin", mimeType: "application/octet-stream", buffer: Buffer.from([3]),
  });
  await page.waitForFunction(() => globalThis.pendingUpload);
  await page.evaluate(() => testWorker.onmessage({ data: { type: "usb-bus-state", active: false } }));
  await page.waitForFunction(() => document.getElementById("storage").getAttribute("aria-busy") === "false");
  assert.equal(await page.locator("#storage-connect").textContent(), "Connect USB");
  await page.evaluate(() => { deferUpload = false; releaseUpload(); });
  await page.waitForFunction(() => globalThis.uploads.length === 8);
  await page.evaluate(() => testWorker.onmessage({ data: { type: "usb-bus-state", active: true } }));
  await page.locator("#storage-connect").click();
  await page.waitForFunction(() => document.querySelectorAll("#storage-tree .storage-file").length === 4);
  await page.locator('.storage-directory[data-storage-path="/samples"] .wb-title').click();
  await page.waitForFunction(() => document.querySelector('[data-storage-path="/samples/huge.bin"]'));

  const beforeHuge = await page.evaluate(() => messages.filter((m) => m.type === "msc-download").length);
  await openRowMenu("/samples/huge.bin");
  await page.getByRole("menuitem", { name: "Preview", exact: true }).click();
  assert.match(await page.locator("#storage-status").textContent(), /Preview is limited.*16 MiB/);
  assert.equal(await page.evaluate(() => messages.filter((m) => m.type === "msc-download").length), beforeHuge,
    "oversized preview is rejected before USB I/O");
  await openRowMenu("/samples/huge.bin");
  await page.getByRole("menuitem", { name: "Download", exact: true }).click();
  assert.match(await page.locator("#storage-status").textContent(), /over 128 MiB/);
  assert.equal(await page.evaluate(() => messages.filter((m) => m.type === "msc-download").length), beforeHuge,
    "oversized Blob fallback is rejected before USB I/O");

  await openRowMenu("/samples/oversize-stream.bin");
  await page.getByRole("menuitem", { name: "Preview", exact: true }).click();
  await page.waitForFunction(() => document.getElementById("storage-status").textContent.includes("preview exceeds the 16 MiB limit"),
    null, { timeout: 30000 });
  await page.waitForFunction(() => document.getElementById("storage").getAttribute("aria-busy") === "false");
  assert.equal(await page.getByRole("dialog").count(), 0, "an over-cap stream never opens a preview");

  await page.evaluate(() => {
    saved = [];
    globalThis.savedClosed = false;
    globalThis.showSaveFilePicker = async () => ({ createWritable: async () => ({
      write: async (bytes) => saved.push(bytes),
      close: async () => { savedClosed = true; },
      abort: async () => {},
    }) });
  });
  await openRowMenu("/samples/demo.bin");
  await page.getByRole("menuitem", { name: "Download", exact: true }).click();
  await page.waitForFunction(() => globalThis.savedClosed);
  const streamed = await page.evaluate(() => {
    const bytes = new Uint8Array(saved.reduce((n, part) => n + part.length, 0));
    let at = 0;
    for (const part of saved) { bytes.set(part, at); at += part.length; }
    return [...bytes];
  });
  assert.deepEqual(new Uint8Array(streamed), payload, "File System Access stream preserves every chunk");

  // Busy is set before picker acquisition. A detach while createWritable is
  // pending must abort the acquired stream and never submit the USB download.
  await page.evaluate(() => {
    globalThis.savedClosed = false;
    globalThis.savedAborted = false;
    globalThis.showSaveFilePicker = async () => ({ createWritable: () => new Promise((resolve) => {
      globalThis.resolveWritable = () => resolve({
        write: async () => {},
        close: async () => { savedClosed = true; },
        abort: async () => { savedAborted = true; },
      });
    }) });
  });
  const beforeCancelled = await page.evaluate(() => messages.filter((m) => m.type === "msc-download").length);
  await openRowMenu("/samples/demo.bin");
  await page.getByRole("menuitem", { name: "Download", exact: true }).click();
  await page.waitForFunction(() => globalThis.resolveWritable);
  assert.equal(await page.locator("#storage").getAttribute("aria-busy"), "true");
  await page.evaluate(() => {
    testWorker.onmessage({ data: { type: "msc-state", connected: false } });
    resolveWritable();
  });
  await page.waitForFunction(() => globalThis.savedAborted);
  assert.equal(await page.evaluate(() => messages.filter((m) => m.type === "msc-download").length), beforeCancelled);
  assert.equal(await page.evaluate(() => globalThis.savedClosed), false);
  await page.evaluate(() => testWorker.onmessage({ data: { type: "msc-state", connected: true } }));

  await page.evaluate(() => { deferDisconnect = true; });
  await page.locator("#storage-connect").click();  // disconnect
  await page.waitForFunction(() => globalThis.releaseDisconnect);
  await page.evaluate(() => {
    // Force the CI-observed timing: the state acknowledgement arrives while
    // the requested Disconnect button is still the active element.
    const button = document.getElementById("storage-connect");
    button.disabled = false;
    button.focus();
    releaseDisconnect();
  });
  await page.waitForFunction(() => document.getElementById("storage-connect").textContent === "Connect USB" &&
    document.getElementById("storage").getAttribute("aria-busy") === "false");
  assert.equal(await page.evaluate(() => document.activeElement.id), "storage-connect",
    "a normal disconnect also preserves focus on the Connect button");
  await page.evaluate(() => { failConnect = true; });
  await page.locator("#storage-connect").click();
  await page.waitForFunction(() => document.getElementById("storage-status").classList.contains("error"));
  assert.match(await page.locator("#storage-status").textContent(), /no mass storage/);

  await page.setViewportSize({ width: 390, height: 844 });
  await page.waitForFunction(() => document.documentElement.scrollWidth <= innerWidth);
  assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true,
    "no horizontal overflow at phone width");
  const phoneGeometry = await page.locator("#device").evaluate((element) => element.getBoundingClientRect().toJSON());
  await toggle.click();
  assert.deepEqual(await page.locator("#device").evaluate((element) => element.getBoundingClientRect().toJSON()), phoneGeometry,
    "closing the phone overlay does not move the device");
  await toggle.click();
  assert.deepEqual(await page.locator("#device").evaluate((element) => element.getBoundingClientRect().toJSON()), phoneGeometry,
    "opening the phone overlay does not move the device");
  assert.equal(errors.length, 0, errors.join("\n"));
  console.log("storage-browser: tree, escaping, upload/overwrite, chunked downloads, streams, errors and mobile: ok");
} finally {
  await browser.close();
  server.close();
}
