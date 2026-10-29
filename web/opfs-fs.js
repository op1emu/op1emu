// A file system for Emscripten's FS whose files are OPFS files open in this
// Worker (FileSystemSyncAccessHandle): reads and writes go to the file where
// it is, so the 528 MiB NAND image is never in memory and what the guest
// writes is kept. Built as Emscripten's WORKERFS is (library_workerfs.js),
// with writes; it holds the files it was mounted with and no others.
//
//   FS.mount(opfsFs(FS), { files: { "nand.bin": handle, ... } }, "/data")

// Emscripten's errno values (WASI's) and mode bits.
const EPERM = 63, ENOENT = 44, EINVAL = 28;
const SEEK_CUR = 1, SEEK_END = 2;
const S_IFDIR = 0o040000, S_IFREG = 0o100000;

export function opfsFs(FS) {
  const fs = {
    dirty: new Set(),  // handles written since flush()
    mount(mount) {
      const root = node(null, "/", S_IFDIR | 0o777);
      for (const [name, handle] of Object.entries(mount.opts.files)) node(root, name, S_IFREG | 0o666, handle);
      return root;
    },
    // Writes since the last flush, to storage (the browser may hold them
    // until then).
    flush() {
      for (const handle of fs.dirty) handle.flush();
      fs.dirty.clear();
    },
  };

  function node(parent, name, mode, handle) {
    const n = FS.createNode(parent, name, mode);
    n.node_ops = nodeOps;
    n.stream_ops = streamOps;
    n.handle = handle ?? null;
    n.contents = handle ? null : {};
    n.atime = n.mtime = n.ctime = Date.now();
    if (parent) parent.contents[name] = n;
    return n;
  }

  const size = (n) => (n.handle ? n.handle.getSize() : 4096);

  const nodeOps = {
    getattr: (n) => ({
      dev: 1, ino: n.id, mode: n.mode, nlink: 1, uid: 0, gid: 0, rdev: 0, size: size(n),
      atime: new Date(n.atime), mtime: new Date(n.mtime), ctime: new Date(n.ctime),
      blksize: 4096, blocks: Math.ceil(size(n) / 4096),
    }),
    setattr(n, attr) {
      for (const key of ["mode", "atime", "mtime", "ctime"]) if (attr[key] != null) n[key] = attr[key];
      if (attr.size != null && n.handle) {
        n.handle.truncate(attr.size);
        fs.dirty.add(n.handle);
      }
    },
    lookup() { throw new FS.ErrnoError(ENOENT); },
    mknod() { throw new FS.ErrnoError(EPERM); },
    rename() { throw new FS.ErrnoError(EPERM); },
    unlink() { throw new FS.ErrnoError(EPERM); },
    rmdir() { throw new FS.ErrnoError(EPERM); },
    symlink() { throw new FS.ErrnoError(EPERM); },
    readdir: (n) => [".", "..", ...Object.keys(n.contents)],
  };

  const streamOps = {
    read: (stream, buffer, offset, length, position) =>
      stream.node.handle.read(buffer.subarray(offset, offset + length), { at: position }),
    write(stream, buffer, offset, length, position) {
      const { handle } = stream.node;
      fs.dirty.add(handle);
      stream.node.mtime = Date.now();
      return handle.write(buffer.subarray(offset, offset + length), { at: position });
    },
    llseek(stream, offset, whence) {
      let position = offset;
      if (whence === SEEK_CUR) position += stream.position;
      else if (whence === SEEK_END && FS.isFile(stream.node.mode)) position += size(stream.node);
      if (position < 0) throw new FS.ErrnoError(EINVAL);
      return position;
    },
  };

  return fs;
}
