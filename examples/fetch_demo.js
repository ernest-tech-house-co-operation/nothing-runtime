// examples/fetch_demo.js
//
// Demonstrates the native `fetch()` global. Requires `http: true` in
// nthconfig.json.
//
// Run with:
//   echo '{"http": true}' > nthconfig.json
//   nth fetch_demo.js
//
// fetch_demo.js fetches http://example.com/ and prints the response
// status + first 200 chars of the body. nth's fetch is implemented as
// a hand-rolled HTTP/1.1 client over BSD sockets — no libcurl, no
// external dependency, fast cold start.
//
// v0.1 note: nth's `fetch()` performs the request *synchronously*
// under the hood (single-threaded event loop in v0.1) and returns a
// Response-like object directly — not a Promise. The Response's body
// is available immediately via `.bodyText`. The `.text()` and
// `.json()` methods exist for spec compatibility but return thenables
// that may not chain cleanly through `await` in v0.1. See BUILD.md §6
// for the full limitation list.

const res = fetch("http://example.com/");
console.log("status:", res.status);
console.log("body length:", res.bodyText.length);
console.log("body (first 200 chars):");
console.log(res.bodyText.slice(0, 200));
