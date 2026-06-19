#!/usr/bin/env bash
# scripts/build.sh — direct g++/clang++ build (no cmake required, for
# environments without cmake). For cmake-based builds see BUILD.md.
# Used on Linux and macOS. Windows uses scripts/build.ps1 instead.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

OUT="${1:-build/nth}"
mkdir -p "$(dirname "$OUT")"

# --- Discover JSC ----------------------------------------------------------
WEBKIT="$ROOT/third_party/bun-webkit"
if [[ -f "$WEBKIT/include/JavaScriptCore/JavaScript.h" ]]; then
    JSC_INC="-I$WEBKIT/include"
    JSC_LIB="-L$WEBKIT/lib"
    # Use --start-group/--end-group because the static ICU libs have
    # cross-dependencies (icutu → icui18n → icuuc → icudata, plus some
    # reverse references) that a single-pass linker can't resolve.
    JSC_LIBS=(
        -lJavaScriptCore -lWTF -lbmalloc
        -Wl,--start-group
        -licutu -licui18n -licuuc -licudata
        -Wl,--end-group
    )
elif pkg-config --exists javascriptcoregtk-4.1 2>/dev/null; then
    JSC_INC="$(pkg-config --cflags javascriptcoregtk-4.1)"
    JSC_LIB="$(pkg-config --libs-only-L javascriptcoregtk-4.1)"
    JSC_LIBS=( "$(pkg-config --libs-only-l javascriptcoregtk-4.1)" )
elif pkg-config --exists javascriptcoregtk-4.0 2>/dev/null; then
    JSC_INC="$(pkg-config --cflags javascriptcoregtk-4.0)"
    JSC_LIB="$(pkg-config --libs-only-L javascriptcoregtk-4.0)"
    JSC_LIBS=( "$(pkg-config --libs-only-l javascriptcoregtk-4.0)" )
else
    echo "ERROR: JSC not found. Run: bash scripts/fetch_jsc.sh" >&2
    exit 1
fi

# --- Sources ---------------------------------------------------------------
SRC=(
    src/main.cpp
    src/cli/parser.cpp
    src/config/config.cpp
    src/chain/executor.cpp
    src/js/engine.cpp
    src/js/module_loader.cpp
    src/js/globals.cpp
    src/js/fetch.cpp
    src/js/http_server.cpp
    src/server/http.cpp
    src/server/websocket.cpp
    src/runtimes/role_resolver.cpp
    src/runtimes/pm2.cpp
    src/util/subprocess.cpp
    src/util/net.cpp
    src/util/sha1.cpp
)

CXX=${CXX:-g++}
CXXFLAGS=(
    -std=c++17 -O2 -Wall -Wno-unused-parameter -Wno-unused-but-set-variable
    -I src -I third_party $JSC_INC
)
LDFLAGS=(
    $JSC_LIB "${JSC_LIBS[@]}"
    -lpthread -ldl -lrt
)

set -x
"$CXX" "${CXXFLAGS[@]}" "${SRC[@]}" -o "$OUT" "${LDFLAGS[@]}"
set +x
echo "Built: $OUT"
