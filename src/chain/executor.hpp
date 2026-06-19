// chain/executor.hpp — multi-step chain execution
#pragma once
#include <vector>
#include <filesystem>
#include "../cli/parser.hpp"
#include "../config/config.hpp"

namespace nth::chain {

namespace fs = std::filesystem;

struct ChainOutcome {
    int final_exit_code = 0;
    int failed_step = 0;              // 1-indexed step number, or 0 if all ok
    std::string failed_reason;        // human-readable
};

// Execute a parsed chain (`-N file -success -N file ...`). Stops on first
// non-zero exit. Returns the exit code of the last executed step (or 0 if
// all succeeded).
ChainOutcome run(const cli::ParsedCommand& cmd, const config::Config& cfg);

} // namespace nth::chain
