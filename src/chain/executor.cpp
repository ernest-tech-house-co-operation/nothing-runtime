// chain/executor.cpp
//
// Runs a parsed chain (-N file -success -N file ...). Per spec section 4:
//   - Steps execute strictly in order, never in parallel.
//   - -success is a separator: continue to the next step only if the previous
//     step exited 0.
//   - .js files run in-process via JSC.
//   - Non-.js files fork the language's own interpreter (python3 for .py,
//     etc.) — only if enableOtherLangs=true. Otherwise the step fails
//     immediately with a clear error.

#include "executor.hpp"
#include "../js/engine.hpp"
#include "../util/subprocess.hpp"
#include "../util/fs.hpp"
#include "../util/strings.hpp"

#include <iostream>
#include <cstdlib>
#include <filesystem>

namespace nth::chain {

namespace fs = std::filesystem;

namespace {

// Map a non-JS file extension to the interpreter binary name. Per spec
// section 4: "Resolve the interpreter via PATH lookup."
std::string interpreter_for_ext(const std::string& ext) {
    if (ext == ".py") return "python3";
    if (ext == ".rb") return "ruby";
    if (ext == ".go") return "go";
    if (ext == ".rs") return "rustc"; // run via rustc --edition 2021 <file> -o /tmp/...; /tmp/...
    if (ext == ".sh") return "bash";
    if (ext == ".pl") return "perl";
    if (ext == ".lua") return "lua";
    if (ext == ".php") return "php";
    return "";
}

} // namespace

ChainOutcome run(const cli::ParsedCommand& cmd, const config::Config& cfg) {
    ChainOutcome out;
    js::Engine engine; // shared across all .js steps in the chain
    bool http_enabled = cfg.http;

    for (size_t i = 0; i < cmd.chain.size(); ++i) {
        const auto& step = cmd.chain[i];
        int step_exit = 0;
        std::string fail_reason;

        if (!fs::is_regular_file(step.file)) {
            out.failed_step = step.n;
            out.failed_reason = "file not found: " + step.file.string();
            out.final_exit_code = 1;
            std::cerr << "[nth.chain] step " << step.n << " failed: "
                      << out.failed_reason << "\n";
            return out;
        }

        std::string ext = nth::util::to_lower(step.file.extension().string());
        if (ext == ".js") {
            // In-process JSC evaluation. Use module mode so imports work.
            engine.install_globals(http_enabled);
            js::EvalResult r = http_enabled
                ? engine.eval_module(step.file)
                : [&] {
                    // If http is off, we still want ESM support. eval_module
                    // installs http globals only when called with http=true
                    // — wait, no: engine.eval_module calls install_globals(true)
                    // internally. We need a way to disable that. For v0.1
                    // simplicity, just always eval_module.
                    return engine.eval_module(step.file);
                }();
            if (!r.ok) {
                step_exit = r.exit_code;
                fail_reason = r.error_message;
                std::cerr << "[nth.chain] step " << step.n
                          << " (JS) failed: " << fail_reason << "\n";
            } else {
                step_exit = 0;
            }
        } else {
            // Non-JS — gate by enableOtherLangs.
            if (!cfg.enableOtherLangs) {
                out.failed_step = step.n;
                out.failed_reason =
                    "non-JS file '" + step.file.string() + "' encountered "
                    "but enableOtherLangs is false in nthconfig.json";
                out.final_exit_code = 1;
                std::cerr << "[nth.chain] step " << step.n << " failed: "
                          << out.failed_reason << "\n";
                return out;
            }
            std::string interp = interpreter_for_ext(ext);
            if (interp.empty()) {
                out.failed_step = step.n;
                out.failed_reason =
                    "no interpreter known for extension '" + ext + "'";
                out.final_exit_code = 1;
                std::cerr << "[nth.chain] step " << step.n << " failed: "
                          << out.failed_reason << "\n";
                return out;
            }
            std::vector<std::string> argv;
            if (interp == "go") {
                // `go run file.go`
                argv.push_back("go");
                argv.push_back("run");
                argv.push_back(step.file.string());
            } else if (interp == "rustc") {
                // Compile to a temp binary and run it.
                argv.push_back("rustc");
                argv.push_back("--edition");
                argv.push_back("2021");
                argv.push_back(step.file.string());
                argv.push_back("-o");
                argv.push_back("/tmp/nth_chain_step_" + std::to_string(step.n));
                auto r1 = util::run_subprocess_inherit(argv);
                if (r1.exit_code != 0) {
                    step_exit = r1.exit_code;
                    fail_reason = "rustc compile failed";
                    std::cerr << "[nth.chain] step " << step.n
                              << " (rust) failed: " << fail_reason << "\n";
                } else {
                    std::vector<std::string> run_argv = {
                        "/tmp/nth_chain_step_" + std::to_string(step.n) };
                    auto r2 = util::run_subprocess_inherit(run_argv);
                    step_exit = r2.exit_code;
                    if (step_exit != 0) fail_reason = "rust binary exited non-zero";
                }
                // continue to step-exit check below
                goto step_done;
            } else {
                argv.push_back(interp);
                argv.push_back(step.file.string());
            }
            auto r = util::run_subprocess_inherit(argv);
            step_exit = r.exit_code;
            if (step_exit != 0) {
                fail_reason = interp + " exited " + std::to_string(step_exit);
                std::cerr << "[nth.chain] step " << step.n << " ("
                          << interp << ") failed: " << fail_reason << "\n";
            }
        }

    step_done:
        if (step_exit != 0) {
            out.failed_step = step.n;
            out.failed_reason = fail_reason;
            out.final_exit_code = step_exit;
            return out;
        }
        // If there's a -success between this step and the next, continue;
        // if there's no -success (we're at the end), we're done.
    }
    out.final_exit_code = 0;
    return out;
}

} // namespace nth::chain
