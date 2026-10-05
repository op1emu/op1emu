#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
prefix=${GNU_BFIN_ORACLE_PREFIX:-"$repo_root/.cache/gnu-bfin-oracle/install"}
output_dir=${GNU_BFIN_ORACLE_OUTPUT:-"$repo_root/.cache/gnu-bfin-oracle/smoke"}

for tool in as ld objdump run; do
  path="$prefix/bin/bfin-elf-$tool"
  if [ ! -x "$path" ]; then
    printf 'SKIP: GNU Blackfin oracle is absent (%s)\n' "$path"
    exit 77
  fi
done

mkdir -p "$output_dir"
object="$output_dir/oracle-smoke.o"
elf="$output_dir/oracle-smoke.elf"
disassembly="$output_dir/oracle-smoke.dis"
native_trace="$output_dir/oracle-smoke.trace"
jsonl_trace="$output_dir/oracle-smoke.jsonl"

"$prefix/bin/bfin-elf-run" --version | grep -q 'GNU simulator (SIM) 17.2'
"$prefix/bin/bfin-elf-as" --version | grep -q 'GNU assembler (GNU Binutils) 2.44'

"$prefix/bin/bfin-elf-as" -o "$object" "$script_dir/fixture/oracle-smoke.s"
"$prefix/bin/bfin-elf-ld" -T "$script_dir/fixture/oracle-smoke.ld" -o "$elf" "$object"
"$prefix/bin/bfin-elf-objdump" -d -s "$elf" >"$disassembly"

"$prefix/bin/bfin-elf-run" \
  --model bf524 \
  --trace-insn \
  --trace-disasm \
  --trace-register \
  --trace-memory \
  --trace-events \
  --trace-file "$native_trace" \
  "$elf"

"$script_dir/trace_to_jsonl.py" "$native_trace" >"$jsonl_trace"

grep -q 'wrote R2 = 0x12345678' "$native_trace"
grep -q 'wrote R5 = 0x12345684' "$native_trace"
grep -q 'processing exception 0 in EVT16' "$native_trace"
grep -q '"pc"' "$jsonl_trace"

printf 'PASS: GDB 17.2 bfin-elf-run executed the BF524 fixture\n'
printf '  ELF: %s\n  disassembly: %s\n  native trace: %s\n  JSONL trace: %s\n' \
  "$elf" "$disassembly" "$native_trace" "$jsonl_trace"
