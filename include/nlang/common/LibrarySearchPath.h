/*--- LibrarySearchPath.h - ordered library search-path builder ----------*/
/*
 * Header-only, pure-STL utility shared by every tool (ncc/nvm/ndb) and
 * available to the language service / a future LSP without adding link
 * dependencies. It turns the layered sources of library locations into an
 * ordered, normalized and de-duplicated search path.
 *
 * Layer order (first wins), modeled on Python's sys.path and GCC's -I:
 *   1. explicitDirs  - command-line -I (highest)
 *   2. configuredDirs - project file <ImportPaths>
 *   3. baseDirs       - project/source/module directory (local)
 *   4. pathEnv        - NLANG_PATH (after the local dir, like PYTHONPATH)
 *   5. systemDirs     - stdlib dir, executable dir, "." (lowest)
 *
 * Pure: directories are never checked for existence (lexical normalization
 * only), so the result is deterministic and unit-testable.
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace nlang {

struct SearchPathInput {
    std::vector<std::string> explicitDirs;
    std::vector<std::string> configuredDirs;
    std::vector<std::string> baseDirs;
    std::string pathEnv;
    std::vector<std::string> systemDirs;
};

namespace searchpath_detail {

// Platform PATH-list separator (';' on Windows, ':' elsewhere).
inline char ListSeparator() {
#if defined(_WIN32)
    return ';';
#else
    return ':';
#endif
}

// Absolute + lexically normalized spelling (the value handed to consumers).
inline std::string Normalized(const std::string& dir) {
    return std::filesystem::absolute(dir).lexically_normal().string();
}

// De-duplication key: case-insensitive on Windows to match NTFS semantics.
inline std::string DedupKey(const std::string& normalized) {
    std::string key = normalized;
#if defined(_WIN32)
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
#endif
    return key;
}

} // namespace searchpath_detail

// Split a PATH-like value on the platform separator; empty entries dropped.
inline std::vector<std::string> SplitSearchPathEnv(
    const std::string& value) {
    std::vector<std::string> out;
    const char sep = searchpath_detail::ListSeparator();
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t pos = value.find(sep, start);
        const std::string token = value.substr(
            start, pos == std::string::npos
                       ? std::string::npos : pos - start);
        if (!token.empty())
            out.push_back(token);
        if (pos == std::string::npos)
            break;
        start = pos + 1;
    }
    return out;
}

// Build the ordered, normalized, de-duplicated library search path.
inline std::vector<std::string> BuildLibrarySearchPath(
    const SearchPathInput& input) {
    std::vector<std::string> layers;
    auto appendLayer = [&layers](const std::vector<std::string>& dirs) {
        for (const auto& d : dirs)
            if (!d.empty())
                layers.push_back(d);
    };
    appendLayer(input.explicitDirs);
    appendLayer(input.configuredDirs);
    appendLayer(input.baseDirs);
    appendLayer(SplitSearchPathEnv(input.pathEnv));
    appendLayer(input.systemDirs);

    std::vector<std::string> result;
    std::vector<std::string> seen;
    for (const auto& d : layers) {
        const std::string normalized = searchpath_detail::Normalized(d);
        const std::string key = searchpath_detail::DedupKey(normalized);
        if (std::find(seen.begin(), seen.end(), key) != seen.end())
            continue;  // first occurrence wins
        seen.push_back(key);
        result.push_back(normalized);
    }
    return result;
}

} // namespace nlang
