// --- v0.13.9 phase 3 Task 10: reference qualifiers in .ncu signatures ---
// Roundtrip pins for the & / const marks a producer writes and a consumer
// rebuilds: top-level formal flags (ParamTypeDesc), func-type element
// flags (RTK_Func arm) and the version-gate floor the format change
// forces (v2.21). Shares the debugger harness — real in-process compiles.
#include "test_debugger_common.h"

using namespace nlang;

namespace {

//Find a function record by qualified name; null when absent.
const CompiledFunction* FindFunc(const CompiledModule& mod,
    const std::string& name)
{
    const int idx = mod.FindFunction(name);
    return idx < 0 ? nullptr
        : &mod.functions[static_cast<size_t>(idx)];
}

} //namespace

//The producer must serialize a & formal's mark: `int bump(int& v)` writes
//PTDF_Ref on the formal descriptor, `const int&` adds PTDF_Const. Before
//v2.21 the flags byte carried PTDF_Out alone, so a consumer rebuilt `int`
//formals — the by-value call then crashed the &-reading callee body.
static void test_ref_formal_flags_roundtrip()
{
    TEST(ref_formal_flags_roundtrip);
    BuildOutcome b = buildSource("refsig_lib",
        "int bump(int& v) { v = v + 1; return v; }\n"
        "int cb(const int& v) { return v + 1; }\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("refsig_lib");

    const CompiledFunction* bump = FindFunc(mod, "refsig_lib.bump");
    CHECK(bump != nullptr, "bump should be in the module");
    CHECK(bump && bump->paramTypeDescs.size() == 1
        && (bump->paramTypeDescs[0].flags & PTDF_Ref) != 0
        && (bump->paramTypeDescs[0].flags & PTDF_Const) == 0,
        "the & formal descriptor should carry PTDF_Ref");

    const CompiledFunction* cb = FindFunc(mod, "refsig_lib.cb");
    CHECK(cb != nullptr, "cb should be in the module");
    CHECK(cb && cb->paramTypeDescs.size() == 1
        && (cb->paramTypeDescs[0].flags & PTDF_Ref) != 0
        && (cb->paramTypeDescs[0].flags & PTDF_Const) != 0,
        "the const & formal descriptor should carry PTDF_Ref|PTDF_Const");
    PASS();
}

//A func-typed field's signature must keep its & marks through the
//RTK_Func arm: `func<int, int&> f` writes the ref bit on the param
//element (return element stays unmarked). Before v2.21 the arm carried
//no flags at all, so the consumer's rebuilt key spelled
//func<int, int> and every marked consumer spelling mismatched.
static void test_func_sig_elem_flags_roundtrip()
{
    TEST(func_sig_elem_flags_roundtrip);
    BuildOutcome b = buildSource("refsig_box",
        "int bump(int& v) { v = v + 1; return v; }\n"
        "class Box {\n"
        "    public func<int, int&> f;\n"
        "    public Box() { f = bump; }\n"
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("refsig_box");

    const CompiledClass* box = nullptr;
    for (const auto& c : mod.classes)
        if (c.name == "refsig_box.Box")
            box = &c;
    CHECK(box != nullptr, "Box should be in the classes table");
    if (!box)
    {
        PASS();
        return;
    }
    const TypeDesc* funcDesc = nullptr;
    for (const auto& td : box->fieldTypeDescs)
        if (td.kind == RTK_Func)
            funcDesc = &td;
    CHECK(funcDesc != nullptr,
        "the func-typed field should serialize an RTK_Func descriptor");
    CHECK(funcDesc && funcDesc->elems.size() == 2
        && funcDesc->elemFlags.size() == 2,
        "the func descriptor carries one flag byte per element");
    CHECK(funcDesc && (funcDesc->elemFlags[0] & PTDF_Ref) != 0,
        "the int& param element carries the ref bit");
    CHECK(funcDesc && funcDesc->elemFlags[1] == 0,
        "the int return element stays unmarked");
    PASS();
}

//The consumer side of the same roundtrip: a func key imported from a
//producer unit must stay key-equal to the consumer's own marked
//spelling. Before v2.21 the rebuilt field degraded to func<int, int>
//and `func<int, int&> g = b.f;` rejected as incompatible.
static void test_func_key_cross_unit_bind()
{
    TEST(func_key_cross_unit_bind);
    BuildOutcome lib = buildSource("refsig_klib",
        "int bump(int& v) { v = v + 1; return v; }\n"
        "class Box {\n"
        "    public func<int, int&> f;\n"
        "    public Box() { f = bump; }\n"
        "}\n");
    CHECK(lib.ok, "lib build should succeed: " + lib.diagnostics);
    //Remove the producer SOURCE from the import dir — a sibling .n next
    //to the .ncu makes the source-inlining discovery layer inline the
    //library as a source TU instead of resolving the image, and the
    //in-module marks would mask the wire roundtrip this test pins.
    std::filesystem::remove(scratchDir() / "refsig_klib.n");
    BuildOutcome b = buildConsumer("refsig_kmain",
        "import refsig_klib;\n"
        "int main() {\n"
        "    refsig_klib.Box b = refsig_klib.Box();\n"
        "    func<int, int&> g = b.f;\n"
        "    int z = 30;\n"
        "    if (g(z) != 31) { return 2; }\n"
        "    if (z != 31) { return 3; }\n"
        "    return 7;\n"
        "}\n");
    CHECK(b.ok, "the marked func key should bind across the unit border: "
        + b.diagnostics);
    PASS();
}

//The format change (per-element flag bytes in the RTK_Func arm) breaks
//the v2.20 layout, so the floor must reject a one-minor-old module by
//name — the generic floor tests pin v2.0; this one pins the transition.
static void test_loader_rejects_v2_20()
{
    TEST(loader_rejects_v2_20);
    BuildOutcome b = buildSource("refver",
        "int main() { return 0; }\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    const auto modPath = scratchDir() / "refver.ncu";

    std::vector<char> bytes;
    {
        std::ifstream in(modPath, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    CHECK(bytes.size() >= 12, "module file should have a full header");
    CHECK(static_cast<uint8_t>(bytes[10])
            == static_cast<uint8_t>(NCU_FORMAT_MINOR),
        "fresh module should be stamped the current NCU format");
    bytes[10] = static_cast<char>(NCU_FORMAT_MINOR - 1);
    const auto oldPath = scratchDir() / "refver_v220.ncu";
    {
        std::ofstream out(oldPath, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    bool threw = false;
    std::string what;
    try {
        ModuleLoader::Load(oldPath.string());
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(threw, "loader must reject a v2.20 module (floor is current)");
    CHECK(what.find("is outdated; recompile with current ncc")
              != std::string::npos,
        "rejection should hit the floor path, got: " + what);
    PASS();
}

void run_ncu_ref_sig_tests()
{
    test_ref_formal_flags_roundtrip();
    test_func_sig_elem_flags_roundtrip();
    test_func_key_cross_unit_bind();
    test_loader_rejects_v2_20();
}
