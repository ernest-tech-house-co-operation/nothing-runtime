// config/config.hpp — nthconfig.json parser
#pragma once
#include <string>
#include <optional>
#include <map>
#include <nlohmann/json.hpp>

namespace nth::config {

struct Config {
    std::string packageManager = "npm";
    bool userun = false;
    bool autocomplete = false; // stubbed per spec section 5
    std::string testing = "vitest";
    bool http = false;
    bool enableOtherLangs = false;
    std::map<std::string, std::string> runtimes; // role -> command

    static Config defaults();
    // Parse from JSON text. Unknown fields are ignored; missing fields use defaults.
    static Config from_json(const std::string& text);
    std::string to_json() const;
};

// Locate `nthconfig.json` walking up from `start_dir`. Returns empty path if
// none found.
std::filesystem::path find_config(const std::filesystem::path& start_dir);

// Load config from the nthconfig.json nearest to `start_dir`. Returns defaults
// if no config file is found.
Config load_config(const std::filesystem::path& start_dir);

// `nth init default` — write a default nthconfig.json into `dir`. Fails if one
// already exists (does NOT silently overwrite).
bool init_default(const std::filesystem::path& dir, std::string& err);

} // namespace nth::config
