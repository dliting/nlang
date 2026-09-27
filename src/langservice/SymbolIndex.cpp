/*--- SymbolIndex.cpp - parse library .n declaration files ---------------*/
#include "nlang/langservice/SymbolIndex.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace nlang {
namespace langservice {

namespace {

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Split "string path, List<string> items" into ParamInfo.
std::vector<ParamInfo> ParseParams(const std::string& text) {
    std::vector<ParamInfo> params;
    std::string trimmed = Trim(text);
    if (trimmed.empty())
        return params;
    int depth = 0;
    std::string cur;
    std::vector<std::string> items;
    for (char ch : trimmed) {
        if (ch == '<') ++depth;
        else if (ch == '>') --depth;
        if (ch == ',' && depth == 0) {
            items.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    items.push_back(cur);

    for (const std::string& item : items) {
        std::string t = Trim(item);
        if (t.empty())
            continue;
        // Parameter name is the final whitespace-delimited token.
        size_t sp = t.find_last_of(" \t");
        if (sp == std::string::npos) {
            params.push_back({t, ""});
        } else {
            params.push_back({Trim(t.substr(0, sp)), Trim(t.substr(sp + 1))});
        }
    }
    return params;
}

} // namespace

void SymbolIndex::LoadFile(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        return;

    // A declaration line:
    //   [native] <return type> <name> ( <params> ) ;  or  {
    static const std::regex kDecl(
        R"(^\s*(native\s+)?(.+?)\s+([A-Za-z_]\w*)\s*\((.*)\)\s*[;{]\s*$)");
    static const std::regex kNamespace(R"(^\s*namespace\s+(\w+)\s*\{)");

    std::string currentNs;
    std::vector<std::string> pendingDoc;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        std::string trimmed = Trim(line);

        std::smatch m;
        if (std::regex_match(line, m, kNamespace)) {
            currentNs = m[1].str();
            pendingDoc.clear();
            continue;
        }
        if (trimmed == "}") {
            currentNs.clear();
            pendingDoc.clear();
            continue;
        }
        // Only collect doc comments once inside a namespace, so the file
        // header banner is not mistaken for a function's doc.
        if (!currentNs.empty() && trimmed.substr(0, 2) == "//") {
            pendingDoc.push_back(Trim(trimmed.substr(2)));
            continue;
        }
        if (!currentNs.empty() && std::regex_match(line, m, kDecl)) {
            SymbolInfo sym;
            sym.ns = currentNs;
            sym.native = m[1].matched;
            sym.returnType = Trim(m[2].str());
            sym.name = m[3].str();
            sym.params = ParseParams(m[4].str());
            sym.doc = pendingDoc;
            sym.filePath = path;
            sym.line = lineNo;
            m_symbols.push_back(std::move(sym));
            pendingDoc.clear();
        } else if (!trimmed.empty()) {
            // A non-decl line (a body statement, a blank already skipped)
            // breaks the doc run.
            pendingDoc.clear();
        }
    }
}

void SymbolIndex::LoadLibraryDir(const std::string& dir) {
    fs::path p(dir);
    if (!fs::is_directory(p))
        return;
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(p)) {
        if (entry.is_regular_file() && entry.path().extension() == ".n")
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files)
        LoadFile(f.string());
}

const SymbolInfo* SymbolIndex::Resolve(const std::string& ns,
                                       const std::string& name) const {
    for (const SymbolInfo& s : m_symbols) {
        if (s.ns == ns && s.name == name)
            return &s;
    }
    return nullptr;
}

std::vector<const SymbolInfo*> SymbolIndex::CompleteNamespace(
    const std::string& ns) const {
    std::vector<const SymbolInfo*> out;
    for (const SymbolInfo& s : m_symbols) {
        if (s.ns == ns)
            out.push_back(&s);
    }
    return out;
}

std::vector<std::string> SymbolIndex::Namespaces() const {
    std::vector<std::string> out;
    for (const SymbolInfo& s : m_symbols) {
        if (std::find(out.begin(), out.end(), s.ns) == out.end())
            out.push_back(s.ns);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string FindStdLibDir(const std::string& exeDir) {
    fs::path cur = fs::absolute(exeDir);
    for (int up = 0; up < 8; ++up) {
        fs::path candidate = cur / "stdlib";
        if (fs::is_directory(candidate))
            return candidate.string();
        if (!cur.has_parent_path() || cur.parent_path() == cur)
            break;
        cur = cur.parent_path();
    }
    fs::path beside = fs::path(exeDir) / "stdlib";
    if (fs::is_directory(beside))
        return beside.string();
    return "";
}

} // namespace langservice
} // namespace nlang
