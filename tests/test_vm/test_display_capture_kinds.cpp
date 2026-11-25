/*---
    test_display_capture_kinds.cpp — display capture-field kind pins.

    Unit coverage of the 0.13.10 Task 8 field-kind split: a display
    class's &-capture field declares RTK_ByrefBase (the label the class
    mark phase routes conservatively — the (base, cell) pair's base
    half is the only heap edge), while a promoted capture keeps
    RTK_Promoted and a plain value capture keeps its declared runtime
    kind. Before the fix the capture flavor never reached the field's
    kind at all (the synthesis stamped NF_Captured on the captured
    descriptor only, so InstanceFieldSlotKind's flavor arms never fired
    and the & fields declared the plain target kinds — int& as
    RTK_Int32, Counter& as RTK_Struct), leaving the pair's base record
    unmarked once the lambda escaped. The survival runs are
    tests/e2e/lambda_capture_gc_survive.n.

    Byte pins over real-compiled modules through the closure loader —
    a hand-built round-trip pin is redundant here: the real compiler
    produces the bytes and NcuLoader::LoadClosure reads them back, so
    a future loader-side kind validation fails these pins directly
    (the RTK_TypeParam/RTK_Interface files needed the hand-built form
    only because their shapes could not compile yet).
---*/
#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/compiler/CastInfo.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/core/CompiledModule.h"
#include "nlang/core/CompiledModuleRecords.h"
#include "NcuLoader.h"
#include "NcuLinker.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifndef STDLIB_DIR
#define STDLIB_DIR ""
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
#define REQUIRE(cond) \
    do { if (!(cond)) { FAIL("precondition: " #cond); return; } } while(0)

static std::filesystem::path scratchDir()
{
    static const auto dir = std::filesystem::temp_directory_path()
        / "nlang_test_display_capture_kinds";
    std::filesystem::create_directories(dir);
    return dir;
}

//Compile <tag>.n through the real builder and return the linked module
//(byte pins only — nothing executes; the survival shapes run in the
//e2e suite where a reclaimed-record crash can fail the process without
//killing the test binary).
static std::unique_ptr<CompiledModule> buildModule(const std::string& tag,
                                                   const std::string& source)
{
    const auto dir = scratchDir();
    const auto nPath = dir / (tag + ".n");
    const auto modPath = dir / (tag + ".ncu");
    std::filesystem::remove(modPath);
    {
        std::ofstream file(nPath, std::ios::binary);
        file << source;
    }
    BuildParams params;
    params.m_SourceFiles.push_back(nPath.string());
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    if (!builder.BuildArtifacts()) {
        for (auto it = logger.cbegin(); it != logger.cend(); ++it)
            std::cerr << "    " << (*it)->Message() << "\n";
        return nullptr;
    }
    NcuLoader::Options loaderOpts;
    loaderOpts.searchDirs = {dir.string(), STDLIB_DIR};
    NcuLoader::Result loaded =
        NcuLoader::LoadClosure(modPath.string(), loaderOpts);
    return std::make_unique<CompiledModule>(
        NcuLinker::Link(std::move(loaded.units), loaded.entryKey));
}

//The (sole) display class of a one-lambda module, by the $disp_N name
//shape (the key may carry package qualification — match the leaf).
static const CompiledClass* displayClassOf(const CompiledModule& mod)
{
    const CompiledClass* found = nullptr;
    for (const auto& cc : mod.classes)
        if (cc.name.find("$disp_") != std::string::npos) {
            if (found)
                return nullptr;  //ambiguous — let the caller REQUIRE fail
            found = &cc;
        }
    return found;
}

//One display field's declared kind, by field name.
static bool fieldKindOf(const CompiledClass& cc, const std::string& name,
                        uint16_t& kindOut)
{
    for (size_t i = 0; i < cc.fieldNames.size(); ++i)
        if (i < cc.fieldTypeKinds.size() && cc.fieldNames[i] == name) {
            kindOut = cc.fieldTypeKinds[i];
            return true;
        }
    return false;
}

static std::string kindMsg(uint16_t got)
{
    return " — got " + std::to_string(int(got));
}

//A &-captured aggregate local: the display field holds the
//(base, cell) pair — the struct record handle is the only heap edge,
//so the field declares the base-handle label.
static const char kByrefStructSrc[] =
    "struct Counter {\n"
    "    public int n;\n"
    "    public Counter(int v) { n = v; }\n"
    "}\n"
    "int main() {\n"
    "    Counter c = Counter(41);\n"
    "    Counter& rc = c;\n"
    "    func<int> f = () -> rc.n + 1;\n"
    "    return f();\n"
    "}\n";

//A &-captured scalar local plus its promoted target: one display
//hosts both flavors — the box field (x) and the pair field (r).
static const char kPromotedAndByrefSrc[] =
    "int main() {\n"
    "    int x = 7;\n"
    "    int& r = x;\n"
    "    func<int> g = () -> x + r;\n"
    "    return g();\n"
    "}\n";

//A plain value capture stays its declared runtime kind — only
//&-flavored capture fields take the base-handle label.
static const char kValueSrc[] =
    "int main() {\n"
    "    int n = 3;\n"
    "    func<int> f = () -> n + 1;\n"
    "    return f();\n"
    "}\n";

//The wire value: pinned above the interface kind so a future kind
//cannot silently collide (same discipline as the RTK_TypeParam=20 and
//RTK_Interface=25 pins).
static void testByrefBaseKindValuePinned()
{
    TEST(byrefBaseKindValuePinned);
    CHECK(RTK_ByrefBase == uint8_t(26),
          "RTK_ByrefBase must be 26 (got "
          + std::to_string(int(RTK_ByrefBase)) + ")");
    PASS();
}

//Aggregate & capture: the display field declares RTK_ByrefBase —
//before the fix it declared the plain target kind (RTK_Struct), which
//the mark phase accepted only by accident of the base record's own
//family (a struct-local & target); every other target family dangled.
static void testByrefStructFieldDeclaresByrefBase()
{
    TEST(byrefStructFieldDeclaresByrefBase);
    auto mod = buildModule("disp_byref_struct", kByrefStructSrc);
    REQUIRE(mod);
    const CompiledClass* disp = displayClassOf(*mod);
    REQUIRE(disp);
    uint16_t kind = 0;
    REQUIRE(fieldKindOf(*disp, "rc", kind));
    CHECK(kind == RTK_ByrefBase,
          "struct & capture field kind must be RTK_ByrefBase "
          + kindMsg(kind));
    PASS();
}

//Scalar & capture rides the same base-handle label (the cell-addressed
//flavor matters only to the frame band's debug rendering; the field's
//one heap edge is the base record either way). Before the fix the
//field declared RTK_Int32 — the pair's base handle sat in a slot no
//mark route visits.
static void testByrefScalarFieldDeclaresByrefBase()
{
    TEST(byrefScalarFieldDeclaresByrefBase);
    auto mod = buildModule("disp_promoted_byref", kPromotedAndByrefSrc);
    REQUIRE(mod);
    const CompiledClass* disp = displayClassOf(*mod);
    REQUIRE(disp);
    uint16_t kind = 0;
    REQUIRE(fieldKindOf(*disp, "r", kind));
    CHECK(kind == RTK_ByrefBase,
          "scalar & capture field kind must be RTK_ByrefBase "
          + kindMsg(kind));
    //Boundary: the promoted capture in the same display keeps its own
    //kind (the box field is a distinct flavor with a distinct render).
    REQUIRE(fieldKindOf(*disp, "x", kind));
    CHECK(kind == RTK_Promoted,
          "promoted capture field kind must stay RTK_Promoted "
          + kindMsg(kind));
    PASS();
}

//Value capture boundary: a plain int capture keeps RTK_Int32 (inline
//scalar, no heap edge) — the label change is &-flavored only.
static void testValueCaptureStaysPlainKind()
{
    TEST(valueCaptureStaysPlainKind);
    auto mod = buildModule("disp_value_capture", kValueSrc);
    REQUIRE(mod);
    const CompiledClass* disp = displayClassOf(*mod);
    REQUIRE(disp);
    uint16_t kind = 0;
    REQUIRE(fieldKindOf(*disp, "n", kind));
    CHECK(kind == RTK_Int32,
          "value capture field kind must stay RTK_Int32 "
          + kindMsg(kind));
    PASS();
}

int main()
{
    //In-process host init (the ModuleBuilder path needs the runtime's
    //interned-string tables before the first CreateModule).
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();
    testByrefBaseKindValuePinned();
    testByrefStructFieldDeclaresByrefBase();
    testByrefScalarFieldDeclaresByrefBase();
    testValueCaptureStaysPlainKind();
    std::cerr << "\n" << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
