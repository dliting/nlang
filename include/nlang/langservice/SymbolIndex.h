/*--- SymbolIndex.h - index of library .n declaration files --------------*/
/*
 * nlang_langservice indexes library declaration files (stdlib/*.n and, in
 * later phases, third-party libraries) so an IDE or an LSP server can offer
 * signature help, completion and go-to-definition.
 *
 * The index is deliberately self-contained: it parses the small, regular
 * declaration surface (top-level native / nlang function decls with leading
 * '//' doc comments; each file's package comes from its path) and depends
 * only on the C++ standard library. IDE-side it is fed the stdlib, the
 * configured library dirs and the open projects'/editors' source dirs;
 * full semantic analysis of user source is a later phase.
 */
#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace nlang {
namespace langservice {

// Structured type of a parameter or return value. The string spelling is
// kept for display, but consumers (compiler / codegen) switch on TypeKind
// so they never parse type text.
enum class TypeKind : uint8_t {
    Unknown,
    Void,         // no return value
    Int,
    Long,
    Float,
    Double,
    String,
    ListString,
};

TypeKind TypeKindFromName(const std::string& name);
std::string NameOfTypeKind(TypeKind kind);

struct ParamInfo {
    std::string type;      // display spelling, e.g. "List<string>"
    std::string name;
    TypeKind kind = TypeKind::Unknown;
};

struct SymbolInfo {
    std::string pkg;
    std::string name;
    std::string returnType;
    TypeKind returnKind = TypeKind::Unknown;
    std::vector<ParamInfo> params;
    std::vector<std::string> doc;
    std::string filePath;
    int line = 0;          // 1-based line of the declaration
    bool native = false;   // true: implemented outside nlang
};

class SymbolIndex {
public:
    // Drop every indexed symbol and loaded-file/dir marker so the index
    // can be rebuilt from scratch (after the configured library dirs
    // change).
    void Clear();

    // Index every *.n file directly under dir (non-recursive).
    void LoadLibraryDir(const std::string& dir);

    // Same, but parse each dir at most once per generation (Clear resets
    // the guard). Two spellings of one dir (separators, case on Windows,
    // "." / ".." segments) count as the same dir. For callers that feed
    // dirs from several sources (config, projects, open editors).
    void LoadLibraryDirOnce(const std::string& dir);

    // Index one .n file.
    void LoadFile(const std::string& path);

    // Index one .n file at most once (a repeat path is a no-op). Used when
    // discovering third-party library sources so the same file is not parsed
    // twice across imports.
    void LoadFileOnce(const std::string& path);

    // Same guard, with the package supplied by the caller (the matched
    // search root's derived package — vendor/graphics.n under root R is
    // "vendor.graphics", not the bare stem).
    void LoadFileOnce(const std::string& path, const std::string& package);

    // Resolve a qualified name, e.g. Resolve("io", "print").
    // Returns nullptr when unknown.
    const SymbolInfo* Resolve(const std::string& ns,
                              const std::string& name) const;

    // All symbols declared in a namespace (completion after "io.").
    std::vector<const SymbolInfo*> CompletePackage(
        const std::string& ns) const;

    // All indexed namespace names, sorted and unique.
    std::vector<std::string> Packages() const;

    // Whether any symbol is declared under the given namespace. Used by the
    // compiler/codegen to recognize a library namespace from the index
    // instead of a hard-coded name list (so third-party namespaces work too).
    bool HasPackage(const std::string& ns) const;

    std::size_t size() const { return m_symbols.size(); }

private:
    // Index one .n file whose package is already known (the path-derived
    // package; LoadLibraryDir supplies the library root, LoadFile falls
    // back to the file stem).
    void LoadFileWithPackage(const std::string& path,
                             const std::string& package);

    std::vector<SymbolInfo> m_symbols;
    std::unordered_set<std::string> m_loadedFiles;
    std::unordered_set<std::string> m_loadedDirs;
};

// Find the stdlib directory starting from the directory that holds the
// running executable: walk upward looking for a "stdlib" folder; fall back
// to "<exeDir>/stdlib". Returns "" when nothing exists.
std::string FindStdLibDir(const std::string& exeDir);

} // namespace langservice
} // namespace nlang
