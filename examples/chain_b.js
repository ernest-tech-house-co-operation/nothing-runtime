// examples/chain_b.js
// Second step of a chained run. Note: each .js step runs in its own
// JS context (separate JSGlobalContext), so global state does NOT carry
// over between steps — see BUILD.md "Known limitations".
console.log("[step 2] chain_b.js running");
console.log("[step 2] __shared_state from step 1 is:", globalThis.__shared_state);
