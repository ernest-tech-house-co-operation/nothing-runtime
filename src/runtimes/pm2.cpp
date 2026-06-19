// runtimes/pm2.cpp
//
// `nth startalways <file> --name <name>`  →  `pm2 start <file> --name <name>`
// `nth install <args...>`                 →  `<packageManager> install <args...>`
// `nth test <args...>`                    →  `<runtimes.test|testing> test <args...>`
//
// All three are literal pass-throughs per spec section 5b. nth does no
// validation against known tool names.

#include "pm2.hpp"
#include "role_resolver.hpp"
#include "../util/subprocess.hpp"
#include "../util/fs.hpp"

#include <iostream>
#include <filesystem>

namespace nth::runtimes::pm2 {

namespace fs = std::filesystem;

std::string find_pm2(const fs::path& start_dir) {
    // 1. PATH lookup
    if (auto p = util::resolve_on_path("pm2")) return *p;
    // 2. node_modules/.bin/pm2 — search up the directory tree
    fs::path cur = start_dir;
    if (cur.empty()) cur = fs::current_path();
    while (true) {
        auto candidate = cur / "node_modules" / ".bin" / "pm2";
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return candidate.string();
        auto parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return "";
}

util::SubprocessResult start_always(const fs::path& start_dir,
                                    const fs::path& file,
                                    const std::string& name,
                                    const std::vector<std::string>& extra_args) {
    util::SubprocessResult r;
    std::string pm2_path = find_pm2(start_dir);
    if (pm2_path.empty()) {
        r.error_message =
            "pm2 not found on PATH or in any node_modules/.bin.\n"
            "Install it with: nth install -g pm2";
        return r;
    }
    std::vector<std::string> argv;
    argv.push_back(pm2_path);
    argv.push_back("start");
    argv.push_back(file.string());
    if (!name.empty()) {
        argv.push_back("--name");
        argv.push_back(name);
    }
    for (auto& a : extra_args) argv.push_back(a);
    return util::run_subprocess_inherit(argv);
}

util::SubprocessResult install(const config::Config& cfg,
                               const std::vector<std::string>& args) {
    std::vector<std::string> argv;
    argv.push_back(cfg.packageManager);
    argv.push_back("install");
    for (auto& a : args) argv.push_back(a);
    return util::run_subprocess_inherit(argv);
}

util::SubprocessResult test(const config::Config& cfg,
                            const std::vector<std::string>& args) {
    // Per spec section 5c: runtimes.test takes precedence if configured;
    // otherwise fall back to top-level `testing` field. Documented in
    // BUILD.md as a known minor redundancy between the two mechanisms.
    std::string cmd;
    auto it = cfg.runtimes.find("test");
    if (it != cfg.runtimes.end()) cmd = it->second;
    else cmd = cfg.testing;
    std::vector<std::string> argv;
    argv.push_back(cmd);
    argv.push_back("test");
    for (auto& a : args) argv.push_back(a);
    return util::run_subprocess_inherit(argv);
}

} // namespace nth::runtimes::pm2
