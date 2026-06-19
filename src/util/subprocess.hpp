// util/subprocess.hpp — minimal fork/exec wrapper for chain steps and PM2
#pragma once
#include <string>
#include <vector>
#include <optional>

namespace nth::util {

struct SubprocessResult {
    int exit_code = -1;       // raw exit status; -1 if couldn't even start
    bool signaled = false;
    std::string stdout_text;
    std::string stderr_text;
    bool started = false;     // false if binary not found on PATH
    std::string error_message; // populated when started==false
};

// Resolve `cmd` via PATH. Returns empty optional if not found.
std::optional<std::string> resolve_on_path(const std::string& cmd);

// Spawn `argv` (argv[0] is the executable name or path). Captures stdout and
// stderr into the result. Inherits stdin from the parent.
SubprocessResult run_subprocess(const std::vector<std::string>& argv);

// Spawn `argv` inheriting stdout/stderr (no capture, streams straight through).
SubprocessResult run_subprocess_inherit(const std::vector<std::string>& argv);

} // namespace nth::util
