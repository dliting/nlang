// Stage-2 task 2: comma index lists and ND creation resolution.
// Resolution reaches the creation/subscript branches and the emit-side
// temporary gates fire named diagnostics (ND emission lands in task 4).
// In-process pipeline per test_native_array_abi.cpp (static-init contract).
#include "test_support/test_scratch.h"

#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include <nlang/compiler/CastInfo.h>
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "nlang/vm/NativeHost.h"
#include "VmExecutor.h"
#include "NcuLoader.h"
#include "NcuLinker.h"
#include <cstdlib>
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

static std::filesystem::path scratchDir()
{
    static const auto dir = nlang::test::ScratchDir("nlang_test_nd_resolve");
    std::filesystem::create_directories(dir);
    return dir;
}

struct BuildOutcome
{
    bool ok = false;
    std::string diagnostics;
};

static BuildOutcome buildSource(const std::string& tag,
    const std::string& source)
{
    const auto dir = scratchDir();
    const auto nPath = dir / (tag + ".n");
    const auto modPath = dir / (tag + ".ncu");
    std::filesystem::remove(modPath);

    {
        std::ofstream out(nPath, std::ios::binary);
        out << "import io;\n" << source;
    }

    BuildParams params;
    params.m_SourceFiles.push_back(nPath.string());
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;

    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);

    BuildOutcome outcome;
    bool built = false;
    try {
        built = builder.BuildArtifacts();
    } catch (const std::exception& e) {
        outcome.diagnostics += std::string("internal error: ") + e.what();
        return outcome;
    }

    outcome.ok = built && std::filesystem::exists(modPath);
    for (auto it = logger.cbegin(); it != logger.cend(); ++it)
        outcome.diagnostics += (*it)->Message() + "\n";
    return outcome;
}

static bool DiagHas(const BuildOutcome& o, const char* text)
{
    return o.diagnostics.find(text) != std::string::npos;
}

//ND creation resolves and emits (the Task-2 temporary gate is
//gone; runtime behavior is covered by the ndarray e2e suite).
void test_nd_creation_resolves_to_gate()
{
    TEST(nd_creation_emits);
    const BuildOutcome o = buildSource("nd_create",
        "int main() {\n"
        "    int[,] a = int[2,3];\n"
        "    return 0;\n"
        "}\n");
    CHECK(o.ok, "comma creation must compile (gate removed in task 4)");
    CHECK(!DiagHas(o, "syntax error"),
        "comma creation must PARSE (no syntax error)");
    PASS();
}

//Subscript arity: index count must equal the rank.
void test_subscript_arity_diagnostics()
{
    TEST(subscript_arity_diagnostics);
    const BuildOutcome few = buildSource("nd_arity_few",
        "int main() {\n"
        "    int[,] a = int[2,3];\n"
        "    int v = a[1];\n"
        "    return v;\n"
        "}\n");
    CHECK(!few.ok && DiagHas(few, "indices") && !DiagHas(few, "syntax error"),
        "one index on rank-2 must name the arity mismatch");
    const BuildOutcome many = buildSource("nd_arity_many",
        "int main() {\n"
        "    int[,] a = int[2,3];\n"
        "    int v = a[1,2,3];\n"
        "    return v;\n"
        "}\n");
    CHECK(!many.ok && DiagHas(many, "indices"),
        "three indices on rank-2 must name the arity mismatch");
    PASS();
}

//Jagged chains stay rejected; the rank does not confuse the depth walk.
void test_jagged_still_rejected()
{
    TEST(jagged_still_rejected);
    const BuildOutcome o = buildSource("nd_jagged2",
        "int main() {\n"
        "    double[,][] b = null;\n"
        "    return 0;\n"
        "}\n");
    CHECK(!o.ok && DiagHas(o, "jagged"),
        "mixed ND/jagged nesting must stay rejected");
    PASS();
}

