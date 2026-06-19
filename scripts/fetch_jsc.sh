#!/usr/bin/env bash
# scripts/fetch_jsc.sh
#
# Downloads and extracts the prebuilt JavaScriptCore (and supporting libs)
# from oven-sh/WebKit — the same source Bun itself uses for its JSC embed.
#
# This avoids the multi-hour toolchain undertaking of building WebKit from
# source. The engine underneath is unchanged from upstream JSC.
#
# Pinned tag (recorded in BUILD.md):
#   autobuild-cd821fecca0d39c8bac874c283d956868c7f0de0
#
# Usage:
#   bash scripts/fetch_jsc.sh           # autodetects linux-amd64
#   bash scripts/fetch_jsc.sh linux-arm64
#   bash scripts/fetch_jsc.sh darwin-arm64
#
# Re-running is safe: if third_party/bun-webkit/include/JavaScriptCore/JavaScript.h
# already exists, the download is skipped (this is the bug avoided from Bun's
# own CMake — see BUILD.md "Known issues avoided").

set -euo pipefail

TAG="${NTH_WEBKIT_TAG:-autobuild-cd821fecca0d39c8bac874c283d956868c7f0de0}"
TARGET="${1:-}"

if [[ -z "$TARGET" ]]; then
    OS="$(uname -s)"
    ARCH="$(uname -m)"
    case "$OS/$ARCH" in
        Linux/x86_64)         TARGET="linux-amd64" ;;
        Linux/aarch64)        TARGET="linux-arm64" ;;
        Darwin/arm64)         TARGET="darwin-arm64" ;;
        Darwin/x86_64)        TARGET="darwin-amd64" ;;
        MINGW*/x86_64)        TARGET="windows-amd64" ;;
        MSYS*/x86_64)         TARGET="windows-amd64" ;;
        CYGWIN*/x86_64)       TARGET="windows-amd64" ;;
        *) echo "Unsupported platform: $OS/$ARCH"; exit 1 ;;
    esac
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/third_party/bun-webkit"
MARKER="$DEST/include/JavaScriptCore/JavaScript.h"

# Idempotency check — Bun's own Windows build re-downloads on every debug
# rebuild because their CMake doesn't check for the marker file properly.
# We do.
if [[ -f "$MARKER" ]]; then
    echo "[fetch_jsc] already present at $DEST — skipping."
    exit 0
fi

URL="https://github.com/oven-sh/WebKit/releases/download/$TAG/bun-webkit-$TARGET.tar.gz"
echo "[fetch_jsc] downloading $URL"
TMP_TAR="$(mktemp -t nth-webkit-XXXXXX.tar.gz)"
trap 'rm -f "$TMP_TAR"' EXIT
curl -fL --max-time 600 -o "$TMP_TAR" "$URL"

echo "[fetch_jsc] extracting to $DEST"
mkdir -p "$DEST"
tar xzf "$TMP_TAR" -C "$ROOT/third_party" \
    --exclude='bun-webkit/Source' \
    --exclude='bun-webkit/bin/TestWebKitAPI'

if [[ ! -f "$MARKER" ]]; then
    echo "[fetch_jsc] ERROR: extraction finished but marker $MARKER missing"
    exit 1
fi

echo "[fetch_jsc] done. JSC available at $DEST"
echo "[fetch_jsc] tag: $TAG"
