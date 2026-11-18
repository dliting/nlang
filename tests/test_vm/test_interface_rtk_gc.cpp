//v0.13.9 phase 3 Task 12 (D14): interface runtime-kind classification.
//An interface-typed storage location holds a class-instance heap handle,
//and a cross-module type descriptor names the interface itself. Two
//emission tiers carry those facts: the storage tier (locals tables,
//field kinds, signature leaves — RuntimeTypeKind files interface types
//as RTK_Class, the same representation truth as enum -> RTK_Int32) and
//the identity tier (TypeDesc channels and arrayTypes rows — a dedicated
//RTK_Interface kind indexed by the interfaces table, so cast rows and
//linker merges keep interface identity instead of colliding on a
//sentinel). Before the fix both throats degraded interface types to
//RTK_Int32 / RTK_NonSerialized, every GC root routing set skipped them,
//and live instances were swept mid-run (the pi_ac/pi_iarr/pi_iconst2
//repro family). These pins hold the emitted bytes; the survival runs
//are tests/e2e/interface_{local,field,constant,array}_gc_survives.n.
#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/compiler/CastInfo.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "VmBackend.h"
#include "VmExecutor.h"
#include "NcuLoader.h"
#include "NcuLinker.h"
#include "ModuleLoader.h"
#include "BytecodeEmitter.h"
#include "BytecodeOps.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/ScriptLocation.h>
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
        / "nlang_test_interface_rtk";
    std::filesystem::create_directories(dir);
    return dir;
}

//Compile <tag>.n through the real builder and return the linked module
//(byte pins only — nothing executes; the survival shapes run in the
//e2e suite where a CRT assert can fail a process without killing the
//test binary).
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

static const CompiledFunction* entryOf(const CompiledModule& mod)
{
    //Linked function names may carry package qualification — resolve
    //through the entry point (test_ref_ops's EntryFunction discipline).
    if (mod.entryPoint < 0
        || static_cast<size_t>(mod.entryPoint) >= mod.functions.size())
        return nullptr;
    return &mod.functions[static_cast<size_t>(mod.entryPoint)];
}

static const LocalDescriptor* localOf(const CompiledFunction& fn,
                                      const std::string& name)
{
    for (const auto& ld : fn.locals)
        if (ld.name == name)
            return &ld;
    return nullptr;
}

//Row keys are LinkKeyOf spellings — bare for top-level declarations,
//package-qualified otherwise. Match the declared name either way.
static bool keyNames(const std::string& key, const std::string& name)
{
    return key == name
        || (key.size() > name.size() + 1
            && key.compare(key.size() - name.size(), name.size(), name) == 0
            && key[key.size() - name.size() - 1] == '.');
}

static int interfaceRowOf(const CompiledModule& mod, const std::string& name)
{
    for (size_t i = 0; i < mod.interfaces.size(); ++i)
        if (keyNames(mod.interfaces[i].name, name))
            return static_cast<int>(i);
    return -1;
}

//The identity tier's wire value: pinned above the byref/promoted block
//so a future scalar or storage kind cannot silently collide (same
//discipline as the RTK_TypeParam=20 pin).
static void testInterfaceKindValuePinned()
{
    TEST(interfaceKindValuePinned);
    CHECK(RTK_Interface == uint8_t(25),
          "RTK_Interface must be 25 (got "
          + std::to_string(int(RTK_Interface)) + ")");
    PASS();
}

//Direct classification pin: a hand-built interface declaration node
//classifies as the storage tier's class reference (the static entry the
//locals/fields/signature emission consults).
static void testRuntimeTypeKindDirectPin()
{
    TEST(runtimeTypeKindDirectPin);
    SnInterfaceDecl iface(new std::string("Inc"), nullptr,
        ScriptLocation());
    CHECK(VmBackend::RuntimeTypeKind(&iface) == RTK_Class,
          "RuntimeTypeKind(interface) must be RTK_Class");
    PASS();
}

