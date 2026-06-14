#!/usr/bin/env bash
# Build the Loqi reference interpreter. Tuned for Apple Silicon.
set -euo pipefail
cd "$(dirname "$0")/.."

MODE="${1:-release}"
mkdir -p build

COMMON="-std=c11 -Wall -Wextra -Wno-unused-parameter -Isrc"

case "$MODE" in
  release)
    # -O3 + native arch + LTO for a fast Apple Silicon binary.
    FLAGS="-O3 -mcpu=apple-m1 -flto -DNDEBUG"
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
