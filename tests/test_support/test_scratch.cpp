/*---
test_scratch.cpp - unit tests for the per-process scratch directories.

Pins the contract test_support/test_scratch.h offers to the rest of the
suite: two names give two directories, both under the system temp dir, a
fresh name starts empty, and a name keeps its path and its contents for the
rest of the run (the sources one call site writes are what the next call
site compiles).

Cross-process independence is the property the per-process root component
exists for: two test processes running the same test on one machine must not
write the same scratch files. One process cannot observe another, so the
race itself is shown by running a test executable twice at once; what is
pinned here is the root's shape, which keeps that component from quietly
turning back into one shared fixed name.
---*/
#include "test_support/test_scratch.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#if defined(_WIN32)
    #include <process.h>
#else
    #include <unistd.h>
#endif

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

static bool file_has_content(const std::filesystem::path& rFile)
{
    std::error_code ec;
    return std::filesystem::file_size(rFile, ec) > 0 && !ec;
}

//Distinct names mean distinct trees, so a test's sources cannot be the
//directory another test is writing into.
static void test_named_dirs_are_distinct()
{
    TEST(named dirs are distinct);
    const auto alpha = test::ScratchDir("scratch_alpha");
    const auto beta = test::ScratchDir("scratch_beta");
    CHECK(alpha != beta, "two names share one directory");
    const auto temp = std::filesystem::temp_directory_path();
    CHECK(alpha.string().rfind(temp.string(), 0) == 0,
        "scratch dir escapes the system temp dir");
    CHECK(beta.string().rfind(temp.string(), 0) == 0,
        "scratch dir escapes the system temp dir");
    PASS();
}

//A first use creates the directory and nothing else in it: a run must not
//inherit files an earlier run happened to leave behind.
static void test_fresh_dir_is_created_empty()
{
    TEST(fresh dir is created empty);
    const auto dir = test::ScratchDir("scratch_fresh");
    CHECK(std::filesystem::is_directory(dir), "scratch dir was not created");
    CHECK(std::filesystem::is_empty(dir), "scratch dir is not empty on first use");
    PASS();
}

//The same name answers with the same path and keeps what was written under
//it: the emitter and the reader of a scratch tree are separate call sites
//in one test process.
static void test_same_name_keeps_its_contents()
{
    TEST(same name keeps its contents);
    const auto first = test::ScratchDir("scratch_reuse");
    const auto file = first / "seed.n";
    {
        std::ofstream out(file);
        out << "main() {}\n";
    }
    CHECK(file_has_content(file), "seed file was not written");
    const auto again = test::ScratchDir("scratch_reuse");
    CHECK(again == first, "the same name answers with a different path");
    CHECK(std::filesystem::exists(file), "the second call dropped the tree");
    std::filesystem::remove_all(first);
    PASS();
}

//The root's last component identifies this process, which is what makes
//concurrent test processes independent.
static void test_root_carries_the_process_component()
{
    TEST(root carries the process component);
#if defined(_WIN32)
    const std::string ownPid = std::to_string(static_cast<unsigned long>(_getpid()));
#else
    const std::string ownPid = std::to_string(static_cast<unsigned long>(getpid()));
#endif
    CHECK(test::ProcessTag() == ownPid,
        "the process component is not this process's id");
    CHECK(std::all_of(ownPid.begin(), ownPid.end(),
        [](unsigned char ch) { return std::isdigit(ch) != 0; }),
        "the process component is not an id");
    const auto root = test::ScratchRoot();
    CHECK(root.filename() == std::filesystem::path(ownPid),
        "the scratch root is not named after this process");
    CHECK(std::filesystem::is_directory(root), "the scratch root was not created");
    PASS();
}

int main()
{
    test_named_dirs_are_distinct();
    test_fresh_dir_is_created_empty();
    test_same_name_keeps_its_contents();
    test_root_carries_the_process_component();
    std::cerr << "\nscratch: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