//The full emission surface in one real compilation: an interface local,
//an interface-typed field, an interface-typed runtime constant and an
//interface-element array row.
static const char kInterfaceSrc[] =
    "interface Inc { void set(int v); int get(); }\n"
    "class A implements Inc {\n"
    "    public int v = 0;\n"
    "    public void set(int v) { this.v = v; }\n"
    "    public int get() { return this.v; }\n"
    "}\n"
    "interface Holder {\n"
    "    public static const Inc C = A();\n"
    "    int use();\n"
    "}\n"
    "class B implements Holder {\n"
    "    public Inc f;\n"
    "    public int use() { return 0; }\n"
    "}\n"
    "int main() {\n"
    "    Inc x = A();\n"
    "    Inc[] arr = Inc[2];\n"
    "    arr[0] = A();\n"
    "    x.set(3);\n"
    "    return x.get();\n"
    "}\n";

//Storage tier: the interface local's descriptor kind routes to the
//single heap-index GC class (the band mask packs EvalSlotClassOfKind's
//answer; ESC_None here was the swept-live-instance crash).
static void testInterfaceLocalClassifiesAsHeapRoot()
{
    TEST(interfaceLocalClassifiesAsHeapRoot);
    auto mod = buildModule("interface_rtk_local", kInterfaceSrc);
    REQUIRE(mod);
    const CompiledFunction* fn = entryOf(*mod);
    REQUIRE(fn);
    const LocalDescriptor* x = localOf(*fn, "x");
    REQUIRE(x);
    CHECK(VmBackend::EvalSlotClassOfKind(x->typeKind) == ESC_HeapIndex,
          "interface local's slot class must be ESC_HeapIndex (kind="
          + std::to_string(int(x->typeKind)) + ")");
    PASS();
}

//Storage tier on fields: DeclaredFieldAcceptsRuntime accepts a class
//record only under a reference-kind byte — the interface field must
//carry RTK_Class, not the scalar degrade.
static void testInterfaceFieldKindIsClassRef()
{
    TEST(interfaceFieldKindIsClassRef);
    auto mod = buildModule("interface_rtk_field", kInterfaceSrc);
    REQUIRE(mod);
    const CompiledClass* b = nullptr;
    for (const auto& cc : mod->classes)
        if (keyNames(cc.name, "B"))
            b = &cc;
    REQUIRE(b);
    bool found = false;
    for (size_t i = 0; i < b->fieldTypeKinds.size(); ++i)
        if (b->fieldNames.size() > i && b->fieldNames[i] == "f") {
            found = true;
            CHECK(b->fieldTypeKinds[i] == RTK_Class,
                  "interface field kind must be RTK_Class (got "
                  + std::to_string(int(b->fieldTypeKinds[i])) + ")");
        }
    CHECK(found, "field f present in B's record");
    PASS();
}

//Identity tier: the runtime constant's static slot names the interface
//itself — kind RTK_Interface with the interfaces-table row as index
//(MarkStaticSlotRoots's reference arm; RTK_NonSerialized fell out of
//every routing set and the constant's instance was swept mid-churn).
static void testInterfaceConstantDescNamesInterface()
{
    TEST(interfaceConstantDescNamesInterface);
    auto mod = buildModule("interface_rtk_const", kInterfaceSrc);
    REQUIRE(mod);
    const CompiledStaticSlot* c = nullptr;
    for (const auto& slot : mod->staticSlots)
        if (slot.name == "C" && slot.ownerKind == 1)
            c = &slot;
    REQUIRE(c);
    CHECK(c->type.kind == RTK_Interface,
          "constant slot kind must be RTK_Interface (got "
          + std::to_string(int(c->type.kind)) + ")");
    const int incRow = interfaceRowOf(*mod, "Inc");
    REQUIRE(incRow >= 0);
    CHECK(c->type.typeIdx == static_cast<uint16_t>(incRow),
          "constant slot typeIdx must name Inc's interfaces row");
    PASS();
}

