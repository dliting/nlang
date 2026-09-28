/*---
    test_primops.cpp - 0.7.5 kind-immediate opcode family unit tests.

    Three layers, each matching where the semantics live:
    1. Dispatch tables (VmPrimOps.h) driven directly on byte buffers —
       the table cells ARE the op implementations; ops x all 12 registry
       kinds including the ones without a language surface yet (byte
       wrap, ulong, char code-point order, bool Eq/Ne-only gates).
    2. Rn formatter functions (the exact singletons OpPrimToStr calls) —
       per-category decimal/%g/%.17g/true-false/UTF-8 output.
    3. Real .n sources compiled and executed for the handler guards that
       need module-backed state: div/mod-zero exception messages and the
       scalar+string concat end-to-end path.

    Hand-built CompiledModules are retired as a test strategy
    (test_vm.cpp header note: incomplete frame layouts) — layer 3 uses
    the real compiler instead. The OpPrimCast code-point guard is
    exercised by Task 8's char-language e2e.
---*/
#include "VmPrimOps.h"
#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/compiler/CastInfo.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/runtime/RnTypes.h"
#include "nlang/vm/CompiledModule.h"
#include "VmExecutor.h"
#include "ModuleLoader.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
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

//Raw slot load/store — the tables reinterpret slots by carrier width,
//so values must go in and out via memcpy (no alignment assumptions).
template <typename T>
static void putVal(uint8_t* slot, T v) { std::memcpy(slot, &v, sizeof(T)); }
template <typename T>
static T getVal(const uint8_t* slot) { T v; std::memcpy(&v, slot, sizeof(v)); return v; }

// --- Layer 1: dispatch tables ---

void test_add_table()
{
    TEST(add_table);
    uint8_t buf[16];
    std::memset(buf, 0, sizeof(buf));

    putVal<int32_t>(buf, 7); putVal<int32_t>(buf + 8, 35);
    kAddTable[SPR_Int32](buf, buf + 8);
    CHECK(getVal<int32_t>(buf) == 42, "int 7+35");

    putVal<int8_t>(buf, 127); putVal<int8_t>(buf + 8, 1);
    kAddTable[SPR_Byte](buf, buf + 8);
    CHECK(getVal<int8_t>(buf) == -128, "byte 127+1 wraps to -128");

    putVal<uint8_t>(buf, 255); putVal<uint8_t>(buf + 8, 1);
    kAddTable[SPR_UByte](buf, buf + 8);
    CHECK(getVal<uint8_t>(buf) == 0, "ubyte 255+1 wraps to 0");

    putVal<int16_t>(buf, 32767); putVal<int16_t>(buf + 8, 1);
    kAddTable[SPR_Short](buf, buf + 8);
    CHECK(getVal<int16_t>(buf) == -32768, "short 32767+1 wraps");

    putVal<double>(buf, 1.25); putVal<double>(buf + 8, 2.5);
    kAddTable[SPR_Double](buf, buf + 8);
    CHECK(getVal<double>(buf) == 3.75, "double 1.25+2.5");

    putVal<uint64_t>(buf, 0xFFFFFFFFFFFFFFFFULL); putVal<uint64_t>(buf + 8, 2);
    kAddTable[SPR_ULong](buf, buf + 8);
    CHECK(getVal<uint64_t>(buf) == 1, "ulong max+2 wraps to 1");
    PASS();
}

