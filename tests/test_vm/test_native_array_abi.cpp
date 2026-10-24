// NativeHost ABI v6 array-access unit tests: real VmExecutor + real heap
// arrays. Bare-name natives registered in-process exercise getArrayView
// (view fields, payload read/write round-trip, empty arrays, null handle)
// and newArray (mint + fill + negative-length rejection). Mirrors
// test_stdlib.cpp's in-process compile+load+link+execute pipeline —
// including its static-init contract: Runtime::StaticInit owns the
// IdString intern tables and TypeCastInfo::StaticInit the cast table;
// both are required before any in-process ModuleBuilder use.

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
#include <filesystem>
#include <fstream>
#include <functional>
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
    static const auto dir = std::filesystem::temp_directory_path()
        / "nlang_test_native_array_abi";
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

//Compile + execute with in-process natives registered before the run.
//Bare-name natives at TU level resolve as <module-tag>.<name> at the call
//site (TestNatives.h precedent: "native_basic.natConst"), so registration
//keys off the same tag. Returns main()'s value, -1 on build or pipeline
//failure.
static int runSourceWithNatives(const std::string& tag,
    const std::string& source,
    const std::function<void(VmExecutor&, const std::string&)>& registerNatives)
{
    const auto dir = scratchDir();
    const auto modPath = dir / (tag + ".ncu");
    BuildOutcome outcome = buildSource(tag, source);
    if (!outcome.ok) {
        std::cerr << "    build failed:\n" << outcome.diagnostics;
        return -1;
    }
    try {
        NcuLoader::Options loaderOpts;
        loaderOpts.searchDirs = {dir.string(), STDLIB_DIR};
        NcuLoader::Result loaded =
            NcuLoader::LoadClosure(modPath.string(), loaderOpts);
        CompiledModule mod = NcuLinker::Link(std::move(loaded.units),
                                             loaded.entryKey);
        VmExecutor exec;
        if (registerNatives)
            registerNatives(exec, tag);
        return exec.Execute(mod);
    } catch (const std::exception& e) {
        std::cerr << "load/link failed: " << e.what() << "\n";
        return -1;
    }
}

//--- bare-name test natives (ABI v6 exercise) --------------------------------

//int natViewRoundtrip(int[] a) — validate the view's fixed fields, write
//42 through the payload pointer (slot 1), return the element count.
void NatViewRoundtrip(NativeHost* host, uint8_t* ret,
                      const uint8_t* args, int argc) {
    (void)argc;
    NativeArrayView view;
    view.rank = -1; view.elemKind = 0xEE; view.elemCellBytes = 0xEE;
    host->getArrayView(host, nlang::native::ArgInt(args, 0), &view);
    if (view.rank != 1)
        nlang::native::Raise(host, NEXC_Base, "rank must be 1");
    if (view.elemKind != NEK_Int32)
        nlang::native::Raise(host, NEXC_Base, "elemKind must be Int32");
    if (view.elemCellBytes != 4)
        nlang::native::Raise(host, NEXC_Base, "elemCellBytes must be 4");
    if (!view.data || !view.dims || !view.strides)
        nlang::native::Raise(host, NEXC_Base, "view pointers must be set");
    if (view.elemCount >= 2) {
        static_cast<int32_t*>(view.data)[1] = 42;
    }
    nlang::native::ReturnInt(ret, static_cast<int32_t>(view.elemCount));
}

//int[] natMint(int len) — mint via newArray, fill i*2 through the view.
void NatMint(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc) {
    (void)argc;
    const int32_t len = nlang::native::ArgInt(args, 0);
    const int32_t handle = nlang::native::NewArray(host, NEK_Int32, len);
    NativeArrayView view;
    host->getArrayView(host, handle, &view);
    int32_t* cells = static_cast<int32_t*>(view.data);
    for (int32_t i = 0; i < static_cast<int32_t>(view.elemCount); ++i)
        cells[i] = i * 2;
    nlang::native::ReturnInt(ret, handle);
}

static void RegisterTestNatives(VmExecutor& exec, const std::string& tag)
{
    exec.RegisterNative(tag + ".natViewRoundtrip", &NatViewRoundtrip);
    exec.RegisterNative(tag + ".natMint", &NatMint);
}

//--- tests --------------------------------------------------------------------

void test_view_roundtrip_read_write()
{
    TEST(view_roundtrip_read_write);
    const int rc = runSourceWithNatives("abi_roundtrip",
        "native int natViewRoundtrip(int[] a);\n"
        "int main() {\n"
        "    int[] a = int[3];\n"
        "    a[0] = 7; a[2] = 9;\n"
        "    int n = natViewRoundtrip(a);\n"
        "    if (n == 3 && a[1] == 42) return 0;\n"
        "    return 1;\n"
        "}\n",
        &RegisterTestNatives);
    CHECK(rc == 0, "roundtrip must return elemCount 3 and persist the "
                   "payload write (a[1]==42)");
    PASS();
}

