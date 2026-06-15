#!/usr/bin/env bash
# Build the distributable Loqi binaries and package them with checksums.
#
#   Static Linux binaries (musl, via `zig cc`, run anywhere with no libc dependency):
#     - linux-x86_64
#     - linux-arm64
#   Native macOS binary (built only when running on macOS arm64):
#     - macos-arm64
#
# Output: dist/loqi-<version>-<target>.tar.gz (+ LICENSE, README) and dist/SHA256SUMS.
#
# Reproducible locally:
#   brew install zig            # or: download from https://ziglang.org/download/
#   LOQI_RELEASE_VERSION=v0.2.0 ./scripts/release.sh
#
# The tag-triggered GitHub release (.github/workflows/release.yml) runs this on a Linux
# runner (the two musl targets) and a macOS runner (macos-arm64), then verifies each
# runnable binary's `--version` matches the tag before publishing.
set -euo pipefail
cd "$(dirname "$0")/.."

err() { printf 'release: %s\n' "$1" >&2; exit 1; }

VERSION="${LOQI_RELEASE_VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}"
SRC="src/loqi.c"
ZIG="${ZIG:-zig}"
OUT="dist"

CFLAGS=(-std=c11 -O2 -DNDEBUG -Isrc)

rm -rf "$OUT"
mkdir -p "$OUT"

# pack <binary-path> <target-name>: stage the binary as `loqi` alongside LICENSE/README
# and tar it up as loqi-<version>-<target>.tar.gz.
pack() {
  local bin="$1" target="$2" stage="$OUT/loqi-$VERSION-$2"
  mkdir -p "$stage"
  cp "$bin" "$stage/loqi"
  [ -f LICENSE ]   && cp LICENSE "$stage/"   || true
  [ -f README.md ] && cp README.md "$stage/" || true
  tar -C "$OUT" -czf "$OUT/loqi-$VERSION-$target.tar.gz" "loqi-$VERSION-$target"
  rm -rf "$stage"
  echo "  packed $OUT/loqi-$VERSION-$target.tar.gz"
}

built=0

# build_linux <zig-target> <target-name>: cross-compile a static musl binary.
build_linux() {
  echo "==> $2 (musl, static)"
  "$ZIG" cc -target "$1" "${CFLAGS[@]}" "$SRC" -lm -o "$OUT/loqi"
  if ! file "$OUT/loqi" | grep -q "statically linked"; then
    echo "  ERROR: $2 binary is not statically linked" >&2
    file "$OUT/loqi" >&2
    exit 1
  fi
  pack "$OUT/loqi" "$2"
  rm -f "$OUT/loqi"
  built=$((built + 1))
}

# Linux musl targets need zig. A missing zig is a HARD error by default (you asked for a
# release, so a silent macOS-only dist must never look complete); LOQI_SKIP_LINUX=1 is
# the explicit opt-out for a native-only build (e.g. the macOS CI runner has no zig).
if command -v "$ZIG" >/dev/null 2>&1; then
  build_linux x86_64-linux-musl  linux-x86_64
  build_linux aarch64-linux-musl linux-arm64
elif [ "${LOQI_SKIP_LINUX:-0}" = "1" ]; then
  echo "  LOQI_SKIP_LINUX=1: skipping the Linux targets" >&2
else
  err "zig not found (the Linux targets need it). Install zig, set ZIG=/path/to/zig, or set LOQI_SKIP_LINUX=1 to build only the native target."
fi

# macOS arm64: native clang (zig can't link macOS frameworks without the SDK).
if [ "$(uname -s)" = "Darwin" ] && [ "$(uname -m)" = "arm64" ]; then
  echo "==> macos-arm64 (native)"
  clang "${CFLAGS[@]}" "$SRC" -lm -o "$OUT/loqi"
  "$OUT/loqi" --version >/dev/null
  pack "$OUT/loqi" "macos-arm64"
  rm -f "$OUT/loqi"
  built=$((built + 1))
fi

[ "$built" -gt 0 ] || err "no targets could be built on this host (need zig for the Linux targets, or a macOS arm64 host for macos-arm64)"

# Checksums (shasum on macOS, sha256sum on Linux; identical "<hex>  <file>" format).
# Filenames start with "loqi-" so the glob can never expand to a leading-dash option.
(
  cd "$OUT"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum loqi-*.tar.gz > SHA256SUMS
  else
    shasum -a 256 loqi-*.tar.gz > SHA256SUMS
  fi
)

echo "==> dist/ (version $VERSION):"
ls -1 "$OUT"
echo "==> SHA256SUMS:"
cat "$OUT/SHA256SUMS"
