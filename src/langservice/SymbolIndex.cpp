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
                       const std::string& package,
                       const std::vector<std::string>& pendingDoc,
                       const std::string& path, int lineNo) {
    SymbolInfo sym;
    sym.ns = package;
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

// The symbol's package = the file's path made relative to the library root
// and dotted ("net/http.n" -> "net.http"), falling back to the file stem
// outside any root — the same rule as the compiler's DeriveModulePath, so
// the index cannot disagree with the compiler about a file's package
// (phase 5 removed the `namespace` shell syntax; the path is the only
// package source).
std::string PackageFromFilePath(const fs::path& file, const fs::path& root) {
    if (!root.empty()) {
        std::error_code fsError;
        const fs::path rel = fs::relative(file, root, fsError);
        if (!fsError && !rel.empty()) {
            std::string dotted = rel.stem().string();
            for (const fs::path& part : rel.parent_path())
                dotted = part.string() + "." + dotted;
            return dotted;
        }
    }
    return file.stem().string();
}

// Net '{' minus '}' on a line, ignoring braces inside string/char literals
// and after a '//' comment, so a '}' written in text or a comment does not
// change the function-body depth.
int NetBraces(const std::string& line) {
    int net = 0;
    bool inLiteral = false;
    char quote = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (inLiteral) {
            if (ch == '\\' && i + 1 < line.size()) { ++i; continue; }
            if (ch == quote) inLiteral = false;
            continue;
        }
        if (ch == '"' || ch == '\'') { inLiteral = true; quote = ch; continue; }
        if (ch == '/' && i + 1 < line.size() && line[i + 1] == '/') break;
        if (ch == '{') ++net;
        else if (ch == '}') --net;
    }
    return net;
}

//Consume one source line during indexing: a body's braces, doc comments,
//or a declaration. Updates pendingDoc/bodyDepth and appends a recognized
//declaration to symbols. Every declaration belongs to `package` — the
//file's path-derived package; there is no in-file scope syntax anymore.
bool ConsumeIndexLine(const std::string& line, const std::string& trimmed,
    const std::string& package, std::vector<std::string>& pendingDoc,
    int& bodyDepth, const std::string& path, int lineNo,
    std::vector<SymbolInfo>& symbols) {
    if (bodyDepth > 0) {
        bodyDepth += NetBraces(line);
        if (bodyDepth < 0) bodyDepth = 0;
        return false;
    }
    if (trimmed.substr(0, 2) == "//") {
        pendingDoc.push_back(Trim(trimmed.substr(2)));
        return false;
    }
    static const std::regex kDecl(
        R"(^\s*(native\s+)?(.+?)\s+([A-Za-z_]\w*)\s*\((.*)\)\s*[;{]\s*$)");
    std::smatch m;
    if (std::regex_match(line, m, kDecl)) {
        symbols.push_back(
            BuildSymbol(m, package, pendingDoc, path, lineNo));
        pendingDoc.clear();
        bodyDepth = std::max(0, NetBraces(line));
    } else if (!trimmed.empty()) {
        pendingDoc.clear();
    }
    return false;
}

} // namespace

TypeKind TypeKindFromName(const std::string& name) {
    std::string n = Trim(name);
    if (n == "void") return TypeKind::Void;
    if (n == "int") return TypeKind::Int;
    if (n == "float") return TypeKind::Float;
    if (n == "string") return TypeKind::String;
    if (n == "List<string>") return TypeKind::ListString;
    return TypeKind::Unknown;
}

std::string NameOfTypeKind(TypeKind kind) {
    switch (kind) {
    case TypeKind::Void: return "void";
    case TypeKind::Int: return "int";
    case TypeKind::Float: return "float";
    case TypeKind::String: return "string";
    case TypeKind::ListString: return "List<string>";
    default: return "unknown";
    }
}

void SymbolIndex::Clear() {
    m_symbols.clear();
    m_loadedFiles.clear();
}

void SymbolIndex::LoadFile(const std::string& path) {
    //No library root in context: the package degenerates to the file stem
    //(same degenerate branch as the compiler's DeriveModulePath).
    LoadFileWithPackage(path, PackageFromFilePath(fs::path(path), {}));
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
        LoadFileWithPackage(f.string(),
            PackageFromFilePath(f, p));
}

void SymbolIndex::LoadFileWithPackage(const std::string& path,
    const std::string& package) {
    std::ifstream in(path);
    if (!in)
        return;
    std::vector<std::string> pendingDoc;
    std::string line;
    int lineNo = 0;
    //Brace depth inside the function body currently being scanned, so a
    //body's '}' is not mistaken for a declaration boundary. 0 = top level,
    //where declarations are recognized.
    int bodyDepth = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        ConsumeIndexLine(line, Trim(line), package, pendingDoc, bodyDepth,
            path, lineNo, m_symbols);
    }
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
