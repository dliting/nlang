// v0.13.1 Task 4: object-side instantiation descriptors (D1). The
// executor carries one GenericInstDescriptor* per heap slot (a parallel
// array like m_slotKinds); OP_New's descIdx operand resolves through the
// module's descriptor table (interned from the canonical name at run
// start) and AllocClassOnHeap writes the pointer. Real programs compile
// and execute in-process (the test_array_view_record harness pattern,
// including its static-init contract).
#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/compiler/CastInfo.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "VmExecutor.h"
#include "GenericInstDescriptor.h"
#include "NcuLoader.h"
#include "NcuLinker.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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
//Like CHECK but for shape preconditions whose failure would make the
//following assertions meaningless.
#define REQUIRE(cond) \
    do { if (!(cond)) { FAIL("precondition: " #cond); return; } } while(0)

static std::filesystem::path scratchDir()
{
    static const auto dir = std::filesystem::temp_directory_path()
        / "nlang_test_generic_runtime_info";
    std::filesystem::create_directories(dir);
    return dir;
}

//Compile <tag>.n, load the closure, link, execute, keep the executor
//alive so the heap (and the descriptor parallel array) stays inspectable.
struct ExecResult {
    std::unique_ptr<VmExecutor> exec;
    std::unique_ptr<CompiledModule> mod;
    int exitCode = -1;
    bool built = false;
};

static ExecResult runProgram(const std::string& tag,
                             const std::string& source,
                             size_t gcStressThreshold = 0)
{
    ExecResult out;
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
        return out;
    }
    out.built = true;
    NcuLoader::Options loaderOpts;
    loaderOpts.searchDirs = {dir.string(), STDLIB_DIR};
    NcuLoader::Result loaded =
        NcuLoader::LoadClosure(modPath.string(), loaderOpts);
    out.mod = std::make_unique<CompiledModule>(
        NcuLinker::Link(std::move(loaded.units), loaded.entryKey));
    out.exec = std::make_unique<VmExecutor>();
    if (gcStressThreshold != 0)
        out.exec->SetGcStressThresholds(gcStressThreshold);
    out.exitCode = out.exec->Execute(*out.mod);
    return out;
}

//Heap indices whose parallel-array slot carries a descriptor.
static std::vector<int32_t> heapIndicesWithDescriptor(VmExecutor& exec)
{
    std::vector<int32_t> hits;
    const size_t slots = exec.HeapSlotCountForTests();
    for (int32_t i = 1; i < static_cast<int32_t>(slots); ++i)
        if (exec.InstDescriptorOf(i))
            hits.push_back(i);
    return hits;
}

//A concrete Box<int> construction writes its descriptor on the object
//(the spelling round-trips through the .ncu descriptor table and the
//load-time interning).
static void testBoxIntRidesConstruction()
{
    TEST(boxIntRidesConstruction);
    {
        ExecResult r = runProgram("gri_box_int",
            "class Box<T> {\n"
            "    public T value;\n"
            "    public Box(T v) { value = v; }\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box(41);\n"
            "    return b.value + 1;\n"
            "}\n");
        REQUIRE(r.built);
        CHECK(r.exitCode == 42, "program ran to completion");
        const auto hits = heapIndicesWithDescriptor(*r.exec);
        CHECK(hits.size() == 1, "exactly one descriptor-carrying object");
        if (hits.size() == 1) {
            const GenericInstDescriptor* desc =
                r.exec->InstDescriptorOf(hits[0]);
            CHECK(desc->Spelling() == "gri_box_int.Box<int>",
                  "spelling is the module-path-qualified link identity");
            CHECK(desc->TemplateName() == "gri_box_int.Box",
                  "template name is the qualified link identity");
            CHECK(desc->TypeArgs().size() == 1, "one type argument");
        }
    }
    PASS();
}

//Non-instantiation objects carry no descriptor (0xFFFF descIdx).
static void testPlainObjectCarriesNone()
{
    TEST(plainObjectCarriesNone);
    {
        ExecResult r = runProgram("gri_plain",
            "class Plain {\n"
            "    public int p;\n"
            "    public Plain(int v) { p = v; }\n"
            "}\n"
            "int main() {\n"
            "    Plain q = Plain(5);\n"
            "    return q.p;\n"
            "}\n");
        REQUIRE(r.built);
        CHECK(r.exitCode == 5, "program ran to completion");
        CHECK(heapIndicesWithDescriptor(*r.exec).empty(),
              "no descriptor on a non-generic object");
    }
    PASS();
}

//The probe is total: out-of-range and sentinel handles yield nullptr
//instead of crashing.
static void testProbeBoundsAreNull()
{
    TEST(probeBoundsAreNull);
    {
        ExecResult r = runProgram("gri_bounds",
            "class Box<T> {\n"
            "    public T value;\n"
            "    public Box(T v) { value = v; }\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box(41);\n"
            "    return 0;\n"
            "}\n");
        REQUIRE(r.built);
        CHECK(r.exec->InstDescriptorOf(0) == nullptr,
              "sentinel slot 0 carries no descriptor");
        CHECK(r.exec->InstDescriptorOf(-1) == nullptr,
              "negative handle yields nullptr");
        CHECK(r.exec->InstDescriptorOf(
                  static_cast<int32_t>(r.exec->HeapSlotCountForTests()) + 8)
                  == nullptr,
              "past-end handle yields nullptr");
    }
    PASS();
}

//A template-body construction composes its descriptor at run time from
//the receiver's own arguments (D3 compositions): Hold<int> driving Box(x)
//inside its ctor yields the qualified "gri_composition.Box<int>" alongside
//the static "gri_composition.Hold<int>".
static void testCompositionComposesFromReceiver()
{
    TEST(compositionComposesFromReceiver);
    {
        ExecResult r = runProgram("gri_composition",
            "class Box<T> {\n"
            "    public T value;\n"
            "    public Box(T v) { value = v; }\n"
            "}\n"
            "class Hold<T> {\n"
            "    public Box<T> inner;\n"
            "    public Hold(T x) { inner = Box(x); }\n"
            "}\n"
            "int main() {\n"
            "    Hold<int> h = Hold(5);\n"
            "    return h.inner.value;\n"
            "}\n");
        REQUIRE(r.built);
        CHECK(r.exitCode == 5, "program ran to completion");
        const auto hits = heapIndicesWithDescriptor(*r.exec);
        CHECK(hits.size() == 2, "Hold and its inner Box carry descriptors");
        bool sawHold = false, sawBox = false;
        for (int32_t idx : hits) {
            const std::string& spelling =
                r.exec->InstDescriptorOf(idx)->Spelling();
            sawHold = sawHold || spelling == "gri_composition.Hold<int>";
            sawBox = sawBox || spelling == "gri_composition.Box<int>";
        }
        CHECK(sawHold, "receiver Hold<int> rides the static row");
        CHECK(sawBox, "inner Box composed as Box<int> at run time");
    }
    PASS();
}

//Nested composition: a composition argument that is itself a composition-driven
//construction (Box<Box<T>> staged through a typed local).
static void testNestedCompositionComposes()
{
    TEST(nestedCompositionComposes);
    {
        ExecResult r = runProgram("gri_composition_nested",
            "class Box<T> {\n"
            "    public T value;\n"
            "    public Box(T v) { value = v; }\n"
            "}\n"
            "class Hold<T> {\n"
            "    public Box<Box<T>> inner;\n"
            "    public Hold(T x) {\n"
            "        Box<T> one = Box(x);\n"
            "        Box<Box<T>> b = Box(one);\n"
            "        inner = b;\n"
            "    }\n"
            "}\n"
            "int main() {\n"
            "    Hold<int> h = Hold(5);\n"
            "    return h.inner.value.value;\n"
            "}\n");
        REQUIRE(r.built);
        CHECK(r.exitCode == 5, "program ran to completion");
        const auto hits = heapIndicesWithDescriptor(*r.exec);
        CHECK(hits.size() == 3, "Hold + Box<int> + Box<Box<int>>");
        bool sawNested = false;
        for (int32_t idx : hits)
            if (r.exec->InstDescriptorOf(idx)->Spelling()
                    == "gri_composition_nested.Box<"
                       "gri_composition_nested.Box<int>>")
                sawNested = true;
        CHECK(sawNested, "nested composition composed with the qualified "
              "link identity");
    }
    PASS();
}

//Static template members read the frame descriptor cell (0.13.1a): the
//caller-forward channel fills it, so a T-argument construction there
//composes against the caller's instantiation — the former rejection is
//gone, and the built object rides the composed spelling.
static void testStaticBodyComposesFromForwardedContext()
{
    TEST(staticBodyComposesFromForwardedContext);
    {
        ExecResult r = runProgram("gri_static_ctor_fwd",
            "class Box<T> {\n"
            "    public T value;\n"
            "}\n"
            "class Hold<T> {\n"
            "    public static Box<T> make() {\n"
            "        Box<T> b = Box();\n"
            "        return b;\n"
            "    }\n"
            "    public Box<T> viaInstance() { return make(); }\n"
            "}\n"
            "int main() {\n"
            "    Hold<int> h = Hold();\n"
            "    Box<int> b = h.viaInstance();\n"
            "    return 7;\n"
            "}\n");
        REQUIRE(r.built);
        CHECK(r.exitCode == 7, "program ran to completion");
        const auto hits = heapIndicesWithDescriptor(*r.exec);
        CHECK(hits.size() == 2, "Hold and the static-made Box carry "
              "descriptors");
        bool sawBox = false;
        for (int32_t idx : hits)
            if (r.exec->InstDescriptorOf(idx)->Spelling()
                    == "gri_static_ctor_fwd.Box<int>")
                sawBox = true;
        CHECK(sawBox, "the static-body construction composed with the "
              "qualified link identity");
    }
    PASS();
}

//0.13.1a: a bare static call inside a generic class body forwards the
//executing frame's descriptor cell — the callee's typeName(T) reads the
//caller's instantiation, and two instantiations of the same row take
//the same shared body through different cells.
static void testStaticFrameCarriesForwardedContext()
{
    TEST(staticFrameCarriesForwardedContext);
    {
        ExecResult r = runProgram("gri_static_fwd_cell",
            "class Hold<T> {\n"
            "    public static string name() { return typeName(T); }\n"
            "    public string viaInstance() { return name(); }\n"
            "}\n"
            "int main() {\n"
            "    Hold<int> h = Hold();\n"
            "    if (h.viaInstance() != \"int\") { return 3; }\n"
            "    Hold<string> s = Hold();\n"
            "    if (s.viaInstance() != \"string\") { return 4; }\n"
            "    return 0;\n"
            "}\n");
        REQUIRE(r.built);
        CHECK(r.exitCode == 0, "both instantiations read their own "
              "cell through the shared static body");
    }
    PASS();
}

//The D5 slot-recycling invariant, writer side: heap slots recycle across
//kinds, so EVERY allocation path must reset the descriptor carrier cell —
//a struct, box, or view landing on a slot that once held a generic object
//must not inherit its stamp. The stress threshold (4) drives collections
//while the churn loops recycle slots heavily: the Box dies early, then
//struct, boxed-value, and view allocations cycle through the freed slots.
static void testRecycledSlotsCarryNoDescriptor()
{
    TEST(recycledSlotsCarryNoDescriptor);
    {
        ExecResult r = runProgram("gri_recycled_slots",
            "class Box<T> {\n"
            "    public T value;\n"
            "    public Box(T v) { value = v; }\n"
            "}\n"
            "struct Pt {\n"
            "    int x;\n"
            "    int y;\n"
            "    public Pt(int x, int y) { this.x = x; this.y = y; }\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box(41);\n"
            "    b = null;\n"
            "    for (int i = 0; i < 50; i = i + 1) {\n"
            "        Pt p = Pt(i, i);\n"
            "    }\n"
            "    List<long> xs = {};\n"
            "    for (int i = 0; i < 30; i = i + 1) {\n"
            "        xs.add(1099511627776);\n"
            "    }\n"
            "    int[] arr = [1, 2, 3];\n"
            "    for (int i = 0; i < 20; i = i + 1) {\n"
            "        int[] v = arr[0:2];\n"
            "    }\n"
            "    return 0;\n"
            "}\n",
            /*gcStressThreshold=*/4);
        REQUIRE(r.built);
        CHECK(r.exitCode == 0, "program ran to completion");
        const auto hits = heapIndicesWithDescriptor(*r.exec);
        //Only class instances may carry a descriptor (the live List<long>
        //is one); a struct, box, or view slot holding one is a recycled
        //tenant's stamp. The dead Box<int>'s stamp must not survive
        //anywhere.
        bool foreignKind = false, sawDeadBox = false;
        for (int32_t idx : hits) {
            if (r.exec->HeapSlotKindForTests(idx) != RTK_Class)
                foreignKind = true;
            if (r.exec->InstDescriptorOf(idx)->Spelling()
                    == "gri_recycled_slots.Box<int>")
                sawDeadBox = true;
        }
        CHECK(!foreignKind,
              "no struct/box/view slot inherits a previous tenant's "
              "descriptor");
        CHECK(!sawDeadBox, "the dead Box<int>'s stamp did not survive");
    }
    PASS();
}

//Task 2 Step-1b pin (ledgered ruling): a null descriptor cell on a
//generic static's frame must raise the named runtime error — never a
//null deref. ncc cannot emit this shape (an under-specified generic
//call is a compile error, D5), so the bytecode is patched by hand:
//main's OP_CallStatic descIdx operand is zeroed to kInstDescNone and
//the executor defense must fire.
static void testNullDescCellRaisesNamedError()
{
    TEST(nullDescCellRaisesNamedError);
    const std::string src =
        "class Maker<T> {\n"
        "    public static int probe(T v) {\n"
        "        return typeName(T).length();\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    return Maker.probe(7);\n"
        "}\n";
    const auto dir = scratchDir();
    const auto nPath = dir / "gri_null_cell.n";
    const auto modPath = dir / "gri_null_cell.ncu";
    std::filesystem::remove(modPath);
    {
        std::ofstream file(nPath, std::ios::binary);
        file << src;
    }
    BuildParams params;
    params.m_SourceFiles.push_back(nPath.string());
    params.m_sOutputModule = "gri_null_cell";
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    CHECK(builder.BuildArtifacts(), "compile succeeded");
    NcuLoader::Options loaderOpts;
    loaderOpts.searchDirs = {dir.string(), STDLIB_DIR};
    NcuLoader::Result loaded =
        NcuLoader::LoadClosure(modPath.string(), loaderOpts);
    auto mod = std::make_unique<CompiledModule>(
        NcuLinker::Link(std::move(loaded.units), loaded.entryKey));

    //Locate main's OP_CallStatic and strip its descriptor operand. The
    //sanity conditions keep an operand byte that merely repeats the
    //opcode value from being patched (funcIdx/callParamBase stay small
    //and a real descIdx is a table row, never 0xFFFF).
    CompiledFunction& fn = mod->functions[
        static_cast<size_t>(mod->entryPoint)];
    const uint8_t op = static_cast<uint8_t>(OpCode::OP_CallStatic);
    bool patched = false;
    for (size_t pc = 0; pc + 7 <= fn.bytecode.size(); ++pc)
    {
        if (fn.bytecode[pc] != op || fn.bytecode[pc + 5] == 0xFF)
            continue;
        fn.bytecode[pc + 5] = 0xFF;
        fn.bytecode[pc + 6] = 0xFF;
        patched = true;
        break;
    }
    CHECK(patched, "main carries an OP_CallStatic with a real descIdx");

    VmExecutor exec;
    bool bNamedError = false;
    try {
        exec.Execute(*mod);
    } catch (const std::exception& e) {
        bNamedError = std::string(e.what())
            .find("generic operation without an instance context")
            != std::string::npos;
    }
    CHECK(bNamedError, "null cell raises the named runtime error");
    PASS();
}

//Task 6 Review-Focus-1 pin: the static→free chain fills the callee's
//frame context cell from a composition composed against the CALLER's cell.
//Under per-allocation collections (threshold 4) the interned descriptor
//pointers live in host memory and must survive every safepoint; the
//verdicts (exit-code protocol) plus the post-run heap invariants —
//descriptors only on class slots, both composition-composed constructions
//still carrying theirs — prove the fill order beats the safepoint.
static void testGenericFrameSlotUnderGcStress()
{
    TEST(genericFrameSlotUnderGcStress);
    {
        ExecResult r = runProgram("gri_gc_stress_chain",
            "class Hold<U> {\n"
            "    public U held;\n"
            "    public Hold(U v) { held = v; }\n"
            "}\n"
            "class Ctor<T> {\n"
            "    public static string probe(T v) {\n"
            "        return freeTag(v);\n"
            "    }\n"
            "    public static Hold<T> wrap(T v) {\n"
            "        return Hold<T>(v);\n"
            "    }\n"
            "}\n"
            "string freeTag<T>(T v) {\n"
            "    return typeName(T);\n"
            "}\n"
            "int main() {\n"
            "    if (Ctor<int>.probe(3) != \"int\") { return 3; }\n"
            "    if (Ctor<string>.probe(\"s\") != \"string\") { return 4; }\n"
            "    for (int i = 0; i < 40; i = i + 1) {\n"
            "        if (Ctor<long>.probe(1099511627776) != \"long\") { return 5; }\n"
            "    }\n"
            "    Hold<int> a = Ctor<int>.wrap(7);\n"
            "    Hold<string> b = Ctor<string>.wrap(\"w\");\n"
            "    if (a.held != 7) { return 6; }\n"
            "    if (b.held != \"w\") { return 7; }\n"
            "    return 0;\n"
            "}\n",
            /*gcStressThreshold=*/4);
        REQUIRE(r.built);
        CHECK(r.exitCode == 0, "every chain verdict survived the "
              "per-allocation collections");
        const auto hits = heapIndicesWithDescriptor(*r.exec);
        CHECK(hits.size() == 2, "exactly the two live constructions "
              "carry descriptors");
        bool foreignKind = false, sawInt = false, sawString = false;
        for (int32_t idx : hits) {
            if (r.exec->HeapSlotKindForTests(idx) != RTK_Class)
                foreignKind = true;
            const std::string spelling =
                r.exec->InstDescriptorOf(idx)->Spelling();
            sawInt = sawInt || spelling == "gri_gc_stress_chain.Hold<int>";
            sawString = sawString
                || spelling == "gri_gc_stress_chain.Hold<string>";
        }
        CHECK(!foreignKind,
              "descriptor slots stayed kind-restricted under stress");
        CHECK(sawInt && sawString, "both composition-composed constructions "
              "kept their descriptors across the collections");
    }
    PASS();
}

int main()
{
    //Static tables before any compiler work (the test_array_view_record
    //contract): the IdString intern tables and the cast table have no
    //lazy guards — in-process builds dereference both.
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();

    std::cerr << "=== generic runtime info tests ===\n";
    try {
        testBoxIntRidesConstruction();
        testPlainObjectCarriesNone();
        testProbeBoundsAreNull();
        testCompositionComposesFromReceiver();
        testNestedCompositionComposes();
        testStaticBodyComposesFromForwardedContext();
        testStaticFrameCarriesForwardedContext();
        testRecycledSlotsCarryNoDescriptor();
        testGenericFrameSlotUnderGcStress();
        testNullDescCellRaisesNamedError();
    } catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }

    std::error_code ec;
    std::filesystem::remove_all(scratchDir(), ec);

    std::cerr << "\n=== Results: " << g_pass << " passed "
              << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
