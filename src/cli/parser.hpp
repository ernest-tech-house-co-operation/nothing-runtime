// cli/parser.hpp — argument parsing for nth
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace nth::cli {

namespace fs = std::filesystem;

// What did the user want `nth` to do?
enum class CommandType {
    RunScript,        // nth index.js            (or `nth run index.js` if userun)
    PrefixMatch,      // nth in                  (4b)
    Chain,            // nth -1 a.js -success -2 b.js -success c.js
    InitDefault,      // nth init default
    Install,          // nth install <args...>   (5b — pm passthrough)
    Test,             // nth test <args...>      (5b — testing passthrough)
    StartAlways,      // nth startalways <file> --name <name>
    Build,            // nth build <args...>     (runtimes.build)
    Lint,             // nth lint <args...>      (runtimes.lint)
    RolePass,         // nth <role> <args...>    (runtimes.<role>)
    Help,
    Version,
};

struct ChainStep {
    int n = 0;                    // step number (1,2,3...)
    fs::path file;                // file to run
    std::vector<std::string> extra_args; // unused in v0.1 but reserved
};

struct ParsedCommand {
    CommandType type = CommandType::Help;
    fs::path entry_file;                              // RunScript / PrefixMatch (after resolution)
    std::string prefix;                               // PrefixMatch (before resolution)
    std::vector<ChainStep> chain;                     // Chain
    std::vector<std::string> passthrough_args;        // Install/Test/Build/Lint/RolePass
    std::string startalways_name;                     // StartAlways --name value
    std::string role_name;                            // RolePass
    std::vector<std::string> unknown_flags;           // for diagnostics
};

// Parse argv (argv[0] is the binary name).
ParsedCommand parse(int argc, char** argv);

// Print usage to stream.
void print_help(std::ostream& os);

} // namespace nth::cli
