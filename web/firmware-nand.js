// Independent little-endian YAFFS2 format implementation, not a mkyaffs2 port.
// Format references: yaffs2utils revision 1edc1688b7f8db140d2b85448dd68f9e6318dc95
// (obj_hdr, packed tags, ECC definitions), and PR76 provision_nand.py.
export const PAGE_SIZE = 2048, OOB_SIZE = 64, TOTAL_PAGES = 4096 * 64;
export const START_PAGE = 132 * 64, DATA_SIZE = TOTAL_PAGES * PAGE_SIZE;
export const NAND_SIZE = TOTAL_PAGES * (PAGE_SIZE + OOB_SIZE);
const encoder = new TextEncoder();
const tick = () => new Promise(resolve => setTimeout(resolve, 0));
function checkAbort(signal) {
  if (signal?.aborted) throw new DOMException('NAND provisioning cancelled', 'AbortError');
}
function uint(value, fallback, label, max = 0xffffffff) {
  value ??= fallback;
  if (!Number.isInteger(value) || value < 0 || value > max) throw new Error(`Invalid ${label}`);
  return value;
}
function tree(entries) {
  if (!Array.isArray(entries) || entries.length > TOTAL_PAGES - START_PAGE) throw new Error('Invalid or oversized content tree');
  const nodes = new Map(), explicit = new Set();
  for (const entry of entries) {
    if (!entry || typeof entry.name !== 'string' || !['file', 'directory'].includes(entry.type)) throw new Error('Unsupported content entry');
    const name = entry.type === 'directory' ? entry.name.replace(/\/$/, '') : entry.name;
    const parts = name.split('/');
    if (name.length > 4096 || parts.length > 128 || parts[0] !== 'content' || parts.some(p => !p || p === '.' || p === '..' || /[\\\x00-\x1f\x7f]/.test(p) || encoder.encode(p).length > 255 || /[\ud800-\udfff]/u.test(p))) throw new Error(`Unsafe content path: ${entry.name}`);
    if (explicit.has(name)) throw new Error(`Duplicate content path: ${name}`);
    explicit.add(name);
    if (entry.type === 'file' && !(entry.bytes instanceof Uint8Array)) throw new Error(`Missing file bytes: ${name}`);
    if (entry.type === 'directory' && entry.bytes?.length) throw new Error(`Directory contains bytes: ${name}`);
    const old = nodes.get(name);
    if (old && (old.type !== 'directory' || entry.type !== 'directory')) throw new Error(`File/directory conflict: ${name}`);
    const mode = uint(entry.mode, entry.type === 'directory' ? 0o755 : 0o644, 'mode', 0o177777);
    const modeType = mode & 0o170000, expectedType = entry.type === 'file' ? 0o100000 : 0o040000;
    if (modeType && modeType !== expectedType) throw new Error(`Conflicting mode type: ${name}`);
    nodes.set(name, {name, type: entry.type, bytes: entry.bytes, mode: mode & 0o7777, mtime: uint(entry.mtime, 0, 'mtime')});
    for (let i = 1; i < parts.length; i++) {
      const parent = parts.slice(0, i).join('/');
      if (nodes.get(parent)?.type === 'file') throw new Error(`File/directory conflict: ${parent}`);
      if (!nodes.has(parent)) nodes.set(parent, {name: parent, type: 'directory', mode: 0o755, mtime: 0});
    }
    if (nodes.size > TOTAL_PAGES - START_PAGE) throw new Error('Content tree exceeds NAND capacity');
  }
  if (![...nodes.values()].some(n => n.type === 'file')) throw new Error('No regular files in content/');
  const sorted = [...nodes.values()].sort((a, b) => a.name < b.name ? -1 : a.name > b.name ? 1 : 0);
  let pages = 0;
  sorted.forEach((node, i) => { node.id = 257 + i; pages += 1 + (node.type === 'file' ? Math.ceil(node.bytes.length / PAGE_SIZE) : 0); });
  if (pages > TOTAL_PAGES - START_PAGE) throw new Error('Content exceeds NAND capacity');
  for (const node of sorted) {
    const slash = node.name.lastIndexOf('/');
    node.parent = slash < 0 ? 1 : nodes.get(node.name.slice(0, slash)).id;
    node.leaf = encoder.encode(node.name.slice(slash + 1));
  }
  return {nodes: sorted, pages};
}
function header(node) {
  const bytes = new Uint8Array(PAGE_SIZE).fill(255), d = new DataView(bytes.buffer);
  const put = (offset, n) => d.setUint32(offset, n, true);
  put(0, node.type === 'file' ? 1 : 3); put(4, node.parent);
  bytes.fill(0, 10, 265); bytes.set(node.leaf, 10);
  if (node.leaf.length === 255) bytes[265] = 0;
  put(268, node.mode | (node.type === 'file' ? 0o100000 : 0o040000));
  put(272, 0); put(276, 0);
  for (const offset of [280, 284, 288]) put(offset, node.mtime);
  if (node.type === 'file') put(292, node.bytes.length);
  put(460, 0);
  return bytes;
}
function spare(id, chunk, count) {
  const bytes = new Uint8Array(OOB_SIZE).fill(255), d = new DataView(bytes.buffer);
  for (const [i, value] of [0x1000, id, chunk, count].entries()) d.setUint32(4 + i * 4, value, true);
  // Independently derive parity from bit coordinates instead of a copied table.
  let column = 0, row = 0, inverseRow = 0;
  for (let i = 0; i < 16; i++) {
    for (let bit = 0; bit < 8; bit++) {
      if (!(bytes[4 + i] & (1 << bit))) continue;
      for (let axis = 0; axis < 3; axis++) column ^= 1 << (2 * axis + ((bit >> axis) & 1));
      row ^= i; inverseRow ^= ~i;
    }
  }
  bytes[20] = column; // Remaining three ECC alignment bytes stay erased.
  d.setUint32(24, row >>> 0, true); d.setUint32(28, inverseRow >>> 0, true);
  return bytes;
}
/** Stream to an OPFS synchronous access handle. Caller owns close and failure
 * cleanup. Payload views are not copied; working byte buffers use ~1 MiB plus
 * a page/OOB, and tree metadata is proportional to entry count. */
