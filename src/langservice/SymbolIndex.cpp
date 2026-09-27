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
            ParamInfo p{t, "", TypeKindFromName(t)};
            params.push_back(std::move(p));
        } else {
            std::string type = Trim(t.substr(0, sp));
            ParamInfo p{type, Trim(t.substr(sp + 1)), TypeKindFromName(type)};
            params.push_back(std::move(p));
        }
    }
    return params;
}

// Build a SymbolInfo from a declaration regex match.
SymbolInfo BuildSymbol(const std::smatch& m,
                       const std::string& currentNs,
                       const std::vector<std::string>& pendingDoc,
                       const std::string& path, int lineNo) {
    SymbolInfo sym;
    sym.ns = currentNs;
    sym.native = m[1].matched;
    sym.returnType = Trim(m[2].str());
    sym.returnKind = TypeKindFromName(sym.returnType);
    sym.name = m[3].str();
    sym.params = ParseParams(m[4].str());
    sym.doc = pendingDoc;
    sym.filePath = path;
    sym.line = lineNo;
    return sym;
}

// Tracks the current namespace while scanning a declaration file. The
// opening brace is accepted on the same line as `namespace name` (K&R) or
// on the following line (Allman); a lone '}' closes the namespace.
class NamespaceScope {
public:
    // Feed one raw line. True when the line is namespace structure (a
    // head, an Allman brace, or a close) and holds no declaration.
    bool ConsumeStructure(const std::string& line) {
        const std::string t = Trim(line);
        // Complete a pending Allman head: this line must be '{'.
        if (!m_pending.empty()) {
            if (t == "{")
                m_current = m_pending;
            m_pending.clear();
            return true;
        }
        static const std::regex kHead(
            R"(^\s*namespace\s+(\w+)\s*(\{)?\s*$)");
        std::smatch m;
        if (std::regex_match(line, m, kHead)) {
            if (m[2].matched)
                m_current = m[1].str();
            else
                m_pending = m[1].str();
            return true;
        }
        if (t == "}") {
            m_current.clear();
            return true;
        }
        return false;
    }

    const std::string& Current() const { return m_current; }
private:
    std::string m_current;
    std::string m_pending;
};

} // namespace

TypeKind TypeKindFromName(const std::string& name) {
    std::string n = Trim(name);
    if (n == "void") return TypeKind::Void;
    if (n == "int") return TypeKind::Int;
    if (n == "float") return TypeKind::Float;
    if (n == "string") return TypeKind::String;
    if (n == "List<string>") return TypeKind::ListString;
    if (n == "any") return TypeKind::Any;
    return TypeKind::Unknown;
}

std::string NameOfTypeKind(TypeKind kind) {
    switch (kind) {
    case TypeKind::Void: return "void";
    case TypeKind::Int: return "int";
    case TypeKind::Float: return "float";
    case TypeKind::String: return "string";
    case TypeKind::ListString: return "List<string>";
    case TypeKind::Any: return "any";
    default: return "unknown";
    }
}

void SymbolIndex::Clear() {
    m_symbols.clear();
    m_loadedFiles.clear();
}

void SymbolIndex::LoadFile(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        return;

    // A declaration line:
    //   [native] <return type> <name> ( <params> ) ;  or  {
    static const std::regex kDecl(
        R"(^\s*(native\s+)?(.+?)\s+([A-Za-z_]\w*)\s*\((.*)\)\s*[;{]\s*$)");

    NamespaceScope scope;
    std::vector<std::string> pendingDoc;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        std::string trimmed = Trim(line);

        if (scope.ConsumeStructure(line)) {
            pendingDoc.clear();
            continue;
        }
        // Only collect doc comments once inside a namespace, so the file
        // header banner is not mistaken for a function's doc.
        if (!scope.Current().empty() && trimmed.substr(0, 2) == "//") {
            pendingDoc.push_back(Trim(trimmed.substr(2)));
            continue;
        }
        std::smatch m;
        if (!scope.Current().empty() && std::regex_match(line, m, kDecl)) {
            m_symbols.push_back(
                BuildSymbol(m, scope.Current(), pendingDoc, path, lineNo));
            pendingDoc.clear();
        } else if (!trimmed.empty()) {
            // A non-decl line (a body statement, a blank already skipped)
            // breaks the doc run.
            pendingDoc.clear();
        }
    }
}

void SymbolIndex::LoadFileOnce(const std::string& path) {
    // m_loadedFiles guards against parsing the same file twice; the insert
    // succeeds only on the first encounter.
    if (!m_loadedFiles.insert(path).second)
        return;
    LoadFile(path);
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

bool SymbolIndex::HasNamespace(const std::string& ns) const {
    for (const SymbolInfo& s : m_symbols)
        if (s.ns == ns)
            return true;
    return false;
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
