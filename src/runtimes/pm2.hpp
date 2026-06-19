// runtimes/pm2.hpp — `nth startalways` PM2 delegation
#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include "../config/config.hpp"
#include "../util/subprocess.hpp"

namespace nth::runtimes::pm2 {

// Is `pm2` resolvable — either on PATH or as a node_modules/.bin/pm2 entry
// relative to `start_dir`? Returns the resolved path on success, empty on
// failure.
std::string find_pm2(const std::filesystem::path& start_dir);

// Run `nth startalways <file> --name <name>` by translating to
// `pm2 start <file> --name <name>`. Returns the subprocess result.
util::SubprocessResult start_always(const std::filesystem::path& start_dir,
                                    const std::filesystem::path& file,
                                    const std::string& name,
                                    const std::vector<std::string>& extra_args);

// Run `nth install <args...>` as `<packageManager> install <args...>`.
util::SubprocessResult install(const config::Config& cfg,
                               const std::vector<std::string>& args);

// Run `nth test <args...>` as either:
//   - runtimes.test (if configured) — called as `runtimes.test test <args...>`
//   - else, top-level `testing` field — called as `testing test <args...>`
util::SubprocessResult test(const config::Config& cfg,
                            const std::vector<std::string>& args);

} // namespace nth::runtimes::pm2
