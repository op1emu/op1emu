#!/bin/sh
set -eu

GDB_VERSION=17.2
GDB_ARCHIVE="gdb-${GDB_VERSION}.tar.xz"
GDB_URL="https://ftp.gnu.org/gnu/gdb/${GDB_ARCHIVE}"
GDB_SHA256=1c036c0d72e4b3d1fb5c94c88632add6f9d76f4d7c4d2ea793c12a9f19a3228c

BINUTILS_VERSION=2.44
BINUTILS_ARCHIVE="binutils-${BINUTILS_VERSION}.tar.xz"
BINUTILS_URL="https://ftp.gnu.org/gnu/binutils/${BINUTILS_ARCHIVE}"
BINUTILS_SHA256=ce2017e059d63e67ddb9240e9d4ec49c2893605035cd60e92ad53177f4377237

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
cache_dir=${GNU_BFIN_ORACLE_CACHE:-"$repo_root/.cache/gnu-bfin-oracle"}
prefix=${GNU_BFIN_ORACLE_PREFIX:-"$cache_dir/install"}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '%s\n' 1)}

need_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    printf 'error: required host command not found: %s\n' "$1" >&2
    exit 1
  fi
}

fetch_and_verify() {
  url=$1
  digest=$2
  archive=$3

  if [ ! -f "$archive" ]; then
    curl --fail --location --proto '=https' --tlsv1.2 --output "$archive.tmp" "$url"
    mv "$archive.tmp" "$archive"
  fi
  printf '%s  %s\n' "$digest" "$archive" | sha256sum --check --status || {
    printf 'error: SHA-256 mismatch for %s\n' "$archive" >&2
    exit 1
  }
}

for command in curl sha256sum tar make gcc g++ bison flex makeinfo; do
  need_command "$command"
done

mkdir -p "$cache_dir" "$prefix"

gdb_archive="$cache_dir/$GDB_ARCHIVE"
fetch_and_verify "$GDB_URL" "$GDB_SHA256" "$gdb_archive"
if [ ! -d "$cache_dir/gdb-$GDB_VERSION" ]; then
  tar -C "$cache_dir" -xf "$gdb_archive"
fi

gdb_build="$cache_dir/gdb-build"
mkdir -p "$gdb_build"
if [ ! -f "$gdb_build/Makefile" ]; then
  (
    cd "$gdb_build"
    "$cache_dir/gdb-$GDB_VERSION/configure" \
      --target=bfin-elf \
      --prefix="$prefix" \
      --disable-gdb \
      --disable-gdbserver \
      --disable-gold \
      --disable-gprof \
      --disable-gprofng \
      --disable-nls \
      --disable-werror \
      --enable-sim \
      --without-debuginfod \
      --without-zstd
  )
fi
make -C "$gdb_build" -j"$jobs" all-sim
make -C "$gdb_build" install-sim

binutils_archive="$cache_dir/$BINUTILS_ARCHIVE"
fetch_and_verify "$BINUTILS_URL" "$BINUTILS_SHA256" "$binutils_archive"
if [ ! -d "$cache_dir/binutils-$BINUTILS_VERSION" ]; then
  tar -C "$cache_dir" -xf "$binutils_archive"
fi

binutils_build="$cache_dir/binutils-build"
mkdir -p "$binutils_build"
if [ ! -f "$binutils_build/Makefile" ]; then
  (
    cd "$binutils_build"
    "$cache_dir/binutils-$BINUTILS_VERSION/configure" \
      --target=bfin-elf \
      --prefix="$prefix" \
      --disable-gdb \
      --disable-gdbserver \
      --disable-gold \
      --disable-gprof \
      --disable-gprofng \
      --disable-nls \
      --disable-shared \
      --disable-werror \
      --without-debuginfod \
      --without-zstd
  )
fi
make -C "$binutils_build" -j"$jobs" all-binutils all-gas all-ld
make -C "$binutils_build" install-binutils install-gas install-ld

printf 'GNU Blackfin oracle installed in %s\n' "$prefix"
