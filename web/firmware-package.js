/** In-memory original OP-1 package/loader decoding. No network, persistence,
 * device access, execution or bundled secrets. Ported from checked op-decryptor2. */
export const MAX_INPUT_BYTES = 64 * 1024 * 1024;
export const MAX_ARCHIVE_BYTES = 256 * 1024 * 1024;
export const MAX_DICTIONARY_BYTES = 64 * 1024 * 1024;
export const STOCK_246_SHA256 = 'c5315218f825f143b415ca554516541898abee843d3a236df0b54c04e1fb13a9';
export const ENCRYPTED_246_SHA256 = 'f79d299f6f9019c998013a4176223a6a1bc999a2d8c54451346861f6ea455c74';
export const PLAINTEXT_246_SHA256 = '1e806672dc440d495987c6b1f33b300876e6a3bb1d58254aac5fe60c960e906a';
const check = (ok, message) => { if (!ok) throw new Error(message); };
const asBytes = value => value instanceof Uint8Array ? value : new Uint8Array(value);
const view = bytes => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
const xor = bytes => bytes.reduce((a, b) => a ^ b, 0);
const trustedPackage = Symbol('canonical-package');
export function crc32(input) {
  let crc = 0xffffffff;
  for (const byte of asBytes(input)) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  return (crc ^ 0xffffffff) >>> 0;
}
function tarText(bytes) {
  const end = bytes.indexOf(0);
  return new TextDecoder('utf-8', {fatal: true}).decode(end < 0 ? bytes : bytes.subarray(0, end));
}
function octal(bytes) {
  const text = tarText(bytes).trim();
  check(/^[0-7]*$/.test(text), 'Unsupported or invalid TAR numeric field');
  const n = text ? parseInt(text, 8) : 0;
  check(Number.isSafeInteger(n), 'TAR numeric field exceeds safe range');
  return n;
}
function safeName(name) {
  while (name.startsWith('./')) name = name.slice(2);
  check(name.length > 0 && !name.startsWith('/') && !name.includes('\\') && !/[\x00-\x1f\x7f]/.test(name), 'Unsafe TAR member path');
  check(!name.split('/').some(part => part === '..' || part === '.'), 'Unsafe TAR member path');
  return name;
}
/** Links and extended headers fail closed. Bytes are archive views, not disk writes. */
export function parseTar(input) {
  const bytes = asBytes(input);
  check(bytes.length <= MAX_ARCHIVE_BYTES, 'TAR exceeds archive limit');
  check(bytes.length % 512 === 0, 'TAR is not a multiple of 512 bytes');
  const entries = [], names = new Set();
  let offset = 0, ended = false;
  while (offset < bytes.length) {
    const header = bytes.subarray(offset, offset + 512);
    if (header.every(b => b === 0)) {
      check(bytes.length - offset >= 1024, 'TAR is missing its end markers');
      check(bytes.subarray(offset).every(b => b === 0), 'Nonzero data after TAR end marker');
      ended = true; break;
    }
    const stored = octal(header.subarray(148, 156));
    let sum = 0;
    for (let i = 0; i < 512; i++) sum += i >= 148 && i < 156 ? 32 : header[i];
    check(sum === stored, 'Invalid TAR header checksum');
    const magic = tarText(header.subarray(257, 263));
    // Old GNU stores metadata, not POSIX prefixes, in this region.
    const prefix = magic === 'ustar' && header[262] === 0 ? tarText(header.subarray(345, 500)) : '';
    const name = safeName((prefix ? prefix + '/' : '') + tarText(header.subarray(0, 100)));
    check(!names.has(name), 'Duplicate TAR member'); names.add(name);
    check(names.size <= 65536, 'Too many TAR members');
    const size = octal(header.subarray(124, 136)), type = header[156];
    check(type === 0 || type === 48 || type === 53, 'Unsupported TAR member type (only files and directories allowed)');
    check(type !== 53 || size === 0, 'TAR directory contains a payload');
    const start = offset + 512, end = start + size;
    check(end <= bytes.length, 'Truncated TAR member');
    check(size <= MAX_INPUT_BYTES, 'TAR member exceeds 64 MiB limit');
    entries.push({name, bytes: bytes.subarray(start, end), type: type === 53 ? 'directory' : 'file', mode: octal(header.subarray(100, 108)), mtime: octal(header.subarray(136, 148))});
    offset = start + Math.ceil(size / 512) * 512;
  }
  check(ended, 'TAR is missing its end markers');
  return entries;
}
/** Low-level decoder injection MUST enforce limits during decoding and return
 * {bytes: Uint8Array, bytesRead: number}. Production uses extractFirmware246. */
