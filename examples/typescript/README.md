# TypeScript workflow with `nth`

**nth does not parse TypeScript.** This is deliberate — see `BUILD.md` §6
and the philosophy doc. nth's job is to *run JavaScript fast*, not to be
a compiler.

The recommended workflow uses the `runtimes` table (spec section 5c) to
delegate compilation to `tsc` — the official TypeScript compiler — and
then runs the emitted `.js` output through nth's JSC engine.

## Why not build TS into nth?

1. **Scope discipline.** nth v0.1 is a runtime, not a toolchain. Adding
   TS parsing would either bloat the binary (~40 MB for `tsc`, ~15 MB for
   `swc`) or require a brittle hand-rolled type stripper that breaks on
   real-world TS.

2. **Right tool for the job.** `tsc` is maintained by Microsoft, supports
   every TS feature, and produces spec-compliant JS. nth runs that JS at
   native JSC speed. Each tool does what it's best at.

3. **You still get type-checking.** Running TS via a stripper (Bun's
   approach) skips type-checking entirely — you'd get runtime errors
   instead of compile-time errors. The `tsc` workflow gives you real
   type errors at build time and fast execution at run time.

## Setup

```sh
cd examples/typescript/

# 1. Install TypeScript locally (writes to ./node_modules).
nth install -D typescript

# 2. (Optional) Initialize project config if you don't have one.
#    nth init default
#    Then edit nthconfig.json to add the runtimes.build entry —
#    the nthconfig.json in this folder is already set up.

# 3. Compile TS → JS.
nth build
# This runs:  tsc
# Output:     dist/main.js

# 4. Run the compiled JS.
nth dist/main.js
```

Expected output:

```
Hello, Alice (id=1) — your email is alice@example.com
Hello, Bob (id=2) — your email is bob@example.com
Hello, Carol (id=3) — your email is carol@example.com
Processed 3 users.
```

## What's in `nthconfig.json` for this example

```json
{
  "runtimes": {
    "build": "tsc"
  }
}
```

That single line is the entire TS integration. `nth build` becomes a
literal pass-through to `tsc` — `nth` doesn't know or care that the
configured tool is a TypeScript compiler. The same mechanism works for
`esbuild`, `swc`, `vite build`, or any other JS-producing build tool:

```json
{
  "runtimes": {
    "build": "esbuild"
  }
}
```

…then `nth build src/main.ts --bundle --outfile=dist/main.js` runs
`esbuild src/main.ts --bundle --outfile=dist/main.js` directly.

## Faster dev iteration with `--watch`

For development, run `tsc --watch` in one terminal and `nth` in another:

```sh
# Terminal 1
nth build --watch

# Terminal 2 — re-run whenever you want to test
nth dist/main.js
```

Or, if you want a single-command dev loop, configure `package.json`
scripts and use `npm run` (which `nth install` already gives you via
npm):

```json
{
  "scripts": {
    "dev": "tsc --watch & nth dist/main.js"
  }
}
```

## Production deployment

In production, you typically only ship the compiled `dist/` directory
plus `nth` itself. TypeScript source and `tsc` stay in your build
pipeline, not in the runtime image:

```dockerfile
# Build stage
FROM node:20 AS build
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci
COPY tsconfig.json ./
COPY src ./src
RUN npx tsc

# Runtime stage — only nth + compiled JS
FROM debian:bookworm-slim
COPY nth /usr/local/bin/nth
COPY --from=build /app/dist /app/dist
WORKDIR /app
CMD ["nth", "dist/main.js"]
```

The final image contains no TypeScript compiler, no source files, no
Node.js — just `nth` and the JS it runs.

## Summary

| Concern            | Handled by       | Why                                  |
|--------------------|------------------|--------------------------------------|
| Type-checking      | `tsc`            | It's the spec — only MS maintains it |
| JS compilation     | `tsc`            | Produces spec-compliant ES2022       |
| Running the JS     | `nth`            | JSC is the fastest JS engine         |
| Binary size        | nth stays lean   | No 40 MB TS compiler baked in        |
| Cold start         | nth stays fast   | No transpilation step at runtime     |

This is the "Nothing" philosophy in practice: nth does the one thing
it's good at (running JS), and gets out of the way for everything else.