void test_sub_mul_div_mod_neg()
{
    TEST(sub_mul_div_mod_neg);
    uint8_t buf[16];
    std::memset(buf, 0, sizeof(buf));

    putVal<int32_t>(buf, 35); putVal<int32_t>(buf + 8, 7);
    kSubTable[SPR_Int32](buf, buf + 8);
    CHECK(getVal<int32_t>(buf) == 28, "int 35-7");

    putVal<uint16_t>(buf, 300); putVal<uint16_t>(buf + 8, 300);
    kMulTable[SPR_UShort](buf, buf + 8);
    CHECK(getVal<uint16_t>(buf) == 24464, "ushort 300*300 wraps mod 2^16");

    putVal<int32_t>(buf, 45); putVal<int32_t>(buf + 8, 7);
    kDivTable[SPR_Int32](buf, buf + 8);
    CHECK(getVal<int32_t>(buf) == 6, "int 45/7 truncates to 6");

    putVal<uint32_t>(buf, 7); putVal<uint32_t>(buf + 8, 2);
    kDivTable[SPR_UInt32](buf, buf + 8);
    CHECK(getVal<uint32_t>(buf) == 3, "uint 7/2");

    putVal<float>(buf, 1.0f); putVal<float>(buf + 8, 4.0f);
    kDivTable[SPR_Float](buf, buf + 8);
    CHECK(getVal<float>(buf) == 0.25f, "float 1/4");

    putVal<double>(buf, 1.0); putVal<double>(buf + 8, 8.0);
    kDivTable[SPR_Double](buf, buf + 8);
    CHECK(getVal<double>(buf) == 0.125, "double 1/8");

    putVal<int32_t>(buf, -7); putVal<int32_t>(buf + 8, 3);
    kModTable[SPR_Int32](buf, buf + 8);
    CHECK(getVal<int32_t>(buf) == -1, "int -7%3 C remainder semantics");

    putVal<uint64_t>(buf, 10); putVal<uint64_t>(buf + 8, 3);
    kModTable[SPR_ULong](buf, buf + 8);
    CHECK(getVal<uint64_t>(buf) == 1, "ulong 10%3");

    putVal<int32_t>(buf, 42);
    kNegTable[SPR_Int32](buf);
    CHECK(getVal<int32_t>(buf) == -42, "int negation");

    putVal<float>(buf, 1.5f);
    kNegTable[SPR_Float](buf);
    CHECK(getVal<float>(buf) == -1.5f, "float negation");
    PASS();
}

//Relational/equality rows: ordered kinds carry all six ops; bool rows
//carry Eq/Ne only — the nullptr IS the gate the resolver relies on.
void test_cmp_table()
{
    TEST(cmp_table);
    uint8_t l[8], r[8];
    std::memset(l, 0, sizeof(l)); std::memset(r, 0, sizeof(r));

    //long: 5 vs 3 — all six ops
    putVal<int64_t>(l, 5); putVal<int64_t>(r, 3);
    CHECK(kCmpTable[0][SPR_Long](l, r) == 0, "long 5<3 false");
    CHECK(kCmpTable[1][SPR_Long](l, r) == 0, "long 5<=3 false");
    CHECK(kCmpTable[2][SPR_Long](l, r) == 1, "long 5>3 true");
    CHECK(kCmpTable[3][SPR_Long](l, r) == 1, "long 5>=3 true");
    CHECK(kCmpTable[4][SPR_Long](l, r) == 0, "long 5==3 false");
    CHECK(kCmpTable[5][SPR_Long](l, r) == 1, "long 5!=3 true");

    //double: equal pair
    putVal<double>(l, 2.5); putVal<double>(r, 2.5);
    CHECK(kCmpTable[1][SPR_Double](l, r) == 1, "double 2.5<=2.5 true");
    CHECK(kCmpTable[4][SPR_Double](l, r) == 1, "double 2.5==2.5 true");
    putVal<double>(l, 1.5);
    CHECK(kCmpTable[0][SPR_Double](l, r) == 1, "double 1.5<2.5 true");

    //char: uint32 code-point order ('a' 0x61 vs 'b' 0x62, 'zhong' 0x4E2D)
    putVal<uint32_t>(l, 0x61); putVal<uint32_t>(r, 0x62);
    CHECK(kCmpTable[0][SPR_Char](l, r) == 1, "char a<b");
    CHECK(kCmpTable[3][SPR_Char](l, r) == 0, "char a>=b false");
    putVal<uint32_t>(l, 0x4E2D);
    CHECK(kCmpTable[2][SPR_Char](l, r) == 1, "char U+4E2D > b");

    //bool: Eq/Ne live, Less is nullptr
    putVal<int32_t>(l, 1); putVal<int32_t>(r, 0);
    CHECK(kCmpTable[4][SPR_Bool](l, r) == 0, "bool 1==0 false");
    CHECK(kCmpTable[5][SPR_Bool](l, r) == 1, "bool 1!=0 true");
    CHECK(kCmpTable[0][SPR_Bool] == nullptr, "bool Less row is gated off");
    CHECK(kCmpTable[2][SPR_Bool] == nullptr, "bool Greater row is gated off");

    //arithmetic gates: bool/char rows are nullptr
    CHECK(kAddTable[SPR_Bool] == nullptr, "add on bool gated off");
    CHECK(kAddTable[SPR_Char] == nullptr, "add on char gated off");
    CHECK(kSubTable[SPR_Bool] == nullptr, "sub on bool gated off");
    CHECK(kMulTable[SPR_Char] == nullptr, "mul on char gated off");
    CHECK(kDivTable[SPR_Bool] == nullptr, "div on bool gated off");
    CHECK(kModTable[SPR_Char] == nullptr, "mod on char gated off");
    CHECK(kNegTable[SPR_Bool] == nullptr, "neg on bool gated off");
    PASS();
}

