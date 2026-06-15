#!/usr/bin/env bash
# Build the Loqi reference interpreter. Tuned for Apple Silicon.
set -euo pipefail
cd "$(dirname "$0")/.."

MODE="${1:-release}"
mkdir -p build

COMMON="-std=c11 -Wall -Wextra -Wno-unused-parameter -Isrc"

case "$MODE" in
  release)
    # Portable release by default (builds on macOS + Linux, any CPU). Set
    # LOQI_NATIVE=1 to tune for the host's Apple Silicon (-mcpu=apple-m1 + LTO).
    FLAGS="-O2 -DNDEBUG"
    if [ "${LOQI_NATIVE:-0}" = "1" ]; then
      FLAGS="-O3 -mcpu=apple-m1 -flto -DNDEBUG"
    fi
    ;;
  debug)
    FLAGS="-O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer"
    ;;
  *)
    echo "usage: build.sh [release|debug]" >&2; exit 2;;
esac

echo "==> Building loqi ($MODE)"
clang $COMMON $FLAGS src/loqi.c -lm -o build/loqi
echo "==> Done: build/loqi"
build/loqi --version
