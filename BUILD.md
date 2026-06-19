# BUILD.md — `nth` v0.1 build notes

*Nothing Ecosystem · Ernest Tech House · 2026*

This document records the two implementation decisions the spec section 7
explicitly called out as "most likely to need follow-up": **how JSC was
sourced/linked per platform**, and **which HTTP client library was chosen
for `fetch()`**. Plus everything else you need to build `nth` from source.

---

## 1. Prerequisites

- C++17 compiler (g++ 9+, clang++ 10+, MSVC 2019 16.11+)
- CMake 3.16+ (optional — `scripts/build.sh` and `scripts/build.ps1`
  do a direct compiler build without cmake)
- `pkg-config` (Linux only — used as a fallback for distro-managed JSC)
- On Windows: Visual Studio 2019+ with the "Desktop development with C++"
  workload (cl.exe + Windows SDK). The prebuilt JSC is built with MSVC,
  so we MUST use MSVC here — MinGW would mismatch the C++ ABI and fail
  at link time.
- No OpenSSL required anymore — SHA-1 and base64 for the WebSocket
  handshake are now vendored in `src/util/sha1.cpp` (public-domain
  implementation by Steve Reid). This keeps the build simpler on both
  Linux and Windows.
- `nlohmann/json` — vendored at `third_party/nlohmann/json.hpp`. The
  build expects it to exist; if missing, download with:
  ```sh
  curl -fsSL -o third_party/nlohmann/json.hpp \
    https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
  ```

---

## 2. How JSC was sourced and linked

Per spec section 3, JavaScriptCore is the JS engine — same engine Bun
uses. Building WebKit/JSC from source is a multi-hour toolchain
undertaking even for the Bun team; we **do not** do that. Instead,
per-platform resolution:

### macOS
Link directly against the system `JavaScriptCore.framework` — no
download, no vendoring. CMake's `find_library(JavaScriptCore)`
handles this in `cmake/SetupJavaScriptCore.cmake`.

### Linux / Windows
Vendor **prebuilt JSC binaries from `oven-sh/WebKit`** — the
MIT-licensed WebKit fork the Bun team maintains specifically for this
purpose. Each tagged release on that repo publishes per-platform
tarballs containing the static libraries and headers needed to link
against, with no need to compile WebKit at all.

#### Pinned tag

```
NTH_WEBKIT_TAG=autobuild-cd821fecca0d39c8bac874c283d956868c7f0de0
```

This tag was current at implementation time. To pin to a different
release, set the `NTH_WEBKIT_TAG` environment variable before running
`scripts/fetch_jsc.sh`. The exact commit hash used is recorded above
and in `scripts/fetch_jsc.sh` for reproducibility.

#### Fetching the prebuilt

```sh
bash scripts/fetch_jsc.sh                  # autodetects linux-amd64
bash scripts/fetch_jsc.sh linux-arm64      # explicit target
bash scripts/fetch_jsc.sh darwin-arm64     # macOS (optional — system
                                           # framework is preferred)
```

This downloads `bun-webkit-<target>.tar.gz` (~470 MB) from
`https://github.com/oven-sh/WebKit/releases/download/<tag>/`, extracts
it under `third_party/bun-webkit/`, and verifies the marker file
`include/JavaScriptCore/JavaScript.h` exists.

#### Known issue avoided (per spec section 3)

Bun's own Windows build has a bug where the prebuilt gets redownloaded
on every debug rebuild unnecessarily, caused by a `package.json`-
existence check in their CMake that doesn't apply to Windows webkit
builds. `scripts/fetch_jsc.sh` avoids this by checking for the marker
file (`include/JavaScriptCore/JavaScript.h`) before downloading and
exiting early if it already exists. The fetch is genuinely idempotent.

#### Caveat (per spec section 3)

