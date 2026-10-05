# Blackfin BF52x LDR Format

This document describes the BF52x loader stream used by the original OP-1's
ADSP-BF524 and records observations from firmware 246. It deliberately
separates physical file records from records executed by the Boot ROM. It does
not define or implement decryption.

## Confidence

- **CONFIRMED**: established by the BF52x hardware reference, corroborating
  ADI-derived tooling, and/or direct structural observation.
- **LIKELY**: supported by multiple observations but not established by an
  authoritative OP-1 source or controlled execution trace.
- **ASSUMED**: a working interpretation that must not drive emulation as fact.
- **UNKNOWN**: evidence is insufficient.

## Sources And Scope

The primary format authority is chapter 17 of the Analog Devices
*ADSP-BF52x Blackfin Processor Hardware Reference*. The GPL-2.0 `ldr-utils`
implementation at commit `2253efd17594fe3943ed31cf37b0dce0f61ada8c`
corroborates the layout: its BF527 family aliases include BF524 and use the
BF54x-format block routines. Local observations concern the 2,171,624-byte
firmware 246 `OP1_vdk.ldr`, SHA-256
`f79d299f6f9019c998013a4176223a6a1bc999a2d8c54451346861f6ea455c74`.

## Physical Record Format

An LDR is physically a sequence of variable-length records. Every record starts
with a 16-byte header of four little-endian 32-bit words:

| Offset | Size | Field |
| ---: | ---: | --- |
| `0x00` | 4 | block code |
| `0x04` | 4 | target address |
| `0x08` | 4 | byte count |
| `0x0c` | 4 | argument |

For a normal or `IGNORE` record, `byte_count` payload bytes physically follow
the header. A `FILL` record has no physical payload; `byte_count` is the number
of destination bytes synthesized by repeating the complete little-endian
32-bit `argument` pattern. Zero counts are valid.

The manual's prose refers once to a 32-byte header checksum, but its diagram,
`ldr-utils`, and all 801 firmware headers establish a 16-byte header.
**CONFIRMED:** the 31/32-byte wording is a documentation error.

### Block Code

| Bits | Mask | Name | Generic meaning |
| ---: | ---: | --- | --- |
| 0-3 | `0x0000000f` | `DMACODE` | Boot-source DMA width/source-modify code |
| 4 | `0x00000010` | `SAVE` | Hibernate/save hint; unused by on-chip Boot ROM |
| 5 | `0x00000020` | `AUX` | Second-stage-loader nesting; unused by on-chip Boot ROM |
| 6-7 | `0x000000c0` | reserved | Must not be assigned semantics |
| 8 | `0x00000100` | `FILL` | Generate destination data; no file payload |
| 9 | `0x00000200` | `QUICKBOOT` | Full-boot-only block |
| 10 | `0x00000400` | `CALLBACK` | Invoke registered callback after loading |
| 11 | `0x00000800` | `INIT` | Call target after optional payload load |
| 12 | `0x00001000` | `IGNORE` | Redirect/advance boot source rather than normal load |
| 13 | `0x00002000` | `INDIRECT` | Stage through an intermediate L1 buffer |
| 14 | `0x00004000` | `FIRST` | First block of a DXE |
| 15 | `0x00008000` | `FINAL` | Finish the DXE/boot and transfer control |
| 16-23 | `0x00ff0000` | `HDRCHK` | Header XOR checksum byte |
| 24-31 | `0xff000000` | `HDRSGN` | BF52x signature, fixed at `0xad` |

XORing all 16 header bytes, including `HDRCHK`, must produce zero. `DMACODE` 6
means 16-bit DMA with source modify 2. The Boot ROM tests this field on the
first block and ignores it later, although firmware 246 repeats 6 in every
physical header. `ldr-utils` calls bit 4 `SAFE`; use the vendor name `SAVE` in
BF52x-facing APIs and treat `safe` only as a tooling alias.

## Generic Boot ROM Semantics

These semantics describe execution, not merely physical parsing:

- A normal target receives payload bytes. `FILL` begins filling at the target.
- The manual says target and count should be divisible by four and states that
  `FILL` requires both to be divisible by four.
- `IGNORE` source-pointer behavior depends on boot mode. In master modes, count
  is a signed two's-complement offset; in slave modes, it must be positive and
  bytes are consumed. A file alone does not identify the mode.