//Cast grid cells: C#-unchecked numeric semantics + the bool gates and
//the bounds guard (bad kind immediates must yield nullptr, never read
//out of bounds).
void test_cast_table()
{
    TEST(cast_table);
    uint8_t buf[8];
    std::memset(buf, 0, sizeof(buf));

    //long -> int32: low-32 truncation
    putVal<int64_t>(buf, 0x123456789LL);
    PrimCastCell(SPR_Long, SPR_Int32)(buf);
    CHECK(getVal<int32_t>(buf) == 0x23456789, "long->int truncates low 32");

    //double -> float: round-to-nearest narrowing
    putVal<double>(buf, 0.1);
    PrimCastCell(SPR_Double, SPR_Float)(buf);
    CHECK(getVal<float>(buf) == 0.1f, "double->float narrows");

    //byte -> long: sign extension through the widening chain
    putVal<int8_t>(buf, -5);
    PrimCastCell(SPR_Byte, SPR_Long)(buf);
    CHECK(getVal<int64_t>(buf) == -5, "byte->long sign-extends");

    //ulong -> double
    putVal<uint64_t>(buf, 2);
    PrimCastCell(SPR_ULong, SPR_Double)(buf);
    CHECK(getVal<double>(buf) == 2.0, "ulong->double");

    //int <-> char value round-trip (the validity guard is handler-side)
    putVal<int32_t>(buf, 65);
    PrimCastCell(SPR_Int32, SPR_Char)(buf);
    CHECK(getVal<uint32_t>(buf) == 65, "int->char code point 65");
    PrimCastCell(SPR_Char, SPR_Int32)(buf);
    CHECK(getVal<int32_t>(buf) == 65, "char->int round-trip");

    //bool rows/cols and out-of-range rows are nullptr
    CHECK(PrimCastCell(SPR_Bool, SPR_Int32) == nullptr, "bool source gated");
    CHECK(PrimCastCell(SPR_Int32, SPR_Bool) == nullptr, "bool target gated");
    CHECK(PrimCastCell(SPR_Char, SPR_Bool) == nullptr, "char->bool gated");
    CHECK(PrimCastCell(-1, 0) == nullptr, "negative row guarded");
    CHECK(PrimCastCell(0, SPR_Count) == nullptr, "row-count boundary guarded");
    CHECK(PrimCastCell(200, 0) == nullptr, "bad kind immediate guarded");
    PASS();
}

// --- Layer 2: the Rn formatters OpPrimToStr dispatches to ---

void test_rn_formatters()
{
    TEST(rn_formatters);
    uint8_t v[8];
    std::memset(v, 0, sizeof(v));

    putVal<int32_t>(v, -42);
    CHECK(RnInt32::Instance()->ValueToString(v) == "-42", "int32 %d");

    putVal<int8_t>(v, -128);
    CHECK(RnByte::Instance()->ValueToString(v) == "-128", "byte promotes to decimal");

    putVal<int64_t>(v, 9223372036854775807LL);
    CHECK(RnLong::Instance()->ValueToString(v) == "9223372036854775807",
        "long %lld");

    putVal<uint64_t>(v, 18446744073709551615ULL);
    CHECK(RnULong::Instance()->ValueToString(v) == "18446744073709551615",
        "ulong %llu");

    putVal<float>(v, 1.5f);
    CHECK(RnFloat::Instance()->ValueToString(v) == "1.5", "float %g");

    putVal<double>(v, 2.5);
    CHECK(RnDouble::Instance()->ValueToString(v) == "2.5", "double %.17g exact");

    putVal<int32_t>(v, 1);
    CHECK(RnBool::Instance()->ValueToString(v) == "true", "bool true");
    putVal<int32_t>(v, 0);
    CHECK(RnBool::Instance()->ValueToString(v) == "false", "bool false");

    //char: one UTF-8 code point — 'A' 1 byte, U+4E2D ('zhong') 3 bytes
    putVal<uint32_t>(v, 0x41);
    CHECK(RnChar::Instance()->ValueToString(v) == "A", "char ASCII");
    putVal<uint32_t>(v, 0x4E2D);
    CHECK(RnChar::Instance()->ValueToString(v) == "\xE4\xB8\xAD",
        "char U+4E2D encodes 3 UTF-8 bytes");
    PASS();
}

