// examples/server.js
// Run with: nth server.js
// (Requires nthconfig.json with `"http": true`.)
//
// Demonstrates Nth.serve(), fetch(), Request/Response, and WebSocket
// upgrade — the four primitives nth exposes that JS server frameworks
// (Elysia, etc.) build on top of.

Nth.serve({
    port: 3000,
    fetch(req) {
        const url = req.url;
        if (url === "/") {
            return new Response("hello from nth", {
                status: 200,
                headers: { "Content-Type": "text/plain" }
            });
        }
        if (url === "/json") {
            return new Response(JSON.stringify({ ok: true, time: Date.now() }), {
                status: 200,
                headers: { "Content-Type": "application/json" }
            });
        }
        if (url === "/echo-method") {
            return new Response("method was " + req.method, { status: 200 });
        }
        return new Response("not found: " + url, { status: 404 });
    },
    websocket: {
        open(peer) {
            console.log("[ws] client connected");
            peer.send("welcome to nth websocket");
        },
        message(peer, data) {
            console.log("[ws] received:", data);
            peer.send("echo: " + data);
        },
        close(peer) {
            console.log("[ws] client disconnected");
        }
    }
});

console.log("server.js evaluated — Nth.serve() registered. Ctrl-C to stop.");