- `INDIRECT` stages data in an L1 temporary buffer. The documented default is
  `0xff907e00..0xff907fff` (512 bytes), loaded only after indirect blocks finish.
- Scratchpad `0xffb00000..0xffb00fff` is unsupported as a boot destination.
  Boot ROM workspace `0xff807ff0..0xff807fff` is also unavailable while booting.
- `CALLBACK` invokes a function previously registered in Boot ROM boot data. A
  callback can perform CRC, decryption, or decompression, but the flag does not
  identify which operation is requested.
- `INIT` invokes boot-time code at the target. It is not the application entry;
  a zero-sized `INIT` may invoke already-resident code.
- `FIRST.target_address` is copied to `EVT1` and is the default application
  entry. `FIRST.argument` is a relative next-DXE pointer or the next free boot
  source location for a single-DXE stream. `FIRST` cannot combine with `FILL`;
  combining it with `IGNORE` is explicitly supported.
- `FINAL` ends normal Boot ROM processing, performs housekeeping, and by
  default jumps through `EVT1`. Initcode can customize exit behavior.

## Firmware 246 Physical Observations

The whole file can be walked as 801 structurally valid physical records that
land exactly at EOF:

| Observation | Value |
| --- | ---: |
| Physical headers | 801 |
| Header bytes | 12,816 |
| Physical payload bytes | 2,158,808 |
| Exact bytes consumed | 2,171,624 (`0x2122e8`) |
| Valid `0xad` signatures / zero XOR headers | 801 / 801 |
| DMA code 6 | 801 |
| Unknown or reserved flag bits | 0 occurrences |
| `FILL` | 413 |
| `CALLBACK` / `INDIRECT` | 46 / 46 |
| `IGNORE` | 2 |
| `FIRST` / `FINAL` | 1 / 1 |
| `INIT`, `SAVE`, `AUX`, `QUICKBOOT` | 0 each |
| Nonzero arguments | 178 |
| Unaligned targets | 0 |
| Counts not divisible by four | block 711 only |

Non-ignored loaded/fill targets span external SDRAM and on-chip L1 regions;
the overall observed span is `0x01000000..0xffa0bfb3`, not one contiguous
allocation.

### Physical Structure Versus Execution

The boundary is explicit:

| Physical block | File offset | Block code | Target | Count | Argument | Flags |
| ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1 | `0x00000000` | `0xad7f5006` | `0xffa00000` | `0` | `0x002122d8` | `IGNORE|FIRST` |
| 800 | `0x002121d8` | `0xad748006` | `0xffa00000` | `0` | `0` | `FINAL` |
| 801 | `0x002121e8` | `0xad7e3406` | `0xff907e00` | `0xf0` | `0` | `CALLBACK|IGNORE|INDIRECT` |

**CONFIRMED physical facts:** record 801 is a coherent 16-byte header plus 240
payload bytes, all physical records reach exact EOF, and the first record's
extent resolves as `0x10 + 0x2122d8 = 0x2122e8`. The post-final 256 bytes are
therefore deliberately included in the physical DXE extent.

**CONFIRMED generic execution boundary:** ordinary Boot ROM control flow exits
at block 800's `FINAL` and jumps through `EVT1`. It does not continue to record
801 as a subsequent executed boot block. Under those generic semantics,
`EVT1` is `0xffa00000`; this does not establish useful firmware execution.

**UNKNOWN:** which OP-1 component, tool, or earlier stage owns the post-final
record. Its payload has ten 24-byte-shaped units, each with eight nonzero bytes
followed by sixteen zero bytes, and targets the default indirect-buffer base.
Those structural facts do not prove that Boot ROM loads it or identify its
contents.

### Callback And Encryption Boundary

Blocks 712 through 799 include 45 pre-final `CALLBACK|INDIRECT` records; the
post-final record makes 46 physical records and 80,564 physical payload bytes.
No `INIT` record occurs in this LDR.

Community research states that OP-1 code uses an 8-of-24 modified-XTEA framing
and a key associated with BF524 OTP page `0xd0`. Local flag patterns and entropy
measurements are compatible with that report but do not prove its semantics.

- **LIKELY:** callback/indirect code payloads use the community-described
  8-of-24 framing.
- **UNKNOWN:** cipher variant, key derivation, callback address and registration,
  chaining, padding, and treatment of short records.
- **UNKNOWN:** how a callback is registered despite the absence of local
  `INIT`; the separate bootloader, OTP preboot code, or prior boot state are
  possibilities, not facts.

