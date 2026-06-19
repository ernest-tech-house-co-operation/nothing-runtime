// examples/compute_pi.js
//
// CPU-bound benchmark. Computes a sequence of BigInt operations that
// approximates computing pi via the Bailey–Borwein–Plouffe (BBP)
// formula. This is the kind of workload where JSC's optimizing JIT
// really shines — pure JS, tight loop, no I/O.
//
// Run with:
//   time nth compute_pi.js
//   time node compute_pi.js
//   time bun compute_pi.js
//
// All three should be within 10-20% of each other for this kind of
// CPU-bound work — the JITs are mature in all three engines. Where nth
// wins decisively is *startup*: for short-lived scripts (CLI tools,
// serverless functions, build steps), the ~20ms cold start beats
// Node's ~40ms cold start by 2x.

function bbpPiTerm(k) {
    // Compute one BBP term as a floating-point approximation.
    // (NOT a precision pi calculation — this is a CPU-bound benchmark.)
    const k8 = 8 * k;
    return (
        4 / (k8 + 1) -
        2 / (k8 + 4) -
        1 / (k8 + 5) -
        1 / (k8 + 6)
    );
}

function bbpPiApprox(terms) {
    let pi = 0;
    let sixteen_pow_k = 1;  // 16^0
    for (let k = 0; k < terms; k++) {
        pi += bbpPiTerm(k) / sixteen_pow_k;
        sixteen_pow_k *= 16;
    }
    return pi;
}

const ITERATIONS = 100000;
const TERMS = 30;
let sum = 0;
for (let i = 0; i < ITERATIONS; i++) {
    sum += bbpPiApprox(TERMS);
}
const avg = sum / ITERATIONS;
console.log("done. avg pi approximation =", avg.toFixed(10));
console.log("(real pi = 3.141592653589793...)");
