// cli/parser.cpp
#include "parser.hpp"
#include "../util/strings.hpp"

#include <iostream>
#include <sstream>
#include <cstdlib>

namespace nth::cli {

namespace fs = std::filesystem;

namespace {
bool is_step_flag(const std::string& a, int& n) {
    if (a.size() < 2 || a[0] != '-') return false;
    // "-N" where N is an integer starting at 1
    for (size_t i = 1; i < a.size(); ++i) {
        if (a[i] < '0' || a[i] > '9') return false;
    }
    try {
        n = std::stoi(a.substr(1));
        return n >= 1;
    } catch (...) { return false; }
}
} // namespace

ParsedCommand parse(int argc, char** argv) {
    ParsedCommand r;

    // argv[0] is binary name.
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);

    if (args.empty()) {
        r.type = CommandType::Help;
        return r;
    }

    const std::string& first = args[0];

    // Subcommand-style entries.
    if (first == "--help" || first == "-h" || first == "help") {
        r.type = CommandType::Help;
        return r;
    }
    if (first == "--version" || first == "-v" || first == "version") {
        r.type = CommandType::Version;
        return r;
    }
    if (first == "init") {
        // `nth init default` only
        if (args.size() >= 2 && args[1] == "default") {
            r.type = CommandType::InitDefault;
        } else {
            r.type = CommandType::Help;
        }
        return r;
    }
    if (first == "install") {
        r.type = CommandType::Install;
        for (size_t i = 1; i < args.size(); ++i)
            r.passthrough_args.push_back(args[i]);
        return r;
    }
    if (first == "test") {
        r.type = CommandType::Test;
        for (size_t i = 1; i < args.size(); ++i)
            r.passthrough_args.push_back(args[i]);
        return r;
    }
    if (first == "startalways") {
        r.type = CommandType::StartAlways;
        // nth startalways <file> --name <name>
        for (size_t i = 1; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (a == "--name" && i + 1 < args.size()) {
                r.startalways_name = args[i + 1];
                ++i;
            } else if (a.size() > 2 && a.compare(0, 7, "--name=") == 0) {
                r.startalways_name = a.substr(7);
            } else if (a.empty() || a[0] == '-') {
                r.unknown_flags.push_back(a);
            } else if (r.entry_file.empty()) {
                r.entry_file = a;
            } else {
                r.passthrough_args.push_back(a);
            }
        }
        return r;
    }

    // Role pass-through: nth build, nth lint, or any role in runtimes table.
    // We can't know at parse time which roles are configured (config hasn't
    // been loaded yet), so we accept "build"/"lint" directly, and defer
    // arbitrary role lookup to main.cpp via CommandType::RolePass with the
    // first token as the role name.
    if (first == "build") {
        r.type = CommandType::Build;
        for (size_t i = 1; i < args.size(); ++i)
            r.passthrough_args.push_back(args[i]);
        return r;
    }
    if (first == "lint") {
        r.type = CommandType::Lint;
        for (size_t i = 1; i < args.size(); ++i)
            r.passthrough_args.push_back(args[i]);
        return r;
    }
    if (first == "run") {
        // `nth run <file>` — accepted unconditionally; if userun is false,
        // main.cpp will reject this as a usage error per spec.
        if (args.size() < 2) {
            r.type = CommandType::Help;
            return r;
        }
        r.type = CommandType::RunScript;
        r.entry_file = args[1];
        for (size_t i = 2; i < args.size(); ++i)
            r.passthrough_args.push_back(args[i]);
        return r;
    }

    // Anything else — check whether this is a chain (-N first) or a single
    // file/prefix run.
    int step_n = 0;
    if (is_step_flag(first, step_n)) {
        // Chain grammar.
        r.type = CommandType::Chain;
        size_t i = 0;
        ChainStep* cur = nullptr;
        while (i < args.size()) {
            const std::string& a = args[i];
            int n = 0;
            if (is_step_flag(a, n)) {
                ChainStep s;
                s.n = n;
                if (i + 1 < args.size()) {
                    s.file = args[i + 1];
                    i += 2;
                } else {
                    std::cerr << "nth: -" << n << " flag given but no file follows.\n";
                    r.type = CommandType::Help;
                    return r;
                }
                r.chain.push_back(s);
                cur = &r.chain.back();
                continue;
            }
            if (a == "-success") {
                // separator token, just consumed
                ++i;
                continue;
            }
            if (a.empty()) { ++i; continue; }
            if (a[0] == '-') {
                std::cerr << "nth: unknown flag in chain: " << a << "\n";
                r.type = CommandType::Help;
                return r;
            }
            // Bare token without a -N prefix in chain mode: error.
            std::cerr << "nth: unexpected token '" << a
                      << "' in chain — every step must start with -N.\n";
            r.type = CommandType::Help;
            return r;
        }
        (void)cur;
        return r;
    }

    // Single token that's not a flag and not a subcommand. Could be:
    //   - a path to an existing .js file → RunScript
    //   - a prefix → PrefixMatch (resolved by main.cpp)
    //   - else → RolePass (could be a configured runtime role)
    if (first.empty() || first[0] == '-') {
        std::cerr << "nth: unknown flag: " << first << "\n";
        r.type = CommandType::Help;
        return r;
    }
    // Defer the "is it a file?" decision to main.cpp — it needs the config
    // loaded first (to know if userun is on) and the working directory.
    r.type = CommandType::RolePass;
    r.role_name = first;
    for (size_t i = 1; i < args.size(); ++i)
        r.passthrough_args.push_back(args[i]);
    return r;
}

void print_help(std::ostream& os) {
    os <<
        "nth — Nothing Runtime — v0.1\n"
        "\n"
        "USAGE:\n"
        "  nth <file.js>                      Run a JavaScript file via embedded JSC.\n"
        "  nth <prefix>                       Prefix-match a .js file in cwd (e.g. `nth in`).\n"
        "  nth run <file.js>                  Explicit run subcommand (only if userun=true).\n"
        "  nth -1 a.js -success -2 b.js       Chained multi-step run.\n"
        "  nth init default                   Write a default nthconfig.json.\n"
        "  nth install <args...>              Pass-through to the configured package manager.\n"
        "  nth test <args...>                 Pass-through to the configured test runner.\n"
        "  nth startalways <file> --name N    Delegate to PM2 (npm install -g pm2 first).\n"
        "  nth build <args...>                Delegate to runtimes.build.\n"
        "  nth lint <args...>                 Delegate to runtimes.lint.\n"
        "  nth <role> <args...>               Delegate to runtimes.<role>.\n"
        "  nth --version | --help\n"
        "\n"
        "CHAIN GRAMMAR:\n"
        "  -N <file>          Begin step N with the given file.\n"
        "  -success           Separator — continue to next step only if previous exited 0.\n"
        "  .js files run in-process via JavaScriptCore.\n"
        "  Non-JS files fork the language's own interpreter (requires enableOtherLangs=true).\n"
        "\n"
        "CONFIG (nthconfig.json):\n"
        "  packageManager     Command to use for `nth install` (default: \"npm\").\n"
        "  userun             If true, `nth <file>` is rejected; use `nth run <file>`.\n"
        "  autocomplete       (stubbed in v0.1)\n"
        "  testing            Test runner for `nth test` (default: \"vitest\").\n"
        "  http               Expose Nth.serve / fetch / Request / Response globals.\n"
        "  enableOtherLangs   Allow non-.js files in chain steps.\n"
        "  runtimes           Map of role -> command for adjacent tasks (test/build/lint/...).\n"
        "\n";
}

} // namespace nth::cli