This is a *fork* of WebKit with Bun-specific embedder patches (e.g.
custom `Error.prototype.stack` formatting hooks), not vanilla upstream
WebKit. The JSC engine underneath is unchanged, but if any JSC
behavior ever seems to diverge from plain JavaScriptCore documentation,
this is why.

### Distro fallback

If you'd rather use your distro's package-managed JSC (same engine,
just packaged differently), install
`libjavascriptcoregtk-4.1-dev` (Debian/Ubuntu) or the equivalent on
your distro, and **don't** run `fetch_jsc.sh`. The CMake setup will
auto-detect via pkg-config.

This is functionally equivalent to the vendored-prebuilt approach —
same engine, no rebuild from source — and is preferred on Linux distros
that ship recent enough JSC builds.

---

## 3. HTTP client choice for `fetch()`

**Implementation:** hand-rolled HTTP/1.1 client over plain BSD sockets.
**No external HTTP library** (libcurl, Boost.Beast, etc.) is linked.

**Rationale:** Per spec section 3b, "Startup time matters... avoid
heavyweight C++ HTTP libraries if a leaner one gets the job done." The
hand-rolled client adds zero new dependencies and keeps the binary
lean. It's also consistent with the hand-rolled HTTP server
(`server/http.cpp`) — same socket discipline, same parser style.

**Supported:**
- Methods: GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS — anything
  you pass as the `method` option.
- Custom request headers.
- Request body (string only — no streaming).
- Response status, headers, body.
- `Response.text()` and `Response.json()` returning thenables (a
  minimal Promise-shaped object — see "Known limitations" below).

**Not supported in v0.1 (consistent with spec section 6 non-goals):**
- HTTPS / TLS — plain HTTP only.
- HTTP/2.
- Redirects.
- Chunked transfer-encoding in responses — we read until EOF or
  `Content-Length` bytes.
- Keep-alive — one request per connection (we send
  `Connection: close`).
- Streaming request bodies — the entire body must be in memory before
  the request is sent.

This is enough for the "minimal subset Elysia actually touches" per
spec section 3b.

---

## 4. Build

```sh
cd nth/
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

The resulting binary is `build/nth`. Install with `make install`
(defaults to `/usr/local/bin/nth`).

### Cross-platform notes

- **macOS:** Just `cmake .. && make`. No prebuilt download needed —
  the system framework is used.
- **Linux:** Run `bash scripts/fetch_jsc.sh` first (one-time, ~470 MB
  download), or install `libjavascriptcoregtk-4.1-dev` via your
  distro's package manager. Then `cmake .. && make` (or
  `bash scripts/build.sh`).
- **Windows:** Run `pwsh scripts/fetch_jsc.ps1` first (one-time,
  ~300 MB download). Then either open the project in Visual Studio and
  build via CMake, OR run `pwsh scripts/build.ps1` from a "Developer
  PowerShell for VS" window. The build script will auto-detect VS via
  `vswhere` if you're not in a Developer PowerShell.

### Cross-platform helpers

`src/util/net.{hpp,cpp}` provides a thin socket abstraction:
- `socket_t` type (alias for `int` on POSIX, `SOCKET` on Windows)
- `kInvalidSocket` (POSIX: `-1`, Windows: `INVALID_SOCKET`)
- `init()` (no-op on POSIX, `WSAStartup` on Windows)
- `close_socket()` (`close` on POSIX, `closesocket` on Windows)
- `set_nonblocking()` (`fcntl` on POSIX, `ioctlsocket` on Windows)
- `last_sock_error()` (`errno` on POSIX, `WSAGetLastError` on Windows)

The server event loop uses `select()` instead of `poll()` — `select()`
is portable across POSIX and Winsock, while `WSAPoll` (the Windows
equivalent of `poll()`) is notoriously buggy.

---

## 5. Smoke test

```sh
cd examples/
../build/nth hello.js                # → Hello from nth — JavaScriptCore embed.
../build/nth hel                     # prefix-match (section 4b) — same result

