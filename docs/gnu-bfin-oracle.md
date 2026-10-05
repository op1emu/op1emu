# GNU Blackfin Execution Oracle

This developer-only environment runs redistributable synthetic Blackfin code
in GNU GDB 17.2's simulator as an external process. No GNU simulator source is
copied into or linked with the emulator.

## Confidence and scope

- **CONFIRMED:** the official GDB 17.2 release provides `sim/bfin`, an explicit
  `bf524` model, and a standalone `bfin-elf-run` executable.
- **CONFIRMED:** GDB 17.2 defaults to `bf537`; every OP-1 oracle invocation must
  pass `--model bf524`.
- **CONFIRMED:** the BF524 model maps data SRAM at `0xff800000` and instruction
  SRAM at `0xffa00000`.
- **CONFIRMED:** the model represents BF52x devices but leaves TIMER, SPORT,
  PORT_MUX, and MUSB address ranges as read/write stubs. Model selection is not
  evidence of complete BF524 or OP-1 hardware behavior.
- **UNKNOWN:** behavior not covered by the synthetic fixture, especially
  undocumented silicon behavior and OP-1 board peripherals.

This setup does not run proprietary firmware, select an embedded CPU core, or
make GNU sim part of the emulator. GNU sources, build trees, binaries, traces,
and fixture outputs stay under the repository's ignored `.cache/` directory by
default.

## Pinned sources

The build script downloads over HTTPS and verifies SHA-256 before extraction or
building:

| Component | Official source | SHA-256 |
| --- | --- | --- |
| GDB 17.2 | `https://ftp.gnu.org/gnu/gdb/gdb-17.2.tar.xz` | `1c036c0d72e4b3d1fb5c94c88632add6f9d76f4d7c4d2ea793c12a9f19a3228c` |
| binutils 2.44 | `https://ftp.gnu.org/gnu/binutils/binutils-2.44.tar.xz` | `ce2017e059d63e67ddb9240e9d4ec49c2893605035cd60e92ad53177f4377237` |

The GDB digest is independently published in the GNU `info-gnu` GDB 17.2
release announcement. The GDB release archive excludes GAS, LD, and the
`binutils` command-line programs, so the separately pinned Blackfin-capable
binutils release supplies `bfin-elf-as`, `bfin-elf-ld`, and
`bfin-elf-objdump`.

## Host prerequisites

On Debian or Ubuntu, install the equivalent of:

```sh
sudo apt-get install build-essential bison flex texinfo curl xz-utils \
  libgmp-dev libmpfr-dev libexpat1-dev libncurses-dev
```

The build requires a C/C++ compiler, GNU Make, Bison, Flex, Texinfo, `curl`,
`tar`, `xz`, and `sha256sum`. It does not require a preinstalled Blackfin
toolchain. The script accepts `JOBS`, `GNU_BFIN_ORACLE_CACHE`, and
`GNU_BFIN_ORACLE_PREFIX`; paths default to `.cache/gnu-bfin-oracle`.

## Build and smoke test

```sh
tools/gnu-bfin-oracle/build.sh
tools/gnu-bfin-oracle/smoke.sh
```

The build uses `--target=bfin-elf`, enables GDB's simulator, disables GDB and
unneeded programs, and builds only `all-sim`, `all-binutils`, `all-gas`, and
`all-ld` plus their install targets. The smoke test assembles and links
`fixture/oracle-smoke.s`, disassembles it, and executes it as:

```sh
GDB_SRC="$PWD/.cache/gnu-bfin-oracle/gdb-17.2"
GDB_BUILD="$PWD/.cache/gnu-bfin-oracle/gdb-build"
PREFIX="$PWD/.cache/gnu-bfin-oracle/install"
mkdir -p "$GDB_BUILD"
(cd "$GDB_BUILD" && "$GDB_SRC/configure" --target=bfin-elf \
  --prefix="$PREFIX" --disable-gdb --disable-gdbserver --disable-gold \
  --disable-gprof --disable-gprofng --disable-nls --disable-werror \
  --enable-sim --without-debuginfod --without-zstd)
make -C "$GDB_BUILD" all-sim
make -C "$GDB_BUILD" install-sim

BINUTILS_SRC="$PWD/.cache/gnu-bfin-oracle/binutils-2.44"
BINUTILS_BUILD="$PWD/.cache/gnu-bfin-oracle/binutils-build"
mkdir -p "$BINUTILS_BUILD"
(cd "$BINUTILS_BUILD" && "$BINUTILS_SRC/configure" --target=bfin-elf \
  --prefix="$PREFIX" --disable-gdb --disable-gdbserver --disable-gold \
  --disable-gprof --disable-gprofng --disable-nls --disable-shared \
  --disable-werror --without-debuginfod --without-zstd)
make -C "$BINUTILS_BUILD" all-binutils all-gas all-ld
make -C "$BINUTILS_BUILD" install-binutils install-gas install-ld
```

