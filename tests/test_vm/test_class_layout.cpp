// VmClassLayout unit tests (0.9.12 T1/T2): the runtime instance-field
// layout view joins the superClassIdx chain base-first over OWN-only
// CompiledClass records (NCU v2.7 — the wire no longer flattens the
// inherited surface). Hand-built records — no codegen, no linking.
#include "VmClassLayout.h"
#include "nlang/vm/CompiledModule.h"
#include "nlang/runtime/Runtime.h"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) \
    do { if (cond) ++g_pass; else { ++g_fail; std::fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

namespace {

// One own field appended to the record's parallel arrays (the v2.7
// wire shape: own fields only). structIdx/classIdx default to the
// 0xFFFF "not a nested type" sentinel.
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

// View vs record, field by field — valid for ROOT classes (super -1),
// where the joined view is exactly the record.
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

// T2 contract: records carry own fields only; the view joins the chain
// base-first, declaration order within each level, entries borrowing the
// owning record's strings.
void TestClassLayoutOwnOnlyChainJoin() {
    std::vector<CompiledClass> classes;
    // Base: scalar, string, nested struct (structs[0]), nested class (self).
    CompiledClass base = MakeClass("Base", -1);
    AddField(base, "a", RTK_Int32);
    AddField(base, "s", RTK_String);
    AddField(base, "st", RTK_Struct, /*structIdx=*/0);
    AddField(base, "c", RTK_Class, 0xFFFF, /*classIdx=*/0);
    classes.push_back(base);
    // Mid and Leaf carry ONLY their own field — v2.7 records no longer
    // flatten the inherited surface.
    CompiledClass mid = MakeClass("Mid", 0);
    AddField(mid, "m", RTK_Int32);
    classes.push_back(mid);
    CompiledClass leaf = MakeClass("Leaf", 1);
    AddField(leaf, "l", RTK_Int32);
    classes.push_back(leaf);

    VmClassLayout layout;
    layout.Build(classes);

    // Root class: the joined view is exactly the record.
    CheckMatchesRecord(layout, 0, classes[0], "Base view == record");

    // Three-level join order: base fields first (declaration order),
    // then Mid's, then Leaf's.
    const auto& leafFields = layout.Fields(2);
    CHECK(leafFields.size() == 6, "leaf view joins all three levels");
    if (leafFields.size() == 6) {
        const char* want[6] = { "a", "s", "st", "c", "m", "l" };
        for (int i = 0; i < 6; ++i)
            CHECK(*leafFields[i].name == want[i], "join order is base-first");
        // Nested-type indices ride the joined entries unchanged.
        CHECK(leafFields[2].structIdx == 0 && leafFields[2].kind == RTK_Struct,
              "inherited struct index survives the join");
        CHECK(leafFields[3].classIdx == 0 && leafFields[3].kind == RTK_Class,
              "inherited class index survives the join");
    }
    // Name pointers alias the OWNING record's string — an inherited
    // entry borrows the base record's, never a copy.
    CHECK(layout.Fields(2)[0].name == &classes[0].fieldNames[0],
          "inherited entry aliases the base record string");
    CHECK(layout.Fields(2)[4].name == &classes[1].fieldNames[0],
          "mid entry aliases the mid record string");
    CHECK(layout.Fields(1)[4].name == &classes[1].fieldNames[0],
          "own entry aliases the own record string");

    // Exception family shape (v2.7): the message/backtrace pair lives
    // on the Exception base record ONLY; a subclass record carries no
    // copy and still sees the pair at view index 0/1 — the offsets
    // FindClassFieldOffset hardcodes (field i at cell 1+2i: message
    // index 0 -> offset 4, backtrace index 1 -> offset 12).
    CompiledClass exc = MakeClass("Exception", -1);
    AddField(exc, "message", RTK_String);
    AddField(exc, "backtrace", RTK_String);
    classes.push_back(exc);
    CompiledClass iae = MakeClass("IllegalArgumentException", 3);
    classes.push_back(iae);
    layout.Build(classes);
    CheckMatchesRecord(layout, 3, classes[3], "Exception view == record");
    CHECK(layout.Fields(4).size() == 2, "IAE view inherits the pair");
    CHECK(*layout.Fields(4)[0].name == "message", "IAE view[0] message (offset 4)");
    CHECK(*layout.Fields(4)[1].name == "backtrace", "IAE view[1] backtrace (offset 12)");
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

// Defensive bounds (the loader validated chain termination/acyclicity at
// load time, T2.7 — Build is the second line for hand-built or corrupt
// tables): a cycle never reaches -1 and must throw, and an out-of-range
// super index must throw instead of reading past the table.
void TestClassLayoutDepthBound() {
    VmClassLayout layout;

    std::vector<CompiledClass> cyc;
    cyc.push_back(MakeClass("A", 1));
    cyc.push_back(MakeClass("B", 0));
    bool threw = false;
    try { layout.Build(cyc); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw, "super chain cycle rejected");

    // Self-loop: the depth bound fires on the second visit.
    std::vector<CompiledClass> loop;
    loop.push_back(MakeClass("Self", 0));
    threw = false;
    try { layout.Build(loop); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw, "self-loop super rejected");

    std::vector<CompiledClass> oor;
    oor.push_back(MakeClass("Solo", 7));
    threw = false;
    try { layout.Build(oor); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw, "out-of-range super index rejected");
}

} // namespace

int main() {
    Runtime::StaticInit();   //in-process host requirement (IdString tables)
    std::fprintf(stderr, "=== VmClassLayout View Tests ===\n");
    TestClassLayoutOwnOnlyChainJoin();
    TestRebuildReplacesView();
    TestClassLayoutDepthBound();
    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
