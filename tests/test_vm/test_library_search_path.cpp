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
using nlang::BuildLibrarySearchPathLayered;
using nlang::FormatSearchDirs;
using nlang::SearchDirEntry;
using nlang::SearchLayer;

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

static void TestLayeredAttribution() {
    // The layered variant tags each surviving entry with the layer that
    // contributed it, in the canonical order.
    SearchPathInput in;
    in.explicitDirs = {"ed"};
    in.configuredDirs = {"cd"};
    in.baseDirs = {"bd"};
    in.pathEnv = std::string("pd") + kSep;
    in.systemDirs = {"sd"};
    auto traced = BuildLibrarySearchPathLayered(in);
    CHECK(traced.size() == 5, "layered: all five entries survive");
    CHECK(traced[0].layer == SearchLayer::CommandLine,
          "layered: -I first");
    CHECK(traced[1].layer == SearchLayer::Project,
          "layered: project second");
    CHECK(traced[2].layer == SearchLayer::Local, "layered: local third");
    CHECK(traced[3].layer == SearchLayer::Environment,
          "layered: NLANG_PATH fourth");
    CHECK(traced[4].layer == SearchLayer::System, "layered: system last");
    // Parity with the flat build: same directories in the same order.
    auto flat = BuildLibrarySearchPath(in);
    CHECK(flat.size() == traced.size(), "layered: size parity with flat");
    bool same = flat.size() == traced.size();
    for (size_t i = 0; same && i < traced.size(); ++i)
        same = flat[i] == traced[i].dir;
    CHECK(same, "layered: directory order parity with flat");
}

static void TestLayeredDedupKeepsFirstLayer() {
    // A directory appearing in two layers survives once, attributed to
    // the FIRST (higher-priority) layer — the layer that actually wins.
    SearchPathInput in;
    in.explicitDirs = {"dup"};
    in.systemDirs = {"dup", "sd"};
    auto traced = BuildLibrarySearchPathLayered(in);
    CHECK(traced.size() == 2, "layered dedup: one entry for the duplicate");
    CHECK(traced[0].layer == SearchLayer::CommandLine,
          "layered dedup: first layer wins the attribution");
    CHECK(traced[1].dir == Key("sd"), "layered dedup: survivor keeps order");
}

static void TestFormat() {
    // The --verbose listing: a header line, then one "dir (layer)" line
    // per entry in search order.
    std::vector<SearchDirEntry> entries = {
        {Key("ed"), SearchLayer::CommandLine},
        {Key("sd"), SearchLayer::System},
    };
    const std::string text = FormatSearchDirs(entries);
    const std::string want =
        "import search path (first match wins):\n"
        + Key("ed") + " (-I)\n" + Key("sd") + " (system)\n";
    CHECK(text == want, "format: header + 'dir (layer)' lines");
}

int main() {
    std::printf("=== LibrarySearchPath Unit Tests ===\n");
    TestSplit();
    TestLayeredOrder();
    TestDedup();
    TestNormalize();
#ifdef _WIN32
    TestCaseFold();
#endif
    TestLayeredAttribution();
    TestLayeredDedupKeepsFirstLayer();
    TestFormat();
    std::printf("\n=== Results: %s (%d pass, %d fail) ===\n",
        g_fail == 0 ? "all passed" : "FAILURES", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
