// runtimes/role_resolver.cpp
//
// Single shared helper used by all `nth <role>` commands (test/build/lint/
// arbitrary role). Per spec section 5c: looks up runtimes.<role>, resolves
// the command via PATH, executes with forwarded args. No validation against
// known tool names.

#include "role_resolver.hpp"

#include <iostream>

namespace nth::runtimes {

namespace fs = std::filesystem;

util::SubprocessResult run_role(const config::Config& cfg,
                                const std::string& role,
                                const std::string& subcommand,
                                const std::vector<std::string>& extra_args,
                                const fs::path& /*cwd*/) {
    util::SubprocessResult r;
    auto it = cfg.runtimes.find(role);
    if (it == cfg.runtimes.end()) {
        r.error_message = "no runtimes." + role + " configured in nthconfig.json";
        return r;
    }
    std::vector<std::string> argv;
    argv.push_back(it->second);
    if (!subcommand.empty()) argv.push_back(subcommand);
    for (auto& a : extra_args) argv.push_back(a);

    // Inherit stdout/stderr — we want the tool's output to stream through to
    // the user as-is, not get captured and re-emitted.
    r = util::run_subprocess_inherit(argv);
    return r;
}

} // namespace nth::runtimes
