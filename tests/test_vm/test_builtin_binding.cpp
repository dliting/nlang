/*---
    test_builtin_binding.cpp — builtin-decls phase-2 binding-table
    consistency guard: every declared native member of the four generic
    prelude classes (List/Dict/Channel/Future) must carry a stamped
    intrinsic record after the build — a missing binding row would fall
    to the host-native channel at run time ("no native binding"). Also
    pins the two-ctor Channel surface, the ctor-less Future, and the
    zero-arg channel ctor's contract of never reading the unstaged
    capacity slot.
---*/
#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/CastInfo.h"
#include "nlang/compiler/Logger.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "nlang/vm/Intrinsics.h"
#include "VmBackend.h"
#include "builder/ModuleRegistry.h"
#include "VmExecutor.h"
#include "IHostIo.h"
#include "NcuLoader.h"
#include "NcuLinker.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

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

static std::filesystem::path scratchDir()
{
    static const auto dir = std::filesystem::temp_directory_path()
        / "nlang_test_builtin_binding";
    std::filesystem::create_directories(dir);
    return dir;
}

//Compile + link one source (test_stdlib_concurrency.cpp harness shape);
//throws std::runtime_error with diagnostics on build failure — a
//failure here is a test bug, not behavior under test.
static CompiledModule compileAndLink(const std::string& tag,
                                     const std::string& source)
{
    const auto dir = scratchDir();
    const auto nPath = dir / (tag + ".n");
    const auto modPath = dir / (tag + ".ncu");
    std::filesystem::remove(modPath);
    {
        std::ofstream out(nPath, std::ios::binary);
        out << source;
    }
    BuildParams params;
    params.m_SourceFiles.push_back(nPath.string());
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    if (!builder.BuildArtifacts() || !std::filesystem::exists(modPath)) {
        std::string diag;
        for (auto it = logger.cbegin(); it != logger.cend(); ++it)
            diag += (*it)->Message() + "\n";
        throw std::runtime_error("build failed:\n" + diag);
    }
    NcuLoader::Options loaderOpts;
    loaderOpts.searchDirs = {dir.string(), STDLIB_DIR};
    NcuLoader::Result loaded =
        NcuLoader::LoadClosure(modPath.string(), loaderOpts);
    return NcuLinker::Link(std::move(loaded.units), loaded.entryKey);
}

//Execute a linked module on a single carrier with captured host output.
static int executeModule(CompiledModule& mod, std::string& rOut)
{
    class CaptureIo : public IHostIo {
    public:
        void OnOutput(std::string_view text) override { m_out += text; }
        std::string m_out;
    };
    VmExecutor exec;
    CaptureIo io;
    exec.SetHostIo(&io);
    exec.SetCarrierThreadCount(1);
    const int rc = exec.Execute(mod);
    rOut = io.m_out;
    return rc;
}

//Compile + link + execute one source; returns main()'s value.
static int executeSource(const std::string& tag,
                         const std::string& source, std::string& rOut)
{
    CompiledModule mod = compileAndLink(tag, source);
    return executeModule(mod, rOut);
}

static const CompiledClass* findClass(const CompiledModule& mod,
                                      const char* name)
{
    const int idx = mod.FindClass(name);
    return idx >= 0 ? &mod.classes[static_cast<size_t>(idx)] : nullptr;
}

//Instantiates all four classes and calls at least one member of each —
//instantiation is what registers the declared records into the module.
static const char* const kSurfaceProbe = R"NLANG(import concurrent;
import thread;
int main() {
    List<int> l = [1, 2];
    l.add(3);
    if (l.get(2) != 3 || l.length() != 3 || l.indexOf(2) != 1
        || !l.contains(1)) { return 1; }
    l.set(0, 10);
    l.removeAt(0);
    if (l[0] != 2) { return 2; }
    l.clear();
    Dict<string, int> d = {};
    d.set("a", 1);
    if (!d.containsKey("a") || d.get("a") != 1 || d.count() != 1) { return 3; }
    List<string> ks = d.keys();
    if (d.remove("a") != 1 || ks.length() != 1) { return 4; }
    d.clear();
    Channel<int> c = Channel(2);
    c.send(5);
    if (c.recv() != 5) { return 5; }
    c.close();
    Promise<int> p = Promise();
    p.complete(7);
    Future<int> f = p.future();
    if (f.get() != 7 || !f.isDone()) { return 6; }
    return 0;
}
)NLANG";

static const char* const kChannelProbe = R"NLANG(import concurrent;
int main() {
    Channel<int> c = Channel(2);
    c.close();
    //Zero-arg construction: the bare invoke (the `new ...()` spelling is
    //not a NLang form — `new` has been an ordinary identifier since
    //0.9.0 D8; the init-list `= {}` twin is exercised by the e2e probe).
    Channel<int> u = Channel();
    u.close();
    return 0;
}
)NLANG";

