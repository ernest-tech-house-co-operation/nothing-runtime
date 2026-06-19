// js/module_loader.hpp — ESM resolution + evaluation
//
// v0.1 scope (per spec section 3c):
//   - ESM only (import/export syntax).
//   - Relative imports: resolve against importing file's directory.
//   - Bare/package imports: walk up looking for node_modules/<pkg>/, then
//     read package.json — "module" first, then "main", then index.js.
//   - Cache modules by absolute path so each module runs once.
//
// NOT supported (per spec):
//   - CommonJS interop (require/module.exports).
//   - package.json "exports" map resolution.
//   - Native addons, JSON imports, WASM imports.
#pragma once
#include <JavaScriptCore/JavaScript.h>
#include <string>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace nth::js::module_loader {

namespace fs = std::filesystem;

// Install module-loader hooks on the given context. The hooks intercept
// import specifier resolution and module source fetching, routing both
// through our C++ resolver.
void install(JSGlobalContextRef ctx);

// Evaluate `entry` (an absolute .js path) as an ES module and return the
// process exit status.
struct EvalResult {
    bool ok = false;
    std::string error_message;
    int exit_code = 0;
};
EvalResult eval_entry(JSGlobalContextRef ctx, const fs::path& entry);

// Resolve a specifier relative to an importer. Returns the resolved absolute
// file path, or empty on failure (with err set).
fs::path resolve_specifier(const fs::path& importer_dir,
                           const std::string& specifier,
                           std::string& err);

} // namespace nth::js::module_loader