These are the configure and build commands used by `build.sh`; the script adds
parallel `make` jobs. After building, the smoke test executes the simulator as:

```sh
.cache/gnu-bfin-oracle/install/bin/bfin-elf-run \
  --model bf524 --trace-insn --trace-disasm --trace-register \
  --trace-memory --trace-events --trace-file TRACE FIXTURE.elf
```

The fixture clears its working registers, constructs `R0 = 0x12345678`,
computes `R1 = 12`, stores and reloads `0x12345678`, then stores and reloads
`0x12345684`. Its linker script places code at `0x1000` and data at `0x2000`;
GNU sim creates ELF-backed memory there, so this proves ISA execution and not
physical BF524 SRAM behavior. It exits through GNU sim's `EXCPT 0` syscall
bridge with status zero. A zero process status plus both load-back values in
the register trace proves that the simulator executed arithmetic and memory
effects rather than merely accepting the ELF file.

`test.sh` is suitable for an automated optional check. It prints `SKIP` and
returns success when the external tools are absent; `smoke.sh` itself returns
77 for that condition so test harnesses can distinguish a skip.

## Noninteractive observations

The standalone runner has no interactive stepping prompt. Its per-instruction
trace is the practical deterministic stepping interface:

```sh
bfin-elf-run --model bf524 --trace-insn --trace-disasm \
  --trace-register --trace-memory --trace-events --trace-file trace.txt test.elf
```

- `--trace-insn --trace-disasm` records each executed PC and disassembly.
- `--trace-register` records register accesses and resulting values.
- `--trace-memory` enables the common simulator memory trace category, but GDB
  17.2's Blackfin target does not print ordinary architectural loads/stores in
  this fixture. Store effects are observed by explicit load-back instructions.
- `--trace-events` records simulator events, including exception activity.
- `--watch-pc-break ADDRESS` stops when the PC reaches an address; use
  `--watch-pc-break !START,END` to guard an expected PC range.
- `bfin-elf-objdump -d -s test.elf` gives instruction bytes, disassembly, and
  section contents independently of execution.

The runner does not expose a general pre-run command for arbitrary register or
memory writes. Synthetic tests should establish controlled state in assembly
and initialized ELF sections, as the fixture does. This is more reproducible
than depending on undocumented `--do-command` internals. A future need for
interactive mutation should justify building `bfin-elf-gdb`; it is not needed
for this minimal oracle.

## JSON Lines trace

`trace.schema.json` defines trace format version 1. Each line is one JSON
object with required monotonic `seq` and hexadecimal `pc`. Optional fields can
carry instruction bytes/disassembly, changed registers, memory reads/writes,
and exception/interrupt events. Hexadecimal strings avoid JSON number-width
ambiguity.

`trace_to_jsonl.py` currently emits only sequence, PC, and disassembly because
those fields are directly present in pinned GNU sim `disasm:` records.
Instruction bytes remain representable in the schema and available from
`bfin-elf-objdump`, but are not present in native run trace records. The adapter
intentionally does not infer bytes or guess how verbose register and memory
lines group with multi-issue packets. Extend it only alongside fixtures that
prove such grouping.

## Known limitations

- GNU sim is an oracle, not physical BF524 hardware. Vendor documentation and
  controlled hardware results outrank it when they disagree.
- The simulator has incomplete peripherals and a restricted MMU model.
- Known DSP parallel-packet ordering behavior requires a dedicated regression
  before those packets are trusted for differential results.
- Trace text is a GNU sim diagnostic interface, not a promised stable API. The
  smoke test fails if the pinned output no longer matches the adapter.
- Timing and host syscall behavior are not OP-1 platform behavior.
- GPL source and generated binaries remain external build artifacts. Do not
  copy simulator implementation into emulator code or distribute a linked
  combined work without a separate licensing decision.
