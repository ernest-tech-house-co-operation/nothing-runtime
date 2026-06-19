# nth — Nothing Runtime — v0.1

`nth` is a single-binary C++ program that runs JavaScript directly by
embedding JavaScriptCore (the same engine Bun uses). It can chain
execution of other language files by shelling out to their existing
installed runtimes, has a native HTTP/WebSocket server surface built
in so JS server frameworks (Elysia, etc.) can run on it, and offers a
PM2-style daemon/process-management command that delegates to the real
`pm2`.

**No Node.js involved anywhere. No TypeScript support in v0.1 — `.js`
only.**

> The goal of `nth` is to run JavaScript fast and on absolutely low RAM.
> Everything else is someone else's problem.

## Why nth exists

Modern JS runtimes are optimized for long-running servers, not the
workloads that have exploded since 2015 — serverless functions, CLI
tools, edge compute, embedded scripting. For those workloads, "fast
startup and low RAM" beats "mature ecosystem and high throughput."

`nth` is built for the first column. See **`PHILOSOPHY.md`** for the
full design rationale.

## Performance headlines

| Test Case                  | nth          | Bun          | Node.js        | Winner |
|----------------------------|--------------|--------------|----------------|--------|
| Hello World script         | 0.020s (20ms)| 0.021s (21ms)| ~0.040s (40ms) | nth    |
| HTTP request (single)      | 0.010s (10ms)| 0.013s (13ms)| ~0.015s (15ms) | nth    |
| CLI startup (real-world)   | ~0.025s      | ~0.035s      | ~0.060s        | nth    |
| Binary size                | 322 MB       | 100+ MB      | ~50 MB         | Node   |

Full methodology + more tests in **`BENCHMARKS.md`**.

## Quick start

```sh
# 1. Build (Linux — fetch JSC prebuilt first)
bash scripts/fetch_jsc.sh
bash scripts/build.sh

# 2. Run a script
./build/nth examples/hello.js

# 3. Or via prefix-match (section 4b of the spec)
./build/nth hel              # finds and runs examples/hello.js

# 4. Initialize a project config
./build/nth init default     # writes ./nthconfig.json

# 5. Chain (with a Python step — requires enableOtherLangs:true)
./build/nth -1 a.js -success -2 b.py -success -3 c.js

# 6. Run an HTTP server (set "http": true in nthconfig.json first)
echo '{"http": true}' > nthconfig.json
./build/nth examples/server.js &
curl http://localhost:3000/
```

## Documentation

| Doc                  | What's in it                                                       |
|----------------------|--------------------------------------------------------------------|
| **`README.md`**      | This file — quick start + doc index.                               |
| **`BUILD.md`**       | Build instructions, JSC sourcing, HTTP client choice, known limits.|
| **`PHILOSOPHY.md`**  | Why `nth` exists. Design principles. What it is NOT.               |
| **`BENCHMARKS.md`**  | Performance comparison vs Bun vs Node. Methodology. Where nth wins.|
| **`examples/`**      | Runnable demos for every feature.                                  |
| **`examples/typescript/README.md`** | How to use TypeScript with `nth` (via `runtimes.build = "tsc"`). |

## Examples

| Example                              | What it demonstrates                          |
|--------------------------------------|------------------------------------------------|
| `examples/hello.js`                  | Minimal script + `console.log`.                |
| `examples/cold_start.js`             | Pure cold-start benchmark.                     |
| `examples/modules/`                  | ESM module resolution + relative imports.      |
| `examples/chain_a.js` + `chain_b.js` | Chain grammar (`-1 a.js -success -2 b.js`).    |
| `examples/chain_with_python.js` + `.py` | Cross-language chain with `enableOtherLangs`. |
| `examples/server.js`                 | `Nth.serve()` with HTTP + WebSocket.           |
| `examples/server_low_ram.js`         | Minimal HTTP server for RSS benchmarking.      |
| `examples/fetch_demo.js`             | Native `fetch()` global.                       |
| `examples/compute_pi.js`             | CPU-bound benchmark (BBP pi).                  |
| `examples/typescript/`               | TS workflow: `nth build` (tsc) + `nth dist/main.js`. |

## What nth does NOT do (v0.1 non-goals, per spec section 6)

- No bundler, no package manager implementation, no AOT compilation.
- No Node.js compatibility layer (no `require`, no `process`, no `fs`).
- No CommonJS, no `package.json` "exports" map resolution.
- No HTTPS/TLS — plain HTTP/WS only.
- No custom daemon/process-manager logic — `nth startalways` delegates
  to the real, separately-installed PM2.
- No TypeScript — `.js` only. Use the `runtimes.build = "tsc"` workflow
  described in `examples/typescript/README.md`.
- No Go, no Rust, no separate processes/IPC for JS execution.

See `BUILD.md` §6 for the full list.

## Spec compliance

This implements the `nth` Build Directive v0.1 in full. See `BUILD.md`
for the two implementation decisions the spec called out as needing
follow-up:

1. **JSC source per platform** — system framework on macOS, vendored
   prebuilt from `oven-sh/WebKit` on Linux/Windows (with a distro
   pkg-config fallback).
2. **HTTP client library** — hand-rolled HTTP/1.1 over BSD sockets,
   no external library.

## Cross-platform support

`nth` v0.1 supports **Linux** and **Windows** (macOS works via the
system JSC framework but isn't smoke-tested in CI). See `BUILD.md`
for platform-specific build instructions.

GitHub Actions workflows under `.github/workflows/`:
- **`release.yml`** — manual "Run workflow" button → builds Linux +
  Windows binaries and publishes a GitHub Release.
- **`ci.yml`** — runs on every push/PR, builds both platforms.

*Nothing Ecosystem · Ernest Tech House · Kenya · 2026*
"# nothing-runtime" 
