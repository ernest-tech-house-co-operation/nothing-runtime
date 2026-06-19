// examples/chain_with_python.js
// First step of a cross-language chain. Requires `enableOtherLangs: true`
// in nthconfig.json.
//
// Try:
//   echo '{"enableOtherLangs": true}' > nthconfig.json
//   nth -1 chain_with_python.js -success -2 chain_with_python.py -success -3 chain_with_python.js
//
// Each .js step runs in-process via JSC (sub-millisecond context
// creation). The .py step forks python3. The -success separators
// ensure that if any step fails (non-zero exit), the chain stops
// immediately — no cascading damage.

console.log("[js step 1] preparing data for Python");
const data = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
console.log("[js step 1] data:", JSON.stringify(data));
console.log("[js step 1] handing off to Python");
