/*---
    GenerateUnits.cpp — 字节码产码驱动：单函数发射（GenerateFunction）、
    每单元的字节码游走（GenerateAllBytecode）与逐单元驱动（GenerateUnits）。
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
        //Multi-segment refusal already happened in the whole-tree
        //pre-check (RejectMultiSegmentNatives below) — every native the
        //build accepted reaches the record with its signature
        //descriptors.
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
//cross-unit reference left as a placeholder slot + import record — nothing
//external is merged in; the load-time linker resolves the placeholders
//from the peer images.
VmBackend::UnitBuildResult VmBackend::GenerateUnits(
    SnNamespace& root, const std::vector<uint32_t>& unitIdxs) {
    //Whole-tree pre-checks, before any image is built: both concerns
    //span units, so neither is judgeable from inside the per-unit loop
    //(library TUs never reach codegen, and each unit's entry scan only
    //sees its own candidates). Either refusal logs and fails the build.
    if (RejectAmbiguousUnitEntries(root) || RejectMultiSegmentNatives(root))
        return {};
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
        //Entry collection: the pre-check above already refused a unit
        //set with two executable mains, so at most one unit owns an
        //entry and first-wins needs no tie-break — the per-unit gate in
        //ResolveEntryPoint routes the entry to its owning unit's image
        //only.
        if (!m_entryKey.empty())
            out.entryKey = m_entryKey;
        out.units.push_back(std::move(m_compiledModule));
    }
    //Drop the unit selection so a reused instance cannot leak the last
    //unit's filter into a later walk.
    m_currentUnitIdx = NO_UNIT;
    return out;
}

} //namespace nlang
