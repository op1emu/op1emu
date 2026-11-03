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
  aif: ["aiff", "audio/aiff"], aiff: ["aiff", "audio/aiff"], aifc: ["aiff", "audio/aiff"],
}));

// AIFF is not consistently supported by the browser media decoders (notably
// Chromium). Decode it with the small, MIT-licensed audiojs decoder and hand
// the PCM to wavesurfer.js through a browser-friendly WAV container.
let audioModules;
function loadAudioModules() {
  return audioModules ??= Promise.all([
    import("@audio/decode-aiff"),
    import("wavesurfer.js"),
  ]).then(([decoder, wavesurfer]) => ({
    decodeAiff: decoder.default,
    WaveSurfer: wavesurfer.default,
  }));
}

function pcmToWav(channelData, sampleRate) {
  // PCM16 keeps the compatibility bridge small; the original float samples
  // remain available to wavesurfer for the waveform itself.
  const channels = channelData.length;
  const frames = channelData.reduce((length, channel) => Math.min(length, channel.length), Infinity);
  if (!channels || !Number.isFinite(frames) || !frames || !Number.isFinite(sampleRate) || sampleRate <= 0)
    throw new Error("AIFF contains no playable PCM samples");
  const dataSize = frames * channels * 2;
  const bytes = new ArrayBuffer(44 + dataSize);
  const view = new DataView(bytes);
  const text = (at, value) => [...value].forEach((character, index) => view.setUint8(at + index, character.charCodeAt(0)));
  text(0, "RIFF"); view.setUint32(4, 36 + dataSize, true); text(8, "WAVE");
  text(12, "fmt "); view.setUint32(16, 16, true); view.setUint16(20, 1, true);
  view.setUint16(22, channels, true); view.setUint32(24, Math.round(sampleRate), true);
  view.setUint32(28, Math.round(sampleRate) * channels * 2, true); view.setUint16(32, channels * 2, true);
  view.setUint16(34, 16, true); text(36, "data"); view.setUint32(40, dataSize, true);
  let at = 44;
  for (let frame = 0; frame < frames; frame++) {
    for (const channel of channelData) {
      const sample = Math.max(-1, Math.min(1, channel[frame] || 0));
      view.setInt16(at, sample < 0 ? sample * 32768 : sample * 32767, true);
      at += 2;
    }
  }
  return new Blob([bytes], { type: "audio/wav" });
}

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
  let audioPlayer = null;

  function cleanup() {
    if (audioPlayer) {
      audioPlayer.destroy();
      audioPlayer = null;
    }
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
    let node, displayed = false;
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
    } else if (kind === "aiff") {
      const [{ decodeAiff, WaveSurfer }, bytes] = await Promise.all([
        loadAudioModules(),
        blob.arrayBuffer(),
      ]);
      if (!isValid()) throw new DOMException("Preview cancelled", "AbortError");
      // @audio/decode-aiff is synchronous. The caller caps previews at 16 MiB
      // so this bounded decode cannot grow without limit on the UI thread.
      const decoded = decodeAiff(new Uint8Array(bytes));
      const wav = pcmToWav(decoded.channelData, decoded.sampleRate);
      const frames = decoded.channelData.reduce((length, channel) => Math.min(length, channel.length), Infinity);
      node = document.createElement("div");
      node.className = "storage-audio-preview";
      const waveform = document.createElement("div");
      waveform.className = "storage-waveform";
      waveform.setAttribute("role", "group");
      waveform.setAttribute("aria-label", `${name} waveform`);
      node.append(waveform);
      const note = document.createElement("p");
      note.className = "storage-preview-note";
      note.textContent = `${decoded.channelData.length} channel${decoded.channelData.length === 1 ? "" : "s"} · ${Math.round(decoded.sampleRate).toLocaleString()} Hz`;
      node.append(note);
      const wave = WaveSurfer.create({
        container: waveform,
        height: 112,
        waveColor: getComputedStyle(document.documentElement).getPropertyValue("--muted").trim(),
        progressColor: getComputedStyle(document.documentElement).getPropertyValue("--accent").trim(),
        cursorColor: getComputedStyle(document.documentElement).getPropertyValue("--fg").trim(),
        cursorWidth: 2,
        normalize: true,
        dragToSeek: true,
        mediaControls: true,
      });
      audioPlayer = wave;
      // The renderer measures its host when it loads. Mount and open the
      // dialog first so a browser does not report a zero-width hidden host.
      content.append(node);
      title.textContent = name;
      dialog.showModal();
      displayed = true;
      try {
        // Pass the decoded PCM as peaks so wavesurfer renders it directly and
        // only the playback bridge needs the WAV container.
        await wave.loadBlob(wav, decoded.channelData, frames / decoded.sampleRate);
      } catch (error) {
        if (audioPlayer !== wave || !dialog.open) return false;
        if (audioPlayer === wave) audioPlayer = null;
        wave.destroy();
        if (dialog.open) dialog.close();
        throw error;
      }
      if (audioPlayer !== wave || !dialog.open) return false;
      if (!isValid()) {
        wave.destroy();
        if (audioPlayer === wave) audioPlayer = null;
        if (dialog.open) dialog.close();
        throw new DOMException("Preview cancelled", "AbortError");
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
    if (!displayed) {
      content.append(node);
      title.textContent = name;
      dialog.showModal();
    }
    return true;
  }

  closeButton.addEventListener("click", () => dialog.close());
  dialog.addEventListener("close", cleanup);
  dialog.addEventListener("keydown", (event) => event.stopPropagation());
  return { show, close };
}
