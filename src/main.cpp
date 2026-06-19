// main.cpp — nth CLI entry point
//
// Wires together the CLI parser, config loader, JS engine, chain executor,
// and runtimes/PM2 delegation. Per spec section 2: everything lives in one
// binary, one main().

#include "cli/parser.hpp"
#include "config/config.hpp"
#include "chain/executor.hpp"
#include "js/engine.hpp"
#include "js/http_server.hpp"
#include "runtimes/pm2.hpp"
#include "runtimes/role_resolver.hpp"
#include "util/fs.hpp"
#include "util/strings.hpp"
#include "util/subprocess.hpp"
#include "util/net.hpp"

#include <JavaScriptCore/JavaScript.h>

#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <csignal>
#include <string>

namespace fs = std::filesystem;

namespace {

// Run a single .js file through the JSC engine in module mode. After
// evaluation, if any server was registered via Nth.serve(), run the server
// event loop until interrupted. Returns the process exit code.
int run_js_file(const fs::path& file, const nth::config::Config& cfg,
                const std::vector<std::string>& /*extra*/) {
    if (cfg.userun) {
        std::cerr << "nth: userun=true in nthconfig.json — use `nth run <file>` instead.\n";
        return 1;
    }
    nth::js::Engine engine;
    nth::js::EvalResult r = engine.eval_module(file);
    if (!r.ok) {
        std::cerr << r.error_message << "\n";
        return r.exit_code;
    }
    int server_exit = nth::js::http_server::run_registered_servers(engine.ctx());
    if (server_exit != 0) return server_exit;
    return 0;
}

// Detect whether `nth <token>` should be treated as a single-file run, a
// prefix-match (section 4b), or a runtime role pass-through.
int handle_bare_token(const std::string& token,
                      const std::vector<std::string>& extra,
                      const nth::config::Config& cfg) {
    fs::path as_path(token);
    bool looks_like_path = token.find('/') != std::string::npos
                        || token.find('\\') != std::string::npos;
    if (looks_like_path) {
        if (fs::is_regular_file(as_path)) {
            return run_js_file(as_path, cfg, extra);
        } else {
            std::cerr << "nth: no such file: " << token << "\n";
            return 1;
        }
    }
    // Bare token — check if it's a .js file in cwd.
    fs::path candidate = fs::current_path() / (token + ".js");
    if (fs::is_regular_file(candidate)) {
        return run_js_file(candidate, cfg, extra);
    }
    // Or maybe the token IS the filename with .js already.
    fs::path candidate2 = fs::current_path() / token;
    if (fs::is_regular_file(candidate2)
        && nth::util::ends_with(token, ".js")) {
        return run_js_file(candidate2, cfg, extra);
    }
    // Prefix match (section 4b): files in cwd whose name starts with `token`
    // and ends in `.js`. First match wins, regardless of how many matches.
    auto matches = nth::util::fs::list_files_prefix_js(fs::current_path(), token);
    if (!matches.empty()) {
        return run_js_file(matches[0], cfg, extra);
    }
    // Otherwise — maybe it's a runtime role.
    auto it = cfg.runtimes.find(token);
    if (it != cfg.runtimes.end()) {
        auto r = nth::runtimes::run_role(cfg, token, "", extra, fs::current_path());
        if (!r.started) {
            std::cerr << "nth " << token << ": " << r.error_message << "\n";
            return 127;
        }
        return r.exit_code;
    }
    std::cerr << "nth: '" << token << "' is not a file, a prefix of any .js file "
              << "in the current directory, or a configured runtimes role.\n";
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    // Initialize Winsock on Windows (no-op on POSIX).
    nth::util::net::init();

    auto cmd = nth::cli::parse(argc, argv);
    auto cfg = nth::config::load_config(fs::current_path());

    switch (cmd.type) {
    case nth::cli::CommandType::Help:
        nth::cli::print_help(std::cout);
        return 0;
    case nth::cli::CommandType::Version:
        std::cout << "nth 0.1.0\n";
        return 0;
    case nth::cli::CommandType::InitDefault: {
        std::string err;
        if (!nth::config::init_default(fs::current_path(), err)) {
            std::cerr << "nth init default: " << err << "\n";
            return 1;
        }
        std::cout << "wrote nthconfig.json\n";
        return 0;
    }
    case nth::cli::CommandType::RunScript: {
        // `nth run <file>` — accepted only when userun=true. Spec section 5:
        // userun=false means `nth index.js` works without needing `nth run`.
        // When userun=true, the *bare* path is rejected (handled in
        // handle_bare_token via the userun check in run_js_file). So here,
        // `nth run <file>` should only be accepted when userun=true.
        if (!cfg.userun) {
            std::cerr << "nth: userun=false in nthconfig.json — `nth run <file>` "
                      << "is not enabled. Use `nth <file>` directly.\n";
            return 1;
        }
        if (cmd.entry_file.empty()) {
            std::cerr << "nth run: missing file argument\n";
            return 1;
        }
        return run_js_file(cmd.entry_file, cfg, cmd.passthrough_args);
    }
    case nth::cli::CommandType::PrefixMatch:
        // PrefixMatch path is only set by the parser when explicitly
        // recognized; in practice the parser defers bare tokens to
        // RolePass and we route them via handle_bare_token.
        return handle_bare_token(cmd.prefix.empty()
                                 ? cmd.role_name : cmd.prefix,
                                 cmd.passthrough_args, cfg);
    case nth::cli::CommandType::RolePass:
        return handle_bare_token(cmd.role_name, cmd.passthrough_args, cfg);
    case nth::cli::CommandType::Chain: {
        auto out = nth::chain::run(cmd, cfg);
        return out.final_exit_code;
    }
    case nth::cli::CommandType::Install: {
        auto r = nth::runtimes::pm2::install(cfg, cmd.passthrough_args);
        if (!r.started) {
            std::cerr << "nth install: " << r.error_message << "\n";
            return 127;
        }
        return r.exit_code;
    }
    case nth::cli::CommandType::Test: {
        auto r = nth::runtimes::pm2::test(cfg, cmd.passthrough_args);
        if (!r.started) {
            std::cerr << "nth test: " << r.error_message << "\n";
            return 127;
        }
        return r.exit_code;
    }
    case nth::cli::CommandType::StartAlways: {
        if (cmd.entry_file.empty()) {
            std::cerr << "nth startalways: missing file argument\n";
            return 1;
        }
        auto r = nth::runtimes::pm2::start_always(fs::current_path(),
                                                  cmd.entry_file,
                                                  cmd.startalways_name,
                                                  cmd.passthrough_args);
        if (!r.started) {
            std::cerr << "nth startalways: " << r.error_message << "\n";
            return 127;
        }
        return r.exit_code;
    }
    case nth::cli::CommandType::Build: {
        auto r = nth::runtimes::run_role(cfg, "build", "", cmd.passthrough_args,
                                         fs::current_path());
        if (!r.started) {
            std::cerr << "nth build: " << r.error_message << "\n";
            return 127;
        }
        return r.exit_code;
    }
    case nth::cli::CommandType::Lint: {
        auto r = nth::runtimes::run_role(cfg, "lint", "", cmd.passthrough_args,
                                         fs::current_path());
        if (!r.started) {
            std::cerr << "nth lint: " << r.error_message << "\n";
            return 127;
        }
        return r.exit_code;
    }
    }
    // Unreachable
    return 0;
}
