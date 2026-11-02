#!/usr/bin/env python3
"""Prepare an op1emu split-OOB NAND image from original OP-1 factory content.

Requires an external mkyaffs2 executable (yaffs2utils); does not decrypt LDRs.
"""

import argparse
import io
import lzma
import os
from pathlib import Path, PurePosixPath
import shutil
import struct
import subprocess
import tarfile
import tempfile
import zlib

PAGE_SIZE = 2048
OOB_SIZE = 64
PAGES_PER_BLOCK = 64
TOTAL_BLOCKS = 4096
TAG_OFFSET_IN_IMAGE = 2
TAG_OFFSET_IN_EMULATOR = 4
TAGS_SIZE = 28
FIRST_YAFFS_SEQUENCE = 0x1000


def unpack_op1(package: Path, root: Path) -> None:
    """Extract only content/ from a CRC32 + LZMA-alone + TAR .op1 package."""
    raw = package.read_bytes()
    if len(raw) < 17:
        raise ValueError("Firmware package is too short")
    stored_crc = struct.unpack_from("<I", raw)[0]
    compressed = raw[4:]
    if (zlib.crc32(compressed) & 0xFFFFFFFF) != stored_crc:
        raise ValueError("Firmware package CRC32 mismatch")
    archive = lzma.decompress(compressed, format=lzma.FORMAT_ALONE)
    extracted = 0
    directories = {root: (0o755, 0)}
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as tar:
        for member in tar:
            name = PurePosixPath(member.name)
            if name.is_absolute() or ".." in name.parts:
                raise ValueError(f"Unsafe archive path: {member.name}")
            if not name.parts or name.parts[0] != "content":
                continue
            target = root.joinpath(*name.parts)
            for parent in target.parents:
                if parent == root:
                    break
                directories.setdefault(parent, (0o755, 0))
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
                directories[target] = (member.mode, member.mtime)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                with tar.extractfile(member) as source, target.open("wb") as dest:
                    shutil.copyfileobj(source, dest)
                os.chmod(target, member.mode)
                os.utime(target, (member.mtime, member.mtime))
                extracted += 1
            else:
                raise ValueError(f"Unsupported content entry: {member.name}")
    # Apply directories last, children first: creating descendants changes mtimes
    # and archive modes may remove the write permission needed for extraction.
    for directory in sorted(directories, key=lambda path: len(path.parts), reverse=True):
        mode, mtime = directories[directory]
        os.chmod(directory, mode)
        os.utime(directory, (mtime, mtime))
    if not extracted:
        raise ValueError("Package has no regular files in content/")


def normalize_yaffs_timestamps(interleaved: Path) -> None:
    """Use archived mtime for atime/ctime in stock little-endian YAFFS2 headers.

    ctime cannot be restored with utime, and mkyaffs2's directory scan can change
    atime. Its OOB ECC covers tags only, so changing header data leaves it valid.
    """
    chunk_size = PAGE_SIZE + OOB_SIZE
    with interleaved.open("r+b") as image:
        while True:
            offset = image.tell()
            chunk = image.read(chunk_size)
            if not chunk:
                break
            if len(chunk) != chunk_size:
                raise ValueError("Invalid or incomplete YAFFS2 image size")
            spare = chunk[PAGE_SIZE:]
            sequence, _, chunk_id, _ = struct.unpack_from("<4I", spare, TAG_OFFSET_IN_IMAGE)
            if sequence != FIRST_YAFFS_SEQUENCE:
                raise ValueError("Unsupported YAFFS2 sequence")
            if chunk_id == 0 and struct.unpack_from("<I", chunk)[0] != 4:
                # yaffs_obj_hdr: atime, mtime, ctime are at bytes 280, 284, 288.
                mtime = chunk[284:288]
                image.seek(offset + 280)
                image.write(mtime * 3)
                image.seek(offset + chunk_size)


def build_yaffs(root: Path, interleaved: Path, mkyaffs2: str) -> None:
    program = shutil.which(mkyaffs2)
    if program is None:
        raise ValueError(f"mkyaffs2 not found: {mkyaffs2}")
    cmd = [program, "-p", "2048", "-s", "64", "--all-root", str(root), str(interleaved)]
    completed = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if completed.returncode:
        raise RuntimeError(
            f"mkyaffs2 failed (exit {completed.returncode}).\n"
            + completed.stdout[-3000:] + completed.stderr[-3000:]
            + "\nNote: older yaffs2utils needs a dirname() portability fix on macOS."
        )

    normalize_yaffs_timestamps(interleaved)