static const char* const kFutureProbe = R"NLANG(import thread;
int main() {
    Promise<int> p = Promise();
    p.complete(1);
    Future<int> f = p.future();
    if (f.get() != 1 || !f.isDone()) { return 1; }
    return 0;
}
)NLANG";

//Every declared native member of the four classes must be bound to an
//intrinsic — a missing binding-table row leaves INTR_None on the record
//and only fails at run time ("no native binding"), so guard it here.
void test_declared_members_all_stamped()
{
    TEST(declared_members_all_stamped);
    CompiledModule mod = compileAndLink("binding_sweep", kSurfaceProbe);
    for (const char* name : {"List", "Dict", "Channel", "Future"})
    {
        const CompiledClass* pCls = findClass(mod, name);
        if (!pCls) { FAIL(std::string(name) + " record present in the module"); return; }
        size_t total = 0, stamped = 0;
        for (uint16_t fi : pCls->constructorIndices)
        {
            ++total;
            if (mod.functions[fi].intrinsicId != INTR_None) ++stamped;
        }
        for (uint16_t fi : pCls->methodIndices)
        {
            ++total;
            if (mod.functions[fi].intrinsicId != INTR_None) ++stamped;
        }
        if (total == 0) { FAIL(std::string(name) + " declares members"); return; }
        if (stamped != total)
        {
            FAIL(std::string(name) + ": every member carries an intrinsic id ("
                 + std::to_string(stamped) + "/" + std::to_string(total) + ")");
            return;
        }
    }
    PASS();
}

//The declared two-overload Channel surface: exactly one ctor record
//without a capacity formal (paramCount 1 = this only) and exactly one
//with it (paramCount 2 = this + capacity).
void test_channel_has_two_ctors()
{
    TEST(channel_has_two_ctors);
    CompiledModule mod = compileAndLink("binding_channel", kChannelProbe);
    const CompiledClass* pCls = findClass(mod, "Channel");
    if (!pCls) { FAIL("Channel record present"); return; }
    CHECK(pCls->constructorIndices.size() == 2,
          "Channel declares exactly two ctors (got "
          + std::to_string(pCls->constructorIndices.size()) + ")");
    bool bThisOnly = false, bWithCapacity = false;
    for (uint16_t fi : pCls->constructorIndices)
    {
        const uint16_t params = mod.functions[fi].paramCount;
        if (params == 1) bThisOnly = true;
        if (params == 2) bWithCapacity = true;
    }
    CHECK(bThisOnly && bWithCapacity,
          "one this-only ctor and one this+capacity ctor");
    PASS();
}

//Future is a read handle minted by thread.spawn/promise.future() — no
//user-callable constructor may exist on the record.
void test_future_has_no_ctor()
{
    TEST(future_has_no_ctor);
    CompiledModule mod = compileAndLink("binding_future", kFutureProbe);
    const CompiledClass* pCls = findClass(mod, "Future");
    if (!pCls) { FAIL("Future record present"); return; }
    CHECK(pCls->constructorIndices.empty(),
          "Future declares no constructor");
    PASS();
}

//The zero-arg channel intrinsic must never read the unstaged capacity
//slot (scratch garbage) — a call with junk in slot 1 still constructs.
void test_unbounded_ctor_does_not_read_capacity()
{
    TEST(unbounded_ctor_does_not_read_capacity);
    std::string out;
    const int rc = executeSource("binding_unbounded",
        "import concurrent;\n"
        "int main() {\n"
        "    Channel<int> c = Channel();\n"
        "    c.send(5);\n"
        "    if (c.recv() != 5) { return 1; }\n"
        "    return 0;\n"
        "}\n", out);
    CHECK(rc == 0,
          "zero-arg channel ctor runs without touching the unstaged slot (rc="
          + std::to_string(rc) + ")");
    PASS();
}

static void runGuarded(void (*fn)())
{
    try {
        fn();
    } catch (const std::exception& e) {
        FAIL(std::string("exception: ") + e.what());
    }
}

//D11: the successor "is the builtin declaration" predicate. While the
//classes still live in the prelude both arms read through arm 1; the
//NO_OWNER shape must read false today and after the move. (Prelude-
//owned and stamped-package shapes are pinned in the flip task via
//compileAndLink sources — a fresh registry has no modules, so
//ModulePathOf bounds are not exercised here.)
static void test_builtin_handle_decl_predicate()
{
    TEST(test_builtin_handle_decl_predicate);
    ModuleRegistry registry;   //fresh registry: no units, no prelude
    CHECK(!IsBuiltinHandleDeclName(registry, ModuleRegistry::NO_OWNER,
        "Mutex"), "NO_OWNER must not certify");
    PASS();
}

