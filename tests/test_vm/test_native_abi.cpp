// NativeHost ABI unit tests. A hand-written mock implements the NativeHost
// function table; we verify the C++ convenience wrappers and the ABI
// constants without any VM or real shared library.

#include "nlang/vm/NativeHost.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { ++g_pass; } \
        else { ++g_fail; std::fprintf(stderr, "FAIL: %s\n", msg); } \
    } while (0)

namespace {

struct MockHost {
    // Must be first: callbacks receive a NativeHost* equal to &mock.table
    // and reinterpret_cast it back to MockHost* (standard-layout, first
    // member shares the struct address).
    NativeHost table;

    std::map<int32_t, std::string> strings;   // handle -> text
    std::vector<std::string> createdStrings;
    std::vector<std::string> listItems;
    int32_t listHandle = -1;
    std::string output;
    std::string errorOutput;
    std::string nextLine;
    bool raised = false;
    int raisedKind = -1;
    std::string raisedMessage;
    bool seeded = false;
    int32_t seedValue = 0;
    uint32_t rngValue = 12345u;
};

//v13 string side: the byte length delimits the content, so the mock keeps
//NUL bytes exactly like the real host does.
const char* MockGetString(NativeHost* self, int32_t handle, int64_t* outLen) {
    auto* m = reinterpret_cast<MockHost*>(self);
    auto it = m->strings.find(handle);
    if (it == m->strings.end()) {
        if (outLen) *outLen = 0;
        return nullptr;
    }
    if (outLen) *outLen = static_cast<int64_t>(it->second.size());
    return it->second.c_str();
}

int32_t MockNewString(NativeHost* self, const char* utf8, int64_t len) {
    auto* m = reinterpret_cast<MockHost*>(self);
    m->createdStrings.push_back((utf8 && len > 0)
        ? std::string(utf8, static_cast<size_t>(len)) : std::string());
    return static_cast<int32_t>(1000 + m->createdStrings.size());
}

int32_t MockNewListString(NativeHost* self, const char* const* items,
                          const int64_t* lens, int count) {
    auto* m = reinterpret_cast<MockHost*>(self);
    m->listItems.clear();
    for (int i = 0; i < count; ++i) {
        const int64_t len = (items && items[i] && lens) ? lens[i] : 0;
        m->listItems.push_back((items && items[i] && len > 0)
            ? std::string(items[i], static_cast<size_t>(len)) : std::string());
    }
    m->listHandle = 2000;
    return m->listHandle;
}

void MockWriteOutput(NativeHost* self, const char* text, int64_t len) {
    auto* m = reinterpret_cast<MockHost*>(self);
    if (text && len > 0)
        m->output.append(text, static_cast<size_t>(len));
}

void MockWriteError(NativeHost* self, const char* text, int64_t len) {
    auto* m = reinterpret_cast<MockHost*>(self);
    if (text && len > 0)
        m->errorOutput.append(text, static_cast<size_t>(len));
}

const char* MockReadLine(NativeHost* self, int64_t* outLen) {
    auto* m = reinterpret_cast<MockHost*>(self);
    if (outLen) *outLen = static_cast<int64_t>(m->nextLine.size());
    return m->nextLine.c_str();
}

uint32_t MockNextRandom(NativeHost* self) {
    return reinterpret_cast<MockHost*>(self)->rngValue;
}

void MockSeedRandom(NativeHost* self, int32_t seed) {
    auto* m = reinterpret_cast<MockHost*>(self);
    m->seeded = true;
    m->seedValue = seed;
}

// Raise needs to record into the mock; provide a recording variant.
void MockRaiseRecord(NativeHost* self, int kind, const char* message,
                     int64_t len) {
    auto* m = reinterpret_cast<MockHost*>(self);
    m->raised = true;
    m->raisedKind = kind;
    m->raisedMessage = (message && len > 0)
        ? std::string(message, static_cast<size_t>(len)) : std::string();
}

MockHost MakeMock() {
    MockHost m;
    m.table.abiVersion = NLANG_HOST_ABI_VERSION;
    m.table.getString = &MockGetString;
    m.table.newString = &MockNewString;
    m.table.newListString = &MockNewListString;
    m.table.writeOutput = &MockWriteOutput;
    m.table.writeError = &MockWriteError;
    m.table.readLine = &MockReadLine;
    m.table.raiseException = &MockRaiseRecord;
    m.table.nextRandom = &MockNextRandom;
    m.table.seedRandom = &MockSeedRandom;
    return m;
}

void TestConstants() {
    CHECK(NLANG_VALUE_SIZE == 8, "value slot must be 8 bytes");
    CHECK(NLANG_HOST_ABI_VERSION >= 1u, "abi version defined");
}

void TestIntRoundTrip() {
    uint8_t args[NLANG_VALUE_SIZE * 2] = {};
    int32_t v = -42;
    std::memcpy(args + 1 * NLANG_VALUE_SIZE, &v, sizeof(v));
    CHECK(native::ArgInt(args, 1) == -42, "ArgInt reads slot");
    CHECK(native::ArgInt(args, 0) == 0, "ArgInt zero slot");

    uint8_t ret[NLANG_VALUE_SIZE] = {};
    native::ReturnInt(ret, 777);
    int32_t back = 0;
    std::memcpy(&back, ret, sizeof(back));
    CHECK(back == 777, "ReturnInt writes slot");
}

void TestFloatRoundTrip() {
    const float values[] = {3.5f, -1.25f, 0.0f, 1000.0f};
    for (float v : values) {
        uint8_t ret[NLANG_VALUE_SIZE] = {};
        native::ReturnFloat(ret, v);
        uint8_t args[NLANG_VALUE_SIZE] = {};
        std::memcpy(args, ret, NLANG_VALUE_SIZE);
        float back = native::ArgFloat(args, 0);
        CHECK(std::memcmp(&back, &v, sizeof(float)) == 0,
              "float bit-exact round trip");
    }
}

void TestDoubleRoundTrip() {
    const double values[] = {3.5, -1.25, 0.0, 1.4142135623730951,
                             1.0e300};
    for (double v : values) {
        uint8_t ret[NLANG_VALUE_SIZE] = {};
        native::ReturnDouble(ret, v);
        uint8_t args[NLANG_VALUE_SIZE] = {};
        std::memcpy(args, ret, NLANG_VALUE_SIZE);
        double back = native::ArgDouble(args, 0);
        CHECK(std::memcmp(&back, &v, sizeof(double)) == 0,
              "double bit-exact round trip");
    }
}

void TestLongRoundTrip() {
    const int64_t values[] = {0, -1, 42,
                              INT64_C(9223372036854775807),
                              INT64_C(-9223372036854775807) - 1};
    for (int64_t v : values) {
        uint8_t ret[NLANG_VALUE_SIZE] = {};
        native::ReturnLong(ret, v);
        uint8_t args[NLANG_VALUE_SIZE] = {};
        std::memcpy(args, ret, NLANG_VALUE_SIZE);
        CHECK(native::ArgLong(args, 0) == v, "long round trip");
    }
}

void TestStringArg() {
    MockHost m = MakeMock();
    m.strings[55] = std::string("hello");
    uint8_t args[NLANG_VALUE_SIZE] = {};
    int32_t handle = 55;
    std::memcpy(args, &handle, sizeof(handle));
    std::string s = native::ArgString(&m.table, args, 0);
    CHECK(s == "hello", "ArgString copies text");

    // Null for unknown handle -> empty string, not a crash.
    int32_t bad = 999;
    std::memcpy(args, &bad, sizeof(bad));
    std::string s2 = native::ArgString(&m.table, args, 0);
    CHECK(s2.empty(), "ArgString unknown handle -> empty");
}

void TestReturnString() {
    MockHost m = MakeMock();
    uint8_t ret[NLANG_VALUE_SIZE] = {};
    native::ReturnString(&m.table, ret, std::string("world"));
    CHECK(m.createdStrings.size() == 1 && m.createdStrings[0] == "world",
          "ReturnString creates text");
    int32_t handle = 0;
    std::memcpy(&handle, ret, sizeof(handle));
    CHECK(handle == 1001, "ReturnString writes created handle");
}

void TestListCallback() {
    MockHost m = MakeMock();
    const char* items[] = {"a.n", "b.n", "c.n"};
    const int64_t lens[] = {3, 3, 3};
    int32_t h = m.table.newListString(&m.table, items, lens, 3);
    CHECK(h == 2000, "newListString returns handle");
    CHECK(m.listItems.size() == 3 && m.listItems[1] == "b.n",
          "newListString forwards items");
}

void TestIoCallbacks() {
    MockHost m = MakeMock();
    m.table.writeOutput(&m.table, "line\n", 5);
    CHECK(m.output == "line\n", "writeOutput records text");

    m.table.writeError(&m.table, "boom\n", 5);
    CHECK(m.errorOutput == "boom\n", "writeError records diagnostics");

    m.nextLine = "typed text";
    const char* line = m.table.readLine(&m.table, nullptr);
    std::string copied = line ? line : "";   // copy immediately, as documented
    CHECK(copied == "typed text", "readLine returns line");
}

void TestRandomCallbacks() {
    MockHost m = MakeMock();
    CHECK(m.table.nextRandom(&m.table) == 12345u, "nextRandom forwards");
    m.table.seedRandom(&m.table, 7);
    CHECK(m.seeded && m.seedValue == 7, "seedRandom records seed");
}

void TestRaise() {
    MockHost m = MakeMock();
    native::Raise(&m.table, NEXC_IOException, std::string("boom"));
    CHECK(m.raised, "Raise invokes raiseException");
    CHECK(m.raisedKind == NEXC_IOException, "Raise forwards kind");
    CHECK(m.raisedMessage == "boom", "Raise forwards message");
}

} // namespace

int main() {
    std::fprintf(stderr, "=== NativeHost ABI Unit Tests ===\n");
    TestConstants();
    TestIntRoundTrip();
    TestFloatRoundTrip();
    TestDoubleRoundTrip();
    TestLongRoundTrip();
    TestStringArg();
    TestReturnString();
    TestListCallback();
    TestIoCallbacks();
    TestRandomCallbacks();
    TestRaise();
    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
