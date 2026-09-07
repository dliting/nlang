#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace nlang {

//Reads source files for the `l` command. Resolution order per file:
//the as-recorded path from .nmod v1.9, then <module dir>/<basename>
//(shared .nmod whose recorded path went stale). A miss degrades to an
//empty line list — `l` then shows numbers without text — and is
//cached so a missing file is not re-read on every stop.
class SourceCache {
public:
    explicit SourceCache(const std::string& modulePath);

    //Source lines of `sourceFile`; lines()[n-1] is line n. Returns a
    //stable reference; empty vector when unresolvable.
    const std::vector<std::string>& Lines(const std::string& sourceFile);

private:
    std::string m_moduleDir;
    std::unordered_map<std::string, std::vector<std::string>> m_cache;
};

} // namespace nlang
