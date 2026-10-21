// VmClassLayout unit tests (0.9.12 T1): the runtime instance-field
// layout view must mirror the CompiledClass records field-for-field
// while the producer still ships flattened full-layout field arrays
// (T2 switches Build to the base-first chain join and extends this
// file). Hand-built records — no codegen, no linking.
#include "VmClassLayout.h"
#include "nlang/vm/CompiledModule.h"
#include "nlang/runtime/Runtime.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) \
    do { if (cond) ++g_pass; else { ++g_fail; std::fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

namespace {

// One own field appended to the record's parallel arrays (the shape the
// producer writes today). structIdx/classIdx default to the 0xFFFF
// "not a nested type" sentinel.
void AddField(CompiledClass& cc, const std::string& name, uint16_t kind,
              uint16_t structIdx = 0xFFFF, uint16_t classIdx = 0xFFFF) {
    cc.fieldNames.push_back(name);
    cc.fieldTypeKinds.push_back(kind);
    cc.fieldStructIndices.push_back(structIdx);
    cc.fieldClassIndices.push_back(classIdx);
    ++cc.fieldCount;
}

CompiledClass MakeClass(const std::string& name, int16_t superIdx) {
    CompiledClass cc;
    cc.name = name;
    cc.superClassIdx = superIdx;
    return cc;
}

// View vs record, field by field.
void CheckMatchesRecord(const VmClassLayout& layout, size_t classIdx,
                        const CompiledClass& cc, const char* label) {
    const auto& fields = layout.Fields(classIdx);
    CHECK(fields.size() == cc.fieldCount, label);
    if (fields.size() != cc.fieldCount) return;
    for (uint16_t i = 0; i < cc.fieldCount; ++i) {
        CHECK(*fields[i].name == cc.fieldNames[i], label);
        CHECK(fields[i].kind == cc.fieldTypeKinds[i], label);
        CHECK(fields[i].structIdx == cc.fieldStructIndices[i], label);
        CHECK(fields[i].classIdx == cc.fieldClassIndices[i], label);
    }
}

// T1 contract: the view mirrors the flattened records exactly — same
// field count, same order, same kind/structIdx/classIdx per field, and
// name pointers alias the record strings (borrowed, never copied).
void TestViewMirrorsRecords() {
    std::vector<CompiledClass> classes;
    // Base: scalar, string, nested struct (structs[0]), nested class (self).
    CompiledClass base = MakeClass("Base", -1);
    AddField(base, "a", RTK_Int32);
    AddField(base, "s", RTK_String);
    AddField(base, "st", RTK_Struct, /*structIdx=*/0);
    AddField(base, "c", RTK_Class, 0xFFFF, /*classIdx=*/0);
    classes.push_back(base);
    // Derived: the producer flattens the inherited surface into the
    // record (current form), then appends its own field.
    CompiledClass derived = MakeClass("Derived", 0);
    AddField(derived, "a", RTK_Int32);
    AddField(derived, "s", RTK_String);
    AddField(derived, "st", RTK_Struct, 0);
    AddField(derived, "c", RTK_Class, 0xFFFF, 0);
    AddField(derived, "d", RTK_Int32);
    classes.push_back(derived);
    // Exception family shape: the message/backtrace pair rides on every
    // subclass record today (builtin registration); the view must
    // expose it unchanged at the offsets FindClassFieldOffset hardcodes
    // (message index 0 → offset 4, backtrace index 1 → offset 12).
    CompiledClass exc = MakeClass("Exception", -1);
    AddField(exc, "message", RTK_String);
    AddField(exc, "backtrace", RTK_String);
    classes.push_back(exc);
    CompiledClass iae = MakeClass("IllegalArgumentException", 2);
    AddField(iae, "message", RTK_String);
    AddField(iae, "backtrace", RTK_String);
    classes.push_back(iae);

    VmClassLayout layout;
    layout.Build(classes);

    CheckMatchesRecord(layout, 0, classes[0], "Base view == record");
    CheckMatchesRecord(layout, 1, classes[1], "Derived view == record");
    CheckMatchesRecord(layout, 2, classes[2], "Exception view == record");
    CheckMatchesRecord(layout, 3, classes[3], "IAE view == record");

    // The flattened derived record carries the base fields — the view
    // exposes them (this is what keeps offsets identical to today).
    CHECK(layout.Fields(1).size() == 5, "derived view includes base fields");
    CHECK(*layout.Fields(1)[0].name == "a", "derived field 0 is base's a");
    // Name pointers alias the record strings.
    CHECK(layout.Fields(0)[0].name == &classes[0].fieldNames[0],
          "name pointer aliases the record string");
    CHECK(*layout.Fields(3)[0].name == "message", "IAE view[0] message");
    CHECK(*layout.Fields(3)[1].name == "backtrace", "IAE view[1] backtrace");
}

// Rebuild for a different module replaces the view entirely (the
// InitializeForRun hook runs Build whenever m_currModule is set).
void TestRebuildReplacesView() {
    std::vector<CompiledClass> m1;
    CompiledClass c1 = MakeClass("One", -1);
    AddField(c1, "x", RTK_Int32);
    m1.push_back(c1);
    std::vector<CompiledClass> m2;
    CompiledClass c2 = MakeClass("Two", -1);
    AddField(c2, "y", RTK_String);
    AddField(c2, "z", RTK_Int32);
    m2.push_back(c2);

    VmClassLayout layout;
    layout.Build(m1);
    CHECK(layout.Fields(0).size() == 1, "module 1 view");
    layout.Build(m2);
    CHECK(layout.Fields(0).size() == 2, "module 2 view replaces module 1");
    CHECK(*layout.Fields(0)[0].name == "y", "module 2 field 0 is y");
}

} // namespace

int main() {
    Runtime::StaticInit();   //in-process host requirement (IdString tables)
    std::fprintf(stderr, "=== VmClassLayout View Tests ===\n");
    TestViewMirrorsRecords();
    TestRebuildReplacesView();
    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