void test_mint_and_fill()
{
    TEST(mint_and_fill);
    const int rc = runSourceWithNatives("abi_mint",
        "native int[] natMint(int len);\n"
        "int main() {\n"
        "    int[] m = natMint(4);\n"
        "    if (m.length() != 4) return 1;\n"
        "    if (m[0] != 0 || m[1] != 2 || m[2] != 4 || m[3] != 6)\n"
        "        return 2;\n"
        "    return 0;\n"
        "}\n",
        &RegisterTestNatives);
    CHECK(rc == 0, "minted array must be length 4 with elements i*2");
    PASS();
}

void test_empty_array_view()
{
    TEST(empty_array_view);
    const int rc = runSourceWithNatives("abi_empty",
        "native int natViewRoundtrip(int[] a);\n"
        "int main() {\n"
        "    int[] e = int[0];\n"
        "    int n = natViewRoundtrip(e);\n"
        "    if (n == 0) return 0;\n"
        "    return 1;\n"
        "}\n",
        &RegisterTestNatives);
    CHECK(rc == 0, "zero-length array view must be legal (elemCount 0)");
    PASS();
}

//double[] natAlignD() — mint a double array, verify its payload is
//8-byte aligned, write 1.5 through the view, and return the handle.
//The signature itself declares double[] — a native minting kind K
//declares K[] in its signature, so the scalar array type is always
//registered for the module (the newArray contract).
void NatAlignD(NativeHost* host, uint8_t* ret, const uint8_t* args, int) {
    (void)args;
    const int32_t handle = nlang::native::NewArray(host, NEK_Float64, 4);
    NativeArrayView v;
    host->getArrayView(host, handle, &v);
    if (v.elemCellBytes != 8)
        nlang::native::Raise(host, NEXC_Base, "elemCellBytes must be 8");
    static_cast<double*>(v.data)[0] = 1.5;
    const uintptr_t bits = reinterpret_cast<uintptr_t>(v.data);
    if (bits % 8 != 0)
        nlang::native::Raise(host, NEXC_Base,
                             "double payload is not 8-byte aligned");
    nlang::native::ReturnInt(ret, handle);
}

void test_double_payload_alignment()
{
    TEST(double_payload_alignment);
    const int rc = runSourceWithNatives("abi_align",
        "native double[] natAlignD();\n"
        "int main() {\n"
        "    double[] d = natAlignD();\n"
        "    if (d[0] == 1.5) return 0;\n"
        "    return 1;\n"
        "}\n",
        [](VmExecutor& e, const std::string& tag) {
            e.RegisterNative(tag + ".natAlignD", &NatAlignD);
        });
    CHECK(rc == 0, "double payload must be 8-byte aligned, writable "
                    "through the view, and round-trip by handle");
    PASS();
}

void test_null_handle_raises_npe()
{
    TEST(null_handle_raises_npe);
    const int rc = runSourceWithNatives("abi_null",
        "native int natViewRoundtrip(int[] a);\n"
        "int main() {\n"
        "    int[] b = null;\n"
        "    try {\n"
        "        natViewRoundtrip(b);\n"
        "    } catch (NullPointerException npe) {\n"
        "        return 0;\n"
        "    }\n"
        "    return 1;\n"
        "}\n",
        &RegisterTestNatives);
    CHECK(rc == 0, "null array handle must raise a catchable "
                   "NullPointerException");
    PASS();
}

void test_negative_mint_raises_oob()
{
    TEST(negative_mint_raises_oob);
    const int rc = runSourceWithNatives("abi_negative",
        "native int[] natMint(int len);\n"
        "int main() {\n"
        "    try {\n"
        "        natMint(-1);\n"
        "    } catch (IndexOutOfBoundsException oob) {\n"
        "        return 0;\n"
        "    }\n"
        "    return 1;\n"
        "}\n",
        &RegisterTestNatives);
    CHECK(rc == 0, "negative length must raise the OOB exception family "
                   "(language-side alignment)");
    PASS();
}

int main()
{
    //Static tables before any compiler work (test_stdlib.cpp / ncc /
    //ScriptRunner.h contract): the IdString intern tables and the cast
    //table have no lazy guards — in-process builds dereference both.
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();

    std::cerr << "=== native array ABI tests ===\n";
    try {
        test_view_roundtrip_read_write();
        test_mint_and_fill();
        test_empty_array_view();
        test_double_payload_alignment();
        test_null_handle_raises_npe();
        test_negative_mint_raises_oob();
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
