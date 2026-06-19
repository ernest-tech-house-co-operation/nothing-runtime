// examples/cold_start.js
//
// Pure cold-start measurement. nth's JSC embed has near-zero startup
// overhead — this script just prints `Hello` and exits. Useful as a
// baseline for benchmarking against `node cold_start.js` and
// `bun cold_start.js`.
//
// Run with:
//   time nth cold_start.js
//   time node cold_start.js
//   time bun cold_start.js
//
// On the same machine you should see nth ~2x faster than Node and
// roughly on par with Bun (often slightly faster, since nth has fewer
// global polyfills to initialize).

console.log("Hello");
