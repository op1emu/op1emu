// The BF526's boot ROM, which the browser's build does not carry
// (src/cpu/bootrom.cpp): the bytes of ext/bfin_sim's bf526-0.2.h, a C array
// of hex bytes. virtual:op1emu names the header at the submodule's commit
// and the ROM's sha256 (tools/emulator-assets.js); the Worker fetches it
// once and keeps it in OPFS.

export const BOOT_ROM_SIZE = 0x8000;

export function parseBootRom(text) {
  const bytes = Uint8Array.from(text.match(/0x[0-9a-fA-F]{2}\b/g) ?? [], (hex) => parseInt(hex, 16));
  if (bytes.length !== BOOT_ROM_SIZE) throw new Error(`the boot ROM header has ${bytes.length} bytes, not ${BOOT_ROM_SIZE}`);
  return bytes;
}

export async function sha256(bytes) {
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  return Array.from(digest, (b) => b.toString(16).padStart(2, "0")).join("");
}
