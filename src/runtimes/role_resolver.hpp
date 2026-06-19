// runtimes/role_resolver.hpp — shared helper for runtimes.<role> lookups
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include "../config/config.hpp"
#include "../util/subprocess.hpp"

namespace nth::runtimes {

namespace fs = std::filesystem;

// Resolve a role to a command line and execute it with forwarded args.
// Returns the subprocess result. If the role isn't configured, returns a
// result with started=false and error_message set.
util::SubprocessResult run_role(const config::Config& cfg,
                                const std::string& role,
                                const std::string& subcommand,
                                const std::vector<std::string>& extra_args,
                                const fs::path& cwd);

} // namespace nth::runtimes
