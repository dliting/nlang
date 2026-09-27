// LibrarySearchPath unit tests (header-only, pure STL). Verify the layered
// ordering, environment splitting, normalization and de-duplication. No
// filesystem existence checks are performed by the resolver, so the tests
// are fully deterministic.

#include "nlang/common/LibrarySearchPath.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nlang::SearchPathInput;
using nlang::SplitSearchPathEnv;
using nlang::BuildLibrarySearchPath;

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { ++g_pass; } \
        else { ++g_fail; std::fprintf(stderr, "FAIL: %s\n", msg); } \
    } while (0)

namespace {

#if defined(_WIN32)
const char* kSep = ";";
#else
const char* kSep = ":";
#endif

// The normalized absolute spelling the resolver maps a relative name to.
std::string Key(const std::string& name) {
    return fs::absolute(name).lexically_normal().string();
}

// Index of a relative name in the built list; -1 when absent.
int IndexOf(const std::vector<std::string>& list, const std::string& name) {
    const std::string key = Key(name);
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i] == key)
            return static_cast<int>(i);
    return -1;
}

} // namespace

static void TestSplit() {
    // Basic split on the platform separator, order preserved.
    auto two = SplitSearchPathEnv(std::string("d1") + kSep + "d2");
    CHECK(two.size() == 2, "split: two entries");
    CHECK(two.size() == 2 && two[0] == "d1" && two[1] == "d2",
          "split: order");
    // Empty / leading / trailing / repeated separators drop entries.
    auto gaps = SplitSearchPathEnv(std::string(kSep) + "d1" + kSep + kSep);
    CHECK(gaps.size() == 1 && gaps[0] == "d1", "split: empty entries dropped");
    CHECK(SplitSearchPathEnv("").empty(), "split: empty value -> empty");
}

static void TestLayeredOrder() {
    SearchPathInput in;
    in.explicitDirs = {"ed"};
    in.configuredDirs = {"cd"};
    in.baseDirs = {"bd"};
    in.pathEnv = std::string("pd1") + kSep + "pd2";
    in.systemDirs = {"sd"};
    auto list = BuildLibrarySearchPath(in);
    CHECK(list.size() == 6, "order: all six entries present");
    // CLI -I > project configured > local base > NLANG_PATH > system.
    const int ied = IndexOf(list, "ed");
    const int icd = IndexOf(list, "cd");
    const int ibd = IndexOf(list, "bd");
    const int ip1 = IndexOf(list, "pd1");
    const int ip2 = IndexOf(list, "pd2");
    const int isd = IndexOf(list, "sd");
    CHECK(ied >= 0 && icd >= 0 && ibd >= 0 && ip1 >= 0 && ip2 >= 0
              && isd >= 0, "order: every layer resolved");
    CHECK(ied < icd && icd < ibd, "order: explicit < configured < base");
    // The environment sits AFTER the local base (Python sys.path layout).
    CHECK(ibd < ip1 && ip1 < ip2 && ip2 < isd,
          "order: base < env < system");
}

static void TestDedup() {
    // The same directory appearing in two layers keeps its FIRST occurrence.
    SearchPathInput in;
    in.explicitDirs = {"dup"};
    in.configuredDirs = {"dup"};
    in.systemDirs = {"sd"};
    auto list = BuildLibrarySearchPath(in);
    CHECK(list.size() == 2, "dedup: cross-layer duplicate collapsed");
    CHECK(IndexOf(list, "dup") == 0, "dedup: first occurrence kept");
    // Empty strings are dropped.
    SearchPathInput empty;
    empty.explicitDirs = {"", "keep", ""};
    auto l2 = BuildLibrarySearchPath(empty);
    CHECK(l2.size() == 1 && IndexOf(l2, "keep") == 0,
          "dedup: empty strings dropped");
}

static void TestNormalize() {
    // Lexical normalization folds "a/../z" to "z".
    SearchPathInput in;
    in.explicitDirs = {"q/../z"};
    auto list = BuildLibrarySearchPath(in);
    CHECK(list.size() == 1 && list[0] == Key("z"),
          "normalize: a/../z -> z");
}

#ifdef _WIN32
static void TestCaseFold() {
    // Windows: de-duplication is case-insensitive (NTFS semantics).
    SearchPathInput in;
    in.explicitDirs = {"mydir/Abc"};
    in.systemDirs = {"mydir/abc"};
    auto list = BuildLibrarySearchPath(in);
    CHECK(list.size() == 1, "win: case-insensitive dedup");
}
#endif

int main() {
    std::printf("=== LibrarySearchPath Unit Tests ===\n");
    TestSplit();
    TestLayeredOrder();
    TestDedup();
    TestNormalize();
#ifdef _WIN32
    TestCaseFold();
#endif
    std::printf("\n=== Results: %s (%d pass, %d fail) ===\n",
        g_fail == 0 ? "all passed" : "FAILURES", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