Do not implement or assert decryption from these observations. Header XOR,
payload callbacks, NAND ECC, BF52x Lockbox ECDSA/SHA-1, and the community XTEA
claim are separate mechanisms.

### Alignment Exception

Physical block 711 is a `FILL` to `0x03000000` with count `0x0003ffff`, the only
count not divisible by four. Its signature and checksum are valid, and treating
it as a no-payload fill is required to reach the next header and exact EOF.

This is a **CONFIRMED firmware exception** to the manual's stated `FILL`
alignment requirement. Inspectors should warn without rounding, rejecting
firmware 246, or inventing partial-word behavior. Actual Boot ROM handling of
the final three bytes is **UNKNOWN**.

## Safe Inspector Requirements

The first implementation milestone should inspect metadata only, not load or
execute firmware.

1. Require an explicitly selected BF52x format; one `0xad` byte is not safe
   autodetection.
2. Decode headers with explicit little-endian reads and subtraction-style bounds
   checks. Never cast arbitrary bytes to native structs.
3. Validate signature, all-byte XOR, reserved bits, known flags, DMA code,
   target, count, argument, and physical offset.
4. Advance by 16 bytes for `FILL`, otherwise by 16 plus `byte_count`. Skip or
   hash payload in constant memory rather than allocating according to count.
5. Track `FIRST` and `FINAL`, report duplicate or missing markers, and resolve
   the relative DXE extent with checked arithmetic.
6. Stop Boot ROM execution semantics at the first `FINAL`. Report and optionally
   structurally decode post-final bytes, but always label them non-executed.
7. Warn on alignment violations and forbidden or reserved target ranges. Keep
   the block-711 exception visible rather than normalizing it.
8. Report `CALLBACK`, `INIT`, and `INDIRECT` only as requests. Do not call guest
   addresses, write guest memory, decrypt, decompress, or infer callback state.
9. Return `valid`, `valid-with-warnings`, or `invalid` metadata without emitting
   payload. Firmware 246 is `valid-with-warnings` due to the odd fill count,
   post-final data, callback-without-local-init observation, and unknown OP-1
   semantics.

Synthetic tests must cover truncated headers and payloads, `UINT32_MAX` counts,
big-endian words, invalid signatures/checksums/reserved bits, `FILL` physical
length, odd counts, address wrap, invalid `FIRST|FILL`, missing or duplicate
markers, opaque and block-shaped post-final bytes, signed `IGNORE` counts,
scratch/workspace targets, and callback-without-`INIT`. Fuzz under ASan/UBSan;
require monotonic offsets, bounded memory, deterministic diagnostics, and no
payload output.

## Unresolved Questions

- What registers the OP-1 callback when the main LDR has no `INIT`?
- What exact callback and transformation process the 8-of-24 data?
- What owns or consumes the post-final 256-byte record?
- How does BF524 Boot ROM handle block 711's final partial fill word?
- Is this physical structure stable across other official OP-1 releases?

None of these questions blocks a read-only inspector. All block loading,
callback execution, decryption, and CPU entry remain out of scope until
supported by public evidence or controlled traces.

## Sources

- Link check note (2026-10-04): the authoritative Analog Devices PDF URL below
  timed out during live verification; it is retained from the verified research
  source hierarchy.
- [ADSP-BF52x Blackfin Processor Hardware Reference, Rev. 1.2](https://www.analog.com/media/en/dsp-documentation/processor-manuals/ADSP-BF52x_hwr_rev1.2.pdf)
- [`ldr-utils` at the inspected commit](https://github.com/neuschaefer/ldr-utils/tree/2253efd17594fe3943ed31cf37b0dce0f61ada8c)
- [BF524 family mapping in `ldr-utils`](https://github.com/neuschaefer/ldr-utils/blob/2253efd17594fe3943ed31cf37b0dce0f61ada8c/lfd_bf527.c#L14-L25)
- [BF52x-compatible block implementation](https://github.com/neuschaefer/ldr-utils/blob/2253efd17594fe3943ed31cf37b0dce0f61ada8c/lfd_bf548.c#L14-L85)
- [Community OP-1 research](https://github.com/sualk/op1-docs)
- [Community repacker](https://github.com/op1hacks/op1repacker)
- [Community hardware/dump research](https://github.com/Tolsi/op1dumps)
