// --- ndb source cache (`l` command) ---
// Resolution: as-recorded path, then next to the .nmod, then give up
// (degraded `l`: numbers without text). Negative results are cached —
// a missing file must not be re-read on every stop.

#include "SourceCache.h"
#include <filesystem>
#include <fstream>
#include <iterator>

namespace nlang {

namespace {
const std::vector<std::string> kNoLines;  //shared degraded result
}

SourceCache::SourceCache(const std::string& modulePath)
    : m_moduleDir(std::filesystem::path(modulePath)
          .parent_path().string()) {}

const std::vector<std::string>& SourceCache::Lines(
    const std::string& sourceFile) {
    if (sourceFile.empty())
        return kNoLines;
    auto it = m_cache.find(sourceFile);
    if (it != m_cache.end())
        return it->second;

    std::filesystem::path candidate(sourceFile);
    if (!std::filesystem::exists(candidate)) {
        //Fallback: basename next to the .nmod being debugged.
        std::error_code ec;
        std::filesystem::path byModule(
            std::filesystem::path(m_moduleDir)
            / candidate.filename());
        if (!std::filesystem::exists(byModule, ec)) {
            //Cache the miss (reference stability is part of the
            //contract: emplace no-ops on the second call).
            return m_cache.emplace(sourceFile,
                std::vector<std::string>()).first->second;
        }
        candidate = byModule;
    }

    std::ifstream in(candidate, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        //Windows CRLF: strip the carriage return so `l` output is
        //byte-clean for e2e substring assertions.
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
    }
    return m_cache.emplace(sourceFile, std::move(lines)).first->second;
}

} // namespace nlang
