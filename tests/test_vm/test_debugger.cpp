// --- ndb debugger unit tests ---
// In-process compile+run via the public ModuleBuilder API (see
// test_stdlib.cpp for the harness rationale). Covers the .nmod v1.9
// sourceFile field, the B.1 imported-locals fix, debug hooks, the
// read-only view, the DebugSessionController session semantics (via a
// scripted front end), the ndb CLI adapter's output formats, and the
// IHostIo seam (output capture + readLine rejection).
// MUST call Runtime::StaticInit() before any Build() (IdString tables).

#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "VmExecutor.h"
#include "IDebugHooks.h"
#include "IHostIo.h"
#include "ModuleLoader.h"
#include "Disassembler.h"
#include "DebugSessionController.h"
#include "DebugSession.h"
#include "SourceCache.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
//Like CHECK (aborts the current test function) but the message carries
//the condition text — for shape preconditions whose failure would make
//subsequent indexed assertions out-of-bounds.
#define REQUIRE(cond) \
    do { if (!(cond)) { FAIL("precondition: " #cond); return; } } while(0)

//Scratch directory for temp sources and modules.
static std::filesystem::path scratchDir()
{
    static const auto dir = std::filesystem::temp_directory_path()
        / "nlang_test_debugger";
    std::filesystem::create_directories(dir);
    return dir;
}

struct BuildOutcome
{
    bool ok = false;
    std::string diagnostics;
};

//Write <tag>.n with the raw source (no stdlib imports prepended —
//debugger tests target plain programs), build <tag>.nmod.
static BuildOutcome buildSource(const std::string& tag,
    const std::string& source)
{
    const auto dir = scratchDir();
    const auto nPath = dir / (tag + ".n");
    const auto modPath = dir / (tag + ".nmod");
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

    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);

    BuildOutcome outcome;
    try {
        outcome.ok = builder.Build()
            && std::filesystem::exists(modPath);
    } catch (const std::exception& e) {
        outcome.diagnostics += std::string("internal error: ") + e.what();
        return outcome;
    }
    for (auto it = logger.cbegin(); it != logger.cend(); ++it)
        outcome.diagnostics += (*it)->Message() + "\n";
    return outcome;
}

//Build a consumer TU from raw source with the scratch dir on the
//import path (shared by the import-shaped tests; the imported lib must
//already exist as a compiled .nmod — see the FindModuleFile note in
//test_v19_import_roundtrip).
static BuildOutcome buildConsumer(const std::string& tag,
    const std::string& source)
{
    const auto dir = scratchDir();
    {
        std::ofstream out(dir / (tag + ".n"), std::ios::binary);
        out << source;
    }
    BuildParams params;
    params.m_SourceFiles.push_back((dir / (tag + ".n")).string());
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_ImportDirs.push_back(dir.string());
    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    BuildOutcome outcome;
    outcome.ok = builder.Build();
    for (auto it = logger.cbegin(); it != logger.cend(); ++it)
        outcome.diagnostics += (*it)->Message() + "\n";
    return outcome;
}

//加载刚构建的 <tag>.nmod。
static CompiledModule loadBuilt(const std::string& tag)
{
    return ModuleLoader::Load(
        (scratchDir() / (tag + ".nmod")).string());
}

void test_v19_sourcefile_single_tu()
{
    TEST(v19_sourcefile_single_tu);
    BuildOutcome b = buildSource("srcfile_single",
        "int helper(int v) { return v * 2; }\n"
        "int main() { return helper(21); }\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("srcfile_single");
    const std::string* mainSrc = nullptr;
    const std::string* helperSrc = nullptr;
    for (const auto& f : mod.functions) {
        if (f.name == "main") mainSrc = &f.sourceFile;
        if (f.name == "helper") helperSrc = &f.sourceFile;
    }
    CHECK(mainSrc != nullptr, "main should be in the module");
    CHECK(helperSrc != nullptr, "helper should be in the module");
    CHECK(mainSrc->find("srcfile_single.n") != std::string::npos,
        "main.sourceFile should name the source file, got: " + *mainSrc);
    CHECK(*mainSrc == *helperSrc,
        "same-TU functions should share the source file path");
    PASS();
}

void test_v19_import_roundtrip()
{
    TEST(v19_import_roundtrip);
    //The import target must exist as a compiled .nmod — ModuleBuilder's
    //FindModuleFile searches m_ImportDirs for <name>.nmod, never a .n
    //source, so build the lib module first, then the consumer.
    BuildOutcome lib = buildSource("dbgutil_lib",
        "int triple(int v) { int t = v * 3; return t; }\n");
    CHECK(lib.ok, "lib build should succeed: " + lib.diagnostics);
    BuildOutcome b = buildConsumer("dbgutil_main",
        "import dbgutil_lib;\n"
        "int main() { return dbgutil_lib.triple(14); }\n");
    CHECK(b.ok, "import build should succeed: " + b.diagnostics);

    CompiledModule mod = loadBuilt("dbgutil_main");
    int tripleIdx = mod.FindFunction("triple");
    CHECK(tripleIdx >= 0, "triple should be merged into the consumer");
    const auto& triple = mod.functions[static_cast<size_t>(tripleIdx)];
    CHECK(triple.sourceFile.find("dbgutil_lib.n") != std::string::npos,
        "imported function keeps producer source file, got: "
        + triple.sourceFile);
    CHECK(!triple.locals.empty(),
        "B.1 merge must copy locals (GC roots + debugger visibility)");
    PASS();
}

void test_v19_loader_rejects_v1_8()
{
    TEST(v19_loader_rejects_v1_8);
    BuildOutcome b = buildSource("oldver",
        "int main() { return 0; }\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    const auto modPath = scratchDir() / "oldver.nmod";

    //Patch the minorVer field (header offset 10, little-endian u16:
    //magic[8] + major(u16) + minor(u16)) down to 8 — the loader must
    //refuse v1.8 modules outright (floor bump discipline).
    std::vector<char> bytes;
    {
        std::ifstream in(modPath, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    CHECK(bytes.size() >= 12, "module file should have a full header");
    bytes[10] = 0x08;
    bytes[11] = 0x00;
    const auto oldPath = scratchDir() / "oldver_v18.nmod";
    {
        std::ofstream out(oldPath, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    bool threw = false;
    try {
        ModuleLoader::Load(oldPath.string());
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw, "loader must reject a v1.8 module (floor is 9)");
    PASS();
}

void test_v19_import_gc_roots()
{
    TEST(v19_import_gc_roots);
    //Pins the B.1 locals fix: before it, imported frames had no local
    //descriptors, so MarkPhase marked nothing and every imported-frame
    //heap value was swept. The array must be referenced ONLY from the
    //imported frame (a caller-frame local would root it through the
    //caller's own locals table).
    //
    //The scenario works around the sweeper's reuse dynamics: a swept
    //slot re-enters the free list only at the GC that sweeps it (the
    //rebuild skips kind==0 slots), and vector::clear() keeps the old
    //buffer, so a swept-but-never-reallocated slot still reads its old
    //value. To make the bug observable, the program drives the heap
    //past the GC threshold (1024 slots), allocates `keep`, then runs
    //exactly one more loop iteration — its back-edge GC sweeps keep
    //(no roots under the bug) together with that iteration's single
    //scratch, leaving exactly two slots in the free list. Two
    //straight-line allocations then drain the free list: the second
    //pop recycles keep's slot and assign() zeroes its data, so the
    //final read observes 0 instead of 7. With the fix, keep is marked
    //at that GC and the drain cannot touch its slot.
    //Lib must be a compiled .nmod before the consumer can import it
    //(same FindModuleFile contract as the roundtrip test above).
    BuildOutcome lib = buildSource("gcutil_lib",
        "int churn() {\n"
        "    int i = 0;\n"
        "    while (i < 1100) {\n"
        "        int[] warm = new int[8];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    int[] keep = new int[1];\n"
        "    keep[0] = 7;\n"
        "    i = 0;\n"
        "    while (i < 1) {\n"
        "        int[] scratch = new int[64];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    int[] drainA = new int[8];\n"
        "    int[] drainB = new int[8];\n"
        "    return keep[0] + drainA[0] + drainB[0];\n"
        "}\n");
    CHECK(lib.ok, "lib build should succeed: " + lib.diagnostics);
    BuildOutcome b = buildConsumer("gcutil_main",
        "import gcutil_lib;\n"
        "int main() { return gcutil_lib.churn(); }\n");
    CHECK(b.ok, "import build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("gcutil_main");
    VmExecutor exec;
    CHECK(exec.Execute(mod) == 7,
        "imported-frame local 'keep' must survive GC (B.1 locals fix)");
    PASS();
}

// --- Task 2: hooks + read-only view ---

//Records every checkpoint as (line, depth) pairs.
class RecordingHooks : public IDebugHooks {
public:
    std::vector<std::pair<uint16_t, size_t>> statementStops;
    std::vector<uint16_t> throwLines;
    void OnStatement(const DebugStopInfo& s, IVmDebugView&) override {
        statementStops.push_back({s.line, s.depth});
    }
    void OnThrow(const DebugStopInfo& s, IVmDebugView&) override {
        throwLines.push_back(s.line);
    }
};

void test_hooks_line_sequence()
{
    //Straight-line program: statement checkpoints arrive in source
    //order, one per statement (same-line statements each fire).
    TEST(hooks_line_sequence);
    BuildOutcome b = buildSource("hook_lines",
        "int main() {\n"            //1
        "    int a = 1;\n"          //2
        "    int c = 2;\n"          //3
        "    return a + c;\n"       //4
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("hook_lines");
    VmExecutor exec;
    RecordingHooks hooks;
    exec.SetDebugHooks(&hooks);
    CHECK(exec.Execute(mod) == 3, "program result");
    CHECK(hooks.statementStops.size() == 3, "3 statements = 3 stops");
    CHECK(hooks.statementStops[0].first == 2, "first stop at line 2");
    CHECK(hooks.statementStops[1].first == 3, "second stop at line 3");
    CHECK(hooks.statementStops[2].first == 4, "third stop at line 4");
    for (auto& s : hooks.statementStops)
        CHECK(s.second == 1, "main-only run has depth 1");
    PASS();
}

void test_hooks_call_depths()
{
    //main -> f -> g is C++ recursion; depth tracks the NLang stack.
    TEST(hooks_call_depths);
    BuildOutcome b = buildSource("hook_depths",
        "int g() { return 5; }\n"   //1
        "int f() { return g(); }\n" //2
        "int main() { return f(); }\n"); //3
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("hook_depths");
    VmExecutor exec;
    RecordingHooks hooks;
    exec.SetDebugHooks(&hooks);
    CHECK(exec.Execute(mod) == 5, "program result");
    //Statements: main:3, f:2, g:1 — depth 1, 2, 3.
    REQUIRE(hooks.statementStops.size() == 3);
    CHECK(hooks.statementStops[0] == std::make_pair(uint16_t(3), size_t(1)),
        "main statement at depth 1");
    CHECK(hooks.statementStops[1] == std::make_pair(uint16_t(2), size_t(2)),
        "f statement at depth 2");
    CHECK(hooks.statementStops[2] == std::make_pair(uint16_t(1), size_t(3)),
        "g statement at depth 3");
    PASS();
}

//Captures view state at the stop on a chosen line of a target function.
//Stops fire BEFORE the statement runs — captured values reflect every
//statement up to but excluding stopLine, so tests pick the line
//accordingly (e.g. the return line to see computed locals).
class InspectHooks : public IDebugHooks {
public:
    std::string target;               //capture when frame-0 is this func
    uint16_t stopLine = 0;            //...on this source line
    bool captured = false;
    size_t frameCount = 0;
    uint16_t line = 0;
    std::vector<std::string> frameNames;
    std::vector<std::string> localLines;  //"name=display"
    void OnStatement(const DebugStopInfo& s, IVmDebugView& view) override {
        if (captured || s.line != stopLine
            || view.FrameInfo(0).funcName != target) return;
        captured = true;
        line = s.line;
        frameCount = view.FrameCount();
        for (size_t d = 0; d < frameCount; ++d)
            frameNames.push_back(view.FrameInfo(d).funcName);
        for (const auto& l : view.FrameLocals(0))
            localLines.push_back(l.name + "=" + l.display);
    }
    void OnThrow(const DebugStopInfo&, IVmDebugView&) override {}
};

void test_view_frames_and_locals()
{
    TEST(view_frames_and_locals);
    BuildOutcome b = buildSource("view_locals",
        "class Point { int x; int y; }\n"       //1
        "int inner(Point p) {\n"                //2
        "    int s = p.x + p.y;\n"              //3
        "    return s;\n"                       //4
        "}\n"                                   //5
        "int main() {\n"                        //6
        "    Point p = new Point{x: 6, y: 7};\n"//7
        "    return inner(p);\n"                //8
        "}\n");                                 //9
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("view_locals");
    VmExecutor exec;
    InspectHooks hooks;
    hooks.target = "inner";
    hooks.stopLine = 4;  //return s; — line 3 has run, so s is computed
    exec.SetDebugHooks(&hooks);
    CHECK(exec.Execute(mod) == 13, "program result");
    CHECK(hooks.captured, "should stop inside inner");
    CHECK(hooks.frameCount == 2, "inner + main frames");
    CHECK(hooks.frameNames.size() == 2
        && hooks.frameNames[0] == "inner"
        && hooks.frameNames[1] == "main", "innermost-first ordering");
    //Exact visible-locals pin: filter hidden names (the ndb info-locals
    //contract), then the visible set must be exactly {p, s} — no extra
    //descriptors leak into the display. (Frame temps never appear here:
    //tempSlot1-4/returnSlot/evalArea are raw offsets outside func->locals.)
    std::vector<std::string> visible;
    for (const auto& l : hooks.localLines) {
        size_t eq = l.find('=');
        REQUIRE(eq != std::string::npos);
        std::string name = l.substr(0, eq);
        if (name.size() >= 2 && name[0] == '_' && name[1] == '_') continue;
        if (!name.empty() && name[0] == '$') continue;
        visible.push_back(l);
    }
    CHECK(visible.size() == 2, "exactly two visible locals (param p, s)");
    bool sawP = false, sawS = false;
    for (const auto& l : visible) {
        if (l == "p=Point{x=6, y=7}") sawP = true;
        if (l == "s=13") sawS = true;
    }
    CHECK(sawP, "class local renders one-level fields (got lines mismatch)");
    CHECK(sawS, "int local renders value");
    PASS();
}

void test_hooks_on_throw()
{
    //OnThrow fires at the throw site with the stack still alive; the
    //program then completes the unwind into the catch handler.
    TEST(hooks_on_throw);
    BuildOutcome b = buildSource("hook_throw",
        "int main() {\n"                             //1
        "    try {\n"                                //2
        "        throw new Exception(\"boom\");\n"   //3
        "    } catch (Exception e) {\n"              //4
        "        return 7;\n"                        //5
        "    }\n"                                   //6
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("hook_throw");
    VmExecutor exec;
    RecordingHooks hooks;
    exec.SetDebugHooks(&hooks);
    CHECK(exec.Execute(mod) == 7, "catch handler runs after OnThrow");
    CHECK(hooks.throwLines.size() == 1, "one OnThrow");
    CHECK(hooks.throwLines[0] == 3, "throw anchored at line 3");
    PASS();
}

void test_view_value_kinds()
{
    //Shallow formatters across value kinds: string, struct, array —
    //int/class are covered by view_frames_and_locals above.
    TEST(view_value_kinds);
    BuildOutcome b = buildSource("view_kinds",
        "struct P { int x; int y; }\n"               //1
        "int main() {\n"                             //2
        "    string s = \"hi\";\n"                   //3
        "    P p = new P{x: 1, y: 2};\n"             //4
        "    int[] a = new int[2];\n"                //5
        "    a[0] = 7;\n"                            //6
        "    a[1] = 8;\n"                            //7
        "    return 0;\n"                            //8
        "}\n");                                      //9
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("view_kinds");
    VmExecutor exec;
    InspectHooks hooks;
    hooks.target = "main";
    hooks.stopLine = 8;  //return 0; — s/p/a all assigned by then
    exec.SetDebugHooks(&hooks);
    CHECK(exec.Execute(mod) == 0, "program result");
    REQUIRE(hooks.captured);
    //Exact full-line pins (no substring slop — the render format is
    //the e2e assertion contract).
    bool sawS = false, sawP = false, sawA = false;
    for (const auto& l : hooks.localLines) {
        if (l == "s=\"hi\"") sawS = true;
        if (l == "p=P{x=1, y=2}") sawP = true;
        if (l == "a=int[2]{7, 8}") sawA = true;
    }
    CHECK(sawS, "string local renders quoted");
    CHECK(sawP, "struct local renders one-level fields");
    CHECK(sawA, "array local renders element kind + values");
    PASS();
}

void test_hooks_cpp_exception_propagates()
{
    //A front end that breaks the contract and throws from a
    //checkpoint: the C++ exception does not match the VM's
    //NLangThrow handler, so it propagates out of Execute as a normal
    //C++ exception the embedder can catch — no crash, no silent
    //swallow. (The compliant front end catches its own exceptions;
    //DebugSession's command loop does.)
    TEST(hooks_cpp_exception_propagates);
    BuildOutcome b = buildSource("hook_throw_cpp",
        "int main() {\n"      //1
        "    int a = 1;\n"    //2
        "    return a;\n"     //3
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("hook_throw_cpp");
    VmExecutor exec;
    class ThrowingHooks : public IDebugHooks {
    public:
        void OnStatement(const DebugStopInfo&, IVmDebugView&) override {
            throw std::runtime_error("front end broke the contract");
        }
        void OnThrow(const DebugStopInfo&, IVmDebugView&) override {}
    };
    ThrowingHooks hooks;
    exec.SetDebugHooks(&hooks);
    bool caught = false;
    try {
        exec.Execute(mod);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    CHECK(caught, "C++ exception from a checkpoint escapes Execute "
          "cleanly instead of crashing or vanishing");
    PASS();
}

// --- Task 3: line/pc map ---

void test_linepc_map_first_pc()
{
    //Each line maps to its first statement pc; map ascends by pc.
    TEST(linepc_map_first_pc);
    BuildOutcome b = buildSource("linepc_map",
        "int main() {\n"        //1
        "    int a = 1;\n"      //2
        "    a = a + 1;\n"      //3
        "    return a;\n"       //4
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("linepc_map");
    int mainIdx = mod.FindFunction("main");
    REQUIRE(mainIdx >= 0);
    const auto& mainf = mod.functions[static_cast<size_t>(mainIdx)];
    auto map = BuildLinePcMap(mainf);
    CHECK(map.size() == 3, "3 statement lines (2,3,4)");
    CHECK(map[0].line == 2 && map[1].line == 3 && map[2].line == 4,
        "lines ascend in order");
    CHECK(map[0].pc < map[1].pc && map[1].pc < map[2].pc, "pcs ascend");
    //Each mapped pc must actually sit on an OP_DebugInfo whose
    //operand round-trips the line (self-consistency of the decode).
    const auto& bc = mainf.bytecode;
    for (const auto& e : map) {
        REQUIRE(e.pc + 2 < bc.size());
        uint16_t dec = static_cast<uint16_t>(bc[e.pc + 1] | (bc[e.pc + 2] << 8));
        CHECK(dec == e.line, "pc points at its own line operand");
    }
    PASS();
}

void test_linepc_map_same_line_multi_anchor()
{
    //EVERY statement anchor is an entry (gdb-style one line, multiple
    //locations): the multi-declarator line keeps both declarator
    //anchors, the one-line if/else keeps cond + both arms — `b LINE`
    //can cover each statement on the line, not just the first.
    TEST(linepc_map_same_line_multi_anchor);
    BuildOutcome b = buildSource("linepc_multi_anchor",
        "int main() {\n"                               //1
        "    int a = 1, b = 2;\n"                      //2 (two anchors)
        "    if (a > b) { a = 3; } else { b = 4; }\n"  //3 (three anchors)
        "    return a + b;\n"                          //4
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("linepc_multi_anchor");
    int mainIdx = mod.FindFunction("main");
    REQUIRE(mainIdx >= 0);
    const auto& mainf = mod.functions[static_cast<size_t>(mainIdx)];
    auto map = BuildLinePcMap(mainf);
    //2 (multidecl) + 3 (cond + both if/else arms) + 1 (return) = 6.
    CHECK(map.size() == 6, "one entry per statement anchor");
    const uint16_t want[] = {2, 2, 3, 3, 3, 4};
    for (size_t i = 0; i < map.size(); ++i)
        CHECK(map[i].line == want[i],
            "entry " + std::to_string(i) + " line");
    for (size_t i = 1; i < map.size(); ++i)
        CHECK(map[i - 1].pc < map[i].pc, "pcs ascend");
    PASS();
}

void test_linepc_map_finally_duplicates()
{
    //try/finally emits each finally-body statement's anchor TWICE: the
    //exception-path copy in the handler region FIRST, then the
    //normal-path copy. Every anchor is an entry, so BOTH copies appear
    //for every finally-body statement (review probe
    //temp/linepc_probe.cpp; the single-statement body is pinned by
    //test_linepc_map_finally_single_two_copies below).
    TEST(linepc_map_finally_duplicates);
    BuildOutcome b = buildSource("linepc_finally_dup",
        "int main() {\n"          //1
        "    int a = 0;\n"        //2
        "    try {\n"             //3
        "        a = 1;\n"        //4
        "    } finally {\n"       //5
        "        a = a + 1;\n"    //6
        "        a = a + 2;\n"    //7
        "    }\n"                 //8
        "    return a;\n"         //9
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("linepc_finally_dup");
    int mainIdx = mod.FindFunction("main");
    REQUIRE(mainIdx >= 0);
    const auto& mainf = mod.functions[static_cast<size_t>(mainIdx)];
    auto map = BuildLinePcMap(mainf);
    //Observed marker stream: 2,3,4,6,7,6,7,9 — finally lines 6 and 7
    //each appear twice, exception-path copies first.
    REQUIRE(map.size() == 8);
    const uint16_t want[] = {2, 3, 4, 6, 7, 6, 7, 9};
    for (size_t i = 0; i < map.size(); ++i)
        CHECK(map[i].line == want[i],
            "entry " + std::to_string(i) + " line");
    CHECK(map[3].pc < map[5].pc, "line 6 exception copy precedes normal");
    CHECK(map[4].pc < map[6].pc, "line 7 exception copy precedes normal");
    for (size_t i = 1; i < map.size(); ++i)
        CHECK(map[i - 1].pc < map[i].pc, "pcs ascend");
    //Every entry's pc sits on an OP_DebugInfo round-tripping its line.
    const auto& bc = mainf.bytecode;
    for (const auto& e : map) {
        REQUIRE(e.pc + 2 < bc.size());
        uint16_t dec = static_cast<uint16_t>(bc[e.pc + 1] | (bc[e.pc + 2] << 8));
        CHECK(dec == e.line, "pc points at its own line operand");
    }
    PASS();
}

void test_linepc_map_finally_single_two_copies()
{
    //try/finally compiles a single-statement finally body TWICE: the
    //exception-path copy (handler region) and the normal-path copy.
    //Both are entries — under the old adjacent-collapse the normal-path
    //pc was dropped, so `b LINE` could never fire on the normal path.
    TEST(linepc_map_finally_single_two_copies);
    BuildOutcome b = buildSource("linepc_finally_single",
        "int main() {\n"          //1
        "    int a = 0;\n"        //2
        "    try {\n"             //3
        "        a = 1;\n"        //4
        "    } finally {\n"       //5
        "        a = a + 1;\n"    //6
        "    }\n"                 //7
        "    return a;\n"         //8
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("linepc_finally_single");
    int mainIdx = mod.FindFunction("main");
    REQUIRE(mainIdx >= 0);
    const auto& mainf = mod.functions[static_cast<size_t>(mainIdx)];
    auto map = BuildLinePcMap(mainf);
    //Observed marker stream: 2,3,4,6,6,8 — the finally line twice.
    REQUIRE(map.size() == 6);
    const uint16_t want[] = {2, 3, 4, 6, 6, 8};
    for (size_t i = 0; i < map.size(); ++i)
        CHECK(map[i].line == want[i],
            "entry " + std::to_string(i) + " line");
    CHECK(map[3].line == 6 && map[4].line == 6 && map[3].pc < map[4].pc,
        "finally line twice, exception-path copy first");
    //Both copies' pcs sit on OP_DebugInfo round-tripping the line.
    const auto& bc = mainf.bytecode;
    for (size_t i = 3; i <= 4; ++i) {
        REQUIRE(map[i].pc + 2 < bc.size());
        uint16_t dec = static_cast<uint16_t>(
            bc[map[i].pc + 1] | (bc[map[i].pc + 2] << 8));
        CHECK(dec == 6, "finally copy pc points at line 6");
    }
    PASS();
}

void test_instruction_stride_exact_landing()
{
    //Equivalence guard: the read-side stride table must agree with the
    //compiler's emission — walking every function's bytecode must land
    //exactly on its end without hitting the unknown-opcode throw.
    TEST(instruction_stride_exact_landing);
    BuildOutcome b = buildSource("stride_landing",
        "int twice(int v) { return v * 2; }\n"  //1
        "int main() {\n"                        //2
        "    int sum = 0;\n"                    //3
        "    int[] xs = new int[3];\n"          //4
        "    xs[0] = 1;\n"                      //5
        "    foreach (int x in xs) {\n"         //6
        "        sum = sum + x;\n"              //7
        "    }\n"                               //8
        "    int i = 0;\n"                      //9
        "    while (i < 2) {\n"                 //10
        "        i = i + 1;\n"                  //11
        "    }\n"                               //12
        "    try {\n"                           //13
        "        sum = twice(sum);\n"           //14
        "    } finally {\n"                     //15
        "        sum = sum + 1;\n"              //16
        "    }\n"                               //17
        "    string t = \"v\";\n"               //18
        "    string u = t + \"w\";\n"           //19
        "    if (u == \"vw\") {\n"              //20
        "        sum = sum + 10;\n"             //21
        "    }\n"                               //22
        "    return sum;\n"                     //23
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("stride_landing");
    REQUIRE(mod.functions.size() >= 2);
    bool walked = false;
    for (const auto& f : mod.functions) {
        if (f.bytecode.empty()) continue;
        size_t pc = 0;
        try {
            while (pc < f.bytecode.size())
                pc += InstructionStride(
                    static_cast<OpCode>(f.bytecode[pc]));
        } catch (const std::exception&) {
            FAIL("unknown opcode at pc " + std::to_string(pc)
                + " in " + f.name);
            return;
        }
        CHECK(pc == f.bytecode.size(),
            "walk must land exactly on the end of " + f.name);
        walked = true;
    }
    CHECK(walked, "at least one function walked");
    PASS();
}

// --- Task 4: DebugSession (CLI adapter over the controller) ---

//Drive the ndb CLI adapter (wired to a DebugSessionController) with a
//command script; returns everything the session wrote. Scripts MUST run
//the program to completion (final `c`) — hitting EOF while frozen would
//quit the whole test binary (EOF = q by contract; dbg_quit_eof e2e
//pins that in a subprocess instead).
static std::string RunSession(const std::string& tag,
    const std::string& source, const std::string& commands)
{
    BuildOutcome b = buildSource(tag, source);
    if (!b.ok) return "BUILD FAILED: " + b.diagnostics;
    CompiledModule mod = loadBuilt(tag);
    std::ostringstream out;
    std::istringstream in(commands);
    DebugSession session(mod,
        (scratchDir() / (tag + ".nmod")).string(), in, out);
    DebugSessionController controller(mod, session);
    session.SetController(&controller);
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    exec.Execute(mod);
    return out.str();
}

void test_session_initial_stop_and_continue()
{
    TEST(session_initial_stop_and_continue);
    std::string out = RunSession("sess_init",
        "int main() {\n"      //1
        "    int a = 1;\n"    //2
        "    return a;\n"     //3
        "}\n",
        "c\n");
    CHECK(out.find("Stopped: main (sess_init.n:2)") != std::string::npos,
        "initial stop at main's first statement");
    CHECK(out.find("(ndb) ") != std::string::npos, "prompt shown");
    PASS();
}

void test_session_step_semantics()
{
    TEST(session_step_semantics);
    std::string out = RunSession("sess_step",
        "int inner(int v) {\n"   //1
        "    int r = v + 1;\n"   //2
        "    return r;\n"        //3
        "}\n"                    //4
        "int main() {\n"         //5
        "    int a = 4;\n"       //6
        "    int b = inner(a);\n"//7
        "    return 0;\n"        //8
        "}\n",
        "s\ns\nf\nc\n");
    //s: main:6 -> main:7; s: into inner:2; f: skips inner:3, lands
    //back in main:8 (depth 1 < recorded 2).
    CHECK(out.find("Stopped: main (sess_step.n:6)") != std::string::npos,
        "initial stop line 6");
    CHECK(out.find("Stopped: main (sess_step.n:7)") != std::string::npos,
        "step-into advances one statement");
    CHECK(out.find("Stopped: inner (sess_step.n:2)") != std::string::npos,
        "step-into descends into the call");
    CHECK(out.find("Stopped: main (sess_step.n:8)") != std::string::npos,
        "step-out returns past remaining callee statements");
    CHECK(out.find("Stopped: inner (sess_step.n:3)") == std::string::npos,
        "inner:3 is passed over by finish");
    PASS();
}

void test_session_breakpoint_hit()
{
    TEST(session_breakpoint_hit);
    std::string out = RunSession("sess_bp",
        "int main() {\n"        //1
        "    int t = 0;\n"      //2
        "    int i = 1;\n"      //3
        "    while (i <= 3) {\n"//4
        "        t = t + i;\n"  //5
        "        i = i + 1;\n"  //6
        "    }\n"               //7
        "    return t;\n"       //8
        "}\n",
        "b 5\nc\nc\nc\ni b\nc\n");
    //Loop body line 5 executes 3 times; the script continues past all
    //three stops, checks the hit counter, then runs to completion
    //(final `c` — never end a script frozen, EOF would quit the test
    //binary).
    CHECK(out.find("Breakpoint 1 at main (sess_bp.n:5)") != std::string::npos,
        "set-time report names the resolved location");
    CHECK(out.find("Breakpoint 1, main (sess_bp.n:5)") != std::string::npos,
        "hit-time report");
    CHECK(out.find("hits=3") != std::string::npos,
        "breakpoint counts every hit");
    PASS();
}

void test_session_bt_and_locals()
{
    TEST(session_bt_and_locals);
    std::string out = RunSession("sess_bt",
        "int scale(int v, int k) {\n" //1
        "    int s = v * k;\n"        //2
        "    return s;\n"             //3
        "}\n"                         //4
        "int main() {\n"              //5
        "    int r = scale(5, 3);\n"  //6
        "    return 0;\n"             //7
        "}\n",
        "b 2\nc\nbt\ninfo locals\nc\n");
    CHECK(out.find("#0  scale (sess_bt.n:2)") != std::string::npos,
        "bt frame 0 format");
    CHECK(out.find("#1  main (sess_bt.n:6)") != std::string::npos,
        "bt frame 1 carries the CALLING statement anchor");
    CHECK(out.find("v = 5") != std::string::npos, "param local shown");
    CHECK(out.find("k = 3") != std::string::npos, "param local shown");
    //Hidden-name filter: synthesized locals stay out of the display.
    bool leaked = out.find("__foreach") != std::string::npos
        || out.find("$finally") != std::string::npos;
    CHECK(!leaked, "hidden local names stay internal");
    PASS();
}

void test_session_frame_select()
{
    TEST(session_frame_select);
    std::string out = RunSession("sess_frame",
        "int callee(int v) {\n"  //1
        "    int r = v + 1;\n"   //2
        "    return r;\n"        //3
        "}\n"                    //4
        "int main() {\n"         //5
        "    int a = 9;\n"       //6
        "    int b = callee(a);\n"//7
        "    return 0;\n"        //8
        "}\n",
        "b 2\nc\ninfo locals\nframe 1\ninfo locals\nc\n");
    //First `info locals` runs on frame 0 (callee: v = 9), the second
    //after `frame 1` (main: a = 9) — selection routes the query.
    CHECK(out.find("v = 9") != std::string::npos,
        "frame 0 (default) shows callee's param");
    CHECK(out.find("a = 9") != std::string::npos,
        "frame 1 locals belong to main");
    PASS();
}

void test_session_break_by_func()
{
    TEST(session_break_by_func);
    std::string out = RunSession("sess_bpfunc",
        "int inner(int v) {\n"  //1
        "    int r = v + 1;\n"  //2
        "    return r;\n"       //3
        "}\n"                   //4
        "int main() {\n"        //5
        "    int a = 3;\n"      //6
        "    int b = inner(a);\n"//7
        "    return 0;\n"       //8
        "}\n",
        "b inner\nc\nc\n");
    //b funcName resolves to the function's FIRST statement (line 2)
    //and reports the resolved location at set time (spec §8).
    CHECK(out.find("Breakpoint 1 at inner (sess_bpfunc.n:2)")
            != std::string::npos,
        "set-time report names the first statement");
    CHECK(out.find("Breakpoint 1, inner (sess_bpfunc.n:2)")
            != std::string::npos,
        "hit-time report at the same location");
    PASS();
}

// --- Task 3 (unified API): DebugSessionController ---

//Scripted front end: records stops; the per-stop handler observes and
//queues the resume, which WaitUntilResume issues (default: continue) —
//controller semantics without a CLI.
struct ScriptedFrontEnd : IDebugFrontEnd
{
    enum class Resume { Continue, Into, Over, Out };
    std::vector<StopInfo> stops;
    std::function<void(DebugSessionController&)> onStopped;  //observe/mutate
    Resume resume = Resume::Continue;                        //queued resume
    DebugSessionController* controller = nullptr;
    void OnStopped(const StopInfo& s) override {
        stops.push_back(s);
        resume = Resume::Continue;   //per-stop default
        if (onStopped) onStopped(*controller);
    }
    void OnExited(int) override {}
    void OnRuntimeError(const std::string&) override {}
    void WaitUntilResume() override {
        switch (resume) {
        case Resume::Continue: controller->Continue(); break;
        case Resume::Into:     controller->StepInto(); break;
        case Resume::Over:     controller->StepOver(); break;
        case Resume::Out:      controller->StepOut(); break;
        }
    }
};

void test_controller_initial_stop()
{
    //gdb `start` behavior: the first statement freezes with reason
    //Initial, before anything has run.
    TEST(controller_initial_stop);
    BuildOutcome b = buildSource("ctrl_init",
        "int main() {\n"      //1
        "    int a = 1;\n"    //2
        "    return a;\n"     //3
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_init");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 1, "program result");
    REQUIRE(fe.stops.size() == 1);
    CHECK(fe.stops[0].reason == StopInfo::Reason::Initial,
        "the first stop is the initial stop");
    CHECK(fe.stops[0].line == 2, "first statement line");
    CHECK(fe.stops[0].depth == 1, "main is depth 1");
    CHECK(fe.stops[0].funcIdx == mod.FindFunction("main"),
        "funcIdx indexes the module's function table");
    PASS();
}

void test_controller_step_semantics()
{
    //s: next statement any depth (descends into calls); f: skips the
    //callee's remaining statements, lands at depth < recorded. Same
    //script the CLI test and the dbg_step_semantics e2e pin: s s f c.
    TEST(controller_step_semantics);
    BuildOutcome b = buildSource("ctrl_step",
        "int inner(int v) {\n"   //1
        "    int r = v + 1;\n"   //2
        "    return r;\n"        //3
        "}\n"                    //4
        "int main() {\n"         //5
        "    int a = 4;\n"       //6
        "    int b = inner(a);\n"//7
        "    return 0;\n"        //8
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_step");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    size_t visit = 0;
    fe.onStopped = [&](DebugSessionController&) {
        if (visit == 0 || visit == 1)
            fe.resume = ScriptedFrontEnd::Resume::Into;
        else if (visit == 2)
            fe.resume = ScriptedFrontEnd::Resume::Out;
        ++visit;   //afterwards: default Continue
    };
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 0, "program result");
    REQUIRE(fe.stops.size() == 4);
    CHECK(fe.stops[0].line == 6
        && fe.stops[0].reason == StopInfo::Reason::Initial,
        "initial stop at main's first statement");
    CHECK(fe.stops[1].line == 7 && fe.stops[1].depth == 1
        && fe.stops[1].reason == StopInfo::Reason::Step,
        "s advances one statement in main");
    CHECK(fe.stops[2].line == 2 && fe.stops[2].depth == 2
        && fe.stops[2].reason == StopInfo::Reason::Step,
        "s descends into the call");
    CHECK(fe.stops[3].line == 8 && fe.stops[3].depth == 1
        && fe.stops[3].reason == StopInfo::Reason::Step,
        "f lands back in main past the callee's remaining statements");
    PASS();
}

void test_controller_step_over()
{
    //n: crosses the callee wholesale — depth <= recorded stops, so the
    //callee's statements never freeze and execution lands on the next
    //same-depth statement.
    TEST(controller_step_over);
    BuildOutcome b = buildSource("ctrl_over",
        "int inner(int v) {\n"   //1
        "    int r = v + 1;\n"   //2
        "    return r;\n"        //3
        "}\n"                    //4
        "int main() {\n"         //5
        "    int a = 4;\n"       //6
        "    int b = inner(a);\n"//7
        "    return 0;\n"        //8
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_over");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    size_t visit = 0;
    fe.onStopped = [&](DebugSessionController&) {
        if (visit <= 1)
            fe.resume = ScriptedFrontEnd::Resume::Over;
        ++visit;   //afterwards: default Continue
    };
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 0, "program result");
    REQUIRE(fe.stops.size() == 3);
    CHECK(fe.stops[0].line == 6
        && fe.stops[0].reason == StopInfo::Reason::Initial,
        "initial stop");
    CHECK(fe.stops[1].line == 7 && fe.stops[1].depth == 1,
        "n advances within main");
    CHECK(fe.stops[2].line == 8 && fe.stops[2].depth == 1,
        "n crosses the call, landing on the next same-depth statement");
    for (const auto& s : fe.stops)
        CHECK(s.line != 2 && s.line != 3, "no stop inside inner");
    PASS();
}

void test_controller_breakpoint_hit_and_count()
{
    //One breakpoint id, one hit per stop: the loop body line fires
    //three times, every hit reported under the same id and counted
    //once.
    TEST(controller_breakpoint_hit_and_count);
    BuildOutcome b = buildSource("ctrl_bp",
        "int main() {\n"        //1
        "    int t = 0;\n"      //2
        "    int i = 1;\n"      //3
        "    while (i <= 3) {\n"//4
        "        t = t + i;\n"  //5
        "        i = i + 1;\n"  //6
        "    }\n"               //7
        "    return t;\n"       //8
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_bp");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    //Suffix spec, as the CLI's `b file.n:LINE` form passes through.
    int id = controller.AddBreakpoint("ctrl_bp.n", 5);
    CHECK(id == 1, "first breakpoint gets id 1");
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 6, "program result (1+2+3)");
    REQUIRE(fe.stops.size() == 4);
    CHECK(fe.stops[0].reason == StopInfo::Reason::Initial,
        "initial stop first");
    for (size_t i = 1; i < fe.stops.size(); ++i) {
        CHECK(fe.stops[i].reason == StopInfo::Reason::Breakpoint,
            "hit " + std::to_string(i) + " reports as Breakpoint");
        CHECK(fe.stops[i].breakpointId == id, "same id every hit");
        CHECK(fe.stops[i].line == 5, "loop body line");
    }
    const auto rows = controller.BreakpointRows();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == id && rows[0].hits == 3,
        "one row, one hit per stop");
    PASS();
}

void test_controller_break_by_func()
{
    //b funcName: binds the function's FIRST statement under one id; the
    //hit lands there and counts once.
    TEST(controller_break_by_func);
    BuildOutcome b = buildSource("ctrl_bpfunc",
        "int inner(int v) {\n"   //1
        "    int r = v + 1;\n"   //2
        "    return r;\n"        //3
        "}\n"                    //4
        "int main() {\n"         //5
        "    int a = 3;\n"       //6
        "    int b = inner(a);\n"//7
        "    return 0;\n"        //8
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_bpfunc");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    int id = controller.AddFunctionBreakpoint("inner");
    CHECK(id == 1, "function breakpoint gets id 1");
    const auto rows = controller.BreakpointRows();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].label == "inner (ctrl_bpfunc.n:2)",
        "label names the resolved first statement, got: "
        + rows[0].label);
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 0, "program result");
    REQUIRE(fe.stops.size() == 2);
    CHECK(fe.stops[0].reason == StopInfo::Reason::Initial
        && fe.stops[0].line == 6, "initial stop in main");
    CHECK(fe.stops[1].reason == StopInfo::Reason::Breakpoint
        && fe.stops[1].breakpointId == id && fe.stops[1].line == 2,
        "hit lands at inner's first statement");
    CHECK(controller.BreakpointRows()[0].hits == 1, "counted once");
    PASS();
}

void test_controller_funcbp_all_same_name()
{
    //One id covers every same-named function's first statement (methods
    //on different classes share the bare-name pool): both methods hit,
    //both hits report the same id.
    TEST(controller_funcbp_all_same_name);
    BuildOutcome b = buildSource("ctrl_funcbp",
        "class Left {\n"                        //1
        "    public int val() { return 4; }\n"  //2
        "}\n"                                   //3
        "class Right {\n"                       //4
        "    public int val() { return 5; }\n"  //5
        "}\n"                                   //6
        "int main() {\n"                        //7
        "    Left l = new Left();\n"            //8
        "    Right r = new Right();\n"          //9
        "    return l.val() + r.val();\n"       //10
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_funcbp");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    int id = controller.AddFunctionBreakpoint("val");
    CHECK(id == 1, "one id for the whole name pool");
    CHECK(controller.BreakpointRows().size() == 1, "one row");
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 9, "program result");
    REQUIRE(fe.stops.size() == 3);
    CHECK(fe.stops[0].reason == StopInfo::Reason::Initial,
        "initial stop in main");
    CHECK(fe.stops[1].reason == StopInfo::Reason::Breakpoint
        && fe.stops[1].breakpointId == id,
        "first same-named method hits the shared id");
    CHECK(fe.stops[2].reason == StopInfo::Reason::Breakpoint
        && fe.stops[2].breakpointId == id,
        "second same-named method hits the same id");
    CHECK(fe.stops[1].funcIdx != fe.stops[2].funcIdx,
        "the two hits are different functions");
    CHECK(controller.BreakpointRows()[0].hits == 2, "both counted once");
    PASS();
}

void test_controller_multianchor_finally_one_id()
{
    //try/finally compiles each finally-body statement TWICE (exception-
    //path copy, then normal-path copy) — two anchors on one line. Both
    //live under ONE breakpoint id: every executed copy reports that id
    //and counts one hit; deleting the id once removes both anchors.
    TEST(controller_multianchor_finally_one_id);
    BuildOutcome b = buildSource("ctrl_multianchor",
        "int main() {\n"             //1
        "    int a = 0;\n"           //2
        "    int i = 0;\n"           //3
        "    while (i < 2) {\n"      //4
        "        i = i + 1;\n"       //5
        "        try {\n"            //6
        "            a = a + 1;\n"   //7
        "        } finally {\n"      //8
        "            a = a + 10;\n"  //9  two anchors
        "        }\n"                //10
        "    }\n"                    //11
        "    return a;\n"            //12
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_multianchor");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    int id = controller.AddBreakpoint(
        (scratchDir() / "ctrl_multianchor.n").string(), 9);
    CHECK(id == 1, "one id for the whole line");
    CHECK(controller.BreakpointRows().size() == 1, "one row");
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 22, "program result (2 iterations)");
    REQUIRE(fe.stops.size() == 3);
    CHECK(fe.stops[0].reason == StopInfo::Reason::Initial,
        "initial stop first");
    for (size_t i = 1; i < fe.stops.size(); ++i) {
        CHECK(fe.stops[i].reason == StopInfo::Reason::Breakpoint
            && fe.stops[i].breakpointId == id && fe.stops[i].line == 9,
            "finally line hit " + std::to_string(i)
            + " reports the shared id");
    }
    CHECK(controller.BreakpointRows()[0].hits == 2,
        "one hit per executed anchor copy");
    //Deleting the id once must release BOTH anchors: a second run over
    //the same session stops nowhere.
    CHECK(controller.DeleteBreakpoint(id), "delete succeeds");
    CHECK(!controller.DeleteBreakpoint(id), "second delete fails");
    fe.stops.clear();
    CHECK(exec.Execute(mod) == 22, "re-run result");
    CHECK(fe.stops.empty(), "both anchors are gone after one delete");
    PASS();
}

void test_controller_multianchor_exception_copy()
{
    //The exception-path anchor of a finally line belongs to the SAME
    //breakpoint: when an exception unwinds through the finally, the
    //handler-region copy of the line hits the id (the normal-path copy
    //never runs in this program, so the single Breakpoint stop can only
    //come from the exception copy).
    TEST(controller_multianchor_exception_copy);
    BuildOutcome b = buildSource("ctrl_multianchor_exc",
        "int main() {\n"                            //1
        "    int a = 0;\n"                          //2
        "    try {\n"                               //3
        "        try {\n"                           //4
        "            throw new Exception(\"x\");\n" //5
        "        } finally {\n"                     //6
        "            a = a + 10;\n"                 //7
        "        }\n"                               //8
        "    } catch (Exception e) {\n"             //9
        "        a = a + 100;\n"                    //10
        "    }\n"                                   //11
        "    return a;\n"                           //12
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_multianchor_exc");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    int id = controller.AddBreakpoint(
        (scratchDir() / "ctrl_multianchor_exc.n").string(), 7);
    CHECK(id == 1, "bound");
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 110, "program result");
    REQUIRE(fe.stops.size() == 2);
    CHECK(fe.stops[0].reason == StopInfo::Reason::Initial,
        "initial stop first");
    CHECK(fe.stops[1].reason == StopInfo::Reason::Breakpoint
        && fe.stops[1].breakpointId == id && fe.stops[1].line == 7,
        "exception-path copy hits the same line's id");
    PASS();
}

void test_controller_unbound_lines()
{
    //Lines without a statement anchor (signatures, braces, past EOF)
    //are unbound: id 0, nothing stored. A repeated request returns the
    //same id — the table key is the normalized (file, line) pair.
    TEST(controller_unbound_lines);
    BuildOutcome b = buildSource("ctrl_unbound",
        "int main() {\n"      //1
        "    int a = 1;\n"    //2
        "    return a;\n"     //3
        "}\n");               //4
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_unbound");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    const std::string src = (scratchDir() / "ctrl_unbound.n").string();
    CHECK(controller.AddBreakpoint(src, 1) == 0,
        "function signature line has no anchor");
    CHECK(controller.AddBreakpoint(src, 4) == 0,
        "closing brace line has no anchor");
    CHECK(controller.AddBreakpoint(src, 99) == 0,
        "line past EOF has no anchor");
    CHECK(controller.BreakpointRows().empty(),
        "unbound requests store nothing");
    int id = controller.AddBreakpoint(src, 2);
    CHECK(id != 0, "a statement line binds");
    CHECK(controller.AddBreakpoint(src, 2) == id,
        "re-adding the line returns the same id");
    CHECK(controller.BreakpointRows().size() == 1,
        "the duplicate did not create a second row");
    PASS();
}

void test_controller_window_discipline()
{
    //Resume commands and View() outside the frozen window are front-end
    //programming errors — refused loudly, and the session keeps working
    //afterwards (no resume state leaked out of the window).
    TEST(controller_window_discipline);
    BuildOutcome b = buildSource("ctrl_window",
        "int main() {\n"      //1
        "    int a = 1;\n"    //2
        "    return a;\n"     //3
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_window");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    bool threw = false;
    try { controller.Continue(); }
    catch (const std::logic_error&) { threw = true; }
    CHECK(threw, "Continue outside the frozen window throws");
    threw = false;
    try { controller.StepOver(); }
    catch (const std::logic_error&) { threw = true; }
    CHECK(threw, "StepOver outside the frozen window throws");
    threw = false;
    try { (void)controller.View(); }
    catch (const std::logic_error&) { threw = true; }
    CHECK(threw, "View outside the frozen window throws");
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 1, "program result after refused calls");
    REQUIRE(fe.stops.size() == 1);
    CHECK(fe.stops[0].reason == StopInfo::Reason::Initial,
        "initial stop intact (the refused resumes queued nothing)");
    PASS();
}

void test_controller_break_on_throw()
{
    //SetBreakOnThrow freezes at the throw site (reason Throw) before
    //the unwind; default off never stops there.
    TEST(controller_break_on_throw);
    const std::string source =
        "int main() {\n"                             //1
        "    int a = 1;\n"                           //2
        "    try {\n"                                //3
        "        throw new Exception(\"boom\");\n"   //4
        "    } catch (Exception e) {\n"              //5
        "        return 7;\n"                        //6
        "    }\n"                                    //7
        "}\n";                                       //8
    BuildOutcome b = buildSource("ctrl_throw", source);
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    {
        CompiledModule mod = loadBuilt("ctrl_throw");
        ScriptedFrontEnd fe;
        DebugSessionController controller(mod, fe);
        fe.controller = &controller;
        VmExecutor exec;
        exec.SetDebugHooks(&controller);
        CHECK(exec.Execute(mod) == 7, "program result");
        REQUIRE(fe.stops.size() == 1);
        CHECK(fe.stops[0].reason == StopInfo::Reason::Initial,
            "default off: no stop at the throw");
    }
    {
        CompiledModule mod = loadBuilt("ctrl_throw");
        ScriptedFrontEnd fe;
        DebugSessionController controller(mod, fe);
        fe.controller = &controller;
        controller.SetBreakOnThrow(true);
        CHECK(controller.BreakOnThrow(), "getter echoes the state");
        VmExecutor exec;
        exec.SetDebugHooks(&controller);
        CHECK(exec.Execute(mod) == 7, "catch still completes the program");
        REQUIRE(fe.stops.size() == 2);
        CHECK(fe.stops[1].reason == StopInfo::Reason::Throw
            && fe.stops[1].line == 4 && fe.stops[1].depth == 1,
            "frozen at the throw site with the stack alive");
    }
    PASS();
}

// --- Task 5: SourceCache / l / x / catch ---

void test_sourcecache_resolution()
{
    //As-recorded path resolves; stale recorded path falls back to the
    //.nmod's directory by basename; unresolvable degrades to empty
    //(and the negative result is cached).
    TEST(sourcecache_resolution);
    BuildOutcome b = buildSource("src_cache",
        "int main() {\n"      //1
        "    return 0;\n"     //2
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("src_cache");
    int idx = mod.FindFunction("main");
    REQUIRE(idx >= 0);
    const auto& mainf = mod.functions[static_cast<size_t>(idx)];
    SourceCache cache((scratchDir() / "src_cache.nmod").string());
    const auto& lines = cache.Lines(mainf.sourceFile);
    REQUIRE(lines.size() >= 3);
    CHECK(lines[0].find("int main()") != std::string::npos,
        "as-recorded path resolves to the real source");
    {
        std::ofstream out(scratchDir() / "sibling.n", std::ios::binary);
        out << "line one\nline two\n";
    }
    const auto& sib = cache.Lines("D:\\gone\\away\\sibling.n");
    CHECK(sib.size() == 2 && sib[1] == "line two",
        "stale recorded path falls back to the .nmod directory");
    const auto& none = cache.Lines("Z:\\no\\such\\file.n");
    CHECK(none.empty(), "unresolvable file degrades to empty");
    CHECK(&cache.Lines("Z:\\no\\such\\file.n") == &none,
        "negative result is cached (stable reference)");
    PASS();
}

void test_session_source_list()
{
    TEST(session_source_list);
    std::string out = RunSession("sess_list",
        "int fib(int n) {\n"                  //1
        "    if (n < 2) {\n"                  //2
        "        return n;\n"                 //3
        "    }\n"                             //4
        "    return fib(n - 1) + fib(n - 2);\n"//5
        "}\n"                                 //6
        "int main() {\n"                      //7
        "    int r = fib(10);\n"              //8
        "    return r - 55;\n"                //9
        "}\n",                                //10
        "b 9\nc\nl\nc\n");
    //Initial stop is main:8 (the first statement) — a breakpoint on 8
    //can never hit again, so the script breaks on 9 instead. Window is
    //centered on the stop line: marker on 9, neighbors 4..13 unmarked.
    //Line format = 2-char marker + 5-char right-aligned number + TAB
    //(unmarked line 4 = 6 spaces + '4').
    CHECK(out.find("->    9\t") != std::string::npos,
        "stop line carries the -> marker");
    CHECK(out.find("      4\t") != std::string::npos,
        "window shows unmarked lines (4 = 9 - 5)");
    CHECK(out.find("     10\t") != std::string::npos,
        "window spans past the stop line (10 = 9 + 1 < 9 + 5)");
    PASS();
}

void test_session_disasm_marker()
{
    TEST(session_disasm_marker);
    std::string out = RunSession("sess_x",
        "int main() {\n"       //1
        "    int a = 6;\n"     //2
        "    int b = a * 7;\n" //3
        "    return b - 42;\n" //4
        "}\n",
        "x\nc\n");
    //`x` at the initial stop: the first statement's pc carries the
    //>> marker; the rest are blank-marked disassembly lines.
    CHECK(out.find(">>  00") != std::string::npos,
        "current pc is marked with >>");
    CHECK(out.find("    00") != std::string::npos,
        "other instructions are blank-marked");
    PASS();
}

void test_session_catch_throw()
{
    TEST(session_catch_throw);
    std::string out = RunSession("sess_catch",
        "int main() {\n"                             //1
        "    int a = 1;\n"                           //2
        "    try {\n"                                //3
        "        throw new Exception(\"boom\");\n"   //4
        "    } catch (Exception e) {\n"              //5
        "        return 7;\n"                        //6
        "    }\n"                                    //7
        "}\n",
        "catch on\nc\nc\n");
    //The throw statement's own OP_DebugInfo sets currentLine=4 before
    //OP_Throw runs, so the Throw: report anchors at line 4 with the
    //stack still alive; the second c completes the unwind into catch.
    CHECK(out.find("Break on throw: on") != std::string::npos,
        "catch on echoes the new state");
    CHECK(out.find("Throw: main (sess_catch.n:4)") != std::string::npos,
        "throw site freeze report");
    PASS();
}

// --- Machine-mode debugging: host I/O seam ---

//Captures what io.print emits. IsInputAvailable() keeps the interface
//default (false): output-capable only.
class CapturingHostIo : public IHostIo
{
public:
    std::string captured;
    void OnOutput(std::string_view text) override { captured.append(text); }
};

void test_hostio_output_capture()
{
    //Output bytes arrive verbatim, including the '\n' io.print appends.
    TEST(hostio_output_capture);
    BuildOutcome b = buildSource("hostio_print",
        "import io;\n"
        "int main() { io.print(\"hi\"); return 0; }\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("hostio_print");
    VmExecutor exec;
    CapturingHostIo io;
    exec.SetHostIo(&io);
    CHECK(exec.Execute(mod) == 0, "program result");
    CHECK(io.captured == "hi\n", "expected \"hi\\n\", got: " + io.captured);
    PASS();
}

void test_hostio_readline_rejected()
{
    //A host without input must make io.readLine raise IOException
    //(uncaught here -> NLangThrow escapes Execute) instead of silently
    //consuming the embedder's stream.
    TEST(hostio_readline_rejected);
    BuildOutcome b = buildSource("hostio_readline",
        "import io;\n"
        "int main() { string s = io.readLine(); return 0; }\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("hostio_readline");
    VmExecutor exec;
    CapturingHostIo io;
    exec.SetHostIo(&io);
    std::string raiseText;
    bool raised = false;
    try {
        exec.Execute(mod);
    } catch (const NLangThrow& ex) {
        raised = true;
        raiseText = ex.what();
    }
    CHECK(raised, "readLine must raise IOException when input is unavailable");
    //Identity pin: the raise must come from the io.readLine site itself,
    //not some other channel that happens to throw.
    CHECK(raiseText.rfind("io.readLine", 0) == 0,
        "raise must originate at io.readLine, got: " + raiseText);
    PASS();
}

// --- Loop per-iteration anchors (VmBackend) ---

//Loop anchors must fire every iteration (the back edge has to land on
//the condition-entry anchor). The sentinel C++ exception escapes
//Execute cleanly (pinned by hooks_cpp_exception_propagates) — that is
//the in-process "halt" for an otherwise-infinite loop.
struct StopCountingHooks : IDebugHooks
{
    int stops = 0;
    std::vector<uint16_t> lines;
    void OnStatement(const DebugStopInfo& info, IVmDebugView&) override
    {
        ++stops;
        lines.push_back(info.line);
        if (stops >= 3)
            throw std::runtime_error("sentinel");
    }
    void OnThrow(const DebugStopInfo&, IVmDebugView&) override {}
};

void test_loop_anchor_per_iteration()
{
    //spin's body is empty, so its while line is the only repeatable
    //anchor. The pinned stop sequence nails the fix exactly: stop 1 is
    //the `spin();` call-statement anchor in main (line 6), then one stop
    //per iteration on the while line (2) — neither more nor fewer. Before
    //the fix the loop anchor fired only at entry and the empty back edge
    //never re-hit it, so no 3rd stop existed and Execute hung instead of
    //raising the sentinel.
    TEST(loop_anchor_per_iteration);
    BuildOutcome b = buildSource("loop_anchor",
        "int spin() {\n"        //1
        "    while (1) {}\n"    //2
        "    return 0;\n"       //3
        "}\n"                   //4
        "int main() {\n"        //5
        "    spin();\n"         //6
        "    return 0;\n"       //7
        "}\n");                 //8
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("loop_anchor");
    VmExecutor exec;
    StopCountingHooks hooks;
    exec.SetDebugHooks(&hooks);
    bool sentinel = false;
    try {
        exec.Execute(mod);
    } catch (const std::runtime_error&) {
        sentinel = true;
    }
    CHECK(sentinel, "3rd stop must raise the sentinel (per-iteration "
          "anchors keep a halt reachable in an empty-body loop)");
    CHECK(hooks.lines.size() == 3, "expected exactly 3 stops, got: "
          + std::to_string(hooks.lines.size()));
    std::string seq;
    for (uint16_t l : hooks.lines) {
        if (!seq.empty()) seq += ", ";
        seq += std::to_string(l);
    }
    CHECK(hooks.lines[0] == 6 && hooks.lines[1] == 2
          && hooks.lines[2] == 2,
          "stop sequence must be {6, 2, 2} (main's spin() call, then the "
          "while line once per iteration), got: {" + seq + "}");
    PASS();
}

int main()
{
    //In-process host init: ModuleBuilder's Build() dereferences the
    //IdString static tables — StaticInit must run first or Build()
    //segfaults (a crash try/catch cannot intercept).
    Runtime::StaticInit();

    test_v19_sourcefile_single_tu();
    test_v19_import_roundtrip();
    test_v19_loader_rejects_v1_8();
    test_v19_import_gc_roots();

    test_hooks_line_sequence();
    test_hooks_call_depths();
    test_view_frames_and_locals();
    test_hooks_on_throw();
    test_view_value_kinds();
    test_hooks_cpp_exception_propagates();
    test_linepc_map_first_pc();
    test_linepc_map_same_line_multi_anchor();
    test_linepc_map_finally_duplicates();
    test_linepc_map_finally_single_two_copies();
    test_instruction_stride_exact_landing();

    test_session_initial_stop_and_continue();
    test_session_step_semantics();
    test_session_breakpoint_hit();
    test_session_bt_and_locals();
    test_session_frame_select();
    test_session_break_by_func();

    test_controller_initial_stop();
    test_controller_step_semantics();
    test_controller_step_over();
    test_controller_breakpoint_hit_and_count();
    test_controller_break_by_func();
    test_controller_funcbp_all_same_name();
    test_controller_multianchor_finally_one_id();
    test_controller_multianchor_exception_copy();
    test_controller_unbound_lines();
    test_controller_window_discipline();
    test_controller_break_on_throw();

    test_sourcecache_resolution();
    test_session_source_list();
    test_session_disasm_marker();
    test_session_catch_throw();

    test_hostio_output_capture();
    test_hostio_readline_rejected();

    test_loop_anchor_per_iteration();

    std::cerr << "\ndebugger_tests: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail > 0 ? 1 : 0;
}