// --- Layer 3: compiled programs (handler guards + end-to-end) ---

static std::filesystem::path scratchDir()
{
    static const auto dir = std::filesystem::temp_directory_path()
        / "nlang_test_primops";
    std::filesystem::create_directories(dir);
    return dir;
}

//Compile + execute; returns main()'s value, or -1 when the build failed
//(same harness shape as test_stdlib — real sources, real execution).
static int runSource(const std::string& tag, const std::string& source)
{
    const auto dir = scratchDir();
    const auto nPath = dir / (tag + ".n");
    const auto modPath = dir / (tag + ".nmod");
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

    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    if (!builder.Build() || !std::filesystem::exists(modPath)) {
        for (auto it = logger.cbegin(); it != logger.cend(); ++it)
            std::cerr << (*it)->Message() << "\n";
        return -1;
    }
    CompiledModule mod = ModuleLoader::Load(modPath.string());
    VmExecutor exec;
    return exec.Execute(mod);
}

void test_div_zero_message()
{
    TEST(div_zero_message);
    const int rc = runSource("div_zero",
        "int main() {\n"
        "    int z = 0;\n"
        "    try {\n"
        "        int x = 10 / z;\n"
        "        return 3;\n"
        "    } catch (Exception e) {\n"
        "        if (e.message == \"NLang VM: division by zero\") return 0;\n"
        "        return 2;\n"
        "    }\n"
        "}\n");
    CHECK(rc == 0, "division by zero raises the exact message");
    PASS();
}

void test_mod_zero_message()
{
    TEST(mod_zero_message);
    const int rc = runSource("mod_zero",
        "int main() {\n"
        "    int z = 0;\n"
        "    try {\n"
        "        int x = 10 % z;\n"
        "        return 3;\n"
        "    } catch (Exception e) {\n"
        "        if (e.message == \"NLang VM: modulo by zero\") return 0;\n"
        "        return 2;\n"
        "    }\n"
        "}\n");
    CHECK(rc == 0, "modulo by zero raises the exact message");
    PASS();
}

void test_scalar_concat_end_to_end()
{
    TEST(scalar_concat_end_to_end);
    const int rc = runSource("concat_str",
        "int main() {\n"
        "    string s = \"x\" + 42;\n"
        "    if (s != \"x42\") return 1;\n"
        "    float f = 1.5;\n"
        "    string t = \"v\" + f;\n"
        "    if (t != \"v1.5\") return 2;\n"
        "    int n = 7;\n"
        "    string u = n + \"!\";\n"
        "    if (u != \"7!\") return 3;\n"
        "    return 0;\n"
        "}\n");
    CHECK(rc == 0, "int/float to-string concat via OP_Prim_to_str");
    PASS();
}

int main()
{
#ifdef _WIN32
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS
                             | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    //Hidden host contract (same as ncc/nvm main): the compiler needs the
    //IdString tables, and 0.7.5 split the cast table out of
    //Runtime::StaticInit (module boundary: runtime stays compiler-free).
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();

    std::cerr << "=== NLang PrimOps Unit Tests ===\n\n";

    try {
        test_add_table();
        test_sub_mul_div_mod_neg();
        test_cmp_table();
        test_cast_table();
        test_rn_formatters();
        test_div_zero_message();
        test_mod_zero_message();
        test_scalar_concat_end_to_end();
    } catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }

    std::error_code ec;
    std::filesystem::remove_all(scratchDir(), ec);

    std::cerr << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
