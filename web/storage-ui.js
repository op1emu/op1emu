// Page-side USB storage drawer. Worker requests and transfers are serialized
// here; storage-tree.js only adapts directory data to the tree component.
import { createStorageTree } from "./storage-tree.js";
import { createStorageMenu } from "./storage-menu.js";
import { createStoragePreview } from "./storage-preview.js";

const join = (path, name) => path === "/" ? `/${name}` : `${path}/${name}`;
const MAX_BLOB_BYTES = 128 * 1024 * 1024;
const MAX_PREVIEW_BYTES = 16 * 1024 * 1024;

export function createStorageUI(worker, isRunning) {
  const $ = (id) => document.getElementById(id);
  const aside = $("storage"), status = $("storage-status"), toggle = $("storage-toggle");
  const announcement = $("storage-announcement");
  const pending = new Map(), listings = new Map();
  let nextId = 1, operationSerial = 0, busy = null, connected = false, busActive = false;
  let currentPath = "/", open = false, uploadDestination = null, dropRow = null;

  function say(text, failed = false) {
    status.textContent = text;
    announcement.textContent = text;
    status.classList.toggle("error", failed);
  }

  function updateToggle() {
    toggle.setAttribute("aria-expanded", String(open));
    toggle.setAttribute("aria-label", `${open ? "Hide" : "Show"} USB storage${busActive ? " (bus active)" : ""}`);
    toggle.classList.toggle("bus-active", busActive);
    $("storage-toggle-label").textContent = busActive && !open ? "USB ready" : "USB storage";
  }

  function setOpen(value) {
    open = value;
    document.body.classList.toggle("storage-open", open);
    aside.inert = !open;
    aside.setAttribute("aria-hidden", String(!open));
    updateToggle();
    if (!open) {
      menu.close(false);
      preview.close();
      clearDropTarget();
    }
  }

  function updateBus() {
    $("storage-bus-state").textContent = busActive ? "USB bus active" : "Bus inactive";
    $("storage-bus-state").classList.toggle("active", busActive);
    updateToggle();
  }

  const fileTree = createStorageTree({
    element: $("storage-tree"),
    isInteractive: () => connected && !busy,
    onDirectory: (path) => {
      currentPath = path;
    },
    onContext: (target, position) => menu.open(target, position),
    onLazyLoad: (path) => {
      if (!connected || busy) throw new DOMException("Storage is not available", "AbortError");
      return operation("Reading directory…", () => load(path, true), true);
    },
    onError: (error) => {
      if (error?.name !== "AbortError") say(error?.message ?? String(error), true);
    },
  });

  const menu = createStorageMenu({ element: $("storage-menu"), onAction: handleMenuAction });
  const preview = createStoragePreview({
    dialog: $("storage-preview"), title: $("storage-preview-title"),
    content: $("storage-preview-content"), closeButton: $("storage-preview-close"),
  });

  function handleMenuAction(action, target) {
    if (!connected || busy) return;
    if (action === "upload") requestUpload(target.path);
    else if (action === "download") download(target.path, target.name, target.size);
    else if (action === "preview") previewFile(target.path, target.name, target.size);
  }

  function update() {
    const active = isRunning();
    $("storage-connect").textContent = connected ? "Disconnect USB" : "Connect USB";
    $("storage-connect").disabled = !active || !busActive || Boolean(busy);
    $("storage-refresh").disabled = !connected || Boolean(busy);
    $("storage-pick").disabled = !connected || Boolean(busy);
    aside.setAttribute("aria-busy", busy ? "true" : "false");
    fileTree.setEnabled(connected && !busy);
    if (!connected || busy) {
      menu.close(false);
      clearDropTarget();
    }
  }

  function rejectAll(text) {
    ++operationSerial;
    for (const request of pending.values()) request.reject(new Error(text));
    pending.clear();
    busy = null;
    $("storage-progress").hidden = true;
    update();
  }

  function clearFiles(moveFocus = true) {
    const restoreFocus = moveFocus && (aside.contains(document.activeElement) ||
      $("storage-menu").contains(document.activeElement) ||
      $("storage-preview").contains(document.activeElement));
    listings.clear();
    currentPath = "/";
    uploadDestination = null;
    menu.close(false);
    preview.close();
    clearDropTarget();
    void fileTree.clear();
    if (restoreFocus) toggle.focus();
  }

  function reset(text = "The emulator stopped") {
    connected = false;
    busActive = false;
    clearFiles();
    updateBus();
    rejectAll(text);
    update();
  }

  function send(type, data = {}, onData = null) {
    const id = nextId++;
    const promise = new Promise((resolve, reject) => pending.set(id, { resolve, reject, onData, type }));
    worker.postMessage({ type, id, ...data });
    return promise;
  }

  async function operation(command, work, rethrow = false) {
    if (busy) return;
    const token = busy = ++operationSerial;
    say(command);
    update();
    try {
      return await work(token);
    } catch (e) {
      if (e.name !== "AbortError") say(e.message ?? String(e), true);
      if (rethrow) throw e;
    } finally {
      if (busy === token) {
        busy = null;
        $("storage-progress").hidden = true;
        update();
      }
    }
  }

  async function load(path, force = false) {
    if (!force && listings.has(path)) return listings.get(path);
    const answer = await send("msc-list", { path });
    listings.set(path, answer.entries);
    return answer.entries;
  }

  async function refresh(path = currentPath) {
    const entries = await load(path, true);
    await fileTree.refresh(path, entries);
    say(`Showing ${path}`);
  }

  async function connect() {
    const restoreFocus = document.activeElement === $("storage-connect");
    await operation(connected ? "Disconnecting…" : "Connecting USB…", async () => {
      if (connected) {
        await send("msc-disconnect");
        connected = false;
        clearFiles(false);
        say("USB storage disconnected. Choose Upload firmware in te-boot before reconnecting.");
      } else {
        await send("msc-connect");
        connected = true;
        clearFiles(false);
        const entries = await load("/", true);
        await fileTree.loadRoot(entries);
        say("USB storage connected. Select a directory for uploads.");
      }
      update();
    });
    const focused = document.activeElement;
    if (open && restoreFocus && busActive && isRunning() && !busy &&
        (focused === document.body || focused === document.documentElement))
      $("storage-connect").focus();
  }

  function requestUpload(destination) {
    if (!connected || busy) return;
    uploadDestination = destination;
    $("storage-pick").click();
  }

  async function uploadFiles(files, destination = currentPath) {
    if (!connected || busy || !files.length) return;
    operation("Uploading…", async () => {
      if (!listings.has(destination)) await refresh(destination);
      const existing = new Set((listings.get(destination) ?? []).map((entry) => entry.name));
      for (const file of files) {
        let overwrite = false;
        if (existing.has(file.name)) {
          if (!confirm(`Replace ${file.name} in ${destination}? The existing file will be replaced after the upload completes.`))
            continue;
          overwrite = true;
        }
        say(`Uploading ${file.name}…`);
        await send("msc-upload", { path: join(destination, file.name), file, overwrite });
        existing.add(file.name);
      }
      await refresh(destination);
      say("Upload complete");
    });
  }

  function previewFile(path, name, size) {
    if (busy || !connected) return;
    if (size > MAX_PREVIEW_BYTES) {
      say("Preview is limited to files of 16 MiB or less. Download the file instead.", true);
      return;
    }
    operation(`Loading preview of ${name}…`, async (token) => {
      const parts = [];
      let received = 0;
      await send("msc-download", { path, name }, (bytes) => {
        received += bytes.length;
        if (received > MAX_PREVIEW_BYTES) throw new Error("preview exceeds the 16 MiB limit");
        parts.push(bytes);
      });
      const blob = new Blob(parts);
      if (blob.size > MAX_PREVIEW_BYTES) throw new Error("preview exceeds the 16 MiB limit");
      try {
        await preview.show({ name, blob }, () => open && connected && isRunning() && busy === token);
      } catch (e) {
        if (e.name === "AbortError" && !open && busy === token) {
          say("Preview cancelled");
          return;
        }
        throw e;
      }
      say(`Previewing ${name}`);
    });
  }

  function download(path, name, size) {
    if (busy || !connected) return;
    if (!globalThis.showSaveFilePicker && size > MAX_BLOB_BYTES) {
      say("This download is over 128 MiB. Use a browser with File System Access save streams instead of the in-memory fallback.", true);
      return;
    }
    operation(`Downloading ${name}…`, async (token) => {
      let writable = null, buffered = 0;
      try {
        // Still directly within the click's user activation, but now busy is
        // established before either await and every exit closes the stream.
        if (globalThis.showSaveFilePicker) {
          const handle = await showSaveFilePicker({ suggestedName: name });
          writable = await handle.createWritable();
        }
        if (!connected || !isRunning() || busy !== token) throw new DOMException("Download cancelled", "AbortError");
        const parts = [];
        const onData = async (bytes) => {
          if (writable) {
            await writable.write(bytes);
          } else {
            buffered += bytes.length;
            if (buffered > MAX_BLOB_BYTES) throw new Error("download exceeds the 128 MiB Blob fallback limit");
            parts.push(bytes);
          }
        };
        await send("msc-download", { path, name }, onData);
        if (writable) {
          await writable.close();
          writable = null;
        } else {
          const url = URL.createObjectURL(new Blob(parts));
          const link = document.createElement("a");
          link.href = url;
          link.download = name;
          link.click();
          setTimeout(() => URL.revokeObjectURL(url), 60000);
        }
        say(`Downloaded ${name}`);
      } catch (e) {
        if (writable) {
          await writable.abort().catch(() => {});
          writable = null;
        }
        throw e;
      }
    });
  }

  function handle(data) {
    if (data.type === "usb-bus-state") {
      const active = Boolean(data.active), rising = active && !busActive;
      const falling = !active && busActive;
      busActive = active;
      if (falling) {
        // The old enumeration and FAT mount belong to the previous bus session.
        // Cancel transfers and require an explicit connect after reactivation.
        connected = false;
        clearFiles();
        rejectAll("USB bus inactive. Reconnect after choosing Upload firmware again.");
      }
      updateBus();
      if (rising) {
        say("USB bus active. Open USB storage and connect to browse files.");
      } else if (!active && !connected && !busy) {
        say("USB bus inactive. In te-boot, choose Upload firmware on the OP-1.");
      }
      update();
      return true;
    }
    if (data.type === "msc-state") {
      // A requested disconnect reports its state before its command result.
      // That acknowledgement must not steal focus from connect()'s own restore.
      const expectedDisconnect = !data.connected &&
        [...pending.values()].some((request) => request.type === "msc-disconnect");
      connected = data.connected;
      if (!connected) clearFiles(!expectedDisconnect);
      update();
      return true;
    }
    if (data.type === "msc-progress") {
      if (busy) {
        const progress = $("storage-progress");
        progress.hidden = false;
        progress.max = Math.max(1, data.total);
        progress.value = data.done;
      }
      return true;
    }
    if (data.type === "msc-data") {
      const request = pending.get(data.id);
      if (!request?.onData) return true;
      Promise.resolve().then(() => request.onData(data.bytes)).then(
        () => worker.postMessage({ type: "msc-data-done", id: data.id }),
        (e) => worker.postMessage({ type: "msc-data-done", id: data.id, text: e.message ?? String(e) }));
      return true;
    }
    if (data.type === "msc-result") {
      const request = pending.get(data.id);
      if (request) {
        pending.delete(data.id);
        request.resolve(data);
      }
      return true;
    }
    if (data.type === "error" && data.command?.startsWith("msc-")) {
      const request = pending.get(data.id);
      if (request) {
        pending.delete(data.id);
        request.reject(new Error(data.text));
      } else say(data.text, true);
      return true;
    }
    return false;
  }

  toggle.addEventListener("click", () => setOpen(!open));
  aside.addEventListener("keydown", (event) => {
    if (event.key !== "Escape") return;
    event.preventDefault();
    event.stopPropagation();
    setOpen(false);
    toggle.focus();
  });
  $("storage-connect").addEventListener("click", connect);
  $("storage-refresh").addEventListener("click", () => operation("Refreshing…", refresh));
  $("storage-pick").addEventListener("change", (event) => {
    const destination = uploadDestination ?? currentPath;
    uploadDestination = null;
    uploadFiles([...event.target.files], destination);
    event.target.value = "";
  });
  $("storage-pick").addEventListener("cancel", () => { uploadDestination = null; });

  function clearDropTarget() {
    dropRow?.classList.remove("storage-drop-target");
    dropRow = null;
    delete $("storage-tree").dataset.dropPath;
  }
  function dropDestination(target) {
    const row = target.closest?.("[data-storage-path]");
    if (row && aside.contains(row))
      return row.classList.contains("storage-directory") ? row.dataset.storagePath : null;
    if ($("storage-tree").contains(target)) return "/";
    return currentPath;
  }
  function fileDrag(event) {
    return event.dataTransfer?.types?.includes("Files") ?? false;
  }
  for (const name of ["dragenter", "dragover"]) aside.addEventListener(name, (event) => {
    if (!fileDrag(event)) return;
    event.preventDefault();
    event.stopPropagation();
    const row = event.target.closest?.(".storage-directory[data-storage-path]");
    const accepted = connected && !busy && dropDestination(event.target) !== null;
    event.dataTransfer.dropEffect = accepted ? "copy" : "none";
    if (dropRow !== row) {
      clearDropTarget();
      if (accepted && row) {
        dropRow = row;
        $("storage-tree").dataset.dropPath = row.dataset.storagePath;
        dropRow.classList.add("storage-drop-target");
      }
    }
  }, true);
  aside.addEventListener("drop", (event) => {
    if (!fileDrag(event)) return;
    event.preventDefault();
    event.stopPropagation();
    const destination = dropDestination(event.target);
    const files = [...event.dataTransfer.files];
    clearDropTarget();
    if (connected && !busy && destination !== null) uploadFiles(files, destination);
  }, true);
  aside.addEventListener("dragleave", (event) => {
    if (!aside.contains(event.relatedTarget)) clearDropTarget();
  });
  say("In te-boot, choose Upload firmware on the OP-1, then connect USB here.");
  setOpen(false);
  updateBus();
  update();
  return { handle, reset, update };
}
