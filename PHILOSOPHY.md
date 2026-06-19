# PHILOSOPHY.md — Why `nth` exists

*Nothing Ecosystem · Ernest Tech House · Kenya · 2026*

> The goal of `nth` is to run JavaScript fast and on absolutely low RAM.
> Everything else is someone else's problem.

---

## 1. The problem `nth` solves

Modern JavaScript runtimes are excellent, but they have a structural
problem: **they're built for the wrong default case.**

Node.js was built for long-running servers in 2009, when "the cloud"
meant a few always-on EC2 instances. Its design optimizes for
throughput on a hot process — V8's JIT warmup, libuv's thread pool,
the `require` cache, the `Buffer` polyfill, all of it assumes you'll
start once and serve thousands of requests.

Bun was built in 2022 to fix Node's ergonomics while keeping the
"runtime + toolkit" integration. It's faster than Node at startup
but still ships a bundler, test runner, package manager, and
TypeScript stripper as core features — the binary is 100+ MB.

Neither is wrong. But neither is right for the workloads that have
exploded since 2015:

- **Serverless functions** — Lambda, Cloudflare Workers, Vercel
  Functions. Processes start, do a few milliseconds of work, and
  exit. Cold-start latency is user-visible.
- **CLI tools** — file processors, code generators, git hooks,
  build steps. Run a hundred times per developer per day. Each
  invocation pays the full runtime startup cost.
- **Embedded scripting** — plugins, automation rules, sandboxed
  user code. You ship the runtime with the host app; binary size
  and memory matter.
- **Edge compute** — Cloudflare, Fastly Compute, Akamai EdgeWorkers.
  RAM is billed per millisecond. Idle RSS is money.

For these workloads, "fast startup and low RAM" beats "mature
ecosystem and high throughput." Node and Bun are optimized for the
second column. `nth` is optimized for the first.

---

## 2. The "Nothing" name

`nth` stands for **Nothing Runtime**. The name is a promise:

- **Nothing you didn't ask for.** No `process`, no `Buffer`, no
  `require`, no `fs`, no `crypto`, no `stream`. The only global
  installed by default is `console.log`. Everything else is opt-in
  via `nthconfig.json`.
- **Nothing redundant.** `nth` doesn't reimplement things that
  already exist. Need a package manager? Configure `packageManager:
  "npm"` and `nth` runs `npm` for you. Need a test runner? Configure
  `testing: "vitest"` and `nth test` runs `vitest`. Need a daemon?
  `nth startalways` delegates to PM2. Need TypeScript? Configure
  `runtimes: { build: "tsc" }` and `nth build` runs `tsc`.
- **Nothing in the way.** When you call `nth foo.js`, you get
  JSC's raw evaluation of `foo.js` plus the few globals you opted
  into. There's no transpilation layer, no polyfill layer, no
  framework layer between your code and the engine.

This is the opposite of "batteries included." `nth` is the battery
holder — you bring the batteries.

---

## 3. The three design principles

### 3.1 Do one thing

`nth` runs JavaScript. That's it. It does not:
- Bundle modules into a single file (use `esbuild`).
- Compile TypeScript (use `tsc`).
- Run tests (use `vitest`).
- Manage packages (use `npm` / `pnpm` / `yarn` / `bun install`).
- Manage processes (use `pm2`).
- Polyfill Node.js (use… Node.js).
- Provide a framework (use Elysia, Hono, Express, whatever).

It runs JavaScript. Fast. If a feature isn't about running JavaScript,
`nth` either delegates to a configured external tool or doesn't have
it.

### 3.2 Defer, don't reimplement

When something adjacent is needed (package management, testing,
daemonization, building), `nth` doesn't write its own version. It
defines a clean config field that names the external tool, then
shells out to that tool with the conventional arguments.

This means:
- `nth install foo` runs `<packageManager> install foo`.
- `nth test` runs `<testing|runtimes.test> test`.
- `nth build` runs `runtimes.build`.
- `nth startalways app.js --name foo` runs `pm2 start app.js --name foo`.
- `nth <role>` runs `runtimes.<role>`.

