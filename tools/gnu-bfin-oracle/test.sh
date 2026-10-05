#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if "$script_dir/smoke.sh"; then
  exit 0
else
  status=$?
  if [ "$status" -eq 77 ]; then
    exit 0
  fi
  exit "$status"
fi
