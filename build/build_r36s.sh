#!/bin/sh
# R36S build (Git Bash on Windows): zig c++ -> aarch64-linux-gnu with glibc 2.28.
# SDL2 2.0.9 headers + libSDL2.so were copied out of the Debian buster arm64 sysroot in WSL
# (zig itself does not run under WSL1: CacheCheckFailed). WSL is only used for readelf/qemu.
# Usage: sh build/build_r36s.sh <target>   targets: see build/sources.sh

# the repository as WSL sees it (C:\dir -> /mnt/c/dir), so this works wherever the repository was cloned.
# Two steps on purpose: `A 2>/dev/null || B && C` runs C after a SUCCESSFUL A too, and the variable then holds
# two lines - which reaches `sh -c` as a broken multi-line script.
REPO_DIR=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cd "$REPO_DIR" && pwd -W 2>/dev/null) || REPO_WIN=$REPO_DIR
REPO_DRIVE_UPPER=$(printf '%s' "$REPO_WIN" | cut -c1)
REPO_DRIVE=$(printf '%s' "$REPO_DRIVE_UPPER" | tr 'A-Z' 'a-z')
REPO_WSL="/mnt/$REPO_DRIVE$(printf '%s' "$REPO_WIN" | cut -c3- | tr '\\' '/')"

set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd -W)
TOOLS=${CROSSY_TOOLS:-$(cygpath -m "$LOCALAPPDATA")/BobrHopper/tools}
ZIG="$TOOLS/zig-x86_64-windows-0.14.1/zig.exe"
SDL="$TOOLS/r36s-sdl2"
OUT="$ROOT/out/r36s"
TARGET=${1:-probe}
[ -f "$SDL/lib/libSDL2.so" ] || { echo "missing $SDL: run sh build/setup_tools.sh"; exit 1; }
mkdir -p "$OUT"

. "$ROOT/build/sources.sh"
SOURCES=$(sources_for "$TARGET") || { echo "unknown target $TARGET"; exit 1; }

SRC_ABS=""
for s in $SOURCES; do SRC_ABS="$SRC_ABS $ROOT/$s"; done

BIN="$OUT/$TARGET.aarch64"
"$ZIG" c++ -target aarch64-linux-gnu.2.28 -mcpu=cortex_a35 -std=c++17 -O2 -g0 -Wall -ffp-contract=off \
  -D_REENTRANT -I"$SDL/include/SDL2" -I"$ROOT/src" \
  $SRC_ABS -L"$SDL/lib" -lSDL2 -Wl,--allow-shlib-undefined -o "$BIN"

MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu-22.04 -e sh -c \
  "ls '$REPO_WSL' >/dev/null 2>&1 || sudo -n mount -t drvfs ${REPO_DRIVE_UPPER}: /mnt/${REPO_DRIVE}; sh $REPO_WSL/build/check_elf.sh $REPO_WSL/out/r36s/$TARGET.aarch64"
