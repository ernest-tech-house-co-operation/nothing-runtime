// config/config.cpp
#include "config.hpp"
#include "../util/fs.hpp"
#include "../util/strings.hpp"

#include <fstream>
#include <sstream>
#include <filesystem>

namespace nth::config {

namespace fs = std::filesystem;
using json = nlohmann::json;

Config Config::defaults() {
    Config c;
    // Per spec section 5 defaults — autocomplete defaults to false here (true
    // in the schema example, but the field is stubbed; defaulting to false is
    // safe since it's a no-op until tab-completion is wired up).
    return c;
}

Config Config::from_json(const std::string& text) {
    Config c = Config::defaults();
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception&) {
        // Malformed JSON → silently fall back to defaults. Caller may want to
        // surface a warning; for v0.1 we keep going.
        return c;
    }
    if (j.contains("packageManager") && j["packageManager"].is_string())
        c.packageManager = j["packageManager"].get<std::string>();
    if (j.contains("userun") && j["userun"].is_boolean())
        c.userun = j["userun"].get<bool>();
    if (j.contains("autocomplete") && j["autocomplete"].is_boolean())
        c.autocomplete = j["autocomplete"].get<bool>();
    if (j.contains("testing") && j["testing"].is_string())
        c.testing = j["testing"].get<std::string>();
    if (j.contains("http") && j["http"].is_boolean())
        c.http = j["http"].get<bool>();
    if (j.contains("enableOtherLangs") && j["enableOtherLangs"].is_boolean())
        c.enableOtherLangs = j["enableOtherLangs"].get<bool>();
    if (j.contains("runtimes") && j["runtimes"].is_object()) {
        for (auto it = j["runtimes"].begin(); it != j["runtimes"].end(); ++it) {
            if (it.value().is_string()) {
                c.runtimes[it.key()] = it.value().get<std::string>();
            }
        }
    }
    return c;
}

std::string Config::to_json() const {
    json j;
    j["packageManager"] = packageManager;
    j["userun"] = userun;
    j["autocomplete"] = autocomplete;
    j["testing"] = testing;
    j["http"] = http;
    j["enableOtherLangs"] = enableOtherLangs;
    json rt = json::object();
    for (auto& [k, v] : runtimes) rt[k] = v;
    if (!rt.empty()) j["runtimes"] = rt;
    return j.dump(2);
}

fs::path find_config(const fs::path& start_dir) {
    return nth::util::fs::walk_up_for(start_dir, "nthconfig.json") / "nthconfig.json";
}

Config load_config(const fs::path& start_dir) {
    auto p = find_config(start_dir);
    if (p.empty()) return Config::defaults();
    std::string text;
    if (!nth::util::fs::read_file(p, text)) return Config::defaults();
    return Config::from_json(text);
}

bool init_default(const fs::path& dir, std::string& err) {
    fs::path target = dir / "nthconfig.json";
    if (nth::util::fs::exists(target)) {
        err = "nthconfig.json already exists at " + target.string()
              + " — refusing to overwrite.";
        return false;
    }
    Config c = Config::defaults();
    if (!nth::util::fs::write_file(target, c.to_json() + "\n")) {
        err = "failed to write " + target.string();
        return false;
    }
    return true;
}

} // namespace nth::config
