/*---
test_import_cycle.cpp - v0.12.6 Task 8 (D13) import-cycle detection.

The compiler's import prelude must reject a cyclic import graph before
codegen: with lazy static initialization a cycle deadlocks at runtime
(each unit's $staticInit gate can park on a peer whose own initializer
parks back). Both spec §8 forms are unit-level: the edge graph is fed
directly here (the same graph the production wiring extracts: the root
unit's direct imports plus every loaded image's SymbolImport module
paths). The end-to-end shape — two packages whose delivered records
import each other, each built against the peer's acyclic stub source —
is pinned by mutualPackagesImportCycle in test_module_import.cpp.
---*/
#include "builder/ImportCycleCheck.h"
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define TEST(name) \
    do { std::cerr << "  " << #name << " ... "; } while(0)
#define PASS() \
    do { ++g_pass; std::cerr << "OK\n"; } while(0)
#define FAIL(msg) \
    do { ++g_fail; std::cerr << "FAIL: " << msg << "\n"; } while(0)
#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)

//Direct form: while compiling a, the freshly loaded image of b turns
//out to import a back — the path must name the full cycle.
static void test_direct_cycle() {
    TEST(direct_cycle);
    std::map<std::string, std::vector<std::string>> imageEdges;
    imageEdges["b"] = {"a"};
    std::string cycle = ImportCycleCheck::Check("a", {"b"}, imageEdges);
    CHECK(cycle == "a -> b -> a", "expected 'a -> b -> a', got '" + cycle + "'");
    PASS();
}

//Pre-existing form: a imports b cleanly, but the images b and c already
//reference each other (stale images from earlier compiles). Any cycle
//REACHABLE from the root is reported — the diagnostic names the cycle
//segment, not the root's innocent prefix.
static void test_preexisting_cycle() {
    TEST(preexisting_cycle);
    std::map<std::string, std::vector<std::string>> imageEdges;
    imageEdges["b"] = {"c"};
    imageEdges["c"] = {"b"};
    std::string cycle = ImportCycleCheck::Check("a", {"b"}, imageEdges);
    CHECK(cycle == "b -> c -> b", "expected 'b -> c -> b', got '" + cycle + "'");
    PASS();
}

//A plain chain a -> b -> c must stay silent — the check reports cycles,
//not depth.
static void test_acyclic_chain_silent() {
    TEST(acyclic_chain_silent);
    std::map<std::string, std::vector<std::string>> imageEdges;
    imageEdges["b"] = {"c"};
    imageEdges["c"] = {};
    std::string cycle = ImportCycleCheck::Check("a", {"b"}, imageEdges);
    CHECK(cycle.empty(), "acyclic graph must report nothing");
    PASS();
}

//Self-import is the shortest cycle; the diagnostic degenerates to the
//node naming itself.
static void test_self_import() {
    TEST(self_import);
    std::map<std::string, std::vector<std::string>> imageEdges;
    std::string cycle = ImportCycleCheck::Check("a", {"a"}, imageEdges);
    CHECK(cycle == "a -> a", "expected 'a -> a', got '" + cycle + "'");
    PASS();
}

int main() {
    std::cerr << "=== NLang Import Cycle Unit Tests ===\n\n";
    test_direct_cycle();
    test_preexisting_cycle();
    test_acyclic_chain_silent();
    test_self_import();
    std::cerr << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