`nth` is a translator, not a reimplementation. PM2 already exists and
is excellent — why write a worse version of it? `tsc` already exists
and is the spec — why ship a 40 MB compiler in your runtime?

### 3.3 The runtime is sacred

The one thing `nth` will never delegate is the actual execution of
`.js` files. Per spec section 5c (the "hard rule"):

> `nth <file>` for a `.js` file always runs via `nth`'s own embedded
> JSC engine. There is no config option to replace this with Bun,
> Node, or Deno.

If core JS execution could be silently swapped out, `nth` has no
reason to exist over a shell alias. The whole point is that `nth`
*is* the runtime — a real, embedded JavaScriptCore that runs your
code directly, not a wrapper around someone else's runtime.

---

## 4. Why JavaScriptCore

Three reasons:

### 4.1 It's the fastest at context creation

JSC was designed for Safari's per-tab process model, where new
contexts are created and destroyed constantly. `JSGlobalContextCreate`
is sub-millisecond. V8 (Node) was designed for long-running servers
and has a heavier context-init path.

The numbers in `BENCHMARKS.md` §1 confirm this: nth's 20ms cold
start vs Node's 40ms is largely JSC's context-creation advantage.

### 4.2 It's proven in production

JSC is what Bun uses. It's what Safari uses. It's what macOS apps
embedding JS use. It's battle-tested across billions of devices.

### 4.3 The prebuilt mechanism works

Building JSC from source is a multi-hour toolchain undertaking even
for the Bun team. But `oven-sh/WebKit` publishes prebuilt static
libraries for Linux, macOS, and Windows — same engine, no build pain.
`nth` vendors those prebuilts and links against the C API, which is
the stable embedding surface.

---

## 5. The "absolutely low RAM" goal

The directive that motivated this project says:

> "Run JavaScript fast and on absolutely low RAM."

This needs unpacking, because "low RAM" is relative.

### 5.1 What "low RAM" means here

`nth` does NOT target microcontrollers (those need QuickJS or MuJS).
It targets the workloads where you currently pay for Node or Bun
but don't actually need their full feature set:

- **Serverless functions** with 128 MB or 256 MB memory limits.
  Node's V8 reserves a big chunk of that for its heap; you have
  less room for your actual app data.
- **Small cloud instances** — the $5/month 512 MB VPS running a
  side-project API. Node + Express leaves ~400 MB for the app;
  `nth` + Elysia leaves more.
- **Container fleets** — when you're running 1000 copies of a
  service, every MB of idle RSS is 1 GB of fleet RAM.
- **CI runners** — running hundreds of nth-based tools in parallel
  on a shared runner. Lower per-process RSS = more parallelism.

### 5.2 How nth keeps RAM low

- **No JIT bytecode cache that grows unbounded.** JSC's JIT does
  tier up under load, but the LLInt (the lowest tier) is what
  short-lived scripts use — and it allocates almost nothing.
- **No libuv thread pool.** Node reserves 4 worker threads by
  default, each with its own stack. `nth` is single-threaded for
  v0.1.
- **No built-in module cache beyond what JS actually imports.**
  Node loads `fs`, `http`, `crypto`, `stream`, etc. at startup
  even if you never use them. `nth` only loads what your JS
  imports.
- **Lazy global installation.** `console.log` is always installed
  (cheap). The HTTP server surface (`fetch`, `Request`, `Response`,
  `Nth.serve`) is only installed when `http: true` is set in
  `nthconfig.json`.
- **No package manager running in-process.** `npm` is a separate
  process; `nth` doesn't load its code into your app's heap.

### 5.3 What's left to do on RAM

v0.1's idle RSS of ~40 MB (measured on Linux x86_64 with an idle
HTTP server running) is already excellent — competitive with Bun and
better than Node + Express. Future versions could push further:

- Expose a `--max-heap-size=N` flag that calls JSC's heap-size APIs
  to cap the reservation.