../build/nth -1 chain_a.js -success -2 chain_b.js    # chain grammar

../build/nth modules/main.js         # ESM with relative imports

# Server example — needs nthconfig.json with "http": true
echo '{"http": true}' > nthconfig.json
../build/nth server.js &
curl http://localhost:3000/          # → hello from nth
curl http://localhost:3000/json      # → {"ok": true, "time": ...}
```

---

## 6. Known limitations (v0.1)

These are explicit v0.1 limitations per spec sections 3c, 3b, and 6,
not bugs:

- **No TypeScript support.** `nth` v0.1 runs `.js` files only — TS
  parsing is explicitly out of scope per spec section 6. The
  recommended TS workflow uses the `runtimes` table (spec section 5c)
  to delegate compilation to `tsc`, then runs the emitted JS through
  `nth`. See `examples/typescript/README.md` for a complete working
  example. Adding native TS to `nth` would either bloat the binary
  (~40 MB for `tsc`, ~15 MB for `swc`) or require a brittle hand-rolled
  type stripper — neither is worth the cost when `tsc` already exists
  and produces spec-compliant JS.
- **ESM module loader is a regex-based source-to-source transform**,
  not a full parser. The common forms (`import defaultName from`,
  `import { a, b as c } from`, `import * as ns from`, `import "side-effect"`,
  `export default`, `export const/let/var`, `export function/class`,
  `export { ... }`, `export { ... } from`) all work. Live bindings,
  `import.meta`, top-level await, and dynamic `import()` are not
  supported in v0.1.
- **Each `.js` step in a chain runs in its own `JSGlobalContext`.**
  Module state does NOT carry over between steps. Each step is an
  independent evaluation — this is by design (a chain is "run file A,
  then run file B" — they're not the same program).
- **`package.json` "exports" map resolution is NOT supported.** We
  fall back to `"module"` → `"main"` → `index.js` only. Some modern
  npm packages that rely solely on `"exports"` may not resolve
  correctly. (Spec section 3c.)
- **No CommonJS interop.** `require`/`module.exports` are not
  supported. (Spec section 3c.)
- **No native addon npm packages.** Pure-JS packages only. (Spec
  section 3c.)
- **No HTTPS/TLS.** Both `fetch()` and `Nth.serve()` are plain
  HTTP/WS only. (Spec section 6.)
- **`Promise` objects created from C++** are thenables (duck-typed
  objects with a `.then(onResolve, onReject)` method), not real JSC
  `Promise` instances. This is sufficient for `await fetch(...)` and
  `fetch(...).then(...)` — both patterns work — but
  `instanceof Promise` will return false. A future version should
  switch to the real JSC Promise constructor via the private API.
- **Single-threaded event loop for the server.** No thread pool, no
  multi-core dispatch. (Spec section 3b — acceptable for v0.1.)
- **`testing` (top-level field) and `runtimes.test` are two different
  mechanisms that both end up driving `nth test`.** Per spec section
  5c, this is a known minor redundancy — documented here rather than
  picking one to drop. `runtimes.test` takes precedence if configured;
  otherwise the top-level `testing` field is used.

---

## 7. Project layout

```
nth/
├── CMakeLists.txt
├── BUILD.md                    ← you are here
├── README.md
├── .github/workflows/
│   ├── release.yml             ← manual "Run workflow" → builds + publishes
│   └── ci.yml                  ← runs on every push/PR
├── cmake/
│   ├── SetupJavaScriptCore.cmake
│   └── SetupNlohmannJson.cmake
├── scripts/
│   ├── fetch_jsc.sh            ← idempotent prebuilt JSC fetcher (bash)
│   ├── fetch_jsc.ps1           ← same, for Windows PowerShell
│   ├── build.sh                ← direct g++/clang++ build (Linux/macOS)
│   └── build.ps1               ← MSVC build (Windows)
├── third_party/
│   ├── nlohmann/json.hpp       ← vendored single-header
│   └── bun-webkit/             ← vendored JSC (populated by fetch_jsc.*)
├── src/
│   ├── main.cpp
│   ├── cli/parser.{hpp,cpp}    ← -N/-success grammar + prefix-match
│   ├── config/config.{hpp,cpp} ← nthconfig.json loader
│   ├── chain/executor.{hpp,cpp}← multi-step chain
│   ├── js/
│   │   ├── engine.{hpp,cpp}    ← JSC context wrapper
│   │   ├── module_loader.{hpp,cpp} ← ESM resolution + transform
│   │   ├── globals.{hpp,cpp}   ← console.log + install http globals
│   │   ├── fetch.{hpp,cpp}     ← fetch() + Request/Response
│   │   └── http_server.{hpp,cpp} ← Nth.serve()
│   ├── server/
│   │   ├── http.{hpp,cpp}      ← HTTP/1.1 request parser
│   │   └── websocket.{hpp,cpp} ← RFC 6455 server-side
│   ├── runtimes/
│   │   ├── role_resolver.{hpp,cpp} ← shared helper for runtimes.<role>
│   │   └── pm2.{hpp,cpp}       ← startalways/install/test pass-throughs
│   └── util/
│       ├── strings.hpp
│       ├── fs.hpp
│       ├── net.{hpp,cpp}       ← cross-platform socket wrappers
│       ├── sha1.{hpp,cpp}      ← vendored SHA-1 + base64
│       └── subprocess.{hpp,cpp}← fork/exec (POSIX) / CreateProcess (Win)
└── examples/
    ├── hello.js
    ├── chain_a.js
    ├── chain_b.js
    ├── server.js
    └── modules/
        ├── main.js
        ├── util.js
        └── version.js
