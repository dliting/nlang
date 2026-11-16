//v0.13.9 phase ③ Task 3: the byref slot representation and its opcode
//family. OP_RefOfLocal/OP_RefOfPromoted/OP_RefOfField/OP_RefOfElement/
//OP_RefHandle/OP_LoadRef/OP_StoreRef build, move and dereference the
//(base record handle, cell offset) pair — 8 bytes, one uniform frame
//slot. The modules are REAL-compiled (ModuleBuilder), then the entry's
//bytecode is surgically rewritten: the real prefix (through its first
//top-level OP_Return) establishes the true frame layout, structs and
//arrays; the appended hand-written tail exercises the new opcodes. This
//honors the Phase-10 retirement of hand-BUILT modules (incomplete frame
//layouts crash) while keeping opcode-level white-box coverage ahead of
//the emitter tasks that will produce these instructions from source.
//GC-root classification is pinned the same way: the promoted-slot test
//flips a user local's descriptor kind and band-mask class to what the
//Task-4 emitter will declare, then proves the base record survives
//mid-loop collections (allocation churner, no flags).
#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/compiler/CastInfo.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "VmExecutor.h"
#include "VmBackend.h"
#include "NcuLoader.h"
#include "NcuLinker.h"
#include "BytecodeOps.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
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
#define REQUIRE(cond) \
    do { if (!(cond)) { FAIL("precondition: " #cond); return; } } while(0)

//Byref slot encoding (design D2): low half = base heap-record handle,
//high half = cell offset within that record. RTK_Byref is the declared
//kind of a slot holding such a pair.

static std::filesystem::path scratchDir()
{
    static const auto dir = std::filesystem::temp_directory_path()
        / "nlang_test_ref_ops";
    std::filesystem::create_directories(dir);
    return dir;
}

struct ExecResult {
    std::unique_ptr<VmExecutor> exec;
    std::unique_ptr<CompiledModule> mod;
    int exitCode = -1;
    bool built = false;
    bool ran = false;
    std::string error;
};

//Compile <tag>.n, load the closure, link, apply the caller's surgery to
//the linked module, execute, keep the executor alive so the heap stays
//inspectable. An executor throw (e.g. an unimplemented opcode) is
//captured in error instead of aborting the test.
static ExecResult runProgram(const std::string& tag,
                             const std::string& source,
    const std::function<void(CompiledModule&)>& surgery = nullptr)
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
    if (surgery) {
        try {
            surgery(*out.mod);
        } catch (const std::exception& e) {
            out.error = std::string("surgery: ") + e.what();
            return out;
        }
    }
    out.exec = std::make_unique<VmExecutor>();
    try {
        out.exitCode = out.exec->Execute(*out.mod);
        out.ran = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

//--- bytecode tail emitters (little-endian, the VM's operand order) ---

static void EmitOp(std::vector<uint8_t>& bc, OpCode op)
{
    bc.push_back(static_cast<uint8_t>(op));
}

static void EmitU8(std::vector<uint8_t>& bc, uint8_t v)
{
    bc.push_back(v);
}

static void EmitU16(std::vector<uint8_t>& bc, uint16_t v)
{
    bc.push_back(static_cast<uint8_t>(v & 0xFF));
    bc.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

static void EmitI32(std::vector<uint8_t>& bc, int32_t v)
{
    uint32_t u;
    std::memcpy(&u, &v, sizeof(u));
    bc.push_back(static_cast<uint8_t>(u & 0xFF));
    bc.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
    bc.push_back(static_cast<uint8_t>((u >> 16) & 0xFF));
    bc.push_back(static_cast<uint8_t>((u >> 24) & 0xFF));
}

static void EmitI64(std::vector<uint8_t>& bc, int64_t v)
{
    for (int i = 0; i < 8; ++i)
        bc.push_back(static_cast<uint8_t>(
            (static_cast<uint64_t>(v) >> (8 * i)) & 0xFF));
}

//Truncate the real prefix at its first top-level OP_Return (the test
//sources contain no early returns) and return the truncated size — the
//append point for the hand-written tail.
static size_t TruncateAtFirstReturn(std::vector<uint8_t>& bc)
{
    size_t pc = 0;
    while (pc < bc.size()) {
        const OpCode op = static_cast<OpCode>(bc[pc]);
        if (op == OpCode::OP_Return)
            return pc;
        pc += InstructionStride(op);
    }
    throw std::runtime_error("no OP_Return in prefix");
}

//Tail return discipline: main's return value is the accumulator —
//pResult aliases the return slot (the real prefix's `return code`
//compiles to var_local code → return), so every tail reloads the
//result slot into pResult before OP_Return; a stale accumulator (the
//last constant loaded) would otherwise become the exit code.
static void EmitTailReturn(std::vector<uint8_t>& bc, uint16_t codeOff)
{
    EmitOp(bc, OpCode::OP_VarLocal);
    EmitU16(bc, codeOff);
    EmitOp(bc, OpCode::OP_Return);
}

//The entry function record: the linked image's entryPoint names the
//global main() index (linked function names may carry package
//qualification, so a bare "main" lookup is not reliable).
static CompiledFunction& EntryFunction(CompiledModule& mod)
{
    if (mod.entryPoint < 0
        || static_cast<size_t>(mod.entryPoint) >= mod.functions.size())
        throw std::runtime_error("entry function not found");
    return mod.functions[static_cast<size_t>(mod.entryPoint)];
}

//Descriptor lookup by local name (the source declares every slot the
//tail addresses, so the offsets are the real backend's).
static LocalDescriptor& LocalByName(CompiledFunction& fn,
                                    const std::string& name)
{
    for (auto& ld : fn.locals)
        if (ld.name == name)
            return ld;
    throw std::runtime_error("local not found: " + name);
}

//Band-mask class for one user local's slot: SET the 2-bit class exactly
//(assignment, not OR — the pin must prove the declared class roots the
//slot, unhelped by a wider pre-existing class). This mirrors what the
//Task-4 emitter will ASSIGN for a &-bound local.
static void SetBandClass(CompiledFunction& fn, uint16_t slotOff,
                         uint8_t cls)
{
    const size_t bandIdx = slotOff / kFrameSlotBytes - fn.paramCount;
    const size_t byteIdx = bandIdx >> 2;
    REQUIRE(byteIdx < fn.evalAreaHandleMask.size());
    const size_t shift = (bandIdx & 3) * 2;
    const uint8_t clearMask =
        static_cast<uint8_t>(~(3u << shift));
    fn.evalAreaHandleMask[byteIdx] &= clearMask;
    fn.evalAreaHandleMask[byteIdx] |=
        static_cast<uint8_t>(cls << shift);
}

//Heap fingerprint scan: the first RTK_Struct record whose lo cell of
//field 0 and field 1 equal the given values (the 2-cell field stride).
static int32_t FindStructByFingerprint(VmExecutor& exec, int32_t f0,
                                       int32_t f1)
{
    const int32_t count =
        static_cast<int32_t>(exec.HeapSlotCountForTests());
    for (int32_t h = 1; h < count; ++h) {
        if (exec.HeapSlotKindForTests(h) != RTK_Struct)
            continue;
        if (exec.ReadHeapRecordCellForTests(h, 0) == f0
            && exec.ReadHeapRecordCellForTests(h, 2) == f1)
            return h;
    }
    return 0;
}

static int32_t FindArrayByElemCount(VmExecutor& exec, int32_t elemCount)
{
    const int32_t count =
        static_cast<int32_t>(exec.HeapSlotCountForTests());
    for (int32_t h = 1; h < count; ++h) {
        if (exec.HeapSlotKindForTests(h) != RTK_Array)
            continue;
        if (exec.ReadHeapRecordCellForTests(h, 3) == elemCount)
            return h;
    }
    return 0;
}

//Task 3's Task-4-preview: the promoted-local slot surgery. The Task-4
//emitter will declare an &-bound scalar local's slot as RTK_Byref with
//band class ESC_HeapIndex; the test declares the same on a real-compiled
//int local before the tail turns that slot into a byref pair.
static void PromoteLocalSurgery(CompiledModule& mod)
{
    CompiledFunction& fn = EntryFunction(mod);
    LocalDescriptor& x = LocalByName(fn, "x");
    x.typeKind = RTK_Byref;
    SetBandClass(fn, x.offset, ESC_HeapIndex);
}

//--- tests ---

//GC classification: a byref slot's base handle is a heap-domain record
//(struct/boxed/array — design D3), so the band-mask class is the single
//ESC_HeapIndex domain, never Dual.
static void testEvalSlotClassByref()
{
    TEST(evalSlotClassByrefIsHeapIndex);
    CHECK(VmBackend::EvalSlotClassOfKind(RTK_Byref) == ESC_HeapIndex,
          "RTK_Byref classifies as ESC_HeapIndex");
    PASS();
}

//struct local bind: RefOfLocal builds (p, 0); RefHandle extracts the
//base handle back into a struct-valued slot; the existing field ops
//write through it. Zero allocation on the whole tail.
static void testLocalStructBindWriteThrough()
{
    TEST(localStructBindWriteThrough);
    const char kSrc[] = "struct P {\n"
        "    int x;\n"
        "    int y;\n"
        "}\n"
        "int main() {\n"
        "    P p;\n"
        "    p.x = 1;\n"
        "    p.y = 2;\n"
        "    int r = 0;\n"
        "    int h = 0;\n"
        "    int t1 = 0;\n"
        "    int code = 0;\n"
        "    return code;\n"
        "}\n";
    //Prefix-only allocation baseline (the tail must add zero).
    ExecResult base = runProgram("ref_ops_local_base", kSrc);
    REQUIRE(base.built);
    REQUIRE(base.ran);

    ExecResult r = runProgram("ref_ops_local_bind", kSrc,
        [](CompiledModule& mod) {
            CompiledFunction& fn = EntryFunction(mod);
            const uint16_t pOff = LocalByName(fn, "p").offset;
            const uint16_t rOff = LocalByName(fn, "r").offset;
            const uint16_t hOff = LocalByName(fn, "h").offset;
            const uint16_t t1Off = LocalByName(fn, "t1").offset;
            const uint16_t codeOff = LocalByName(fn, "code").offset;
            auto& bc = fn.bytecode;
            bc.resize(TruncateAtFirstReturn(bc));
            EmitOp(bc, OpCode::OP_RefOfLocal);
            EmitU16(bc, rOff);
            EmitU16(bc, pOff);
            EmitOp(bc, OpCode::OP_RefHandle);
            EmitU16(bc, hOff);
            EmitU16(bc, rOff);
            EmitOp(bc, OpCode::OP_ConstInt32);
            EmitI32(bc, 9);
            EmitOp(bc, OpCode::OP_Assign);
            EmitU16(bc, t1Off);
            EmitOp(bc, OpCode::OP_StoreField);
            EmitU16(bc, hOff);
            EmitU16(bc, 0);   //x: byte offset 0
            EmitU16(bc, t1Off);
            EmitOp(bc, OpCode::OP_LoadField);
            EmitU16(bc, codeOff);
            EmitU16(bc, hOff);
            EmitU16(bc, 0);
            EmitTailReturn(bc, codeOff);
        });
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 9, "write-through read back (exit=" +
        std::to_string(r.exitCode) + ")");
    const int32_t pH = FindStructByFingerprint(*r.exec, 9, 2);
    CHECK(pH > 0, "P record carries x=9 (through the ref), y=2");
    CHECK(r.exec->HeapAllocCountForTests()
              == base.exec->HeapAllocCountForTests(),
          "bind + handle extraction + write-through allocate nothing");
    PASS();
}

//Scalar field bind: RefOfField names (owner record, field cell offset);
//LoadRef/StoreRef dereference the pair. The untouched sibling field
//pins the offset precision.
static void testScalarFieldBindLoadStore()
{
    TEST(scalarFieldBindLoadStore);
    const char kSrc[] = "struct P {\n"
        "    int x;\n"
        "    int y;\n"
        "}\n"
        "int main() {\n"
        "    P p;\n"
        "    p.x = 1;\n"
        "    p.y = 2;\n"
        "    int r = 0;\n"
        "    int t1 = 0;\n"
        "    int code = 0;\n"
        "    return code;\n"
        "}\n";
    ExecResult r = runProgram("ref_ops_field_bind", kSrc,
        [](CompiledModule& mod) {
            CompiledFunction& fn = EntryFunction(mod);
            const uint16_t pOff = LocalByName(fn, "p").offset;
            const uint16_t rOff = LocalByName(fn, "r").offset;
            const uint16_t t1Off = LocalByName(fn, "t1").offset;
            const uint16_t codeOff = LocalByName(fn, "code").offset;
            auto& bc = fn.bytecode;
            bc.resize(TruncateAtFirstReturn(bc));
            EmitOp(bc, OpCode::OP_RefOfField);
            EmitU16(bc, rOff);
            EmitU16(bc, pOff);
            EmitU16(bc, 2);   //y: cell offset 2 (field 1's lo cell)
            EmitOp(bc, OpCode::OP_ConstInt32);
            EmitI32(bc, 7);
            EmitOp(bc, OpCode::OP_Assign);
            EmitU16(bc, t1Off);
            EmitOp(bc, OpCode::OP_StoreRef);
            EmitU8(bc, RTK_Int32);
            EmitU16(bc, rOff);
            EmitU16(bc, t1Off);
            EmitOp(bc, OpCode::OP_LoadRef);
            EmitU8(bc, RTK_Int32);
            EmitU16(bc, codeOff);
            EmitU16(bc, rOff);
            EmitTailReturn(bc, codeOff);
        });
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 7, "LoadRef through the field ref (exit=" +
        std::to_string(r.exitCode) + ")");
    const int32_t pH = FindStructByFingerprint(*r.exec, 1, 7);
    CHECK(pH > 0, "y cell overwritten in place; x untouched");
    PASS();
}

//Wide scalar field: the 8-byte family reads/writes BOTH cells of the
//field row (the long lo/hi placement mirrors test_stabilize_copy's
//wide-row pin). The round-trip lands the loaded pair in the sibling
//double field for heap verification.
static void testWideScalarTwoCellStoreLoad()
{
    TEST(wideScalarTwoCellStoreLoad);
    const char kSrc[] = "struct W {\n"
        "    long id;\n"
        "    double ratio;\n"
        "}\n"
        "int main() {\n"
        "    W w;\n"
        "    w.id = 31;\n"
        "    w.ratio = 2.5;\n"
        "    int r = 0;\n"
        "    int t2 = 0;\n"
        "    int code = 0;\n"
        "    return code;\n"
        "}\n";
    ExecResult r = runProgram("ref_ops_wide_field", kSrc,
        [](CompiledModule& mod) {
            CompiledFunction& fn = EntryFunction(mod);
            const uint16_t wOff = LocalByName(fn, "w").offset;
            const uint16_t rOff = LocalByName(fn, "r").offset;
            const uint16_t t2Off = LocalByName(fn, "t2").offset;
            const uint16_t codeOff = LocalByName(fn, "code").offset;
            auto& bc = fn.bytecode;
            bc.resize(TruncateAtFirstReturn(bc));
            EmitOp(bc, OpCode::OP_ConstInt64);
            EmitI64(bc, 5000000000LL);
            EmitOp(bc, OpCode::OP_Assign);
            EmitU16(bc, t2Off);
            EmitOp(bc, OpCode::OP_RefOfField);
            EmitU16(bc, rOff);
            EmitU16(bc, wOff);
            EmitU16(bc, 0);   //id: cell offset 0
            EmitOp(bc, OpCode::OP_StoreRef);
            EmitU8(bc, RTK_Long);
            EmitU16(bc, rOff);
            EmitU16(bc, t2Off);
            EmitOp(bc, OpCode::OP_LoadRef);
            EmitU8(bc, RTK_Long);
            EmitU16(bc, t2Off);
            EmitU16(bc, rOff);
            //Round-trip: the loaded 8-byte pair into ratio (byte 8).
            EmitOp(bc, OpCode::OP_StoreField);
            EmitU16(bc, wOff);
            EmitU16(bc, 8);
            EmitU16(bc, t2Off);
            EmitOp(bc, OpCode::OP_ConstInt32);
            EmitI32(bc, 42);
            EmitOp(bc, OpCode::OP_Assign);
            EmitU16(bc, codeOff);
            EmitTailReturn(bc, codeOff);
        });
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 42, "program exit");
    const int32_t wH = FindStructByFingerprint(*r.exec, 705032704,
                                               705032704);
    REQUIRE(wH > 0);
    const int32_t idLo = r.exec->ReadHeapRecordCellForTests(wH, 0);
    const int32_t idHi = r.exec->ReadHeapRecordCellForTests(wH, 1);
    CHECK(idLo == 705032704 && idHi == 1,
          "id: both cells of 5000000000 stored through the ref");
    const int32_t ratioLo =
        r.exec->ReadHeapRecordCellForTests(wH, 2);
    const int32_t ratioHi =
        r.exec->ReadHeapRecordCellForTests(wH, 3);
    CHECK(ratioLo == idLo && ratioHi == idHi,
          "LoadRef restored the full pair (round-trip into ratio)");
    PASS();
}

//Array element bind: RefOfElement resolves the record's own payload
//base (header + rank) at runtime; the write lands in the payload cell
//the flat index names.
static void testArrayElementBindWriteThrough()
{
    TEST(arrayElementBindWriteThrough);
    const char kSrc[] = "int main() {\n"
        "    int[] a = [5, 6];\n"
        "    int idx = 0;\n"
        "    int r = 0;\n"
        "    int t1 = 0;\n"
        "    int code = 0;\n"
        "    return code;\n"
        "}\n";
    ExecResult r = runProgram("ref_ops_elem_bind", kSrc,
        [](CompiledModule& mod) {
            CompiledFunction& fn = EntryFunction(mod);
            const uint16_t aOff = LocalByName(fn, "a").offset;
            const uint16_t idxOff = LocalByName(fn, "idx").offset;
            const uint16_t rOff = LocalByName(fn, "r").offset;
            const uint16_t t1Off = LocalByName(fn, "t1").offset;
            const uint16_t codeOff = LocalByName(fn, "code").offset;
            auto& bc = fn.bytecode;
            bc.resize(TruncateAtFirstReturn(bc));
            EmitOp(bc, OpCode::OP_ConstInt32);
            EmitI32(bc, 0);
            EmitOp(bc, OpCode::OP_Assign);
            EmitU16(bc, idxOff);
            EmitOp(bc, OpCode::OP_RefOfElement);
            EmitU16(bc, rOff);
            EmitU16(bc, aOff);
            EmitU16(bc, idxOff);
            EmitOp(bc, OpCode::OP_ConstInt32);
            EmitI32(bc, 9);
            EmitOp(bc, OpCode::OP_Assign);
            EmitU16(bc, t1Off);
            EmitOp(bc, OpCode::OP_StoreRef);
            EmitU8(bc, RTK_Int32);
            EmitU16(bc, rOff);
            EmitU16(bc, t1Off);
            EmitOp(bc, OpCode::OP_LoadRef);
            EmitU8(bc, RTK_Int32);
            EmitU16(bc, codeOff);
            EmitU16(bc, rOff);
            EmitTailReturn(bc, codeOff);
        });
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 9, "LoadRef through the element ref (exit=" +
        std::to_string(r.exitCode) + ")");
    const int32_t aH = FindArrayByElemCount(*r.exec, 2);
    REQUIRE(aH > 0);
    const int32_t payloadBase = 6;  //ArrayHeaderCells(1): 4+1 then pad
    CHECK(r.exec->ReadHeapRecordCellForTests(aH, payloadBase) == 9,
          "payload cell 0 overwritten in place");
    CHECK(r.exec->ReadHeapRecordCellForTests(aH, payloadBase + 1) == 6,
          "payload cell 1 untouched");
    PASS();
}

//The GC pin (design D3/spec §3.2): a byref slot's base record must
//survive mid-loop collections with the slot as its only anchor. The
//surgery declares the promoted-slot shape the Task-4 emitter will
//produce (descriptor RTK_Byref + band class ESC_HeapIndex); the tail
//boxes a value, pairs it into the slot via RefOfPromoted, churns past
//the collection thresholds on a backedge-safepoint loop, then loads
//through the pair. The alloc pin keeps the churn honest — the survival
//must be rooting, not a vacuous no-allocation run.
static void testGcSurvivalThroughByrefSlot()
{
    TEST(gcSurvivalThroughByrefSlot);
    const char kSrc[] = "int main() {\n"
        "    int x = 7;\n"
        "    int c = 0;\n"
        "    int tZero = 0;\n"
        "    int tOne = 0;\n"
        "    int tCond = 0;\n"
        "    int tStg = 0;\n"
        "    int code = 0;\n"
        "    return code;\n"
        "}\n";
    ExecResult r = runProgram("ref_ops_gc_survival", kSrc,
        [](CompiledModule& mod) {
            PromoteLocalSurgery(mod);
            CompiledFunction& fn = EntryFunction(mod);
            const uint16_t xOff = LocalByName(fn, "x").offset;
            const uint16_t cOff = LocalByName(fn, "c").offset;
            const uint16_t zeroOff = LocalByName(fn, "tZero").offset;
            const uint16_t oneOff = LocalByName(fn, "tOne").offset;
            const uint16_t condOff = LocalByName(fn, "tCond").offset;
            const uint16_t stgOff = LocalByName(fn, "tStg").offset;
            const uint16_t codeOff = LocalByName(fn, "code").offset;
            auto& bc = fn.bytecode;
            bc.resize(TruncateAtFirstReturn(bc));
            //x = (box(7), payload cell): the promoted-slot pair.
            //RefOfPromoted writes locals[dst] directly — no accumulator
            //round-trip (an Assign here would clobber the pair's cell
            //half with the stale accumulator).
            EmitOp(bc, OpCode::OP_ConstInt32);
            EmitI32(bc, 7);
            EmitOp(bc, OpCode::OP_Assign);
            EmitU16(bc, stgOff);
            EmitOp(bc, OpCode::OP_VarLocal);
            EmitU16(bc, stgOff);
            EmitOp(bc, OpCode::OP_Box);
            EmitU8(bc, RTK_Int32);
            EmitOp(bc, OpCode::OP_Assign);
            EmitU16(bc, stgOff);
            EmitOp(bc, OpCode::OP_RefOfPromoted);
            EmitU16(bc, xOff);
            EmitU16(bc, stgOff);
            //Churn: 2000 discarded boxes cross the thresholds; the
            //backward Jump is the collection safepoint. Loop while
            //c != 0 (cmpOp 5 = NE; JumpIfNot exits at the zero).
            EmitOp(bc, OpCode::OP_ConstInt32);
            EmitI32(bc, 2000);
            EmitOp(bc, OpCode::OP_Assign);
            EmitU16(bc, cOff);
            EmitOp(bc, OpCode::OP_ConstInt32);
            EmitI32(bc, 0);
            EmitOp(bc, OpCode::OP_Assign);
            EmitU16(bc, zeroOff);
            EmitOp(bc, OpCode::OP_ConstInt32);
            EmitI32(bc, 1);
            EmitOp(bc, OpCode::OP_Assign);
            EmitU16(bc, oneOff);
            const size_t loopTop = bc.size();
            EmitOp(bc, OpCode::OP_VarLocal);
            EmitU16(bc, cOff);
            EmitOp(bc, OpCode::OP_Assign);
            EmitU16(bc, condOff);
            EmitOp(bc, OpCode::OP_Cmp);
            EmitU8(bc, RTK_Int32);
            EmitU8(bc, 5);    //cmpOp 5 = NE
            EmitU16(bc, condOff);
            EmitU16(bc, zeroOff);
            EmitOp(bc, OpCode::OP_JumpIfNot);
            const size_t exitPatchIdx = bc.size();
            EmitU16(bc, 0);   //patched below (exit)
            EmitU16(bc, condOff);
            EmitOp(bc, OpCode::OP_VarLocal);
            EmitU16(bc, stgOff);
            EmitOp(bc, OpCode::OP_Box);
            EmitU8(bc, RTK_Int32);
            EmitOp(bc, OpCode::OP_Sub);
            EmitU8(bc, RTK_Int32);
            EmitU16(bc, cOff);
            EmitU16(bc, oneOff);
            EmitOp(bc, OpCode::OP_Jump);
            EmitU16(bc, static_cast<uint16_t>(loopTop));
            const size_t exitPc = bc.size();
            bc[exitPatchIdx] = static_cast<uint8_t>(exitPc & 0xFF);
            bc[exitPatchIdx + 1] =
                static_cast<uint8_t>((exitPc >> 8) & 0xFF);
            //Load through the promoted pair: the boxed record survived.
            EmitOp(bc, OpCode::OP_LoadRef);
            EmitU8(bc, RTK_Int32);
            EmitU16(bc, codeOff);
            EmitU16(bc, xOff);
            EmitTailReturn(bc, codeOff);
        });
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 7, "base record survived mid-loop collections "
        "(exit=" + std::to_string(r.exitCode) + ")");
    CHECK(r.exec->HeapAllocCountForTests() >= 2001,
          "churn actually allocated (alloc=" +
        std::to_string(r.exec->HeapAllocCountForTests()) + ")");
    PASS();
}

