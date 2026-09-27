/*--- SymbolIndex.h - index of library .n declaration files --------------*/
/*
 * nlang_langservice indexes library declaration files (stdlib/*.n and, in
 * later phases, third-party libraries) so an IDE or an LSP server can offer
 * signature help, completion and go-to-definition.
 *
 * The index is deliberately self-contained: it parses the small, regular
 * declaration surface (namespace blocks, native / nlang function decls with
 * leading '//' doc comments) and depends only on the C++ standard library.
 * Full semantic analysis of user source is a later phase.
 */
#pragma once

#include <string>
#include <vector>

namespace nlang {
namespace langservice {

struct ParamInfo {
    std::string type;
    std::string name;
};

struct SymbolInfo {
    std::string ns;
    std::string name;
    std::string returnType;
    std::vector<ParamInfo> params;
    std::vector<std::string> doc;
    std::string filePath;
    int line = 0;          // 1-based line of the declaration
    bool native = false;   // true: implemented outside nlang
};

class SymbolIndex {
public:
    // Index every *.n file directly under dir (non-recursive).
    void LoadLibraryDir(const std::string& dir);

    // Index one .n file.
    void LoadFile(const std::string& path);

    // Resolve a qualified name, e.g. Resolve("io", "print").
    // Returns nullptr when unknown.
    const SymbolInfo* Resolve(const std::string& ns,
                              const std::string& name) const;

    // All symbols declared in a namespace (completion after "io.").
    std::vector<const SymbolInfo*> CompleteNamespace(
        const std::string& ns) const;

    // All indexed namespace names, sorted and unique.
    std::vector<std::string> Namespaces() const;

    std::size_t size() const { return m_symbols.size(); }

private:
    std::vector<SymbolInfo> m_symbols;
};

// Find the stdlib directory starting from the directory that holds the
// running executable: walk upward looking for a "stdlib" folder; fall back
// to "<exeDir>/stdlib". Returns "" when nothing exists.
std::string FindStdLibDir(const std::string& exeDir);

} // namespace langservice
} // namespace nlang