export async function provisionNand(entries, handle, {onProgress, signal} = {}) {
  checkAbort(signal);
  const {nodes, pages} = tree(entries);
  if (!handle || !['write', 'truncate', 'flush'].every(k => typeof handle[k] === 'function')) throw new Error('A synchronous OPFS access handle is required');
  const write = (bytes, at) => {
    for (let offset = 0; offset < bytes.length;) {
      checkAbort(signal);
      const n = handle.write(bytes.subarray(offset), {at: at + offset});
      if (!Number.isInteger(n) || n <= 0 || n > bytes.length - offset) throw new Error('Incomplete NAND write');
      offset += n;
    }
  };
  handle.truncate(NAND_SIZE);
  const erased = new Uint8Array(1024 * 1024).fill(255);
  for (let at = 0; at < NAND_SIZE; at += erased.length) {
    write(erased.subarray(0, Math.min(erased.length, NAND_SIZE - at)), at);
    onProgress?.({phase: 'erase', completed: Math.min(at + erased.length, NAND_SIZE), total: NAND_SIZE});
    await tick();
  }
  let index = 0;
  const page = async (data, id, chunk, count) => {
    const physical = START_PAGE + index;
    write(data, physical * PAGE_SIZE);
    write(spare(id, chunk, count), DATA_SIZE + physical * OOB_SIZE);
    index++;
    if (index % 64 === 0 || index === pages) {
      onProgress?.({phase: 'content', completed: index, total: pages});
      await tick();
    }
  };
  for (const node of nodes) {
    await page(header(node), node.id, 0, 0xffff);
    if (node.type !== 'file') continue;
    for (let offset = 0; offset < node.bytes.length; offset += PAGE_SIZE) {
      const part = node.bytes.subarray(offset, offset + PAGE_SIZE);
      const data = part.length === PAGE_SIZE ? part : new Uint8Array(PAGE_SIZE).fill(255);
      if (data !== part) data.set(part);
      await page(data, node.id, offset / PAGE_SIZE + 1, part.length);
    }
  }
  checkAbort(signal); handle.flush();
  return {pages, objects: nodes.length, bytes: NAND_SIZE};
}
