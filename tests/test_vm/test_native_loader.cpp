// NativeLibraryLoader unit tests. Real fixture shared libraries are built
// alongside this test; we load them for real (no mocks of the loader) and
// verify discovery, registration, ABI/entry errors and idempotence.

#include "NativeLibraryLoader.h"
#include "nlang/vm/NativeHost.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { ++g_pass; } \
        else { ++g_fail; std::fprintf(stderr, "FAIL: %s\n", msg); } \
    } while (0)

namespace {

// Minimal host for invoking the registered sample functions (string mint).
struct MiniHost {
    NativeHost table;
    std::map<int32_t, std::string> strings;
    int nextHandle = 1;

    MiniHost() {
        std::memset(&table, 0, sizeof(table));
        table.abiVersion = NLANG_HOST_ABI_VERSION;
        table.getString = &GetString;
        table.newString = &NewString;
    }

    static const char* GetString(NativeHost* self, int32_t handle) {
        auto* m = reinterpret_cast<MiniHost*>(self);
        auto it = m->strings.find(handle);
        return it == m->strings.end() ? nullptr : it->second.c_str();
    }
    static int32_t NewString(NativeHost* self, const char* utf8) {
        auto* m = reinterpret_cast<MiniHost*>(self);
        int32_t h = m->nextHandle++;
        m->strings[h] = utf8 ? utf8 : "";
        return h;
    }
};

void PutInt(uint8_t* args, int slot, int32_t v) {
    std::memcpy(args + slot * NLANG_VALUE_SIZE, &v, sizeof(v));
}

int32_t GetRetInt(const uint8_t* ret) {
    int32_t v = 0;
    std::memcpy(&v, ret, sizeof(v));
    return v;
}

void TestLoadAndCall(const std::string& fixtureDir) {
    NativeLibraryLoader loader;
    loader.AddSearchDir(fixtureDir);

    std::map<std::string, NativeFn> fns;
    std::map<std::string, bool> blocking;
    loader.EnsureLoaded("sample",
        [&](const std::string& ns, const std::string& name, NativeFn fn,
            bool isBlocking) {
            fns[ns + "." + name] = fn;
            blocking[ns + "." + name] = isBlocking;
        });

    CHECK(fns.size() == 4, "sample module registers 4 functions");
    CHECK(fns.count("sample.add") && fns.count("sample.doubleFloat")
          && fns.count("sample.greet")
          && fns.count("sample.blockPing"), "sample function names");
    //ABI v5: the blocking registrar routes through the same callback
    //with the flag set; the plain registrar leaves it false.
    CHECK(blocking["sample.blockPing"], "blockPing registered blocking");
    CHECK(!blocking["sample.add"] && !blocking["sample.doubleFloat"]
          && !blocking["sample.greet"],
          "other sample functions registered non-blocking");

    MiniHost host;

    // add(2, 3) -> 5
    uint8_t args[NLANG_VALUE_SIZE * 2] = {};
    PutInt(args, 0, 2);
    PutInt(args, 1, 3);
    uint8_t ret[NLANG_VALUE_SIZE] = {};
    fns["sample.add"](&host.table, ret, args, 2);
    CHECK(GetRetInt(ret) == 5, "sample.add computes 2+3");

    // doubleFloat(2.5) -> 5.0
    float in = 2.5f;
    int32_t inBits = 0;
    std::memcpy(&inBits, &in, sizeof(float));
    PutInt(args, 0, inBits);
    fns["sample.doubleFloat"](&host.table, ret, args, 1);
    float out = 0.0f;
    int32_t outBits = GetRetInt(ret);
    std::memcpy(&out, &outBits, sizeof(float));
    CHECK(out == 5.0f, "sample.doubleFloat computes 2*2.5");

    // greet("bob") -> "hello bob"
    int32_t nameHandle = host.table.newString(&host.table, "bob");
    PutInt(args, 0, nameHandle);
    fns["sample.greet"](&host.table, ret, args, 1);
    int32_t resultHandle = GetRetInt(ret);
    const char* text = host.table.getString(&host.table, resultHandle);
    std::string copied = text ? text : "";
    CHECK(copied == "hello bob", "sample.greet builds greeting");

    // blockPing("bob") -> "blocking:bob" (same call ABI either channel)
    PutInt(args, 0, nameHandle);
    fns["sample.blockPing"](&host.table, ret, args, 1);
    resultHandle = GetRetInt(ret);
    text = host.table.getString(&host.table, resultHandle);
    copied = text ? text : "";
    CHECK(copied == "blocking:bob", "sample.blockPing callable");
}

void TestIdempotent(const std::string& fixtureDir) {
    NativeLibraryLoader loader;
    loader.AddSearchDir(fixtureDir);
    int firstCount = 0;
    loader.EnsureLoaded("sample",
        [&](const std::string&, const std::string&, NativeFn, bool) {
            ++firstCount;
        });
    int secondCount = 0;
    loader.EnsureLoaded("sample",
        [&](const std::string&, const std::string&, NativeFn, bool) {
            ++secondCount;
        });
    CHECK(firstCount == 4, "first init registers 4");
    CHECK(secondCount == 0, "second EnsureLoaded does not re-init");
}

void TestNotFound() {
    NativeLibraryLoader loader;
    loader.AddSearchDir("Z:/no/such/dir/one");
    loader.AddSearchDir("Z:/no/such/dir/two");
    bool threw = false;
    std::string message;
    try {
        loader.EnsureLoaded("ghost",
            [](const std::string&, const std::string&, NativeFn, bool) {});
    } catch (const std::runtime_error& e) {
        threw = true;
        message = e.what();
    }
    CHECK(threw, "missing module throws");
    CHECK(message.find("ghost") != std::string::npos
          && message.find("one") != std::string::npos,
          "missing-module error names the namespace and searched paths");
}

void TestBadAbi(const std::string& fixtureDir) {
    NativeLibraryLoader loader;
    loader.AddSearchDir(fixtureDir);
    bool threw = false;
    std::string message;
    try {
        loader.EnsureLoaded("badabi",
            [](const std::string&, const std::string&, NativeFn, bool) {});
    } catch (const std::runtime_error& e) {
        threw = true;
        message = e.what();
    }
    CHECK(threw, "bad ABI throws");
    CHECK(message.find("ABI") != std::string::npos,
          "bad-ABI error mentions ABI");
}

void TestNoEntry(const std::string& fixtureDir) {
    NativeLibraryLoader loader;
    loader.AddSearchDir(fixtureDir);
    bool threw = false;
    std::string message;
    try {
        loader.EnsureLoaded("noentry",
            [](const std::string&, const std::string&, NativeFn, bool) {});
    } catch (const std::runtime_error& e) {
        threw = true;
        message = e.what();
    }
    CHECK(threw, "missing entry throws");
    CHECK(message.find("nlang_native_init") != std::string::npos,
          "missing-entry error names nlang_native_init");
}

} // namespace

