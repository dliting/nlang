// --- ndb debugger unit tests ---
// In-process compile+run via the public ModuleBuilder API (see
// test_stdlib.cpp for the harness rationale). Covers the .nmod v1.9
// sourceFile field, the B.1 imported-locals fix, debug hooks, the
// read-only view, and DebugSession stepping semantics (Task 4).
// MUST call Runtime::StaticInit() before any Build() (IdString tables).

#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "VmExecutor.h"
#include "IDebugHooks.h"
#include "ModuleLoader.h"
#include "Disassembler.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
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

void test_linepc_map_same_line_collapse()
{
    //CONSECUTIVE same-line anchors collapse to the first entry: the
    //multi-declarator line and the one-line if/else each yield ONE
    //entry, not one per declarator/statement (post-D7 each declarator's
    //initializer AssignStmt carries its own anchor — all on one line).
    TEST(linepc_map_same_line_collapse);
    BuildOutcome b = buildSource("linepc_collapse",
        "int main() {\n"                               //1
        "    int a = 1, b = 2;\n"                      //2 (two anchors)
        "    if (a > b) { a = 3; } else { b = 4; }\n"  //3 (three anchors)
        "    return a + b;\n"                          //4
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("linepc_collapse");
    int mainIdx = mod.FindFunction("main");
    REQUIRE(mainIdx >= 0);
    const auto& mainf = mod.functions[static_cast<size_t>(mainIdx)];
    auto map = BuildLinePcMap(mainf);
    //Without collapse this map would hold 2 + 3 + 1 = 6 entries.
    CHECK(map.size() == 3, "one entry per line, not per anchor");
    CHECK(map[0].line == 2 && map[1].line == 3 && map[2].line == 4,
        "lines 2,3,4 in order");
    CHECK(map[0].pc < map[1].pc && map[1].pc < map[2].pc, "pcs ascend");
    PASS();
}

void test_linepc_map_finally_duplicates()
{
    //try/finally emits each finally-body statement's anchor TWICE: the
    //exception-path copy in the handler region FIRST, then the
    //normal-path copy. With >=2 finally statements the copies are
    //non-adjacent markers, so BOTH survive in the map (review probe
    //temp/linepc_probe.cpp; a single-statement body's copies are
    //consecutive and collapse — pinned separately below).
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

void test_linepc_map_finally_single_collapses()
{
    //Characterization (do NOT read as an endorsement): a SINGLE-statement
    //finally body's exception-path and normal-path anchors are CONSECUTIVE
    //same-line markers, so they collapse to the exception-path entry —
    //the normal-path pc is NOT in the map. T4 breakpoint placement must
    //not assume the map covers every execution path of a finally line.
    TEST(linepc_map_finally_single_collapses);
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
    //Observed marker stream: 2,3,4,6,6,8 — the two L6 anchors collapse.
    REQUIRE(map.size() == 5);
    const uint16_t want[] = {2, 3, 4, 6, 8};
    for (size_t i = 0; i < map.size(); ++i)
        CHECK(map[i].line == want[i],
            "entry " + std::to_string(i) + " line");
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
    test_linepc_map_same_line_collapse();
    test_linepc_map_finally_duplicates();
    test_linepc_map_finally_single_collapses();
    test_instruction_stride_exact_landing();

    std::cerr << "\ndebugger_tests: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail > 0 ? 1 : 0;
}
