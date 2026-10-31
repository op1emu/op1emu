"""Unit tests for the OP-1 NAND provisioner; no firmware bytes needed."""

import io
import hashlib
import shutil
import lzma
import os
import stat
from unittest.mock import patch
from pathlib import Path
import struct
import tarfile
import tempfile
import time
import unittest
import zlib
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from provision_nand import PAGE_SIZE, OOB_SIZE, PAGES_PER_BLOCK, provision, unpack_op1, build_yaffs, normalize_yaffs_timestamps


class ProvisionTests(unittest.TestCase):
    def sample(self, path, seq=0x1000):
        spare = b"\xff" * 2 + struct.pack("<I", seq) + b"\xab" * 24 + b"\xff" * 34
        path.write_bytes(b"A" * PAGE_SIZE + spare + b"B" * PAGE_SIZE + spare)

    def test_interleaved_to_split_oob(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            self.sample(root / "input")
            count = provision(root / "input", root / "nand", start_block=1, total_blocks=3)
            self.assertEqual(count, 2)
            raw = (root / "nand").read_bytes()
            page = PAGES_PER_BLOCK
            self.assertEqual(len(raw), 3 * PAGES_PER_BLOCK * (PAGE_SIZE + OOB_SIZE))
            self.assertEqual(raw[page * PAGE_SIZE:(page + 1) * PAGE_SIZE], b"A" * PAGE_SIZE)
            off = 3 * PAGES_PER_BLOCK * PAGE_SIZE + page * OOB_SIZE
            self.assertEqual(raw[off:off + OOB_SIZE], b"\xff" * 4 +
                             struct.pack("<I", 0x1000) + b"\xab" * 24 + b"\xff" * 32)
            self.assertEqual(raw[:PAGE_SIZE], b"\xff" * PAGE_SIZE)

    def test_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            self.sample(root / "input")
            (root / "nand").write_bytes(b"keep")
            with self.assertRaises(FileExistsError):
                provision(root / "input", root / "nand", 1, 3)
            self.assertEqual((root / "nand").read_bytes(), b"keep")

    def test_exclusive_open_race_preserves_competing_file(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            self.sample(root / "input")
            output = root / "nand"
            original_open = Path.open

            def racing_open(path, mode="r", *args, **kwargs):
                if path == output and mode == "xb":
                    path.write_bytes(b"created by another process")
                return original_open(path, mode, *args, **kwargs)

            with patch.object(Path, "open", racing_open):
                with self.assertRaises(FileExistsError):
                    provision(root / "input", output, 1, 3)
            self.assertEqual(output.read_bytes(), b"created by another process")

    def test_bad_tags_remove_partial_output(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            self.sample(root / "input", seq=0x21)
            with self.assertRaisesRegex(ValueError, "OOB"):
                provision(root / "input", root / "nand", 1, 3)
            self.assertFalse((root / "nand").exists())

    def test_op1_crc_archive_extract(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            stream = io.BytesIO()
            with tarfile.open(fileobj=stream, mode="w") as tf:
                data = b"test data"
                info = tarfile.TarInfo("content/op1.db")
                info.size = len(data)
                tf.addfile(info, io.BytesIO(data))
            compressed = lzma.compress(stream.getvalue(), format=lzma.FORMAT_ALONE)
            (root / "pkg.op1").write_bytes(struct.pack("<I", zlib.crc32(compressed)) + compressed)
            unpack_op1(root / "pkg.op1", root / "root")
            self.assertEqual((root / "root/content/op1.db").read_bytes(), b"test data")

    def test_archive_metadata_is_independent_of_time_and_umask(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            stream = io.BytesIO()
            entries = [("content", None, 0o555, 123456789),
                       ("content/nested", None, 0o750, 123456790),
                       ("content/nested/data", b"synthetic", 0o640, 123456791),
                       ("content/implicit/data", b"implicit", 0o600, 123456792)]
            with tarfile.open(fileobj=stream, mode="w") as tf:
                for name, data, mode, mtime in entries:
                    info = tarfile.TarInfo(name)
                    info.mode, info.mtime = mode, mtime
                    if data is None:
                        info.type = tarfile.DIRTYPE
                    else:
                        info.size = len(data)
                    tf.addfile(info, None if data is None else io.BytesIO(data))
            compressed = lzma.compress(stream.getvalue(), format=lzma.FORMAT_ALONE)
            package = root / "pkg.op1"
            package.write_bytes(struct.pack("<I", zlib.crc32(compressed)) + compressed)
            snapshots = []
            try:
                for index, mask in enumerate((0o022, 0o077)):
                    destination = root / str(index)
                    old_mask = os.umask(mask)
                    try:
                        # Force newly created files to have different initial timestamps.
                        original_open = Path.open
                        def timed_open(path, mode="r", *args, **kwargs):
                            handle = original_open(path, mode, *args, **kwargs)
                            if mode == "wb":
                                os.utime(path, (1800000000 + index, 1800000000 + index))
                            return handle
                        with patch.object(Path, "open", timed_open):
                            unpack_op1(package, destination)
                    finally:
                        os.umask(old_mask)
                    paths = [destination] + sorted(destination.rglob("*"))
                    snapshot = {str(p.relative_to(destination)):
                                (stat.S_IMODE(p.stat().st_mode), p.stat().st_mtime_ns)
                                for p in paths}
                    for name, data, mode, mtime in entries:
                        self.assertEqual(snapshot[name], (mode, mtime * 10**9))
                    self.assertEqual(snapshot["."], (0o755, 0))
                    self.assertEqual(snapshot["content/implicit"], (0o755, 0))
                    snapshots.append(snapshot)
                self.assertEqual(*snapshots)
            finally:
                # Allow TemporaryDirectory to remove archive directories with read-only modes.
                for directory in root.rglob("*"):
                    if directory.is_dir():
                        directory.chmod(0o755)

    def test_normalizes_header_timestamps_only(self):
        with tempfile.TemporaryDirectory() as work:
            image = Path(work) / "image"
            chunks = []
            for object_type, chunk_id in ((1, 0), (3, 0), (4, 0), (1, 1)):
                data = bytearray(b"X" * PAGE_SIZE)
                struct.pack_into("<I", data, 0, object_type)
                struct.pack_into("<3I", data, 280, 111, 222, 333)
                spare = (b"\xff" * 2 + struct.pack("<4I", 0x1000, 257, chunk_id, 0xffff)
                         + b"E" * 12 + b"\xff" * 34)
                chunks.append(bytes(data) + spare)
            image.write_bytes(b"".join(chunks))
            normalize_yaffs_timestamps(image)
            result = image.read_bytes()
            for index, original in enumerate(chunks):
                expected = bytearray(original)
                if index < 2:
                    struct.pack_into("<3I", expected, 280, 222, 222, 222)
                start = index * (PAGE_SIZE + OOB_SIZE)
                self.assertEqual(result[start:start + len(expected)], expected)

    @unittest.skipUnless(shutil.which("mkyaffs2"), "requires existing stock mkyaffs2")
    def test_yaffs_and_nand_repeatability(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            stream = io.BytesIO()
            with tarfile.open(fileobj=stream, mode="w") as tf:
                for name, data in (("content", None), ("content/nested", None),
                                   ("content/nested/data", b"synthetic fixture")):
                    info = tarfile.TarInfo(name)
                    info.mtime = 123456789
                    info.mode = 0o755 if data is None else 0o640
                    if data is None:
                        info.type = tarfile.DIRTYPE
                    else:
                        info.size = len(data)
                    tf.addfile(info, None if data is None else io.BytesIO(data))
            compressed = lzma.compress(stream.getvalue(), format=lzma.FORMAT_ALONE)
            package = root / "pkg.op1"
            package.write_bytes(struct.pack("<I", zlib.crc32(compressed)) + compressed)
            hashes = []
            for index, mask in enumerate((0o022, 0o077)):
                if index:
                    time.sleep(1.1)  # YAFFS timestamps have one-second resolution.
                destination = root / str(index)
                old_mask = os.umask(mask)
                try:
                    unpack_op1(package, destination)
                finally:
                    os.umask(old_mask)
                image, nand = root / f"{index}.yaffs2", root / f"{index}.nand"
                build_yaffs(destination, image, "mkyaffs2")
                provision(image, nand, start_block=1, total_blocks=3)
                hashes.append((hashlib.sha256(image.read_bytes()).hexdigest(),
                               hashlib.sha256(nand.read_bytes()).hexdigest()))
            self.assertEqual(*hashes)

    def test_op1_rejects_crc_corruption(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            (root / "pkg.op1").write_bytes(b"\0" * 4 + b"X" * 20)
            with self.assertRaisesRegex(ValueError, "CRC32"):
                unpack_op1(root / "pkg.op1", root / "root")


if __name__ == "__main__":
    unittest.main()
