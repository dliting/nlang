// NativeHost ABI v7 array-access unit tests: real VmExecutor + real heap
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
#include "VmExecutorNativeHost.h"
#include "NcuLoader.h"
#include "NcuLinker.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
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

//--- bare-name test natives (ABI v7 exercise) --------------------------------

//int natTwoViewsStrideIndependence(int[,] a, int[,,] b) — two live views
//must own their strides storage: the v6 host scratch let the second
//getArrayView overwrite the first view's strides (and two carriers in
//blocking natives could interleave the same way).
void NatTwoViewsStrideIndependence(NativeHost* host, uint8_t* ret,
                                   const uint8_t* args, int argc) {
    (void)argc;
    NativeArrayView v1;
    host->getArrayView(host, nlang::native::ArgInt(args, 0), &v1);
    NativeArrayView v2;
    host->getArrayView(host, nlang::native::ArgInt(args, 1), &v2);
    //v1 is int[2,3] (row-major strides {3,1}); v2 is int[2,2,2] ({4,2,1}).
    if (v1.strides[0] != 3 || v1.strides[1] != 1
        || v2.strides[0] != 4 || v2.strides[1] != 2 || v2.strides[2] != 1)
        nlang::native::Raise(host, NEXC_Base,
            "two live views must keep independent strides");
    nlang::native::ReturnInt(ret, 1);
}

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

//int natHighRankView(int[,,,,,,,,,,,] a) — a rank-12 view: every field
//(rank/dims/strides/elemCount) checked against dims {1,2,1,...,1,3},
//payload read at the last cell, and a write the language side re-reads.
//This also proves the .ncu wire survives rank 12: the module containing
//the rank-12 type was saved and reloaded before this native ever runs.
void NatHighRankView(NativeHost* host, uint8_t* ret,
                     const uint8_t* args, int argc) {
    (void)argc;
    NativeArrayView view;
    host->getArrayView(host, nlang::native::ArgInt(args, 0), &view);
    const int32_t wantDims[12] = {1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3};
    const int32_t wantStrides[12] = {6, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1};
    if (view.rank != 12)
        nlang::native::Raise(host, NEXC_Base, "rank must be 12");
    if (view.elemCount != 6)
        nlang::native::Raise(host, NEXC_Base, "elemCount must be 6");
    for (int32_t d = 0; d < 12; ++d) {
        if (view.dims[d] != wantDims[d])
            nlang::native::Raise(host, NEXC_Base, "dims mismatch");
        if (view.strides[d] != wantStrides[d])
            nlang::native::Raise(host, NEXC_Base, "strides mismatch");
    }
    int32_t* cells = static_cast<int32_t*>(view.data);
    if (cells[5] != 5)
        nlang::native::Raise(host, NEXC_Base, "payload read mismatch");
    cells[0] = 42;
    nlang::native::ReturnInt(ret, 1);
}