//Every ND value path emits (the Task-2 gates are gone; task 5
//landed the rectangular init-list lowering).
void test_emit_gates_cover_nd_paths()
{
    TEST(nd_paths_emit_gates_gone);
    //subscript read + compound assign (+= on an element)
    const BuildOutcome read = buildSource("nd_gate_read",
        "int main() {\n"
        "    int[,] a = int[2,3];\n"
        "    int v = a[0,0];\n"
        "    return v;\n"
        "}\n");
    CHECK(read.ok, "subscript read must emit");

    const BuildOutcome comp = buildSource("nd_gate_comp",
        "int main() {\n"
        "    int[,] a = int[2,3];\n"
        "    a[0,0] += 1;\n"
        "    return 0;\n"
        "}\n");
    CHECK(comp.ok, "compound element assign must emit");

    //subscript store (statement form) and field store of an element
    const BuildOutcome store = buildSource("nd_gate_store",
        "int main() {\n"
        "    int[,] a = int[2,3];\n"
        "    a[0,0] = 1;\n"
        "    return 0;\n"
        "}\n");
    CHECK(store.ok, "subscript store must emit");

    //foreach over an ND source
    const BuildOutcome fe = buildSource("nd_gate_foreach",
        "int main() {\n"
        "    int[,] a = int[2,3];\n"
        "    int s = 0;\n"
        "    foreach (int v in a) s = s + v;\n"
        "    return s;\n"
        "}\n");
    CHECK(fe.ok, "foreach over ND must emit");

    //init-list target with ND type: parses, but ND init-list resolution
    const BuildOutcome il = buildSource("nd_gate_initlist",
        "int main() {\n"
        "    int[,] a = [[1,2],[3,4]];\n"
        "    return 0;\n"
        "}\n");
    CHECK(!DiagHas(il, "syntax error"),
        "nested bracket init list must parse");
    CHECK(il.ok, "ND init-list emits (task 5 lowering)");
    PASS();
}

//Rank cap: the grammar and the creation twin reject ranks above the
//language maximum (64) with a named diagnostic, while every rank up to
//the cap parses and type-checks (rank-9 tokens previously compiled and
//died at runtime in an uncatchable VM range throw). Comma counts that
//would truncate on narrowing to uint8 must be rejected before that.
void test_nd_rank_cap_diagnostics()
{
    TEST(nd_rank_cap_diagnostics);
    const std::string commas9 = std::string(8, ',');
    const BuildOutcome decl9 = buildSource("nd_rank9_ok",
        "int main() {\n"
        "    int[" + commas9 + "] a;\n"
        "    return 0;\n"
        "}\n");
    CHECK(decl9.ok, "rank-9 declaration is legal at cap 64");
    const BuildOutcome create9 = buildSource("nd_rank9_create_ok",
        "int main() {\n"
        "    int[" + commas9 + "] a = int[1,2,3,4,5,6,7,8,9];\n"
        "    return 0;\n"
        "}\n");
    CHECK(create9.ok, "rank-9 creation is legal at cap 64");
    const BuildOutcome edge64 = buildSource("nd_rank64_ok",
        "int main() {\n"
        "    int[" + std::string(63, ',') + "] a;\n"
        "    return 0;\n"
        "}\n");
    CHECK(edge64.ok, "rank-64 declaration stays legal");
    const BuildOutcome over = buildSource("nd_rank65_decl",
        "int main() {\n"
        "    int[" + std::string(64, ',') + "] a;\n"
        "    return 0;\n"
        "}\n");
    CHECK(!over.ok && DiagHas(over, "exceeds the maximum of 64")
        && !DiagHas(over, "syntax error"),
        "rank-65 declaration must name the rank cap 64");
    const BuildOutcome wide = buildSource("nd_rank301_decl",
        "int main() {\n"
        "    int[" + std::string(300, ',') + "] a;\n"
        "    return 0;\n"
        "}\n");
    CHECK(!wide.ok && DiagHas(wide, "exceeds the maximum")
        && !DiagHas(wide, "syntax error"),
        "rank-301 tokens must be rejected before uint8 truncation");
    const BuildOutcome edge = buildSource("nd_rank8_ok",
        "int main() {\n"
        "    int[,,,,,,,] a;\n"
        "    return 0;\n"
        "}\n");
    CHECK(edge.ok, "rank-8 declaration stays legal");
    PASS();
}

