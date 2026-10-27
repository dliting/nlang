/*---
    GenerateUnits.cpp — 字节码产码驱动：单函数发射（GenerateFunction）、
    每单元的字节码游走（GenerateAllBytecode）与逐单元驱动（GenerateUnits）。
    从 VmBackend.cpp 拆出（phase 6 B1 产码切换战役）。
---*/
#include "VmBackend.h"
#include "builder/ModuleRegistry.h"
#include "builder/ThreadNamespace.h"
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/GenericInstanceRegistry.h>

namespace nlang {

void VmBackend::GenerateAllBytecode(SnNamespace& root) {
    GenerateBytecodeRecursive(root);
    //Declared-generic instantiations are detached nodes — the tree walk
    //above never reaches them. Their member records register through
    //RegisterFunctions; their emission (native record fill, signature
    //descriptors) runs here with the same body-or-native filter the
    //tree walk applies.
    m_pRegistry->DeclaredGenerics().ForEachInstance(m_currentUnitIdx,
        [this](SnClassDecl& inst) {
            for (auto& member : inst.Members())
            {
                if (member.Kind() != NK_Function)
                    continue;
                auto& func = static_cast<SnFunction&>(member);
                if (!func.Body() && !func.ContainFlags(NF_Native))
                    continue;
                auto it = m_funcIndexMap.find(&func);
                if (it != m_funcIndexMap.end())
                    GenerateFunction(func, it->second);
            }
        });
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
        //Multi-segment refusal already happened in the whole-tree pre-check
        //(RejectMultiSegmentNatives) — every native the build accepted
        //reaches the record with its signature descriptors.
        EmitNativeFunctionRecord(func, compiledFunc);
        m_compiledModule.functions[funcIdx] = std::move(compiledFunc);
        return;
    }

    FuncContext ctx;
    ctx.func = &compiledFunc;
    ctx.nextOffset = 0;
    m_currFunc = &ctx;
    //The emitter pointer only ever points at this function's stack
    //emitter (set by EmitStatement/EmitExpression). Null it here so
    //AllocLocal — which runs for params before body emission — reads a
    //defined null instead of the previous function's destroyed emitter
    //and records declPc 0 ("live from entry") for params/__this.
    m_pCurrEmitter = nullptr;

    AllocParamsAndDefaults(func, ctx, compiledFunc);
    CallSlotStats stats = ReserveReturnAndCallSlots(func, ctx, compiledFunc);

    BytecodeEmitter emitter;
    EmitBodyAndImplicitReturn(func, ctx, emitter);

    compiledFunc.bytecode = emitter.TakeBytes();
    compiledFunc.localsSize = ctx.nextOffset;

    CheckEvalAreaWalkerDrift(func, ctx, stats);
    BuildEvalAreaHandleMask(compiledFunc, ctx);

    m_currFunc = nullptr;
    m_compiledModule.functions[funcIdx] = std::move(compiledFunc);
}

//GenerateFunction's native arm. A FuncContext lets FillNativeFunctionRecord
//call AllocLocal (frame-layout locals for the kind-裁决 chain); null the
//emitter so AllocLocal reads a defined null (not the previous function's
//destroyed emitter).
void VmBackend::EmitNativeFunctionRecord(SnFunction& func,
                                        CompiledFunction& compiledFunc) {
    FuncContext ctx;
    ctx.func = &compiledFunc;
    ctx.nextOffset = 0;
    m_currFunc = &ctx;
    m_pCurrEmitter = nullptr;
    FillNativeFunctionRecord(func, compiledFunc);
    StampIntrinsicChannel(func, compiledFunc);
    m_currFunc = nullptr;
}

//The intrinsic-channel owner of a native declaration. Owner-of-CONTEXT,
//not the flat tag: class members carry no owner tag of their own (the
//IsOwnUnit contract), so a declared prelude ctor/method would read
//NO_OWNER from the flat tag and never stamp. Declared-generic instance
//copies are detached (they never entered the syntax tree), so even the
//context walk finds no tagged ancestor — the template's owner speaks
//for them (the copies substitute into the template's signatures: same
//package, same intrinsic channel).
uint32_t VmBackend::IntrinsicChannelOwnerOf(const SnFunction& func) const
{
    uint32_t owner = m_pRegistry->OwnerOfContext(func);
    if (owner != ModuleRegistry::NO_OWNER)
        return owner;
    const SyntaxNode* pParent = func.Parent();
    if (!pParent || pParent->Kind() != NK_ClassDecl)
        return ModuleRegistry::NO_OWNER;
    auto* pInstance = static_cast<const SnClassDecl*>(pParent);
    if (!pInstance->IsGenericInstantiation())
        return ModuleRegistry::NO_OWNER;
    const SnClassDecl* pTemplate
        = m_pRegistry->DeclaredGenerics().InstanceOf(pInstance);
    if (!pTemplate)
        return ModuleRegistry::NO_OWNER;
    return m_pRegistry->OwnerOf(*pTemplate);
}

