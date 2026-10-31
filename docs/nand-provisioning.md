# Provision an OP-1 NAND image (experimental)

`op1emu` emulates NAND flash; it does **not** automatically install the factory
files contained in an `.op1` update archive. This tool prepares a **new** NAND
image containing the original firmware's `content/` directory, laid out as a
YAFFS2 filesystem.

The result uses op1emu's MT29F4G08 format: 262,144 × 2048-byte data pages,
followed by 262,144 × 64-byte out-of-band (OOB) records (528 MiB total).
It does **not** modify the emulator's CPU, NAND controller, or YAFFS code.

## Requirements

- Python 3 with standard-library `lzma` support
- A working `mkyaffs2` executable from [yaffs2utils](https://github.com/ljvblfz/yaffs2utils)
- Your own original OP-1 firmware package or an already-unpacked `content/` directory

For example, on macOS:

```sh
git clone https://github.com/ljvblfz/yaffs2utils.git
make -C yaffs2utils CC=clang mkyaffs2
```

**macOS caveat:** This older version of yaffs2utils has a directory traversal
portability issue: the two `dirname(mkyaffs2_curfile)` calls in `mkyaffs2.c`
need to restore the path by modifying the original buffer rather than
assuming `dirname()` does it. If scanning succeeds but image creation fails
at a nested directory, fix both instances by replacing:

```c
if (!strcmp(dirname(mkyaffs2_curfile), "."))
    mkyaffs2_curfile[0] = '\0';
```

with:

```c
char *slash = strrchr(mkyaffs2_curfile, '/');
if (slash) *slash = '\0';
else mkyaffs2_curfile[0] = '\0';
```

Then rebuild `mkyaffs2`. This workaround was used for the macOS proof of concept;
the upstream utility is not bundled in this repository.

## Usage

From the op1emu repository root, with a local firmware update package:

```sh
python3 tools/provision_nand.py \
  --firmware /path/to/op1_246.op1 \
  --mkyaffs2 ./yaffs2utils/mkyaffs2 \
  --output nand-246.bin
```

Or use an already-extracted `content/` directory:

```sh
python3 tools/provision_nand.py \
  --content-dir /path/to/unpacked/content \
  --mkyaffs2 ./yaffs2utils/mkyaffs2 \
  --output nand-246.bin
```

`--firmware` validates the package's little-endian CRC32, decompresses its
LZMA-alone payload, and extracts only safe regular files/directories under
`content/`. Both modes run `mkyaffs2` with 2048-byte pages and 64-byte OOB.
The converter relocates 28-byte YAFFS2 tags from OOB offset 2 to offset 4
and writes a split data/OOB NAND image. An existing output is **never overwritten**.
If conversion fails, the partially created output is removed. A failed exclusive
open leaves a competing process's output untouched.

Package extraction preserves each tar member's mode and modification time.
Directory metadata is applied after its children; the staging root and implicit
parent directories use mode 0755 and timestamp zero so extraction time and
umask do not affect the image. Because host change time cannot be restored and
directory scans can update access time, generated YAFFS2 object headers use the
archived modification time for all three timestamps. Reproducibility comparisons require the same
`mkyaffs2` version and options.

The default YAFFS starting block is **132**. This was established by an
empirical OP-1 firmware 246 boot test on an Apple Silicon Mac, not by a
complete proof of the firmware's partition-map internals. You can specify
`--start-block N` for independent experiments. Other firmware revisions and
hardware configurations have not been verified.

### Running

The NAND image contains the factory assets **only**. `OP1_vdk.ldr` from the
official package contains encrypted blocks. Direct LDR boot must use a
compatible, legitimately obtained decrypted LDR, or normal boot must use the
proper OTP/firmware arrangement.

```sh
./build/op1emu nand-246.bin /path/to/plaintext.ldr --nand-snapshot
```

Use `--nand-snapshot` when testing; keep a golden image, then copy it if you
want to persist guest writes using `--nand-rw`.

Do not put NAND images, decrypted firmware, OTP dumps, or factory sound
archives in a pull request. These are user-provided inputs, never bundled.

## Tests and validation

```sh
python3 -m pytest tools/tests -q
```

The included unit tests use only synthetic page data and a synthetic
CRC32/LZMA/TAR package. They cover the split-OOB layout, unsafe overwrite
prevention (including a competing output creator), cleanup after invalid OOB,
package extraction, CRC rejection, and metadata repeatability across timestamps
and umasks. If `mkyaffs2` is on PATH, an additional synthetic integration test
compares SHA256s of repeated YAFFS2 and split-NAND images.

### Status / known gaps

- The manual proof of concept populated 11,810 YAFFS2 pages from 116 factory
  objects, and firmware 246 subsequently reached the synth UI and produced audio.
- The maintainer reports an end-to-end Linux run with firmware 246 and stock
  yaffs2utils (`1edc168`), producing 11,810 pages and reaching the main screen
  behind a real device's boot area (blocks 0–131). The synth worked after a key
  press. This is a maintainer report, not local verification of this revision.
- `mkyaffs2` is an external dependency and requires a macOS portability fix
  for the tested version; removing that friction is a possible follow-up.
- This does not create a self-booting encrypted firmware NAND and does not
  replace the normal TE-BOOT installation mechanism.