export async function extractFirmware(input, {format = 'auto', decompressLzma, [trustedPackage]: trusted = false} = {}) {
  const bytes = asBytes(input);
  check(bytes.length <= MAX_INPUT_BYTES, 'Firmware input exceeds 64 MiB limit');
  check(['auto', 'op1', 'ldr'].includes(format), 'Unsupported firmware format');
  const packageMatches = bytes.length >= 17 && bytes[4] < 225 && view(bytes).getUint32(0, true) === crc32(bytes.subarray(4));
  if (format === 'auto') format = !packageMatches && bytes.length >= 16 && bytes[3] === 0xad && xor(bytes.subarray(0, 16)) === 0 ? 'ldr' : 'op1';
  if (format === 'ldr') return {loader: bytes, entries: [], format, packageCrc32: null};
  check(bytes.length >= 17, 'Truncated OP-1 package header');
  const compressed = bytes.subarray(4), header = view(compressed), packageCrc32 = crc32(compressed);
  check(view(bytes).getUint32(0, true) === packageCrc32, 'Package CRC32 mismatch');
  check(compressed[0] < 225, 'Invalid LZMA properties byte');
  check(header.getUint32(1, true) <= MAX_DICTIONARY_BYTES, 'LZMA dictionary exceeds 64 MiB limit');
  const declared = header.getBigUint64(5, true);
  check(declared === 0xffffffffffffffffn || declared <= BigInt(MAX_ARCHIVE_BYTES), 'Declared LZMA output exceeds archive limit');
  check(typeof decompressLzma === 'function', 'A bounded LZMA-alone decoder is required');
  const result = await decompressLzma(compressed, {maxOutputBytes: MAX_ARCHIVE_BYTES, maxDictionaryBytes: MAX_DICTIONARY_BYTES});
  check(result && result.bytes instanceof Uint8Array, 'LZMA decoder returned invalid output');
  check(result.bytes.length <= MAX_ARCHIVE_BYTES, 'LZMA output exceeds archive limit');
  // Only byte-for-byte canonical packages permit decoders without consumed count.
  check((trusted && result.bytesRead === undefined) || result.bytesRead === compressed.length, 'Unexpected data after LZMA stream');
  check(declared === 0xffffffffffffffffn || declared === BigInt(result.bytes.length), 'LZMA output length mismatch');
  const entries = parseTar(result.bytes);
  const loader = entries.find(entry => entry.name === 'OP1_vdk.ldr' && entry.type === 'file')?.bytes;
  check(loader, 'TAR does not contain a root OP1_vdk.ldr regular file');
  return {loader, entries, format, packageCrc32};
}
export function parseKeyHex(text) {
  check(typeof text === 'string' && /^[0-9a-fA-F]{32}$/.test(text), 'Key must contain exactly 32 hexadecimal digits');
  return Uint32Array.from({length: 4}, (_, i) => parseInt(text.slice(i * 8, i * 8 + 8), 16));
}
export function parseOtpText(text) {
  check(typeof text === 'string' && text.length <= 1024 * 1024, 'OTP text exceeds limit or is invalid');
  let key = null;
  for (const line of text.split(/\r?\n/)) {
    const fields = line.trim().split(/\s+/);
    if (!/^!?0xd0$/i.test(fields[0])) continue;
    check(key === null, 'OTP dump contains duplicate page 0xd0 entries');
    check(fields.length === 3 && fields[1].length === 16 && fields[2].length === 16, 'OTP page 0xd0 must contain exactly two 16-digit hexadecimal words');
    key = parseKeyHex(fields[1] + fields[2]);
  }
  check(key !== null, 'OTP dump does not contain page 0xd0'); return key;
}
export function decryptXteaBlock(input, key) {
  const bytes = asBytes(input);
  check(bytes.length === 8 && key instanceof Uint32Array && key.length === 4, 'Invalid XTEA block or key shape');
  let left = view(bytes).getUint32(0, true), right = view(bytes).getUint32(4, true), sum = 0xc6ef3720;
  const mix = n => (((n << 4) ^ (n >>> 5)) + n) >>> 0;
  for (let i = 0; i < 32; i++) {
    right = (right - (mix(left) ^ ((sum + key[(sum >>> 11) & 3]) >>> 0))) >>> 0;
    sum = (sum - 0x9e3779b9) >>> 0;
    left = (left - (mix(right) ^ ((sum + key[sum & 3]) >>> 0))) >>> 0;
  }
  const out = new Uint8Array(8), data = view(out);
  data.setUint32(0, left, true); data.setUint32(4, right, true); return out;
}
/** Structural transformation, NOT authentication. Validate before mounting/execution.
 * OTP never appears in reports/errors. The temporary parsed key is cleared. */
