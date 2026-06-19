// examples/modules/main.js
// Run with: nth modules/main.js
//
// Demonstrates ESM module resolution (section 3c): relative imports
// resolve against the importing file's directory; bare imports resolve
// by walking up looking for node_modules/<pkg>/.

import { greet, farewell } from "./util.js";
import { VERSION } from "./version.js";

console.log(greet("World"));
console.log("nth demo version:", VERSION);
console.log(farewell("World"));
