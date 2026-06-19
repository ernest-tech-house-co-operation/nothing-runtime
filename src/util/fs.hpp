// util/fs.hpp — small filesystem helpers
#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace nth::util::fs {

namespace stdfs = std::filesystem;

inline bool read_file(const stdfs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

inline bool write_file(const stdfs::path& p, const std::string& content) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << content;
    return f.good();
}

inline bool exists(const stdfs::path& p) {
    std::error_code ec;
    return stdfs::exists(p, ec);
}

inline bool is_regular(const stdfs::path& p) {
    std::error_code ec;
    return stdfs::is_regular_file(p, ec);
}

// Walk up from `start` looking for a directory containing `subdir`.
// Returns the directory that contains it, or empty if not found.
inline stdfs::path walk_up_for(const stdfs::path& start, const std::string& subdir) {
    stdfs::path cur = start;
    if (cur.empty()) cur = stdfs::current_path();
    if (stdfs::is_regular_file(cur)) cur = cur.parent_path();
    while (true) {
        std::error_code ec;
        auto candidate = cur / subdir;
        if (stdfs::is_directory(candidate, ec)) return cur;
        auto parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return {};
}

// List immediate (non-recursive) file entries in `dir` whose name starts with
// `prefix` and ends with `.js`. Returns in whatever order the OS gives.
inline std::vector<stdfs::path> list_files_prefix_js(const stdfs::path& dir,
                                                      const std::string& prefix) {
    std::vector<stdfs::path> out;
    std::error_code ec;
    for (auto it = stdfs::directory_iterator(dir, ec);
         it != stdfs::directory_iterator(); ++it) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        auto name = it->path().filename().string();
        if (name.size() >= prefix.size() + 3
            && name.compare(0, prefix.size(), prefix) == 0
            && name.size() >= 3
            && name.compare(name.size() - 3, 3, ".js") == 0) {
            out.push_back(it->path());
        }
    }
    return out;
}

} // namespace nth::util::fs
