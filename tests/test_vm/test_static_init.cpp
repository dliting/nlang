/*---
test_static_init.cpp - lazy static-initialization emitter shapes.

In-process ModuleBuilder coverage of v0.12.6 D2/D4 (spec §6.2): a
class whose statics all lack initializers keeps staticInitFuncIdx at
0xFFFF and emits no OP_ClassInitGate anywhere (zero cost, D9 zero
slots); a class with an initialized static synthesizes exactly one
"<class>.$staticInit" function record whose index the class record
carries, with a gate sequence at its access sites and the Done/Fail
protocol opcodes inside the initializer itself. Runtime once/retry
semantics are pinned by the e2e probes; these assertions pin the
emitter shapes the probes cannot see.
---*/
#include "test_support/test_scratch.h"
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/CastInfo.h>
#include <nlang/compiler/Logger.h>
#include "BytecodeOps.h"
#include "ModuleLoader.h"
#include "VmExecutor.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define TEST(name) \
    do { std::cerr << "  " << #name << " ... "; } while(0)
#define PASS() \
    do { std::cerr << "ok\n"; g_pass++; } while(0)
#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAILED: " << msg << "\n"; g_fail++; return; \
        } \
    } while(0)

namespace {

class MemLogger : public CompileLogger {
public:
    void WriteLog(CompileLogLevel level, const ISourceLocation*,
                  const char* szMessage) override
    {
        if (level == CLL_Error || level == CLL_Fatal)
            m_errors.emplace_back(szMessage);
    }
    const std::vector<std::string>& errors() const { return m_errors; }
private:
    std::vector<std::string> m_errors;
};

struct CompileOutcome {
    std::unique_ptr<BuildParams> params;
    std::unique_ptr<MemLogger> logger;
    bool ok = false;
};

//Compile one body to a loaded module image (production path: real
//.ncu on disk, then ModuleLoader::Load — the same bytes nvm executes).
CompileOutcome compileOne(const char* szBody)
{
    static uint32_t runCount = 0;
    //In-process host init: runtime + cast table (the 0.7.5 split moved
    //the latter out of Runtime::StaticInit) before any ModuleBuilder.
    static const bool bRuntimeReady = [] {
        Runtime::StaticInit();
        TypeCastInfo::StaticInit();
        return true;
    }();
    (void)bRuntimeReady;
    const auto dir = nlang::test::ScratchDir("nlang_static_init_tests");
    std::error_code fsError;
    std::filesystem::create_directories(dir, fsError);
    const auto file = dir / "main.n";
    {
        std::ofstream stream(file, std::ios::binary);
        stream << szBody;
        if (!stream.good())
            return {};
    }
    CompileOutcome out;
    out.params = std::make_unique<BuildParams>();
    out.params->m_SourceFiles.push_back(file.string());
    out.params->m_sProjectDir = dir.string();
    out.params->m_sOutputModule =
        "static_init_" + std::to_string(++runCount);
    out.params->m_sOutputDir = dir.string();
    out.params->m_sTempDir = dir.string();
    out.logger = std::make_unique<MemLogger>();
    auto builder = std::make_unique<ModuleBuilder>(
        *out.params, *out.logger);
    try { out.ok = builder->BuildArtifacts(); }
    catch (const std::exception&) { out.ok = false; }
    return out;
}

CompiledModule loadOne(const char* szBody)
{
    auto out = compileOne(szBody);
    if (!out.ok) {
        std::string joined = "compile failed:";
        for (const auto& e : out.logger->errors())
            joined += "\n    " + e;
        std::cerr << "FAILED: " << joined << "\n";
        g_fail++;
        return {};
    }
    const auto modPath = nlang::test::ScratchDir("nlang_static_init_tests")
        / (out.params->m_sOutputModule + ".ncu");
    return ModuleLoader::Load(modPath.string());
}

//Stride-aware opcode presence: raw byte scans misread operand bytes
//as opcodes, so step the same InstructionStride table the linker and
//disassembler walk.
bool bytecodeHasOp(const std::vector<uint8_t>& code, OpCode op)
{
    size_t pc = 0;
    while (pc < code.size()) {
        const auto insOp = static_cast<OpCode>(code[pc]);
        if (insOp == op)
            return true;
        pc += InstructionStride(insOp);
    }
    return false;
}

bool moduleHasOp(const CompiledModule& mod, OpCode op)
{
    for (const auto& fn : mod.functions)
        if (bytecodeHasOp(fn.bytecode, op))
            return true;
    return false;
}

bool endsWith(const std::string& text, const std::string& tail)
{
    return text.size() >= tail.size()
        && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

const CompiledClass* findClass(const CompiledModule& mod,
                               const std::string& suffix)
{
    for (const auto& cc : mod.classes)
        if (endsWith(cc.name, suffix))
            return &cc;
    return nullptr;
}

//Function records carry bare member names (see "writeInt" etc.), so the
//synthesized initializer is named exactly "$staticInit" — the owning
//class is disambiguated by the class record's staticInitFuncIdx.
size_t countStaticInitRecords(const CompiledModule& mod)
{
    size_t found = 0;
    for (const auto& fn : mod.functions)
        if (fn.name == "$staticInit")
            ++found;
    return found;
}

} //namespace

//D9/D2 shape: no initialized static — no initializer record, no gate
//anywhere, staticInitFuncIdx stays 0xFFFF (the zero-slot fast path).
static void test_initializerless_class_has_no_init_machinery()
{
    TEST(initializerless_class_has_no_init_machinery);
    CompiledModule mod = loadOne(
        "class Plain {\n"
        "    public static int zero;\n"
        "}\n"
        "int main() {\n"
        "    return Plain.zero;\n"
        "}\n");
    CHECK(!mod.classes.empty(), "module compiled with class records");
    const CompiledClass* cc = findClass(mod, "Plain");
    CHECK(cc != nullptr, "Plain class record present");
    CHECK(cc->staticInitFuncIdx == 0xFFFF,
        "initializer-less class keeps staticInitFuncIdx at 0xFFFF");
    CHECK(countStaticInitRecords(mod) == 0,
        "no $staticInit function record was minted");
    CHECK(!moduleHasOp(mod, OpCode::OP_ClassInitGate),
        "no OP_ClassInitGate appears in any function");
    PASS();
}

//D2/D4 shape: one initialized static — exactly one $staticInit record
//whose index rides the class record, protocol opcodes inside it, and
//a gate at the access site.
static void test_initialized_class_synthesizes_static_init()
{
    TEST(initialized_class_synthesizes_static_init);
    CompiledModule mod = loadOne(
        "class Cfg {\n"
        "    public static int value = side() + 1;\n"
        "}\n"
        "int side() {\n"
        "    return 40;\n"
        "}\n"
        "int main() {\n"
        "    return Cfg.value;\n"
        "}\n");
    const CompiledClass* cc = findClass(mod, "Cfg");
    CHECK(cc != nullptr, "Cfg class record present");
    CHECK(countStaticInitRecords(mod) == 1,
        "exactly one $staticInit function record was minted");
    CHECK(cc->staticInitFuncIdx != 0xFFFF,
        "class record carries the initializer function index");
    CHECK(cc->staticInitFuncIdx < mod.functions.size(),
        "initializer index addresses the function table");
    CHECK(mod.functions[cc->staticInitFuncIdx].name == "$staticInit",
        "the carried index names the $staticInit record");
    CHECK(bytecodeHasOp(
              mod.functions[cc->staticInitFuncIdx].bytecode,
              OpCode::OP_ClassInitDone),
        "$staticInit publishes completion via OP_ClassInitDone");
    CHECK(bytecodeHasOp(
              mod.functions[cc->staticInitFuncIdx].bytecode,
              OpCode::OP_ClassInitFail),
        "$staticInit resets the owner via OP_ClassInitFail");
    size_t mainIdx = mod.functions.size();
    for (size_t i = 0; i < mod.functions.size(); ++i)
        if (endsWith(mod.functions[i].name, "main"))
            mainIdx = i;
    CHECK(mainIdx < mod.functions.size(), "main function record found");
    CHECK(bytecodeHasOp(mod.functions[mainIdx].bytecode,
                        OpCode::OP_ClassInitGate),
        "the access site in main opens with an OP_ClassInitGate");
    PASS();
}

//v0.12.6 Task 7 (spec D3/D12): a foldable `static const` field occupies
//no runtime slot and triggers nothing — its staticFields entry carries
//slotIdx 0xFFFF with the folded value, and only the mutable sibling
//holds a slot in the module's staticSlots table.
static void test_folded_static_const_occupies_no_slot()
{
    TEST(folded_static_const_occupies_no_slot);
    CompiledModule mod = loadOne(
        "class MathU {\n"
        "    public static const int k = 40;\n"
        "    public static int m;\n"
        "}\n"
        "int main() {\n"
        "    MathU.m = MathU.k + 1;\n"
        "    return MathU.m;\n"
        "}\n");
    const CompiledClass* cc = findClass(mod, "MathU");
    CHECK(cc != nullptr, "MathU class record present");
    const CompiledStaticField* folded = nullptr;
    const CompiledStaticField* mutableField = nullptr;
    for (const auto& sf : cc->staticFields) {
        if (sf.name == "k")
            folded = &sf;
        if (sf.name == "m")
            mutableField = &sf;
    }
    CHECK(folded != nullptr, "folded const appears in staticFields");
    CHECK(folded->slotIdx == 0xFFFF,
        "folded const carries slotIdx 0xFFFF (no slot)");
    CHECK(folded->foldedValue.tag == RTK_Int32
              && folded->foldedValue.intValue == 40,
        "folded const carries its folded value");
    CHECK(mutableField != nullptr, "mutable static appears in staticFields");
    CHECK(mutableField->slotIdx != 0xFFFF
              && mutableField->slotIdx < mod.staticSlots.size(),
        "mutable static holds a real slot index");
    size_t usedSlots = 0;
    for (const auto& sf : cc->staticFields)
        if (sf.slotIdx != 0xFFFF)
            ++usedSlots;
    CHECK(mod.staticSlots.size() == usedSlots,
        "the module's slot table holds only the mutable field's slot");
    CHECK(!moduleHasOp(mod, OpCode::OP_ClassInitGate),
        "a fully-folded class emits no initialization gate");
    PASS();
}

int main()
{
#ifdef _WIN32
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS
                             | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::cerr << "=== NLang Static Init Unit Tests ===\n\n";

    try { test_initializerless_class_has_no_init_machinery(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_initialized_class_synthesizes_static_init(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }
    try { test_folded_static_const_occupies_no_slot(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }

    std::cerr << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
