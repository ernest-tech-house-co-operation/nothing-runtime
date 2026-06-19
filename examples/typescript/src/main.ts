// examples/typescript/src/main.ts
//
// TypeScript source for the nth TS workflow demo. nth itself does NOT
// parse TypeScript — it runs the compiled .js output. The build step
// (tsc) is delegated to the configured `runtimes.build` tool, exactly
// as the spec's section 5c prescribes.
//
// Workflow (see examples/typescript/README.md for full instructions):
//   nth install -D typescript         # install tsc locally
//   nth init default                   # write nthconfig.json (if missing)
//   # edit nthconfig.json to add:  "runtimes": { "build": "tsc" }
//   nth build                          # tsc compiles src/*.ts → dist/*.js
//   nth dist/main.js                   # nth runs the compiled JS

interface User {
    id: number;
    name: string;
    email: string;
}

function greet(user: User): string {
    return `Hello, ${user.name} (id=${user.id}) — your email is ${user.email}`;
}

function processUsers(users: User[]): string[] {
    return users.map(u => greet(u));
}

const users: User[] = [
    { id: 1, name: "Alice", email: "alice@example.com" },
    { id: 2, name: "Bob",   email: "bob@example.com"   },
    { id: 3, name: "Carol", email: "carol@example.com" },
];

for (const line of processUsers(users)) {
    console.log(line);
}

console.log(`Processed ${users.length} users.`);