def provision(interleaved: Path, output: Path, start_block: int = 132,
              total_blocks: int = TOTAL_BLOCKS) -> int:
    """Convert 2112-byte interleaved chunks into op1emu's split data/OOB layout."""
    if not (0 <= start_block < total_blocks):
        raise ValueError("Start block outside NAND")
    size = interleaved.stat().st_size
    chunk_size = PAGE_SIZE + OOB_SIZE
    if not size or size % chunk_size:
        raise ValueError("Invalid or incomplete YAFFS2 image size")
    count = size // chunk_size
    total_pages = total_blocks * PAGES_PER_BLOCK
    start_page = start_block * PAGES_PER_BLOCK
    if start_page + count > total_pages:
        raise ValueError("YAFFS2 image does not fit at the requested start block")
    if output.exists():
        raise FileExistsError(f"Refusing to overwrite existing NAND: {output}")
    data_region_size = total_pages * PAGE_SIZE
    target = output.open("xb")
    try:
        with target, interleaved.open("rb") as source:
            erased = b"\xff" * (1024 * 1024)
            bytes_left = total_pages * chunk_size
            while bytes_left:
                part = min(bytes_left, len(erased))
                target.write(erased[:part])
                bytes_left -= part
            for i in range(count):
                data = source.read(PAGE_SIZE)
                spare = source.read(OOB_SIZE)
                if len(data) != PAGE_SIZE or len(spare) != OOB_SIZE:
                    raise ValueError(f"Truncated YAFFS2 image at page {i}")
                if (spare[:2] != b"\xff\xff" or
                    spare[30:] != b"\xff" * 34 or
                    struct.unpack_from("<I", spare, TAG_OFFSET_IN_IMAGE)[0]
                        != FIRST_YAFFS_SEQUENCE):
                    raise ValueError(f"Unsupported YAFFS2 OOB layout at page {i}")
                page = start_page + i
                translated = (b"\xff" * TAG_OFFSET_IN_EMULATOR
                              + spare[TAG_OFFSET_IN_IMAGE:TAG_OFFSET_IN_IMAGE + TAGS_SIZE]
                              + b"\xff" * (OOB_SIZE - TAG_OFFSET_IN_EMULATOR - TAGS_SIZE))
                target.seek(page * PAGE_SIZE)
                target.write(data)
                target.seek(data_region_size + page * OOB_SIZE)
                target.write(translated)
        return count
    except BaseException:
        output.unlink(missing_ok=True)
        raise


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--firmware", type=Path, help="Original OP-1 .op1 update package")
    source.add_argument("--content-dir", type=Path, help="Unpacked firmware content/ directory")
    parser.add_argument("--output", type=Path, required=True, help="New NAND image path (must not exist)")
    parser.add_argument("--mkyaffs2", default="mkyaffs2", help="Path to a working mkyaffs2 executable")
    parser.add_argument("--start-block", type=int, default=132,
                        help="YAFFS2 first block (default: 132; tested with firmware 246)")
    args = parser.parse_args()
    try:
        if args.output.exists():
            raise FileExistsError(f"Refusing to overwrite {args.output}")
        with tempfile.TemporaryDirectory(prefix="op1emu-nand-") as workspace:
            work = Path(workspace)
            root = work / "fs-root"
            root.mkdir()
            if args.firmware:
                unpack_op1(args.firmware, root)
            else:
                if not args.content_dir.is_dir():
                    raise ValueError(f"Not a directory: {args.content_dir}")
                shutil.copytree(args.content_dir, root / "content")
            interleaved = work / "content.yaffs2"
            build_yaffs(root, interleaved, args.mkyaffs2)
            pages = provision(interleaved, args.output, args.start_block)
            print(f"Wrote {pages} YAFFS2 pages to {args.output} at block {args.start_block}")
            print("This initializes content only; an encrypted LDR still needs OTP/decryption.")
    except (OSError, ValueError, RuntimeError, lzma.LZMAError, tarfile.TarError) as exc:
        parser.exit(1, f"error: {exc}\n")


if __name__ == "__main__":
    main()
