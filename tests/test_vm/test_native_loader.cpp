// NativeLibraryLoader unit tests. Real fixture shared libraries are built
// alongside this test; we load them for real (no mocks of the loader) and
// verify discovery, registration, ABI/entry errors and idempotence.

#include "NativeLibraryLoader.h"
#include "nlang/vm/NativeHost.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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
    loader.EnsureLoaded("sample",
        [&](const std::string& ns, const std::string& name, NativeFn fn) {
            fns[ns + "." + name] = fn;
        });

    CHECK(fns.size() == 3, "sample module registers 3 functions");
    CHECK(fns.count("sample.add") && fns.count("sample.doubleFloat")
          && fns.count("sample.greet"), "sample function names");

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
}

void TestIdempotent(const std::string& fixtureDir) {
    NativeLibraryLoader loader;
    loader.AddSearchDir(fixtureDir);
    int firstCount = 0;
    loader.EnsureLoaded("sample",
        [&](const std::string&, const std::string&, NativeFn) {
            ++firstCount;
        });
    int secondCount = 0;
    loader.EnsureLoaded("sample",
        [&](const std::string&, const std::string&, NativeFn) {
            ++secondCount;
        });
    CHECK(firstCount == 3, "first init registers 3");
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
            [](const std::string&, const std::string&, NativeFn) {});
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
            [](const std::string&, const std::string&, NativeFn) {});
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
            [](const std::string&, const std::string&, NativeFn) {});
    } catch (const std::runtime_error& e) {
        threw = true;
        message = e.what();
    }
    CHECK(threw, "missing entry throws");
    CHECK(message.find("nlang_native_init") != std::string::npos,
          "missing-entry error names nlang_native_init");
}

} // namespace

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
    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
