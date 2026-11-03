// Bounded, passive previews. FAT names and file bytes are never inserted as
// HTML; media uses an object URL that is revoked whenever the dialog closes.
const MAX_HEX_BYTES = 64 * 1024;
const FORMATS = new Map(Object.entries({
  txt: ["text", "text/plain"], log: ["text", "text/plain"], md: ["text", "text/plain"],
  json: ["text", "application/json"], csv: ["text", "text/csv"], xml: ["text", "text/xml"],
  ini: ["text", "text/plain"], cfg: ["text", "text/plain"],
  png: ["image", "image/png"], jpg: ["image", "image/jpeg"], jpeg: ["image", "image/jpeg"],
  gif: ["image", "image/gif"], webp: ["image", "image/webp"], bmp: ["image", "image/bmp"],
  wav: ["audio", "audio/wav"], mp3: ["audio", "audio/mpeg"], ogg: ["audio", "audio/ogg"],
  flac: ["audio", "audio/flac"], m4a: ["audio", "audio/mp4"], aac: ["audio", "audio/aac"],
}));

function formatHex(bytes) {
  const lines = [];
  for (let at = 0; at < bytes.length; at += 16) {
    const row = bytes.subarray(at, Math.min(at + 16, bytes.length));
    const hex = [...row].map((byte) => byte.toString(16).padStart(2, "0")).join(" ").padEnd(47, " ");
    const text = [...row].map((byte) => byte >= 32 && byte < 127 ? String.fromCharCode(byte) : ".").join("");
    lines.push(`${at.toString(16).padStart(8, "0")}  ${hex}  |${text}|`);
  }
  return lines.join("\n") || "(empty file)";
}

export function createStoragePreview({ dialog, title, content, closeButton }) {
  let objectUrl = null;

  function cleanup() {
    for (const media of content.querySelectorAll("audio")) {
      media.pause();
      media.removeAttribute("src");
      media.load();
    }
    if (objectUrl) URL.revokeObjectURL(objectUrl);
    objectUrl = null;
    content.replaceChildren();
  }

  function close() {
    if (dialog.open) dialog.close();
    else cleanup();
  }

  async function show({ name, blob }, isValid = () => true) {
    if (dialog.open) throw new Error("Close the current preview first");
    cleanup();
    const extension = name.includes(".") ? name.split(".").at(-1).toLowerCase() : "";
    const [kind, mime] = FORMATS.get(extension) ?? ["hex", "application/octet-stream"];
    let node;
    if (kind === "text") {
      const text = await blob.text();
      if (!isValid()) throw new DOMException("Preview cancelled", "AbortError");
      node = document.createElement("pre");
      node.className = "storage-preview-text";
      node.textContent = text;
    } else if (kind === "hex") {
      const bytes = new Uint8Array(await blob.slice(0, MAX_HEX_BYTES).arrayBuffer());
      if (!isValid()) throw new DOMException("Preview cancelled", "AbortError");
      node = document.createElement("pre");
      node.className = "storage-hex";
      node.textContent = formatHex(bytes);
      if (blob.size > MAX_HEX_BYTES) {
        const note = document.createElement("p");
        note.className = "storage-preview-note";
        note.textContent = `Showing the first 64 KiB of ${blob.size.toLocaleString()} bytes.`;
        content.append(note);
      }
    } else {
      if (!isValid()) throw new DOMException("Preview cancelled", "AbortError");
      node = document.createElement(kind === "image" ? "img" : "audio");
      if (kind === "image") node.alt = name;
      else {
        node.controls = true;
        node.preload = "metadata";
      }
      objectUrl = URL.createObjectURL(new Blob([blob], { type: mime }));
      node.src = objectUrl;
    }
    content.append(node);
    title.textContent = name;
    dialog.showModal();
  }

  closeButton.addEventListener("click", () => dialog.close());
  dialog.addEventListener("close", cleanup);
  dialog.addEventListener("keydown", (event) => event.stopPropagation());
  return { show, close };
}