//Identity tier on arrays: the row keeps interface identity — the cast
//row key (elemKind, elemTypeIdx, rank) distinguishes Inc[] from both
//int[] and any other interface's array (a shared sentinel row made
//`I[] as J[]` pass; the scalar degrade made `Inc[] as int[]` pass).
static void testInterfaceArrayRowKeepsIdentity()
{
    TEST(interfaceArrayRowKeepsIdentity);
    auto mod = buildModule("interface_rtk_array", kInterfaceSrc);
    REQUIRE(mod);
    const int incRow = interfaceRowOf(*mod, "Inc");
    REQUIRE(incRow >= 0);
    bool found = false;
    for (const auto& at : mod->arrayTypes)
        if (at.elemKind == RTK_Interface) {
            found = true;
            CHECK(at.elemTypeIdx == static_cast<uint16_t>(incRow),
                  "interface array row must name Inc's row (got "
                  + std::to_string(int(at.elemTypeIdx)) + ")");
            CHECK(at.rank == 1, "Inc[] is rank 1");
        }
    CHECK(found, "an RTK_Interface array row exists");
    PASS();
}

//Serialization pin: both identity-tier channels move the kind as a raw
//byte — the loader must round-trip RTK_Interface (no whitelist rejects
//it) and ValidateTypeDescIndices must accept the interfaces-row index.
static void testInterfaceKindSurvivesNcuRoundTrip()
{
    TEST(interfaceKindSurvivesNcuRoundTrip);
    CompiledModule mod;
    mod.name = "interface_roundtrip";
    CompiledFunction func;
    func.name = "main";
    func.localsSize = 8;
    func.paramCount = 0;
    func.returnTypeKind = RTK_Void;
    BytecodeEmitter em;
    em.Emit(OpCode::OP_Return);
    func.bytecode = em.TakeBytes();
    func.evalAreaHandleMask.assign(1, 0);
    mod.functions.push_back(std::move(func));
    mod.interfaces.resize(1);
    mod.interfaces[0].name = "Inc";
    CompiledStaticSlot slot;
    slot.name = "C";
    slot.ownerKey = "Inc";
    slot.ownerKind = 1;
    slot.type.kind = RTK_Interface;
    slot.type.typeIdx = 0;
    mod.staticSlots.push_back(std::move(slot));

    const auto path = scratchDir() / "interface_rtk_roundtrip.ncu";
    {
        std::ofstream fs(path, std::ios::binary);
        CHECK(WriteCompiledModule(fs, mod, "main"),
              "WriteCompiledModule failed");
    }
    bool loadedOk = false;
    CompiledModule loaded;
    std::string loadError;
    try {
        loaded = ModuleLoader::Load(path.string());
        loadedOk = true;
    } catch (const std::exception& e) {
        loadError = e.what();
    }
    std::filesystem::remove(path);
    CHECK(loadedOk, "loader accepts RTK_Interface descriptors ("
          + loadError + ")");
    CHECK(loaded.staticSlots.size() == size_t(1),
          "static slot round-trips");
    CHECK(loaded.staticSlots[0].type.kind == RTK_Interface,
          "RTK_Interface kind survives the round trip");
    CHECK(loaded.staticSlots[0].type.typeIdx == 0,
          "interface row index survives the round trip");
    PASS();
}

int main()
{
    //In-process host init (the ModuleBuilder path needs the runtime's
    //interned-string tables before the first CreateModule).
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();
    testInterfaceKindValuePinned();
    testRuntimeTypeKindDirectPin();
    testInterfaceLocalClassifiesAsHeapRoot();
    testInterfaceFieldKindIsClassRef();
    testInterfaceConstantDescNamesInterface();
    testInterfaceArrayRowKeepsIdentity();
    testInterfaceKindSurvivesNcuRoundTrip();
    std::cerr << "\n" << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
