// USB Mass Storage commands for the emulator Worker. The C++ side owns FAT
// and USB; this module only moves bounded file chunks and reports progress.
const CHUNK = 64 * 1024;
const MAX_FAT_FILE = 0xffffffff;

export function createStorageWorker(op1, post) {
  const readAcks = new Map();
  let busActive = false;
  const yieldToEmulator = () => new Promise((resolve) => setTimeout(resolve, 0));
  const error = () => {
    const pointer = op1._op1_msc_error();
    const text = pointer ? op1.UTF8ToString(pointer) : "USB storage operation failed";
    return new Error(text);
  };
  const requireOk = (result) => {
    if (result < 0) throw error();
  };
  const withPath = (path, call) => {
    if (typeof path !== "string" || !path.startsWith("/") || path.includes("\0"))
      throw new Error("invalid storage path");
    const pointer = op1.stringToNewUTF8(path);
    try { return call(pointer); } finally { op1._free(pointer); }
  };
  const result = (id, command, value = {}) => post({ type: "msc-result", id, command, ...value });

  async function upload(id, data) {
    const { path, file, overwrite } = data;
    if (!(file instanceof File)) throw new Error("upload needs a browser File");
    if (!Number.isSafeInteger(file.size) || file.size > MAX_FAT_FILE)
      throw new Error("file is too large for this FAT volume");
    let buffer = 0, began = false;
    try {
      requireOk(withPath(path, (pointer) => op1._op1_msc_begin_write(pointer, overwrite ? 1 : 0)));
      began = true;
      buffer = op1._malloc(CHUNK);
      if (!buffer) throw new Error("cannot allocate the upload buffer");
      for (let at = 0; at < file.size;) {
        const bytes = new Uint8Array(await file.slice(at, Math.min(at + CHUNK, file.size)).arrayBuffer());
        if (!bytes.length) throw new Error("the selected file changed while it was being read");
        op1.HEAPU8.set(bytes, buffer);
        requireOk(op1._op1_msc_write(buffer, bytes.length));
        at += bytes.length;
        post({ type: "msc-progress", id, done: at, total: file.size });
        await yieldToEmulator();
      }
      requireOk(op1._op1_msc_commit_write());
      began = false;
      result(id, "msc-upload", { path, size: file.size });
    } catch (e) {
      if (began) op1._op1_msc_abort_write();
      throw e;
    } finally {
      if (buffer) op1._free(buffer);
    }
  }

  async function download(id, data) {
    const size = withPath(data.path, (pointer) => op1._op1_msc_open_read(pointer));
    if (size < 0) throw error();
    let buffer = 0, closed = false;
    try {
      buffer = op1._malloc(CHUNK);
      if (!buffer) throw new Error("cannot allocate the download buffer");
      let done = 0;
      for (;;) {
        const count = op1._op1_msc_read(buffer, CHUNK);
        if (count < 0) throw error();
        if (!count) break;
        const bytes = op1.HEAPU8.slice(buffer, buffer + count);
        post({ type: "msc-data", id, bytes }, [bytes.buffer]);
        done += count;
        post({ type: "msc-progress", id, done, total: size });
        await new Promise((resolve, reject) => readAcks.set(id, { resolve, reject }));
        await yieldToEmulator();
      }
      if (done !== size) throw new Error(`file size changed while reading (${done} of ${size} bytes)`);
      const closeResult = op1._op1_msc_close_read();
      closed = true;
      requireOk(closeResult);
      result(id, "msc-download", { name: data.name, size });
    } finally {
      readAcks.delete(id);
      if (buffer) op1._free(buffer);
      if (!closed) op1._op1_msc_close_read();
    }
  }

  async function command(data) {
    const { id } = data;
    switch (data.type) {
      case "msc-connect":
        requireOk(op1._op1_msc_connect());
        post({ type: "msc-state", connected: true });
        result(id, data.type);
        break;
      case "msc-disconnect":
        op1._op1_msc_disconnect();
        post({ type: "msc-state", connected: false });
        result(id, data.type);
        break;
      case "msc-list": {
        const pointer = withPath(data.path, (path) => op1._op1_msc_list(path));
        if (!pointer) throw error();
        result(id, data.type, { path: data.path, entries: JSON.parse(op1.UTF8ToString(pointer)) });
        break;
      }
      case "msc-upload": await upload(id, data); break;
      case "msc-download": await download(id, data); break;
      default: throw new Error(`unknown storage command ${data.type}`);
    }
  }

  function acknowledge({ id, text }) {
    const pending = readAcks.get(id);
    if (!pending) return;
    readAcks.delete(id);
    if (text) pending.reject(new Error(text));
    else pending.resolve();
  }

  function pollBus() {
    const active = Boolean(op1._op1_usb_bus_active());
    if (active === busActive) return;
    busActive = active;
    let mounted = false;
    if (!active) {
      mounted = Boolean(op1._op1_msc_connected());
      if (mounted) op1._op1_msc_disconnect();
      // A download may be waiting for the page to acknowledge its last chunk.
      // Wake it so its finally blocks run instead of leaving the command stuck.
      for (const request of readAcks.values())
        request.reject(new DOMException("USB bus inactive", "AbortError"));
      readAcks.clear();
    }
    post({ type: "usb-bus-state", active });
    if (mounted) post({ type: "msc-state", connected: false });
  }

  return { command, acknowledge, pollBus, connected: () => Boolean(op1._op1_msc_connected()) };
}