- Offer a `--no-jit` flag for cases where the LLInt is enough and
  you want to skip JIT memory allocation entirely.
- Build a shared-library variant (libnth.so / nth.dll) so multiple
  `nth` processes share the read-only JSC text segment instead of
  each carrying their own.

These are v0.2+ concerns. v0.1 establishes the baseline: a runtime
that's already best-in-class on RAM while being fastest on startup.

---

## 6. What `nth` is NOT

Setting expectations honestly:

- **Not a Node.js replacement.** If your code does `require("fs")`
  or `process.exit(0)`, it won't run on `nth` v0.1. The spec
  deliberately excludes Node polyfills. Use Node if you need Node's
  ecosystem.
- **Not a Bun replacement.** Bun ships a bundler, test runner,
  package manager, and TS stripper as core features. `nth` ships
  none of those — it delegates. If you want the integrated toolkit,
  use Bun.
- **Not a TypeScript runtime.** `nth` runs JavaScript. TypeScript
  is a build step (`tsc`), configured via `runtimes.build`. See
  `examples/typescript/README.md`.
- **Not a framework.** `nth` exposes `Nth.serve()` — a primitive
  for accepting HTTP/WebSocket connections. Routing, middleware,
  templating: that's Elysia's job (or Hono's, or Express's, or
  whatever you bring).
- **Not for everyone.** If you're building a high-throughput API
  server that runs 24/7 with heavy CPU per request, Node's cluster
  mode or Bun's worker threads will outperform `nth` v0.1. Use
  the right tool.

---

## 7. The target user

`nth` is built for developers who:

1. **Care about cold-start latency.** Serverless, CLI tools, build
   scripts, edge compute.
2. **Don't need Node's ecosystem.** Either because they're writing
   fresh code targeting web-standard globals (`fetch`, `Request`,
   `Response`), or because they're using a framework like Elysia
   that already targets the Bun-style API surface.
3. **Want a runtime, not a toolkit.** They've already chosen their
   package manager (`pnpm`), test runner (`vitest`), build tool
   (`esbuild`), process manager (`pm2`). They want a runtime that
   runs JS and gets out of the way.
4. **Are comfortable with config-driven delegation.** `nth`'s
   `nthconfig.json` is the contract. If you don't like the
   "configure then delegate" model, `nth` will frustrate you.

If that's you, welcome. If not, Node and Bun are both excellent —
use them.

---

## 8. The road ahead (v0.2 and beyond)

v0.1 establishes the core: a fast, minimal, single-binary JS runtime
with HTTP/WebSocket support, ESM modules, chain execution, and
config-driven delegation for everything else.

Likely directions for future versions (NOT commitments):

- **Heap-size controls** — `--max-heap-size=N` flag.
- **Shared-library build** — `libnth.so` / `nth.dll` for embedding.
- **Worker threads** — for CPU-bound server workloads.
- **Real JSC module-loader hooks** — replacing the regex-based
  transform with proper module loader integration, enabling live
  bindings and `import.meta`.
- **Real Promises from C++** — replacing the thenable hack with
  the JSC Promise constructor.
- **HTTPS/TLS** — for `fetch()` and `Nth.serve()`.
- **Windows production polish** — v0.1 has Windows code paths but
  hasn't been battle-tested on Windows.

Unlikely directions (against the philosophy):

- Built-in bundler. Use `esbuild`.
- Built-in TypeScript parsing. Use `tsc` or `swc`.
- Node.js polyfills. Use Node.
- Built-in package manager. Use `npm` / `pnpm` / `yarn` / `bun`.
- A `nth`-specific framework. Use Elysia or Hono.

The philosophy is the contract. If a future version of `nth` ships
a built-in bundler, it has lost its reason to exist.

---

## 9. In one sentence

`nth` exists to run JavaScript fast and on absolutely low RAM,
delegating everything else to the tools that already do it best.

*Nothing Ecosystem · Ernest Tech House · Kenya · 2026*
