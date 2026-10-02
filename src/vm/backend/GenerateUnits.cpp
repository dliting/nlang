/*---
    GenerateUnits.cpp — 字节码产码驱动：单函数发射（GenerateFunction）、
    合并单趟（GenerateAllBytecode）与 phase 6 逐单元产码（GenerateUnits）。
    从 VmBackend.cpp 拆出（phase 6 B1 产码切换战役）。
---*/
#include "VmBackend.h"
#include "builder/ModuleRegistry.h"
#include <nlang/compiler/SnData.h>

namespace nlang {

void VmBackend::GenerateAllBytecode(SnNamespace& root) {
    GenerateBytecodeRecursive(root);
    //Phase 5: entryPoint write-side close-out (definition in Register.cpp).
    ResolveEntryPoint(root);
}

//Per-function emission driver. The record is emitted OUT of the table:
//body emission can append placeholder function records (a cross-unit
//call slots through FunctionSymbolSlot), reallocating
//m_compiledModule.functions — a record left in the table (and
//FuncContext::func pointing into it) would dangle across it. The
//vacated slot keeps the index reserved; nothing reads own records
//during emission (placeholder dedup scans the import region only),
//and the finished record moves back after.
void VmBackend::GenerateFunction(SnFunction& func, size_t funcIdx) {
    CompiledFunction compiledFunc =
        std::move(m_compiledModule.functions[funcIdx]);

    CollectSignatureTypeDescs(func, compiledFunc);

    if (func.ContainFlags(NF_Native)) {
        //Rejection keeps the signature descs in the record (the build
        //fails on the logged error, but the vacated slot must not keep
        //a moved-from husk either way).
        if (!RejectMultiSegmentNativePackage(func))
            FillNativeFunctionRecord(func, compiledFunc);
        m_compiledModule.functions[funcIdx] = std::move(compiledFunc);
        return;
    }

    FuncContext ctx;
    ctx.func = &compiledFunc;
    ctx.nextOffset = 0;
    m_currFunc = &ctx;

    AllocParamsAndDefaults(func, ctx, compiledFunc);
    CallSlotStats stats = ReserveReturnAndCallSlots(func, ctx, compiledFunc);

    BytecodeEmitter emitter;
    EmitBodyAndImplicitReturn(func, ctx, emitter);

    compiledFunc.bytecode = emitter.TakeBytes();
    compiledFunc.localsSize = ctx.nextOffset;

    CheckEvalAreaWalkerDrift(func, ctx, stats);

    m_currFunc = nullptr;
    m_compiledModule.functions[funcIdx] = std::move(compiledFunc);
}

//Phase 6 per-unit build (VmBackend.h contract): one image per unit, every
//cross-unit reference left as a placeholder slot + import record. The
//registration phases mirror GenerateStatements minus both MergeImported
//passes — nothing external is merged in; the load-time linker resolves
//the placeholders from the peer images.
VmBackend::UnitBuildResult VmBackend::GenerateUnits(
    SnNamespace& root, const std::vector<uint32_t>& unitIdxs) {
    UnitBuildResult out;
    for (uint32_t unitIdx : unitIdxs) {
        BeginUnit(unitIdx, m_pRegistry->ModulePathOf(unitIdx));
        RegisterBuiltinClasses();
        RegisterStructs(root);
        RegisterClasses(root);
        ResolveStructClassRefs();
        RegisterArrayTypes(root);
        RegisterEnums(root);
        RegisterFunctions(root);
        PopulateClassMethods(root);
        GenerateAllBytecode(root);
        //Entry collection: the merged-namespace front end rejects
        //duplicate top-level names across units at resolve time, so at
        //most one unit owns a main and first-wins needs no tie-break —
        //the per-unit gate in ResolveEntryPoint routes the entry to its
        //owning unit's image only.
        if (!m_entryKey.empty())
            out.entryKey = m_entryKey;
        out.units.push_back(std::move(m_compiledModule));
    }
    //Leave the backend in the default mode: a reused instance must not
    //keep filtering walks to the last generated unit.
    m_currentUnitIdx = MERGED_MODE;
    return out;
}

} //namespace nlang