//Brace rows above the leaf level are depth mismatches, not rows: the
//shape gate and the emitter share one row predicate (bracket form).
void test_brace_row_is_depth_mismatch()
{
    TEST(brace_row_is_depth_mismatch);
    const BuildOutcome o = buildSource("nd_brace_row",
        "int main() {\n"
        "    int[,] m = [{{1, 2}, {3, 4}}];\n"
        "    return 0;\n"
        "}\n");
    CHECK(!o.ok && DiagHas(o, "depth does not match")
        && !DiagHas(o, "syntax error"),
        "brace rows must name the depth mismatch");
    PASS();
}
//--- D3: dynamic-shape (expression) entries in ND initializers ---------------

//Shared row factory: returns a fresh int[w] row.
static const char* kDynRowFactory =
    "int[] makeRow(int w) {\n"
    "    int[] r = int[w];\n"
    "    for (int i = 0; i < w; i += 1) {\n"
    "        r[i] = i;\n"
    "    }\n"
    "    return r;\n"
    "}\n";

//An array-valued entry of the matching row rank resolves (D3 rule 2).
void test_nd_initlist_dynamic_row_expr()
{
    TEST(nd_initlist_dynamic_row_expr);
    const BuildOutcome o = buildSource("nd_dyn_row",
        std::string(kDynRowFactory) +
        "int main() {\n"
        "    int[,] m = [makeRow(2), makeRow(2)];\n"
        "    return 0;\n"
        "}\n");
    CHECK(!DiagHas(o, "depth does not match"),
        "expression rows must not hit the literal-depth gate");
    CHECK(!DiagHas(o, "ncompatible"),
        "well-typed expression rows must pass the cast adjudication");
    //Task 6: the dynamic lowering (OP_AssembleArrayN) makes the build
    //green end to end.
    CHECK(o.ok, "expression rows must lower via the dynamic arm");
    PASS();
}

//Deep mixing: a rank-3 target mixes a 2-D literal row with an
//expression row returning int[,] (D3: same-level mixing is legal).
void test_nd_initlist_dynamic_mixed_rows()
{
    TEST(nd_initlist_dynamic_mixed_rows);
    const BuildOutcome o = buildSource("nd_dyn_mixed",
        "int[,] makeRow2(int w) {\n"
        "    int[,] r = int[1,w];\n"
        "    return r;\n"
        "}\n"
        "int main() {\n"
        "    int[,,] t = [[[1, 2]], makeRow2(2)];\n"
        "    return 0;\n"
        "}\n");
    CHECK(!DiagHas(o, "depth does not match"),
        "mixed rows must not hit the literal-depth gate");
    CHECK(!DiagHas(o, "ncompatible"),
        "well-typed mixed rows must pass the cast adjudication");
    //Task 6: mixed rows lower through the same dynamic arm.
    CHECK(o.ok, "mixed rows must lower via the dynamic arm");
    PASS();
}

//A scalar entry where a row belongs takes the existing cast
//diagnostic family (no new dedicated diagnostic).
void test_nd_initlist_dynamic_scalar_entry_rejected()
{
    TEST(nd_initlist_dynamic_scalar_entry);
    const BuildOutcome o = buildSource("nd_dyn_scalar",
        "int main() {\n"
        "    int[,] m = [42];\n"
        "    return 0;\n"
        "}\n");
    CHECK(!o.ok,
        "a scalar entry where a row belongs must be rejected");
    CHECK(DiagHas(o, "ncompatible"),
        "the rejection must come from the cast diagnostics");
    PASS();
}

//A rank-mismatched expression entry (int[] into an int[,,] target
//whose rows are int[,]) is rejected by the same family.
void test_nd_initlist_dynamic_rank_mismatch_rejected()
{
    TEST(nd_initlist_dynamic_rank_mismatch);
    const BuildOutcome o = buildSource("nd_dyn_rank",
        std::string(kDynRowFactory) +
        "int main() {\n"
        "    int[,,] t = [makeRow(2)];\n"
        "    return 0;\n"
        "}\n");
    CHECK(!o.ok,
        "an int[] entry into an int[,,] target must be rejected");
    CHECK(DiagHas(o, "an array value only converts to the same array type"),
        "the rank mismatch must come from the cast diagnostics");
    PASS();
}