//D8: bare-or-unique-suffix class lookup. A package-qualified row
//("pkg.Thing") must answer to the bare name exactly once; two such
//rows must refuse (-1) rather than pick one. Synthetic module — the
//lookup is pure table walking, no compiler involved.
static void test_find_class_bare_or_unique_suffix()
{
    TEST(test_find_class_bare_or_unique_suffix);
    CompiledModule m;
    m.classes.push_back({"Thread"});        //exact bare hit wins first
    m.classes.push_back({"concurrent.Mutex"});
    m.classes.push_back({"main.Box"});
    CHECK(m.FindClassBareOrUniqueSuffix("Thread") == 0,
        "exact bare key must win");
    CHECK(m.FindClassBareOrUniqueSuffix("Mutex") == 1,
        "unique package-qualified suffix must hit");
    CHECK(m.FindClassBareOrUniqueSuffix("Box") == 2,
        "main-package suffix must hit");
    CHECK(m.FindClassBareOrUniqueSuffix("Nope") == -1,
        "zero hits must miss");
    m.classes.push_back({"other.Mutex"});
    CHECK(m.FindClassBareOrUniqueSuffix("Mutex") == -1,
        "two suffix hits must refuse");
    PASS();
}

//D10: reverse lookup over the binding rows — which stamped package
//declares this bare name? Pre-move truth (these expectations flip to
//concurrent/thread in the move task).
static void test_binding_package_for_name()
{
    TEST(test_binding_package_for_name);
    std::string pkg;
    CHECK(BindingPackageForName("Mutex", pkg) && pkg == "concurrent",
        "Mutex ctor row must resolve to the moved package");
    CHECK(BindingPackageForName("Semaphore", pkg) && pkg == "concurrent",
        "the /1 ctor-key form must resolve to the moved package");
    CHECK(BindingPackageForName("Channel", pkg) && pkg == "concurrent",
        "the zero-formal ctor key must resolve too");
    CHECK(BindingPackageForName("Promise", pkg) && pkg == "thread",
        "the Promise ctor row must resolve to thread");
    CHECK(!BindingPackageForName("Thread", pkg),
        "Thread is minted by spawn with no ctor row - no package answer");
    CHECK(BindingPackageForName("spawn", pkg) && pkg == "thread",
        "qualified free-function form must resolve");
    CHECK(!BindingPackageForName("lock", pkg),
        "a bare method segment must NOT resolve (precision: the note "
        "fires on the class name the user wrote, not on method names)");
    CHECK(!BindingPackageForName("NotAClass", pkg),
        "unknown name must not resolve");
    PASS();
}

//Post-flip: a consumer of the moved classes links the package units,
//and the merged image must carry the handle classes with their hidden
//__handle slot first (the runtime writes slot 1 of the row — a missing
//slot is an out-of-bounds write). Plain classes carry package-qualified
//row names; the generic template rows (Channel/Future) stay bare.
static void test_moved_handle_slots()
{
    TEST(test_moved_handle_slots);
    {
        CompiledModule mod = compileAndLink("moved_conc_slots",
            "import concurrent;\n"
            "int main() {\n"
            "    Mutex m = Mutex();\n"
            "    m.lock();\n"
            "    m.unlock();\n"
            "    return 0;\n"
            "}\n");
        const CompiledClass* cls = findClass(mod, "concurrent.Mutex");
        CHECK(cls != nullptr,
            "concurrent.Mutex must be in the linked image");
        CHECK(!cls->fieldNames.empty()
            && cls->fieldNames[0] == "__handle",
            "concurrent.Mutex must lead with the __handle slot");
    }
    {
        CompiledModule mod = compileAndLink("moved_thread_slots",
            "import thread;\n"
            "void job() { }\n"
            "int main() {\n"
            "    Thread t = thread.spawn(job);\n"
            "    thread.join(t);\n"
            "    return 0;\n"
            "}\n");
        const CompiledClass* cls = findClass(mod, "thread.Thread");
        CHECK(cls != nullptr,
            "thread.Thread must be in the linked image");
        CHECK(!cls->fieldNames.empty()
            && cls->fieldNames[0] == "__handle",
            "thread.Thread must lead with the __handle slot");
        const CompiledClass* fut = findClass(mod, "Future");
        CHECK(fut != nullptr,
            "Future is a generic template row under a bare key");
        CHECK(!fut->fieldNames.empty()
            && fut->fieldNames[0] == "__handle",
            "Future must lead with the __handle slot");
    }
    PASS();
}

int main()
{
    //In-process ModuleBuilder host: the interned IdString tables must
    //exist before the first Build, and the compiler-side cast table
    //must be anchored too (test_stdlib_concurrency.cpp same shape).
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();
    runGuarded(test_moved_handle_slots);
    runGuarded(test_declared_members_all_stamped);
    runGuarded(test_channel_has_two_ctors);
    runGuarded(test_future_has_no_ctor);
    runGuarded(test_unbounded_ctor_does_not_read_capacity);
    runGuarded(test_builtin_handle_decl_predicate);
    runGuarded(test_find_class_bare_or_unique_suffix);
    runGuarded(test_binding_package_for_name);

    std::cerr << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail == 0 ? 0 : 1;
}
