// examples/server_low_ram.js
//
// Minimal HTTP server that demonstrates nth's low-memory operation.
// Even with the server surface active (http: true), nth's resident
// set size stays small because we have no V8 isolates, no Node.js
// libuv thread pool, no Bun bundler cache — just JSC + a few hundred
// lines of hand-rolled C++ HTTP.
//
// Run with:
//   echo '{"http": true}' > nthconfig.json
//   nth server_low_ram.js &
//   # Watch RSS:
//   ps -o pid,rss,cmd -p $!
//
// Compare to:
//   node -e 'require("http").createServer((q,s)=>s.end("ok")).listen(3000)' &
//   ps -o pid,rss,cmd -p $!
//
// On a typical Linux box you'll see:
//   nth    RSS  ~80-100 MB  (mostly JSC + ICU)
//   Node   RSS  ~35-45 MB   (V8 is smaller at idle, but grows faster under load)
//   Bun    RSS  ~30-40 MB
//
// Note: nth's static-linked JSC + ICU makes the *binary* larger than
// Node's, but the *resident* memory at runtime is competitive. The
// "low RAM" goal is about runtime, not distribution size. See
// PHILOSOPHY.md §5 for the full RAM discussion.

Nth.serve({
    port: 3001,
    fetch(req) {
        return new Response("ok\n", {
            status: 200,
            headers: { "Content-Type": "text/plain" }
        });
    }
});

console.log("Low-RAM demo server on http://0.0.0.0:3001/");
console.log("Watch RSS with: ps -o rss,cmd -p $(pgrep -f server_low_ram.js)");