//Comma sizes in declaration position parse so the parser can name
//the C-style form and point at the creation twin (0.10.7 D3:
//parse, then reject with guidance).
void test_decl_comma_size_named_reject()
{
    TEST(decl_comma_size_named_reject);
    const char *msg = "array declarations take one size per bracket";
    const BuildOutcome bare = buildSource("nd_declsize_bare",
        "int main() {\n"
        "    int a[2, 3];\n"
        "    return 0;\n"
        "}\n");
    CHECK(!bare.ok && DiagHas(bare, msg)
        && !DiagHas(bare, "syntax error"),
        "bare comma-size declaration must name the creation twin");
    const BuildOutcome init = buildSource("nd_declsize_init",
        "int main() {\n"
        "    int a[2, 3] = 1;\n"
        "    return 0;\n"
        "}\n");
    CHECK(!init.ok && DiagHas(init, msg),
        "comma-size with initializer must name the creation twin");
    const BuildOutcome cont = buildSource("nd_declsize_cont",
        "int main() {\n"
        "    int x = 1, a[2, 3];\n"
        "    return 0;\n"
        "}\n");
    CHECK(!cont.ok && DiagHas(cont, msg),
        "continuation declarator must name the creation twin");
    const BuildOutcome binit = buildSource("nd_declsize_binit",
        "int main() {\n"
        "    int a[2, 3] = {1};\n"
        "    return 0;\n"
        "}\n");
    CHECK(!binit.ok && DiagHas(binit, msg),
        "brace-initializer comma-size must name the creation twin");
    const BuildOutcome cinit = buildSource("nd_declsize_cinit",
        "int main() {\n"
        "    int x = 1, a[2, 3] = 2;\n"
        "    return 0;\n"
        "}\n");
    CHECK(!cinit.ok && DiagHas(cinit, msg),
        "continuation with initializer must name the creation twin");
    const BuildOutcome cbinit = buildSource("nd_declsize_cbinit",
        "int main() {\n"
        "    int x = 1, a[2, 3] = {1};\n"
        "    return 0;\n"
        "}\n");
    CHECK(!cbinit.ok && DiagHas(cbinit, msg),
        "continuation with brace initializer must name the creation twin");
    PASS();
}

//The two D-8 workload axes of the reject soak. NLANG_TEST_ROUNDS is the
//round count this config's registration asks for (tests/CMakeLists.txt
//gives 100 to plain trees and 30 to the instrumented ASan tree so the
//same detector fits every timed leg); NLANG_TEST_WORK_FACTOR scales the
//whole workload uniformly for the work-factor rehearsal the timing
//checker drives. A missing or malformed value is named on stderr and
//falls back, so the self-report line always states the rounds actually
//run rather than the ones someone meant to set.
static constexpr int DEFAULT_SOAK_ROUNDS = 100;
static constexpr long MAX_ENV_COUNT = 1000000;

static int positiveEnvCount(const char *name, int fallback)
{
    const char *raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0')
        return fallback;
    char *end = nullptr;
    const long value = std::strtol(raw, &end, 10);
    if (*end != '\0' || value < 1 || value > MAX_ENV_COUNT) {
        std::cerr << "warning: " << name << "=\"" << raw
            << "\" is not a positive count; using " << fallback << "\n";
        return fallback;
    }
    return static_cast<int>(value);
}

static int soakRounds()
{
    //A sub-unity factor cannot happen here (positiveEnvCount already
    //refuses it), so this is exactly max(factor, 1) x rounds. Each
    //input is capped at MAX_ENV_COUNT on its own, yet the product of
    //two capped inputs can reach 1e12 and overflow int, so the product
    //carries the same named bound: the soak length stays representable
    //and the self-report states the rounds actually run instead of a
    //wrapped value (the clamp names itself, as malformed inputs do).
    const int rounds = positiveEnvCount("NLANG_TEST_ROUNDS",
        DEFAULT_SOAK_ROUNDS);
    const int factor = positiveEnvCount("NLANG_TEST_WORK_FACTOR", 1);
    const long long product = 1LL * rounds * factor;
    if (product > MAX_ENV_COUNT)
    {
        std::cerr << "warning: rounds x factor = " << product
            << " exceeds the soak bound; using " << MAX_ENV_COUNT << "\n";
        return static_cast<int>(MAX_ENV_COUNT);
    }
    return static_cast<int>(product);
}