static void RegisterTestNatives(VmExecutor& exec, const std::string& tag)
{
    exec.RegisterNative(tag + ".natViewRoundtrip", &NatViewRoundtrip);
    exec.RegisterNative(tag + ".natTwoViewsStrideIndependence",
                        &NatTwoViewsStrideIndependence);
    exec.RegisterNative(tag + ".natMint", &NatMint);
    exec.RegisterNative(tag + ".natHighRankView", &NatHighRankView);
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

void test_two_views_stride_independence()
{
    TEST(two_views_stride_independence);
    const int rc = runSourceWithNatives("abi_two_views",
        "native int natTwoViewsStrideIndependence(int[,] a, int[,,] b);\n"
        "int main() {\n"
        "    int[,] a = int[2, 3];\n"
        "    int[,,] b = int[2, 2, 2];\n"
        "    return natTwoViewsStrideIndependence(a, b) - 1;\n"
        "}\n",
        &RegisterTestNatives);
    CHECK(rc == 0, "two live views must keep independent strides");
    PASS();
}

void test_high_rank_view()
{
    TEST(high_rank_view);
    //Rank 12 = dims {1,2,1,...,1,3}, elemCount 6. The subscript
    //a[0,1,0,...,2] is flat cell 5; the native re-derives every view
    //field and writes cell 0 for the language side to read back.
    const int rc = runSourceWithNatives("abi_high_rank",
        "native int natHighRankView(int[,,,,,,,,,,,] a);\n"
        "int main() {\n"
        "    int[,,,,,,,,,,,] a = int[1,2,1,1,1,1,1,1,1,1,1,3];\n"
        "    a[0,1,0,0,0,0,0,0,0,0,0,2] = 5;\n"
        "    if (natHighRankView(a) != 1) return 1;\n"
        "    if (a[0,0,0,0,0,0,0,0,0,0,0,0] != 42) return 2;\n"
        "    return 0;\n"
        "}\n",
        &RegisterTestNatives);
    CHECK(rc == 0, "rank-12 view must expose correct dims/strides and "
                   "persist the payload round-trip");
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

//--- corrupt-record defenses (0.11.4 B) ---------------------------------------
//White-box: mint through the real newArray callback, corrupt the heap
//record through test probes, then drive the production getArrayView
//callback. A truncated dims section or a bad arrayTypes index must throw
//before any header cell is read or caller memory touched.
struct ExecWithModule {
    std::unique_ptr<VmExecutor> exec;
    std::unique_ptr<CompiledModule> mod;
    VmNativeHost host;
};

//CHECK does a bare `return`, so this helper returns void and fills an
//out-param (no value-returning function may CHECK mid-way).
static void makeExecutorWithArrayModule(const std::string& tag,
                                        ExecWithModule& out)
{
    const auto dir = scratchDir();
    const auto nPath = dir / (tag + ".n");
    const auto modPath = dir / (tag + ".ncu");
    std::filesystem::remove(modPath);
    {
        std::ofstream out(nPath, std::ios::binary);
        out << "int main() {\n"
               "    int[] a = int[3];\n"
               "    return 0;\n"
               "}\n";
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
        FAIL("defense fixture must build");
        return;
    }
    NcuLoader::Options loaderOpts;
    loaderOpts.searchDirs = {dir.string(), STDLIB_DIR};
    NcuLoader::Result loaded =
        NcuLoader::LoadClosure(modPath.string(), loaderOpts);
    out.mod = std::make_unique<CompiledModule>(
        NcuLinker::Link(std::move(loaded.units), loaded.entryKey));
    out.exec = std::make_unique<VmExecutor>();
    out.exec->InitializeForRun(*out.mod);
    out.exec->InitNativeHost(out.host);
}

void test_truncated_dims_record_throws()
{
    TEST(truncated_dims_record_throws);
    ExecWithModule env;
    makeExecutorWithArrayModule("abi_trunc", env);
    const int32_t handle =
        env.host.c.newArray(&env.host.c, NEK_Int32, 3);
    CHECK(handle > 0, "newArray must mint a live handle");
    //Keep the 4-cell fixed header, drop the dims run: the stride loop
    //would read past the record before this fix.
    env.exec->TruncateArrayRecordForTests(handle, 4);
    NativeArrayView view;
    bool threw = false;
    try {
        env.host.c.getArrayView(&env.host.c, handle, &view);
    } catch (...) {
        threw = true;
    }
    CHECK(threw, "truncated dims section must throw, not read OOB");
    PASS();
}

void test_corrupt_array_type_index_throws()
{
    TEST(corrupt_array_type_index_throws);
    ExecWithModule env;
    makeExecutorWithArrayModule("abi_badidx", env);
    const int32_t handle =
        env.host.c.newArray(&env.host.c, NEK_Int32, 3);
    CHECK(handle > 0, "newArray must mint a live handle");
    env.exec->OverwriteArrayRecordCellForTests(handle, 1, 0x7FFFFFF0);
    NativeArrayView view;
    bool threw = false;
    try {
        env.host.c.getArrayView(&env.host.c, handle, &view);
    } catch (...) {
        threw = true;
    }
    CHECK(threw, "out-of-range arrayTypes index must throw, not read OOB");
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
        test_high_rank_view();
        test_two_views_stride_independence();
        test_mint_and_fill();
        test_empty_array_view();
        test_double_payload_alignment();
        test_null_handle_raises_npe();
        test_negative_mint_raises_oob();
        test_truncated_dims_record_throws();
        test_corrupt_array_type_index_throws();
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
