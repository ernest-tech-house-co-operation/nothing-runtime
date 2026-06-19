// js/engine.hpp — JavaScriptCore embed wrapper
#pragma once
#include <JavaScriptCore/JavaScript.h>
#include <string>
#include <functional>
#include <memory>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace nth::js {

namespace fs = std::filesystem;

struct EvalResult {
    bool ok = false;
    std::string error_message;   // uncaught exception text (or other error)
    int exit_code = 0;           // 0 on success, 1 on uncaught exception
};

// A persistent JSC global context. One per process is sufficient for v0.1.
class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Install globals. idempotent per engine. http_enabled controls whether
    // fetch/Request/Response/Nth.serve are exposed.
    void install_globals(bool http_enabled);

    // Evaluate `source` as a *script* (not a module) — used for the simplest
    // `nth index.js` path when no imports are detected.
    EvalResult eval_script(const std::string& source, const fs::path& file);

    // Evaluate `file` as an ES module, recursively resolving imports via the
    // module loader. The module loader's resolve callback is set up by the
    // engine.
    EvalResult eval_module(const fs::path& file);

    JSGlobalContextRef ctx() const { return ctx_; }

    // Module-graph integration: the module loader calls this when JSC asks it
    // to resolve+fetch a specifier.
    void set_module_loader_root(const fs::path& root) { module_root_ = root; }
    const fs::path& module_root() const { return module_root_; }

    // Helper for native globals to call back into JS error reporting.
    void report_exception(const std::string& msg);

private:
    JSGlobalContextRef ctx_ = nullptr;
    bool globals_installed_ = false;
    fs::path module_root_;
};

// Convert a JSValueRef to a printable string (uses ToString).
std::string value_to_string(JSGlobalContextRef ctx, JSValueRef v);

// Throw a JS TypeError exception with the given message. Returns nullptr.
// Sets *exception to a JSValueRef the caller can return as the function's
// result.
JSValueRef make_error(JSGlobalContextRef ctx, const std::string& msg, JSValueRef* exception);

} // namespace nth::js