//Reference value move: assigning one byref local to another is the
//existing 8-byte raw slot copy — 1000 iterations allocate nothing and
//the copy aliases the same storage (load through r2).
static void testByrefRawMoveZeroAlloc()
{
    TEST(byrefRawMoveZeroAlloc);
    const char kSrc[] = "struct P {\n"
        "    int x;\n"
        "    int y;\n"
        "}\n"
        "int main() {\n"
        "    P p;\n"
        "    p.x = 1;\n"
        "    p.y = 2;\n"
        "    int r = 0;\n"
        "    int r2 = 0;\n"
        "    int code = 0;\n"
        "    return code;\n"
        "}\n";
    ExecResult base = runProgram("ref_ops_move_base", kSrc);
    REQUIRE(base.built);
    REQUIRE(base.ran);
    ExecResult r = runProgram("ref_ops_raw_move", kSrc,
        [](CompiledModule& mod) {
            CompiledFunction& fn = EntryFunction(mod);
            const uint16_t pOff = LocalByName(fn, "p").offset;
            const uint16_t rOff = LocalByName(fn, "r").offset;
            const uint16_t r2Off = LocalByName(fn, "r2").offset;
            const uint16_t codeOff = LocalByName(fn, "code").offset;
            auto& bc = fn.bytecode;
            bc.resize(TruncateAtFirstReturn(bc));
            EmitOp(bc, OpCode::OP_RefOfLocal);
            EmitU16(bc, rOff);
            EmitU16(bc, pOff);
            for (int i = 0; i < 1000; ++i) {
                EmitOp(bc, OpCode::OP_VarLocal);
                EmitU16(bc, rOff);
                EmitOp(bc, OpCode::OP_Assign);
                EmitU16(bc, r2Off);
            }
            EmitOp(bc, OpCode::OP_LoadRef);
            EmitU8(bc, RTK_Int32);
            EmitU16(bc, codeOff);
            EmitU16(bc, r2Off);
            EmitTailReturn(bc, codeOff);
        });
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 1, "the copy aliases the same storage (exit=" +
        std::to_string(r.exitCode) + ")");
    CHECK(r.exec->HeapAllocCountForTests()
              == base.exec->HeapAllocCountForTests(),
          "1000 raw byref moves allocate nothing");
    PASS();
}

int main()
{
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();
    testEvalSlotClassByref();
    testLocalStructBindWriteThrough();
    testScalarFieldBindLoadStore();
    testWideScalarTwoCellStoreLoad();
    testArrayElementBindWriteThrough();
    testGcSurvivalThroughByrefSlot();
    testByrefRawMoveZeroAlloc();
    std::cerr << "ref_ops: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