```

---

## 8. GitHub Actions — releases and CI

This repo ships two workflow files under `.github/workflows/`:

### `release.yml` — manual release builder

Triggered **manually** via the "Run workflow" button in the Actions tab.
Does NOT run on push.

1. Go to the repo's **Actions** tab.
2. Select **Release** in the left sidebar.
3. Click **Run workflow** (top right).
4. Enter the version tag you want to publish (e.g. `v0.1.1`) and choose
   whether to mark it as a pre-release.
5. Click the green **Run workflow** button.

The workflow:
- Builds `nth` on `ubuntu-latest` (Linux x86_64) and `windows-latest`
  (Windows x86_64) in parallel.
- Runs the smoke tests on each platform to verify the binary works.
- Creates a Git tag matching the version input.
- Creates a GitHub Release with both binaries (`nth-linux-amd64` and
  `nth-windows-amd64.exe`) attached, plus auto-generated release notes
  from the commit history since the last tag.

### `ci.yml` — continuous integration

Runs on every push to `main`/`master` and every pull request. Builds
both platforms and runs smoke tests, but does NOT create a release.
Artifacts are uploaded for download (7-day retention) so contributors
can grab a fresh binary without running the build themselves.

### Required secrets

Neither workflow requires any custom secrets — both use the built-in
`GITHUB_TOKEN` that GitHub Actions auto-provides. The `Release` workflow
needs `permissions: contents: write` (already declared in the YAML) so
it can push tags and create releases.

---

## 9. Reproducibility

To reproduce this build:

1. Check out this source tree.
2. Fetch the JSC prebuilt:
   - Linux/macOS: `bash scripts/fetch_jsc.sh`
   - Windows: `pwsh scripts/fetch_jsc.ps1`
   (Skipped on macOS if using the system framework instead.)
3. Build:
   - Linux/macOS: `bash scripts/build.sh` (or `cmake .. && make`)
   - Windows: `pwsh scripts/build.ps1` (or `cmake .. && cmake --build .`)

The exact JSC version is recorded as `NTH_WEBKIT_TAG` at the top of
`scripts/fetch_jsc.sh`. The HTTP client choice and all other
implementation decisions are documented in this file.
