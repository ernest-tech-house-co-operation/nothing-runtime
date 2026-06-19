// examples/chain_a.js
// First step of a chained run.
// Try: nth -1 chain_a.js -success -2 chain_b.js
console.log("[step 1] chain_a.js running");
globalThis.__shared_state = 42;
console.log("[step 1] set __shared_state =", globalThis.__shared_state);