//ScopedEnv pins an environment variable for one check and restores (or
//unsets) it on scope exit: CHECK returns early, so plain set/restore
//pairs would leak the value into the cases that follow. An empty value
//unsets under MSVC's _putenv_s, matching POSIX unsetenv.
class ScopedEnv
{
public:
    ScopedEnv(const char *name, const char *value) : m_name(name)
    {
        const char *prev = std::getenv(name);
        m_hadPrev = prev != nullptr;
        if (m_hadPrev)
            m_prev = prev;
        setRaw(value);
    }
    ~ScopedEnv() { setRaw(m_hadPrev ? m_prev.c_str() : nullptr); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void setRaw(const char *value)
    {
#ifdef _WIN32
        _putenv_s(m_name.c_str(), value != nullptr ? value : "");
#else
        if (value != nullptr)
            ::setenv(m_name.c_str(), value, 1);
        else
            ::unsetenv(m_name.c_str());
#endif
    }
    std::string m_name;
    std::string m_prev;
    bool m_hadPrev = false;
};

void test_decl_comma_size_reject_stress()
{
    TEST(decl_comma_size_reject_stress);
    //The reject branches now explicitly delete the abandoned
    //ConcreteParamList (container + elements). A botched ownership
    //transfer surfaces here as a crash or heap corruption over enough
    //iterations; the diagnostic must stay stable throughout. All six
    //declared forms rotate so every delete site (first-declarator and
    //continuation families × bare / expression / brace initializer) is
    //exercised.
    const char *msg = "array declarations take one size per bracket";
    static const char *const sources[] = {
        "int main() {\n    int a[2, 1 + 2 * 3];\n    return 0;\n}\n",
        "int main() {\n    int a[2, 1 + 2 * 3] = 5;\n    return 0;\n}\n",
        "int main() {\n    int a[2, 1 + 2 * 3] = {5};\n    return 0;\n}\n",
        "int main() {\n    int x = 1, a[2, 1 + 2 * 3];\n    return 0;\n}\n",
        "int main() {\n    int x = 1, a[2, 1 + 2 * 3] = 5;\n    return 0;\n}\n",
        "int main() {\n    int x = 1, a[2, 1 + 2 * 3] = {5};\n    return 0;\n}\n",
    };
    const int rounds = soakRounds();
    //D-8 self-report (the single source): the timing checker reads this
    //line from the junit <system-out> and requires rounds to equal the
    //table row's reference work times the run's work factor, which
    //catches a registration or table that stopped steering this loop.
    std::cout << "rounds=" << rounds << "\n";
    int srcIdx = 0;
    for (int i = 0; i < rounds; ++i)
    {
        srcIdx = 0;
        for (const char *src : sources)
        {
            const BuildOutcome outcome = buildSource("nd_declsize_stress", src);
            if (!(!outcome.ok && DiagHas(outcome, msg)
                  && !DiagHas(outcome, "syntax error")))
            {
                std::cerr << "    stress failure at iteration " << i
                    << ", source " << srcIdx << ", ok=" << outcome.ok
                    << "\n    diagnostics:\n" << outcome.diagnostics;
            }
            CHECK(!outcome.ok && DiagHas(outcome, msg)
                && !DiagHas(outcome, "syntax error"),
                "stress iteration must keep the named diagnostic");
            ++srcIdx;
        }
    }
    PASS();
}

void test_soak_rounds_env_scaling()
{
    TEST(soak_rounds_env_scaling);
    //Pin the two D-8 workload axes: the soak length is the registered
    //rounds times the work factor, and a missing or sub-unity factor
    //never shrinks the soak below its registered reference workload
    //(the checker's self-report assertion trusts that contract). Every
    //axis is pinned per block, so an ambient factor from a rehearsal
    //reconfigure cannot sway a block that means factor 1.
    {
        const ScopedEnv rounds("NLANG_TEST_ROUNDS", "7");
        const ScopedEnv factor("NLANG_TEST_WORK_FACTOR", "2");
        CHECK(soakRounds() == 14, "rounds must follow rounds x factor");
    }
    {
        const ScopedEnv rounds("NLANG_TEST_ROUNDS", "7");
        const ScopedEnv factor("NLANG_TEST_WORK_FACTOR", "0");
        CHECK(soakRounds() == 7, "a sub-unity factor must not shrink the soak");
    }
    {
        const ScopedEnv rounds("NLANG_TEST_ROUNDS", "9");
        const ScopedEnv factor("NLANG_TEST_WORK_FACTOR", nullptr);
        CHECK(soakRounds() == 9, "a missing factor means factor 1");
    }
    {
        const ScopedEnv rounds("NLANG_TEST_ROUNDS", "seven");
        const ScopedEnv factor("NLANG_TEST_WORK_FACTOR", "1");
        CHECK(soakRounds() == DEFAULT_SOAK_ROUNDS,
            "a malformed count must fall back, not vanish");
    }
    {
        const ScopedEnv rounds("NLANG_TEST_ROUNDS", "1000000");
        const ScopedEnv factor("NLANG_TEST_WORK_FACTOR", "1000000");
        //Each input is capped at MAX_ENV_COUNT on its own, but the
        //product of two capped inputs (1e12) overflows int: the product
        //must clamp to the same bound, not wrap into garbage that the
        //self-report would then print as the rounds actually run.
        CHECK(soakRounds() == MAX_ENV_COUNT,
            "an overflowing product must clamp, not wrap");
    }
    PASS();
}

void test_leading_comma_diagnostics()
{
    TEST(leading_comma_diagnostics);
    const BuildOutcome formal = buildSource("nd_leadcomma_formal",
        "int f(, int x) { return x; }\n"
        "int main() { return f(1); }\n");
    CHECK(!formal.ok && DiagHas(formal, "Unexpected ',' in a param list."),
        "leading comma in a param list keeps its named diagnostic");
    const BuildOutcome call = buildSource("nd_leadcomma_call",
        "int f(int x) { return x; }\n"
        "int main() { return f(, 1); }\n");
    CHECK(!call.ok
        && DiagHas(call, "expected a param before ','"),
        "leading comma in a call names the corrected message");
    PASS();
}

int main(int argc, char *argv[])
{
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();

    //--stress runs only the reject soak; the default run excludes it.
    //The two forms are a partition registered as separate ctest entries
    //(nd_resolve_tests / nd_resolve_stress_tests), so a routine sweep
    //skips the soak by name while the full suite still covers it: the
    //soak dominates the file (its per-config seconds live in
    //tests/gate_time_reference.txt) while everything else is seconds.
    //The soak length comes from the D-8 axes (see soakRounds) and is
    //self-reported as rounds=<n> on stdout. An unrecognized argument
    //fails loud instead of masquerading as a green fast run.
    bool stressOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--stress") {
            stressOnly = true;
        } else {
            std::cerr << "unknown argument: " << argv[i]
                << " (only --stress is accepted)\n";
            return 2;
        }
    }

    std::cerr << "=== ND resolve "
        << (stressOnly ? "stress only" : "tests")
        << " ===\n";
    try {
        if (stressOnly) {
            test_decl_comma_size_reject_stress();
        } else {
            test_nd_creation_resolves_to_gate();
            test_subscript_arity_diagnostics();
            test_jagged_still_rejected();
            test_emit_gates_cover_nd_paths();
            test_nd_rank_cap_diagnostics();
            test_brace_row_is_depth_mismatch();
            test_decl_comma_size_named_reject();
            test_soak_rounds_env_scaling();
            test_leading_comma_diagnostics();
            test_nd_initlist_dynamic_row_expr();
            test_nd_initlist_dynamic_mixed_rows();
            test_nd_initlist_dynamic_scalar_entry_rejected();
            test_nd_initlist_dynamic_rank_mismatch_rejected();
        }
    } catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << "\n";
        g_fail++;
    }

    std::error_code ec;
    std::filesystem::remove_all(scratchDir(), ec);

    std::cerr << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
