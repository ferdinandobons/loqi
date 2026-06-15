#!/bin/sh
# Loqi installer. Downloads the matching prebuilt static binary from the latest GitHub
# release, verifies its SHA256, and installs it to ~/.local/bin. POSIX sh, no deps but
# curl + tar.
#
#   curl -fsSL https://ferdinandobons.github.io/loqi/install.sh | sh
#
# Env overrides:
#   LOQI_VERSION=v0.2.0        install a specific tag instead of the latest
#   LOQI_INSTALL_DIR=/usr/bin  install somewhere else (default ~/.local/bin)
set -eu

REPO="ferdinandobons/loqi"
INSTALL_DIR="${LOQI_INSTALL_DIR:-$HOME/.local/bin}"

err() { printf 'loqi-install: %s\n' "$1" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || err "required tool not found: $1"; }

need curl
need tar
need uname

# Map the host to a release target name (must match scripts/release.sh).
os="$(uname -s)"
arch="$(uname -m)"
case "$os" in
  Darwin)
    case "$arch" in
      arm64|aarch64) target="macos-arm64" ;;
      *) err "unsupported macOS arch '$arch': prebuilt binaries are arm64-only; build from source with clang" ;;
    esac ;;
  Linux)
    case "$arch" in
      x86_64|amd64)  target="linux-x86_64" ;;
      aarch64|arm64) target="linux-arm64" ;;
      *) err "unsupported Linux arch '$arch'" ;;
    esac ;;
  *) err "unsupported OS '$os' (only macOS and Linux have prebuilt binaries; build from source)" ;;
esac

# Resolve the tag (latest release unless pinned). Parse the API without a jq dependency.
# Distinguish "API unreachable / rate-limited" from "parsed no tag" so the error is
# actionable: unauthenticated GitHub API is 60 req/hr per IP and is easy to exhaust.
tag="${LOQI_VERSION:-}"
if [ -z "$tag" ]; then
  api="$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest")" \
    || err "GitHub API request failed (network error, or the unauthenticated 60 req/hr per-IP rate limit). Retry later, or pin a version: LOQI_VERSION=vX.Y.Z"
  tag="$(printf '%s\n' "$api" | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1)"
  [ -n "$tag" ] || err "the GitHub API returned no release tag (no release yet, or rate-limited). Pin a version: LOQI_VERSION=vX.Y.Z"
fi
# Normalize: accept 0.2.0 or v0.2.0. Release tags and assets always carry the leading v.
tag="v${tag#v}"

asset="loqi-$tag-$target.tar.gz"
base="https://github.com/$REPO/releases/download/$tag"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

echo "Downloading $asset ($tag) ..."
curl -fSL "$base/$asset"   -o "$tmp/$asset"      || err "download failed: $base/$asset"
curl -fsSL "$base/SHA256SUMS" -o "$tmp/SHA256SUMS" || err "could not fetch SHA256SUMS"

# Verify the checksum for THIS asset (exact filename match on field 2).
expected="$(awk -v f="$asset" '$2 == f {print $1}' "$tmp/SHA256SUMS")"
[ -n "$expected" ] || err "no checksum for $asset in SHA256SUMS"
if command -v sha256sum >/dev/null 2>&1; then
  actual="$(sha256sum "$tmp/$asset" | awk '{print $1}')"
elif command -v shasum >/dev/null 2>&1; then
  actual="$(shasum -a 256 "$tmp/$asset" | awk '{print $1}')"
else
  err "need sha256sum or shasum to verify the download"
fi
[ "$actual" = "$expected" ] || err "checksum mismatch for $asset (expected $expected, got $actual)"

# Unpack and install.
tar -C "$tmp" -xzf "$tmp/$asset"
mkdir -p "$INSTALL_DIR"
if command -v install >/dev/null 2>&1; then
  install -m 0755 "$tmp/loqi-$tag-$target/loqi" "$INSTALL_DIR/loqi"
else
  cp "$tmp/loqi-$tag-$target/loqi" "$INSTALL_DIR/loqi"
  chmod 0755 "$INSTALL_DIR/loqi"
fi

echo "Installed loqi $tag to $INSTALL_DIR/loqi"
case ":$PATH:" in
  *":$INSTALL_DIR:"*) ;;
  *) echo "Note: $INSTALL_DIR is not on your PATH. Add it to your shell profile:"
     echo "  export PATH=\"$INSTALL_DIR:\$PATH\"" ;;
esac
"$INSTALL_DIR/loqi" --version 2>/dev/null || true
echo "Runtime needs: curl on PATH and, for model calls, ANTHROPIC_API_KEY."
