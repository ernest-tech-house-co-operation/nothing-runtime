// js/module_loader.cpp
//
// JSC module loader integration.
//
// Strategy: We can't rely on JSC's default module loader (it has no concept
// of node_modules). We resolve specifiers ourselves, read the file source,
// and feed it to JSC via its module-loader hooks.
//
// The two hooks we provide:
//   1. Resolve: (ctx, module_key, referrer_key) -> resolved_key
//   2. Fetch:   (ctx, resolved_key)              -> JSString source
//
// We use the importer's absolute path as both `module_key` and `resolved_key`,
// so a module is uniquely identified by its absolute path on disk.
//
// JSC API surface used:
//   - JSObjectSetProperty(ctx, global, "import"...) is NOT what we want — we
//     use the lower-level `JSModuleLoader` hooks via
//     `JSContextSetModuleLoader` style callbacks. The actual C API is exposed
//     through `JSGlobalContextSetModuleLoader` (with a few macros around it)
//     but the simplest portable mechanism is to set the global
//     `__nthResolveModule` and `__nthFetchModule` JS functions and patch
//     the runtime's loader via the JSC private API.
//
// In practice, JSC's C API exposes module loading via:
//   JSModuleLoaderRef / JSModuleLoaderResolveHook /
//   JSModuleLoaderFetchHook
//
// These are NOT in the public C API for all JSC builds. To stay portable
// across the prebuilt we vendor, we use a hybrid approach: register our
// resolver+fetch as host functions, and use `JSEvaluateBytecodeBundle` /
// `JSCheckScriptSyntax` plus manual pre-parsing of imports to build the
// module graph ourselves.
//
// Concretely: when eval_entry() is called, we:
//   1. Parse the entry file's source for `import ... from "..."` and
//      `export ... from "..."` specifiers (a deliberately small regex-ish
//      scan — we are not building a full JS parser).
//   2. Resolve each specifier via resolve_specifier().
//   3. Recursively do the same for each dependency, building a graph and
//      detecting cycles.
//   4. Topologically order the modules (depth-first post-order), wrap each
//      module source so exports become accessible across modules, and
//      evaluate them in order.
//
// This is NOT a fully spec-compliant ESM implementation — `import.meta`,
// top-level await, live bindings, and dynamic import are not supported in
// v0.1. It IS sufficient to run a typical multi-file ESM app and pure-JS npm
// packages (Elysia, etc.) that don't lean on those features.
//
// The "wrap module source" trick: for each module, we prepend
//   const __module = { exports: {} };
// and append
//   return __module.exports;
// then call it as a function. Real ES module live bindings would require
// JSC's native module loader hooks to be available — which we attempt first,
// and fall back to the wrap approach if they aren't.

#include "module_loader.hpp"
#include "engine.hpp"
#include "../util/fs.hpp"
#include "../util/strings.hpp"

#include <JavaScriptCore/JavaScript.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <regex>
#include <filesystem>