//0.11.4 C: build-tree fallback. With no explicit search dirs, the loader
//must still find the stdlib native modules through the built-in fallback
//(walk up from the executable to <build>/native/<config>). The test exe
//runs from <build>/tests/<config>, so the walk reaches the build root.
void TestBuildTreeFallback() {
    NativeLibraryLoader loader;
    const std::string path = loader.LocateModuleFile("io");
    CHECK(!path.empty(), "fallback must resolve nlang_io in the build tree");
    CHECK(path.find("native") != std::string::npos,
          "resolved path must come from the native output directory");
}

//0.11.4 C review: pin the explicit-dirs-before-fallback order. A decoy
//nlang_io.dll (the badabi fixture renamed) sits in an explicit search
//dir while the real module is reachable through the fallback: correct
//order loads the decoy and dies on its ABI check; a reversed order
//would load the real module silently.
void TestExplicitDirsWinOverFallback() {
    namespace fs = std::filesystem;
    const fs::path decoyDir = fs::temp_directory_path()
        / "nlang_loader_order_test";
    std::error_code ec;
    fs::remove_all(decoyDir, ec);
    fs::create_directories(decoyDir, ec);
    fs::copy_file(fs::path(FIXTURE_DIR) / "nlang_badabi.dll",
                  decoyDir / "nlang_io.dll",
                  fs::copy_options::overwrite_existing, ec);
    CHECK(!ec, "decoy copy must succeed");

    NativeLibraryLoader loader;
    loader.AddSearchDir(decoyDir.string());
    bool threw = false;
    std::string message;
    try {
        loader.EnsureLoaded("io",
            [](const std::string&, const std::string&, NativeFn, bool) {});
    } catch (const std::runtime_error& e) {
        threw = true;
        message = e.what();
    }
    CHECK(threw, "the explicit-dir decoy must win the load");
    CHECK(message.find("ABI") != std::string::npos,
          "the decoy is detectable through its ABI mismatch");

    fs::remove_all(decoyDir, ec);
}

int main() {
    std::string fixtureDir = FIXTURE_DIR;
    std::fprintf(stderr, "=== NativeLibraryLoader Unit Tests ===\n");
    std::fprintf(stderr, "fixtureDir=%s\n", fixtureDir.c_str());
    auto run = [](const char* name, void(*fn)(const std::string&),
                  const std::string& arg) {
        try {
            fn(arg);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "UNCAUGHT in %s: %s\n", name, e.what());
            ++g_fail;
        }
    };
    run("LoadAndCall", &TestLoadAndCall, fixtureDir);
    run("Idempotent", &TestIdempotent, fixtureDir);
    try { TestNotFound(); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "UNCAUGHT in NotFound: %s\n", e.what());
        ++g_fail;
    }
    run("BadAbi", &TestBadAbi, fixtureDir);
    run("NoEntry", &TestNoEntry, fixtureDir);
    try { TestBuildTreeFallback(); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "UNCAUGHT in BuildTreeFallback: %s\n", e.what());
        ++g_fail;
    }
    try { TestExplicitDirsWinOverFallback(); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "UNCAUGHT in ExplicitDirsWinOverFallback: %s\n",
                     e.what());
        ++g_fail;
    }
    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
