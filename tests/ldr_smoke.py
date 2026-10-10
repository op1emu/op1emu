#!/usr/bin/env python3
"""End-to-end smoke test: run op1emu on a synthetic LDR and a missing NAND.

No real NAND, firmware or otp.bin is involved. The LDR is a single block that
holds one RTS, so the emulator must load it, JIT it, return to the sentinel
address and shut down cleanly.

usage: ldr_smoke.py OP1EMU LDRDUMP
"""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TIMEOUT_S = 60

BFLAG_FIRST = 0x00004000
BFLAG_FINAL = 0x00008000
L1_INSTRUCTION_BASE = 0xFFA00000
RTS = bytes([0x10, 0x00])


def make_ldr() -> bytes:
    header = struct.pack("<IIII", BFLAG_FIRST | BFLAG_FINAL, L1_INSTRUCTION_BASE, len(RTS), 0)
    return header + RTS


def tail(text: str, lines: int = 40) -> str:
    return "\n".join(text.splitlines()[-lines:])


def run(args, cwd):
    return subprocess.run(
        [str(a) for a in args], cwd=cwd, capture_output=True, text=True, timeout=TIMEOUT_S
    )


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    op1emu, ldrdump = (Path(p).resolve() for p in sys.argv[1:])

    with tempfile.TemporaryDirectory(prefix="op1-ldr-smoke-") as tmp:
        work = Path(tmp)
        (work / "gui").mkdir()
        (work / "gui" / "ui.json").symlink_to(ROOT / "gui" / "ui.json")
        ldr = work / "tiny.ldr"
        ldr.write_bytes(make_ldr())
        # Safety net only; the run is expected to end by itself.
        script = work / "script"
        script.write_text("wait 30000\nquit\n")

        dump = run([ldrdump, ldr], work)
        if dump.returncode != 0:
            print(f"ldrdump exited {dump.returncode}\n{tail(dump.stdout + dump.stderr)}")
            return 1

        # --nand-snapshot: a missing NAND is read as erased pages. Without it
        # the emulator would create a 528 MB erased image.
        emu = run(
            [op1emu, work / "missing.nand", ldr, "--nand-snapshot", "--headless",
             "--deterministic", "--input-script", script],
            work,
        )
        output = emu.stdout + emu.stderr
        if emu.returncode != 0:
            print(f"op1emu exited {emu.returncode}\n{tail(output)}")
            return 1
        if "Finished executing DXE" not in output:
            print(f"op1emu did not reach the end of the DXE\n{tail(output)}")
            return 1
        if (work / "missing.nand").exists():
            print("op1emu created a NAND image in snapshot mode")
            return 1

    print("ldr-smoke: ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.TimeoutExpired as e:
        print(f"timed out after {TIMEOUT_S}s: {e.cmd}")
        sys.exit(1)