export function decryptLoader(input, otpText = null) {
  const source = asBytes(input);
  check(source.length <= MAX_INPUT_BYTES, 'Loader exceeds 64 MiB limit');
  const bytes = Uint8Array.from(source), data = view(bytes);
  const key = otpText === null ? null : parseOtpText(otpText);
  const report = {records: 0, payloadRecords: 0, fillRecords: 0, encryptedRecords: 0, decryptedUnits: 0, encryptedTailUnits: 0, finalOffset: 0, finalEnd: 0, trailerBytes: 0};
  let offset = 0;
  try {
    while (true) {
      check(offset !== bytes.length, 'Loader ended without a FINAL record');
      check(offset + 16 <= bytes.length, 'Truncated loader header');
      const header = bytes.subarray(offset, offset + 16);
      check(header[3] === 0xad, 'Invalid loader signature');
      check(xor(header) === 0, 'Invalid loader header XOR checksum');
      const flags = data.getUint16(offset, true), fill = !!(flags & 0x100), callback = !!(flags & 0x400);
      check(!callback || (!fill && (flags & 0x2000)), 'Unsupported CALLBACK flags');
      const length = fill ? 0 : data.getUint32(offset + 8, true), start = offset + 16, end = start + length;
      check(end <= bytes.length, 'Truncated loader payload'); report.records++;
      if (fill) report.fillRecords++; else if (length) report.payloadRecords++;
      if (callback) {
        check(key, 'OTP page 0xd0 is required for encrypted CALLBACK records'); report.encryptedRecords++;
        for (let p = start; p + 8 <= end; p += 24) {
          bytes.set(decryptXteaBlock(bytes.subarray(p, p + 8), key), p); report.decryptedUnits++;
          if (end - p < 24) report.encryptedTailUnits++;
        }
        data.setUint16(offset, flags & ~0x400, true); header[2] = 0; header[2] = xor(header);
      }
      if (flags & 0x8000) {
        report.finalOffset = offset; report.finalEnd = end; report.trailerBytes = bytes.length - end;
        return {bytes, report};
      }
      offset = end;
    }
  } finally { key?.fill(0); }
}
export async function sha256(input) {
  check(globalThis.crypto?.subtle, 'SHA-256 requires a secure browser context');
  const digest = new Uint8Array(await globalThis.crypto.subtle.digest('SHA-256', asBytes(input)));
  return Array.from(digest, byte => byte.toString(16).padStart(2, '0')).join('');
}
export async function validateFirmware246(input) {
  const digest = await sha256(input);
  check(digest === PLAINTEXT_246_SHA256, 'Firmware 246 plaintext SHA-256 mismatch; unsupported firmware or incorrect OTP (nothing loaded)');
  return {version: 246, sha256: digest};
}
/** Production gate: only canonical stock-246 compressed bytes reach a decoder.
 * Prevents attacker-chosen streams reaching callback decoders without output caps.
 * Raw loaders must also be canonical. Validate plaintext after decryption. */
export async function extractFirmware246(input, options = {}) {
  const bytes = asBytes(input);
  check(bytes.length <= MAX_INPUT_BYTES, 'Firmware input exceeds 64 MiB limit');
  const digest = await sha256(bytes);
  if (digest === STOCK_246_SHA256) {
    const result = await extractFirmware(bytes, {...options, format: 'op1', [trustedPackage]: true});
    check(await sha256(result.loader) === ENCRYPTED_246_SHA256, 'Canonical package loader SHA-256 mismatch'); return result;
  }
  check(digest === ENCRYPTED_246_SHA256 || digest === PLAINTEXT_246_SHA256, 'Unsupported firmware: select the original OP-1 firmware 246 package or its canonical loader');
  return extractFirmware(bytes, {format: 'ldr'});
}