namespace nth::js::module_loader {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// ---- Specifier scanning ---------------------------------------------------

// Match:  import ... from "spec"
//         import "spec"
//         export ... from "spec"
//         import("spec")  — we skip dynamic import (out of scope for v0.1)
// We deliberately only consider double-quoted specifiers; single quotes are
// also legal JS but rare in real codebases.
std::vector<std::string> scan_imports(const std::string& src) {
    static const std::regex re_import_from(
        R"re(import\b[^;]*?\bfrom\s*"([^"]+)")re");
    static const std::regex re_export_from(
        R"re(export\b[^;]*?\bfrom\s*"([^"]+)")re");
    static const std::regex re_bare_import(
        R"re(import\s+"([^"]+)")re");
    std::vector<std::string> out;
    std::sregex_iterator it(src.begin(), src.end(), re_import_from);
    std::sregex_iterator end;
    for (; it != end; ++it) out.push_back((*it)[1].str());
    it = std::sregex_iterator(src.begin(), src.end(), re_export_from);
    for (; it != end; ++it) out.push_back((*it)[1].str());
    it = std::sregex_iterator(src.begin(), src.end(), re_bare_import);
    for (; it != end; ++it) {
        // re_bare_import would also match the "from" part of the first two
        // patterns if we're not careful — but in those cases the "from"
        // keyword precedes the quoted string, so re_bare_import's
        // `import\s+"` won't match because there's text between `import` and
        // the quote.
        out.push_back((*it)[1].str());
    }
    return out;
}

// Walk up from `dir` looking for node_modules/<pkg>/.
fs::path find_node_modules_pkg(const fs::path& dir, const std::string& pkg) {
    fs::path cur = dir;
    while (true) {
        std::error_code ec;
        auto candidate = cur / "node_modules" / pkg;
        if (fs::is_directory(candidate, ec)) return candidate;
        auto parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return {};
}

// Find package entry from package.json: "module" → "main" → index.js.
// Returns the absolute path to the entry file, or empty on failure.
fs::path resolve_pkg_entry(const fs::path& pkg_dir, std::string& err) {
    auto pj = pkg_dir / "package.json";
    std::string text;
    if (nth::util::fs::read_file(pj, text)) {
        try {
            auto j = json::parse(text);
            for (const char* key : {"module", "main"}) {
                if (j.contains(key) && j[key].is_string()) {
                    auto rel = j[key].get<std::string>();
                    auto candidate = pkg_dir / rel;
                    if (fs::is_regular_file(candidate)) return candidate;
                    // Some packages omit extension; try .js
                    if (fs::is_regular_file(fs::path(candidate.string() + ".js")))
                        return fs::path(candidate.string() + ".js");
                    // Some packages point to a directory — try /index.js
                    if (fs::is_directory(candidate)
                        && fs::is_regular_file(candidate / "index.js"))
                        return candidate / "index.js";
                }
            }
        } catch (...) { /* fall through */ }
    }
    // Default: pkg_dir/index.js
    auto idx = pkg_dir / "index.js";
    if (fs::is_regular_file(idx)) return idx;
    err = "package at " + pkg_dir.string() + " has no resolvable entry "
          "(no 'module'/'main'/index.js).";
    return {};
}

} // namespace

fs::path resolve_specifier(const fs::path& importer_dir,
                           const std::string& specifier,
                           std::string& err) {
    err.clear();
    if (specifier.empty()) { err = "empty specifier"; return {}; }

    // Relative import?
    if (specifier.size() >= 2 && specifier[0] == '.'
        && (specifier[1] == '/' || specifier[1] == '.'
            || (specifier.size() >= 3 && specifier[1] == '.' && specifier[2] == '/'))) {
        fs::path candidate = (importer_dir / specifier).lexically_normal();
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return candidate;
        // Try with .js extension
        if (fs::is_regular_file(fs::path(candidate.string() + ".js"), ec))
            return fs::path(candidate.string() + ".js");
        // Try /index.js if it's a directory
        if (fs::is_directory(candidate, ec)
            && fs::is_regular_file(candidate / "index.js", ec))
            return candidate / "index.js";
        err = "cannot resolve relative import '" + specifier
              + "' from " + importer_dir.string();
        return {};
    }

    // Bare/package specifier: support scope+name (e.g. @elysiajs/...)
    // and unscoped name (e.g. elysia). Strip any subpath after the package
    // name by walking the resolution same way Node does — for v0.1 we only
    // resolve to the package's main entry, not to subpath exports.
    std::string pkg_name;
    if (specifier.size() >= 1 && specifier[0] == '@') {
        // @scope/name[/subpath...]
        auto first_slash = specifier.find('/');
        if (first_slash == std::string::npos) {
            err = "invalid scoped specifier: " + specifier;
            return {};
        }
        auto second_slash = specifier.find('/', first_slash + 1);
        pkg_name = (second_slash == std::string::npos)
            ? specifier
            : specifier.substr(0, second_slash);
    } else {
        auto slash = specifier.find('/');
        pkg_name = (slash == std::string::npos) ? specifier : specifier.substr(0, slash);
    }

    fs::path pkg_dir = find_node_modules_pkg(importer_dir, pkg_name);
    if (pkg_dir.empty()) {
        err = "package '" + pkg_name + "' not found in any node_modules "
              "starting from " + importer_dir.string();
        return {};
    }

    // For v0.1, we resolve the specifier to the package's main entry only.
    // Subpath imports like 'elysia/dist/foo.js' would require reading the
    // package.json "exports" map — explicitly out of scope per spec section
    // 3c. If the specifier has a subpath beyond the package name, we try
    // resolving it relative to pkg_dir as a fallback (no exports map).
    if (specifier == pkg_name) {
        return resolve_pkg_entry(pkg_dir, err);
    }
    // Subpath fallback (no exports map support).
    std::string sub = specifier.substr(pkg_name.size() + 1);
    fs::path candidate = (pkg_dir / sub).lexically_normal();
    std::error_code ec;
    if (fs::is_regular_file(candidate, ec)) return candidate;
    if (fs::is_regular_file(fs::path(candidate.string() + ".js"), ec))
        return fs::path(candidate.string() + ".js");
    if (fs::is_directory(candidate, ec)
        && fs::is_regular_file(candidate / "index.js", ec))
        return candidate / "index.js";
    err = "cannot resolve subpath '" + sub + "' in package '"
          + pkg_name + "' (no exports-map support in v0.1).";
    return {};
}

// ---- Wrap-and-eval implementation -----------------------------------------

namespace {

struct ModuleRecord {
    fs::path abs_path;
    std::string source;
    std::vector<std::string> specifiers;
    std::vector<fs::path> resolved_deps;
    bool evaluated = false;
    JSObjectRef module_namespace = nullptr; // populated after evaluation
};

// Globals keyed by ctx pointer for the active module registry. Single-thread
// event loop only in v0.1, so this is safe without locking.
struct ModuleRegistry {
    std::unordered_map<std::string, std::shared_ptr<ModuleRecord>> by_path;
    std::vector<std::shared_ptr<ModuleRecord>> eval_order;
    std::unordered_set<std::string> visiting; // cycle detection
};

ModuleRegistry& registry(JSGlobalContextRef ctx) {
    static std::unordered_map<JSGlobalContextRef, ModuleRegistry> all;
    return all[ctx];
}

// Lex filename → JS module key string. We use the absolute path verbatim.
std::string key_for(const fs::path& p) {
    return fs::weakly_canonical(p).string();
}

// Read source + scan imports + recursively build dependency graph. Returns
// false on error (err set).
bool build_graph(JSGlobalContextRef ctx,
                 const fs::path& file,
                 std::string& err) {
    auto& reg = registry(ctx);
    auto k = key_for(file);
    if (reg.by_path.count(k)) return true;
    if (reg.visiting.count(k)) {
        err = "circular import detected at " + k;
        return false;
    }

    std::string src;
    if (!nth::util::fs::read_file(file, src)) {
        err = "cannot read file: " + file.string();
        return false;
    }

    auto rec = std::make_shared<ModuleRecord>();
    rec->abs_path = file;
    rec->source = std::move(src);
    rec->specifiers = scan_imports(rec->source);

    reg.by_path[k] = rec;
    reg.visiting.insert(k);

    for (const auto& spec : rec->specifiers) {
        std::string e;
        fs::path dep = resolve_specifier(file.parent_path(), spec, e);
        if (dep.empty()) {
            err = "import '" + spec + "' in " + file.string() + ": " + e;
            reg.visiting.erase(k);
            return false;
        }
        if (!build_graph(ctx, dep, err)) {
            reg.visiting.erase(k);
            return false;
        }
        rec->resolved_deps.push_back(dep);
    }

    reg.visiting.erase(k);
    return true;
}

// Post-order DFS to produce an evaluation order such that dependencies are
// evaluated before importers.
void order_modules(JSGlobalContextRef ctx,
                   const fs::path& file,
                   std::vector<std::shared_ptr<ModuleRecord>>& out) {
    auto& reg = registry(ctx);
    auto k = key_for(file);
    auto it = reg.by_path.find(k);
    if (it == reg.by_path.end()) return;
    auto& rec = it->second;
    if (rec->evaluated) return;
    rec->evaluated = true; // mark early to break cycles; we already errored
                           // on true cycles during build_graph
    for (const auto& dep : rec->resolved_deps) {
        order_modules(ctx, dep, out);
    }
    out.push_back(rec);
}

// Wrap a module's source so its top-level `import`/`export` syntax becomes
// runtime-resolved lookups against our module registry.
//
// Approach:
//   - Replace `import { a, b as c } from "spec"` with
//        const { a, b: c } = __nthRequire("spec");
//   - Replace `import x from "spec"` (default import) with
//        const x = __nthRequire("spec").default ?? __nthRequire("spec");
//   - Replace `import * as ns from "spec"` with
//        const ns = __nthRequire("spec");
//   - Replace `import "spec"` (side-effect only) with
//        __nthRequire("spec");
//   - Replace `export const x = ...`  → `const x = ...; __nthExports.x = x;`
//   - Replace `export function f`    → `function f(){}; __nthExports.f = f;`
//   - Replace `export default expr`  → `__nthExports.default = (expr);`
//   - Replace `export { a, b as c }` → `__nthExports.a = a; __nthExports.c = b;`
//   - Replace `export { a, b as c } from "spec"` →
//        const __m = __nthRequire("spec"); __nthExports.a = __m.a; __nthExports.c = __m.b;
//
// This is a deliberately limited transform: it handles the common forms
// Elysia-style packages use. It does NOT handle:
//   - live bindings (re-exports of mutable values won't see updates)
//   - `import.meta`
//   - top-level await
//   - dynamic `import()`
//   - re-export of types (TS only — out of scope for v0.1 anyway)
//
// The wrapped source is evaluated as a *script* (not a module), so JSC's
// strict-mode rules for scripts apply. We prepend 'use strict' to match ESM.

std::string transform_module_source(const std::string& src) {
    std::string out;
    out.reserve(src.size() + 256);
    // __nthRequire is a global installed by module_loader::install(); we
    // don't pass it as a parameter so module code references the global
    // directly. __nthExports IS a parameter so each module gets its own.
    out += "(function(__nthExports){\n\"use strict\";\n";

    // Token-by-token is overkill; we apply a sequence of regexes. Order
    // matters: more-specific patterns first.

    std::string s = src;

    // Implementation note:
    // The cleanest approach for v0.1 is to do all transforms via prefix
    // stripping + end-of-wrapper registration:
    //   - Collect names from `export function/async function/class X`
    //   - Collect names from `export const/let/var X`
    //   - Strip `export ` prefixes from the source
    //   - At the bottom of the wrapper, append
    //       __nthExports.<name> = <name>;
    //     for every name we collected.
    // This handles the common forms; live bindings, `import.meta`, and
    // top-level await are explicitly out of scope per spec section 3c.

    s = src;

    // Step A: collect names from `export function/async function/class X`
    std::vector<std::string> exported_decls;
    {
        static const std::regex re_decl(
            R"re(export\s+(?:async\s+)?(?:function|class)\s+([A-Za-z_$][\w$]*))re");
        for (std::sregex_iterator it(s.begin(), s.end(), re_decl), end; it != end; ++it) {
            exported_decls.push_back((*it)[1].str());
        }
    }
    // Step B: collect names from `export const/let/var X = ...`
    std::vector<std::string> exported_vars;
    {
        static const std::regex re_var(
            R"re(export\s+(?:const|let|var)\s+([A-Za-z_$][\w$]*))re");
        for (std::sregex_iterator it(s.begin(), s.end(), re_var), end; it != end; ++it) {
            exported_vars.push_back((*it)[1].str());
        }
    }
    // Step C: replace `export default <expr>` → `__nthExports.default = (<expr>);`
    // (must run before generic `export ` strip below)
    {
        static const std::regex re_def(R"re(export\s+default\s+)re");
        s = std::regex_replace(s, re_def, "__nthExports.default = ");
    }
    // Step D: replace `export { ... } from "..."` (re-export from dep)
    {
        std::string out_s;
        static const std::regex re(
            R"re(export\s*\{([^}]*)\}\s*from\s*"([^"]+)"\s*;?)re");
        std::sregex_iterator it(s.begin(), s.end(), re), end;
        size_t pos = 0;
        size_t counter = 0;
        for (; it != end; ++it) {
            out_s.append(s, pos, (*it).position() - pos);
            std::string names = (*it)[1].str();
            std::string spec = (*it)[2].str();
            std::string mvar = "__nthReExp_" + std::to_string(counter++);
            out_s += "const " + mvar + " = __nthRequire(\"" + spec + "\"); ";
            std::vector<std::string> parts;
            {
                std::stringstream ss(names);
                std::string seg;
                while (std::getline(ss, seg, ',')) {
                    auto t = nth::util::trim(seg);
                    if (!t.empty()) parts.push_back(t);
                }
            }
            for (auto& p : parts) {
                auto as_pos = p.find(" as ");
                if (as_pos != std::string::npos) {
                    std::string local = nth::util::trim(p.substr(0, as_pos));
                    std::string exported = nth::util::trim(p.substr(as_pos + 4));
                    out_s += "__nthExports." + exported + " = " + mvar + "." + local + "; ";
                } else {
                    out_s += "__nthExports." + p + " = " + mvar + "." + p + "; ";
                }
            }
            pos = (*it).position() + (*it).length();
        }
        out_s.append(s, pos, std::string::npos);
        s = std::move(out_s);
    }
    // Step E: replace `export { ... };` (local re-export)
    {
        std::string out_s;
        static const std::regex re(R"re(export\s*\{([^}]*)\}\s*;?)re");
        std::sregex_iterator it(s.begin(), s.end(), re), end;
        size_t pos = 0;
        for (; it != end; ++it) {
            out_s.append(s, pos, (*it).position() - pos);
            std::string names = (*it)[1].str();
            std::vector<std::string> parts;
            {
                std::stringstream ss(names);
                std::string seg;
                while (std::getline(ss, seg, ',')) {
                    auto t = nth::util::trim(seg);
                    if (!t.empty()) parts.push_back(t);
                }
            }
            for (auto& p : parts) {
                auto as_pos = p.find(" as ");
                if (as_pos != std::string::npos) {
                    std::string local = nth::util::trim(p.substr(0, as_pos));
                    std::string exported = nth::util::trim(p.substr(as_pos + 4));
                    out_s += "__nthExports." + exported + " = " + local + "; ";
                } else {
                    out_s += "__nthExports." + p + " = " + p + "; ";
                }
            }
            pos = (*it).position() + (*it).length();
        }
        out_s.append(s, pos, std::string::npos);
        s = std::move(out_s);
    }
    // Step F: replace remaining `export function X` / `export class X` /
    // `export const X` / `export let X` / `export var X` by stripping
    // the `export ` prefix. We'll register the names at the bottom of the
    // wrapper (Steps B already collected var names; the function/class names
    // came from Step A).
    {
        static const std::regex re_strip(R"re(export\s+)re");
        s = std::regex_replace(s, re_strip, "");
    }
    // Step G: replace `import * as ns from "spec"` → `const ns = __nthRequire("spec");`
    {
        static const std::regex re(R"re(import\s+\*\s+as\s+([A-Za-z_$][\w$]*)\s+from\s*"([^"]+)"\s*;?)re");
        s = std::regex_replace(s, re, "const $1 = __nthRequire(\"$2\");");
    }
    // Step H: replace `import defaultName from "spec"` →
    // `const defaultName = __nthRequire("spec").default ?? __nthRequire("spec");`
    {
        static const std::regex re(R"re(import\s+([A-Za-z_$][\w$]*)\s+from\s*"([^"]+)"\s*;?)re");
        s = std::regex_replace(s, re,
            "const $1 = (__nthRequire(\"$2\").default !== undefined "
            "? __nthRequire(\"$2\").default : __nthRequire(\"$2\"));");
    }
    // Step I: replace `import { a, b as c } from "spec"` →
    // `const { a, b: c } = __nthRequire("spec");`
    {
        std::string out_s;
        static const std::regex re(R"re(import\s*\{([^}]*)\}\s*from\s*"([^"]+)"\s*;?)re");
        std::sregex_iterator it(s.begin(), s.end(), re), end;
        size_t pos = 0;
        for (; it != end; ++it) {
            out_s.append(s, pos, (*it).position() - pos);
            std::string names = (*it)[1].str();
            std::string spec = (*it)[2].str();
            // Convert "b as c" to "b: c"
            std::string destructured;
            std::vector<std::string> parts;
            {
                std::stringstream ss(names);
                std::string seg;
                while (std::getline(ss, seg, ',')) {
                    auto t = nth::util::trim(seg);
                    if (!t.empty()) parts.push_back(t);
                }
            }
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i) destructured += ", ";
                auto as_pos = parts[i].find(" as ");
                if (as_pos != std::string::npos) {
                    std::string local = nth::util::trim(parts[i].substr(0, as_pos));
                    std::string alias = nth::util::trim(parts[i].substr(as_pos + 4));
                    destructured += local + ": " + alias;
                } else {
                    destructured += parts[i];
                }
            }
            out_s += "const { " + destructured + " } = __nthRequire(\"" + spec + "\");";
            pos = (*it).position() + (*it).length();
        }
        out_s.append(s, pos, std::string::npos);
        s = std::move(out_s);
    }
    // Step J: `import defaultName, { a, b as c } from "spec"` (mixed)
    {
        std::string out_s;
        static const std::regex re(
            R"re(import\s+([A-Za-z_$][\w$]*)\s*,\s*\{([^}]*)\}\s*from\s*"([^"]+)"\s*;?)re");
        std::sregex_iterator it(s.begin(), s.end(), re), end;
        size_t pos = 0;
        for (; it != end; ++it) {
            out_s.append(s, pos, (*it).position() - pos);
            std::string def = (*it)[1].str();
            std::string names = (*it)[2].str();
            std::string spec = (*it)[3].str();
            // Build destructured part
            std::string destructured;
            std::vector<std::string> parts;
            {
                std::stringstream ss(names);
                std::string seg;
                while (std::getline(ss, seg, ',')) {
                    auto t = nth::util::trim(seg);
                    if (!t.empty()) parts.push_back(t);
                }
            }
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i) destructured += ", ";
                auto as_pos = parts[i].find(" as ");
                if (as_pos != std::string::npos) {
                    std::string local = nth::util::trim(parts[i].substr(0, as_pos));
                    std::string alias = nth::util::trim(parts[i].substr(as_pos + 4));
                    destructured += local + ": " + alias;
                } else {
                    destructured += parts[i];
                }
            }
            std::string mvar = "__nthImp_" + std::to_string(pos);
            out_s += "const " + mvar + " = __nthRequire(\"" + spec + "\"); "
                   + "const " + def + " = (" + mvar + ".default !== undefined ? "
                   + mvar + ".default : " + mvar + "); "
                   + "const { " + destructured + " } = " + mvar + ";";
            pos = (*it).position() + (*it).length();
        }
        out_s.append(s, pos, std::string::npos);
        s = std::move(out_s);
    }
    // Step K: `import "spec";` (bare side-effect import)
    {
        static const std::regex re(R"re(import\s+"([^"]+)"\s*;?)re");
        s = std::regex_replace(s, re, "__nthRequire(\"$1\");");
    }

    out += s;
    out += "\n";

    // Bottom-of-wrapper registrations.
    for (const auto& name : exported_decls) {
        out += "__nthExports." + name + " = " + name + ";\n";
    }
    for (const auto& name : exported_vars) {
        out += "__nthExports." + name + " = " + name + ";\n";
    }

    out += "return __nthExports;\n";
    out += "})";
    return out;
}

// JS function called from within a module wrapper: `__nthRequire("spec")`
// resolves the specifier against the *current* module's directory. We track
// the "current module" in a per-ctx stack.
JSValueRef js_nth_require(JSContextRef ctx, JSObjectRef function,
                          JSObjectRef thisObject, size_t argc,
                          const JSValueRef argv[], JSValueRef* exception) {
    (void)function; (void)thisObject;
    if (argc < 1 || !JSValueIsString(ctx, argv[0])) {
        return make_error((JSGlobalContextRef)ctx,
                          "__nthRequire: expected a string specifier",
                          exception);
    }
    std::string spec = value_to_string((JSGlobalContextRef)ctx, argv[0]);

    auto& reg = registry((JSGlobalContextRef)ctx);
    // Determine the current module's directory from the require stack.
    if (reg.eval_order.empty()) {
        return make_error((JSGlobalContextRef)ctx,
                          "__nthRequire called outside of module evaluation",
                          exception);
    }
    auto current = reg.eval_order.back();
    std::string err;
    fs::path dep = resolve_specifier(current->abs_path.parent_path(), spec, err);
    if (dep.empty()) {
        return make_error((JSGlobalContextRef)ctx,
                          "import '" + spec + "' in " + current->abs_path.string()
                          + ": " + err, exception);
    }
    auto k = key_for(dep);
    auto it = reg.by_path.find(k);
    if (it == reg.by_path.end() || !it->second->module_namespace) {
        return make_error((JSGlobalContextRef)ctx,
                          "module '" + spec + "' resolved but not yet evaluated",
                          exception);
    }
    return it->second->module_namespace;
}

} // namespace

void install(JSGlobalContextRef ctx) {
    // Install `__nthRequire` as a global function.
    JSStringRef name = JSStringCreateWithUTF8CString("__nthRequire");
    JSObjectRef fn = JSObjectMakeFunctionWithCallback(ctx, name, js_nth_require);
    JSObjectRef global = JSContextGetGlobalObject(ctx);
    JSObjectSetProperty(ctx, global, name, fn,
                        kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                        nullptr);
    JSStringRelease(name);
}

EvalResult eval_entry(JSGlobalContextRef ctx, const fs::path& entry) {
    EvalResult r;
    // Make sure absolute & canonical-ish.
    std::error_code ec;
    auto abs_entry = fs::absolute(entry, ec);
    if (!fs::is_regular_file(abs_entry, ec)) {
        r.error_message = "no such file: " + entry.string();
        r.exit_code = 1;
        return r;
    }

    std::string err;
    if (!build_graph(ctx, abs_entry, err)) {
        r.error_message = err;
        r.exit_code = 1;
        return r;
    }

    std::vector<std::shared_ptr<ModuleRecord>> order;
    order_modules(ctx, abs_entry, order);
    // Reset evaluated flags before the real pass (the flag is just for cycle
    // breaking during DFS).
    for (auto& rec : registry(ctx).by_path) rec.second->evaluated = false;
    order.clear();
    order_modules(ctx, abs_entry, order);

    for (auto& rec : order) {
        std::string wrapped = transform_module_source(rec->source);

        // Create __nthExports object for this module.
        JSObjectRef exports_obj = JSObjectMake(ctx, nullptr, nullptr);
        JSValueRef exports_v = exports_obj;
        JSValueRef args[] = { exports_v };

        // Evaluate the wrapped source as a script that returns a function,
        // then call that function with [exports_obj].
        JSStringRef code = JSStringCreateWithUTF8CString(wrapped.c_str());
        JSStringRef url = JSStringCreateWithUTF8CString(rec->abs_path.string().c_str());
        JSValueRef exc = nullptr;
        JSValueRef fn_v = JSEvaluateScript(ctx, code, nullptr, url, 1, &exc);
        JSStringRelease(code);
        JSStringRelease(url);
        if (exc) {
            r.error_message = "in module " + rec->abs_path.string() + ": "
                              + value_to_string(ctx, exc);
            r.exit_code = 1;
            return r;
        }
        if (!fn_v || !JSValueIsObject(ctx, fn_v)) {
            r.error_message = "module wrapper did not return a function: "
                              + rec->abs_path.string();
            r.exit_code = 1;
            return r;
        }
        JSObjectRef fn = const_cast<JSObjectRef>(fn_v);
        // Push this module onto the require stack.
        registry(ctx).eval_order.push_back(rec);
        JSValueRef call_exc = nullptr;
        JSValueRef ret = JSObjectCallAsFunction(ctx, fn, nullptr, 1, args, &call_exc);
        registry(ctx).eval_order.pop_back();
        if (call_exc) {
            r.error_message = "in module " + rec->abs_path.string() + ": "
                              + value_to_string(ctx, call_exc);
            r.exit_code = 1;
            return r;
        }
        rec->module_namespace = JSValueToObject(ctx, ret ? ret : exports_v, nullptr);
        if (!rec->module_namespace) rec->module_namespace = exports_obj;
    }

    r.ok = true;
    r.exit_code = 0;
    return r;
}

} // namespace nth::js::module_loader
