# Original OP-1 Firmware Container

This document describes the original Teenage Engineering OP-1 firmware 246
container. It does not describe the OP-1 Field. Results are based on the
official `op1_246.op1` image and public format references; no proprietary
payload bytes are reproduced here.

## Confidence

- **CONFIRMED**: directly measured in the official image and/or established by
  an authoritative format specification.
- **LIKELY**: supported by several observations but not established directly.
- **ASSUMED**: a working interpretation that must not become implementation
  behavior without more evidence.
- **UNKNOWN**: available evidence is insufficient.

Unless qualified otherwise, numeric observations below are **CONFIRMED for
firmware 246 only**. Whether every OP-1 firmware version uses the same dialect
and limits is **UNKNOWN**.

## Image Identity

The file currently served by Teenage Engineering as original OP-1 update 246
was compared byte-for-byte by SHA-256 with the locally inspected image.

| Property | Value |
| --- | --- |
| File size | 13,039,128 bytes |
| SHA-256 | `c5315218f825f143b415ca554516541898abee843d3a236df0b54c04e1fb13a9` |
| SHA-1 | `e5c4130aa0eef852aa845a865f9dde0a9c9a` |
| MD5 | `13901daf11a5c2f4561ca895f5d96a51` |
| Stored CRC-32 | `0xcc08445c` |

Printable strings in the main LDR identify `R. 00246` and build
`Rev. 00246; 2022/11/09 16:17:00; 8.1.11.1`. The bootloader LDR identifies
`TE-BOOT v2.30`. Older revision strings also occur in the main LDR; their
meaning is **UNKNOWN** and they are not evidence of a second container version.

## Container Layers

The complete physical layout is:

```text
4-byte little-endian CRC-32
LZMA-Alone stream
  13-byte LZMA-Alone header
  LZMA1 compressed data and end marker
decompressed old GNU tar archive
```

There is no additional magic or version wrapper. The stream is LZMA-Alone,
not `.xz`.

### Checksum

The first four bytes encode an unsigned CRC-32 value in little-endian order.
It is conventional reflected CRC-32/ISO-HDLC, as used by ZIP and exposed by
`zlib.crc32` and `binascii.crc32`:

- reflected polynomial: `0xedb88320`;
- conventional initial and final complement;
- coverage: every byte from offset 4 through end of file;
- stored result: little-endian `uint32_t`.

For firmware 246, CRC-32 over bytes `[4, 13039128)` is `0xcc08445c`. The
covered bytes include the LZMA-Alone header and compressed data. The checksum
does not cover itself or the decompressed tar archive. CRC-32 detects accidental
corruption; it is not authentication.

### LZMA-Alone

The compression header starts at file offset 4:

| Field | `.op1` offset | Firmware 246 value |
| --- | ---: | --- |
| Properties | 4 | `0x5d`: `lc=3`, `lp=0`, `pb=2` |
| Dictionary size | 5-8 | little-endian `0x00800000` (8 MiB) |
| Uncompressed size | 9-16 | `0xffffffffffffffff` (unknown) |
| LZMA1 data | 17-EOF | terminated by end-of-payload marker |

The stream decodes cleanly to 26,368,000 bytes. The original encoder preset,
match finder, nice length, and other choices are **UNKNOWN** because they cannot
be recovered uniquely from the output.

Community `op1repacker` correctly describes the outer format but configures
`lp=1` when writing. Official firmware 246 uses `lp=0`. The repacker may produce
device-accepted images, but its compression recipe does not reproduce the
official image parameters.

### Tar Archive

The decompressed result is an old GNU tar archive:

| Property | Firmware 246 value |
| --- | ---: |
| Archive size | 26,368,000 bytes (51,500 512-byte blocks) |
| SHA-256 | `ee96790ae90e4bb1e21174f08b6d1c0e4c222b7075c1806ddd7221b4059e8171` |
| Members | 117: 107 regular files and 10 directories |
| Sum of regular-file sizes | 26,273,234 bytes |
| End padding | 7,168 zero bytes (14 blocks) |

The first header uses old GNU magic (`OLDGNU_MAGIC`), not POSIX ustar magic.
All header checksums and the two-zero-block terminator validate. Observed member
names are relative, unique, at most 43 bytes long, and contain no `..`
component. There are no links, devices, FIFOs, or other special members.
Directories have archived mode `0040666`, which lacks search bits; archive
ownership, modes, and times must not be replayed blindly.

Top-level contents are:

```text
content/
OP1_vdk.ldr
te-boot.ldr
```

Important member metadata, with offsets measured from the start of the
uncompressed tar archive:

| Member | Header offset | Data offset | Size | SHA-256 |
| --- | ---: | ---: | ---: | --- |
| `OP1_vdk.ldr` | 23,926,272 | 23,926,784 | 2,171,624 | `f79d299f6f9019c998013a4176223a6a1bc999a2d8c54451346861f6ea455c74` |
| `te-boot.ldr` | 26,098,688 | 26,099,200 | 261,604 | `f6233b974bdb6c92f39e73401f206721924ef3ce52abd66b642a5350cebcad93` |

The main image is the exact, case-sensitive root member `OP1_vdk.ldr`, not a
file below `content/`.

## Safe Inspection And Extraction

Treat all firmware as untrusted input. Inspection should work without writing
derived files; extraction should require an explicit request.

1. Read the file size with checked 64-bit arithmetic and require at least the
   four-byte wrapper plus a complete 13-byte LZMA-Alone header and payload.
2. Decode the stored CRC with an explicit little-endian read. Stream CRC-32 over
   all remaining bytes and reject a mismatch before decompression.
3. Parse LZMA-Alone explicitly. Validate properties and dictionary size before
   allocating. For a 246-only parser, accepting only the confirmed parameter
   tuple is the smallest evidence-backed policy.
4. Decode through a maintained liblzma API into a bounded stream or temporary
   file. Enforce configurable input, dictionary-memory, work/time, and output
   limits despite the unknown-size sentinel. A 64 MiB output cap is a
   conservative policy, not a format fact. Require a clean end marker and
   reject trailing or concatenated data.
5. Parse tar before extracting. Validate every header checksum, numeric field,
   size and padding calculation, archive boundary, terminator, and trailing
   byte. Cap member count, name length, member size, and aggregate size.
6. Canonicalize names as portable relative paths. Reject absolute, drive, UNC,
   empty, NUL-containing, backslash-separated, `.` or `..` paths; duplicate
   normalized names; and file/directory collisions.
7. Initially accept only regular files and directories. Reject links, devices,
   FIFOs, sparse files, GNU long-name records, PAX overrides, and unknown types
   until deliberately supported.
8. Extract under a private staging directory through no-follow, directory-
   anchored APIs. Apply safe host modes rather than archived metadata, and
   publish atomically only after complete validation. Never use an unrestricted
   `extractall` operation.
9. Locate `OP1_vdk.ldr` by exact root path and require exactly one regular
   member. Missing, duplicate, wrong-case, nested, or wrong-type lookalikes are
   errors.

Malformed-input tests should use synthetic, redistributable fixtures. Cover
truncation at every layer, stale and wrongly ranged CRCs, malformed properties,
dictionary and output bombs, missing end markers, trailing streams, tar integer
overflow and truncation, unsafe paths, duplicate names, special members, and
publication cleanup. Firmware-dependent tests must remain optional and skip
cleanly when the local image is absent.

## Unresolved Questions

- **UNKNOWN:** whether all earlier official OP-1 releases use the exact wrapper,
  compression tuple, archive dialect, and practical limits seen in 246.
- **UNKNOWN:** which encoder settings beyond fields encoded in the LZMA header
  produced the official stream.
- **UNKNOWN:** the meaning of older revision strings in `OP1_vdk.ldr`.
- LDR block structure and execution semantics are documented separately in
  [ldr-format.md](ldr-format.md).

## Sources

- [Teenage Engineering original OP-1 downloads](https://teenage.engineering/downloads/op-1)
- [Official firmware 246 object](https://teenage.engineering/_software/op-1/op1_246.op1)
- [Pinned op1hacks format notes](https://github.com/op1hacks/docs/blob/38685982be029f678d9da1d01034a7331ec77808/README.md#firmware-description)
- [Pinned op1repacker implementation](https://github.com/op1hacks/op1repacker/blob/3bcffc054423bdf5020f3cbb137228c0c3b60713/op1repacker/op1_repack.py)
- [Pinned LZMA-Alone specification](https://github.com/tukaani-project/xz/blob/17aa2e1a796d3f758802df29afc89dcf335db567/doc/lzma-file-format.txt)
- [GNU tar format documentation](https://www.gnu.org/software/tar/manual/html_node/Standard.html)
- [Python CRC-32 documentation](https://docs.python.org/3/library/binascii.html#binascii.crc32)