//StampIntrinsicChannel: the binding-table arm of the native record
//(builtin-decls source-of-truth design §4.5). A declaration whose
//OWNING unit's package has binding-table rows flips onto the intrinsic
//channel — isNative=false + intrinsicId, signature metadata kept
//(cross-module stubs rebuild from it). Identity is the owning unit's
//module path alone, never the library flag (K3: stdlib.nproj packs
//thread.n as a project unit). Two arms: package-level free functions
//key "pkg.func" (the thread channel as today); type members key
//"Class.method" / constructors "Class" — declared methods must stamp
//so the intrinsicId dispatch sites stay live. Keys outside the table
//keep host-native dispatch (the ordinary native channel).
void VmBackend::StampIntrinsicChannel(SnFunction& func,
                                      CompiledFunction& compiledFunc) {
    const SyntaxNode* pParent = func.Parent();
    const bool bTypeMember = pParent
        && (pParent->Kind() == NK_ClassDecl
            || pParent->Kind() == NK_InterfaceDecl
            || pParent->Kind() == NK_EnumDecl
            || pParent->Kind() == NK_StructDecl);
    const uint32_t owner = IntrinsicChannelOwnerOf(func);
    if (owner == ModuleRegistry::NO_OWNER)
        return;
    const std::string packagePath = m_pRegistry->ModulePathOf(owner);
    if (!IsStampedBindingPackage(packagePath))
        return;
    //Constructor spelling mirrors AppendClassMemberMethods'
    //classification (same-name non-static member), so the stamping gate
    //and the method tables cannot drift. The four enclosing kinds are
    //all SnField-derived; the cast is the name/flag accessor.
    const SnField& parentField = static_cast<const SnField&>(*pParent);
    std::string key;
    if (!bTypeMember)
        key = packagePath + "." + func.Name();
    else if (func.Name() == parentField.Name()
        && !func.ContainFlags(NF_Static))
        key = parentField.Name();
    else
        key = parentField.Name() + "." + func.Name();
    const uint16_t intrinsicId = LookupIntrinsicId(packagePath, key);
    if (intrinsicId != INTR_None) {
        compiledFunc.isNative = false;
        compiledFunc.intrinsicId = intrinsicId;
    }
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
    //Whole-tree, once: which prelude bare names a foreign declaration
    //claims (spec §6.8) — the registration walks below consult the set.
    ComputeShadowedPreludeNames(root);
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
        //0.9.11 D2: struct ctor indices ride the same function records,
        //so the struct counterpart runs beside the class one.
        PopulateStructMethods(root);
        //0.9.10 D5: after RegisterFunctions/PopulateClassMethods — the
        //interface table indexes the function records they created. Must
        //run before GenerateAllBytecode: body emission can append
        //placeholder function records, and the table's indices must stay
        //in the own region.
        RegisterInterfaces(root);
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

//K2 slice 5: root class for one RTK kind byte. The kinds mirror
//MarkFrameLocals' routing exactly — string slots carry store handles,
//the four reference kinds carry heap indices, everything else is scalar.
uint8_t VmBackend::EvalSlotClassOfKind(uint8_t rtk) {
    switch (rtk) {
    case RTK_String:
        return ESC_StrHandle;
    case RTK_Struct:
    case RTK_Class:
    case RTK_Array:
    case RTK_Func:
        return ESC_HeapIndex;
    default:
        return ESC_None;
    }
}

//K2 slice 5: pack the per-slot root classes for the frame band
//[paramCount, localsSize). Union semantics, layered by slot ownership:
//every slot starts None (nothing live claimed it); the fixed scratch
//region [band start, evalAreaBase) — temp pool plus callParam staging,
//whose values are heterogeneous or transient — is forced to the
//conservative Dual; named locals and the return slot are ASSIGNED their
//declared kind (each exclusively owns its slot); claim classes OR in,
//which can only widen (the same slot serves many claims across the
//function — union, not last-wins, and never narrower than any one
//claim). OR-merging into a Dual-initialized array would make every
//claim a no-op — the None start is what makes claim precision carry.
//Params are not in the band — their kinds are declared and
//MarkFrameLocals keeps scanning them precisely.
void VmBackend::BuildEvalAreaHandleMask(CompiledFunction& cf,
                                        const FuncContext& ctx) {
    const size_t totalSlots = cf.localsSize / kFrameSlotBytes;
    if (totalSlots <= cf.paramCount) {
        cf.evalAreaHandleMask.clear();  //bandless (native/builtin records)
        return;
    }
    const size_t bandSlots = totalSlots - cf.paramCount;
    std::vector<uint8_t> classes(bandSlots, ESC_None);
    const size_t scratchEnd = ctx.evalAreaBase / kFrameSlotBytes
        - cf.paramCount;
    for (size_t i = 0; i < scratchEnd && i < bandSlots; ++i)
        classes[i] = ESC_Dual;
    for (const auto& ld : cf.locals) {
        if (ld.isParam)
            continue;
        const size_t idx = ld.offset / kFrameSlotBytes - cf.paramCount;
        if (idx < bandSlots)  //defensive vs. multi-slot descriptors
            classes[idx] = EvalSlotClassOfKind(ld.typeKind);
    }
    if (cf.returnTypeKind != RTK_Void) {
        const size_t idx = ctx.returnSlot / kFrameSlotBytes - cf.paramCount;
        if (idx < bandSlots)
            classes[idx] = EvalSlotClassOfKind(
                static_cast<uint8_t>(cf.returnTypeKind));
    }
    for (const auto& rec : ctx.claimClasses) {
        const size_t first = rec.baseOffset / kFrameSlotBytes
            - cf.paramCount;
        for (size_t i = 0; i < rec.slots; ++i)
            if (first + i < bandSlots)
                classes[first + i] |= rec.slotClass;
    }
    cf.evalAreaHandleMask.assign((bandSlots + 3) / 4, 0);
    for (size_t i = 0; i < bandSlots; ++i)
        cf.evalAreaHandleMask[i >> 2] |=
            static_cast<uint8_t>(classes[i] << ((i & 3) * 2));
}

} //namespace nlang
