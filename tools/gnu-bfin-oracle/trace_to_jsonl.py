#!/usr/bin/env python3
"""Convert proven instruction observations from GNU sim text trace to JSONL."""

import json
import re
import sys


INSN_RE = re.compile(
    r"^disasm:\s+(?P<pc>0x[0-9a-fA-F]+)\s+-(?P<disasm>.*)$"
)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} TRACE", file=sys.stderr)
        return 2

    sequence = 0
    with open(sys.argv[1], encoding="utf-8", errors="replace") as trace:
        for line in trace:
            match = INSN_RE.match(line.rstrip())
            if match is None:
                continue
            event = {
                "seq": sequence,
                "pc": match.group("pc").lower(),
                "instruction": {
                    "disassembly": match.group("disasm").strip(),
                },
            }
            print(json.dumps(event, separators=(",", ":")))
            sequence += 1

    if sequence == 0:
        print("error: no instruction records recognized", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
