/*---
    EmitCall.cpp — 调用发射辅助族：pResult 刷新、复合赋值运算、
    标准库调用、绑定发射、out 参数溢写、调用实参发射。
    从 VmBackend.cpp 抽取（2026-09-25 可维护性重构，零行为变化）。
---*/
#include "VmBackend.h"
#include "EmitPrimOps.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>

namespace nlang {


//Several opcodes read the pResult accumulator (OP_Box/OP_Unbox,
//OP_PrimCast/OP_Prim_to_str).
//EmitExpression only leaves the value in pResult when the source's final
//opcode writes the accumulator (var_local, consts, calls); locals-writing
//sources (binary arithmetic, field/element loads) leave it stale — the
//cast_f2i quirk and the `"s" + (a+b)` dedup bug are both this hole.
//Reload pResult from the slot the value is guaranteed to live in before
//emitting any accumulator-reading opcode.
void VmBackend::EmitPResultRefresh(BytecodeEmitter& emitter, uint16_t slot) {
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(slot);
}

//Phase 9a: emit a compound-assign arithmetic op.
//Executes: locals[dst] = locals[dst] <op> locals[src]
//where op ∈ {Add, Sub, Mul, Div, Mod}. Type determines i32/f32 variant.
void VmBackend::EmitCompoundOp(int opInt,
        BytecodeEmitter& emitter, uint16_t dst, uint16_t src,
        SnField* lhsType) {
    auto op = static_cast<SnBinaryExpr::Operator>(opInt);
    bool isString = lhsType && lhsType->Kind() == NK_String;

    //String only supports += (concat). All other ops are invalid.
    if (isString) {
        if (op == SnBinaryExpr::OP_Add) {
            emitter.Emit(OpCode::OP_Concat_str);
            emitter.EmitUint16(dst);
            emitter.EmitUint16(src);
        }
        //Other ops on string silently ignored (should be caught by resolver).
        return;
    }

    OpCode opc;
    switch (op) {
    case SnBinaryExpr::OP_Add: opc = OpCode::OP_Add; break;
    case SnBinaryExpr::OP_Sub: opc = OpCode::OP_Sub; break;
    case SnBinaryExpr::OP_Mul: opc = OpCode::OP_Mul; break;
    case SnBinaryExpr::OP_Div: opc = OpCode::OP_Div; break;
    case SnBinaryExpr::OP_Mod: opc = OpCode::OP_Mod; break;
    default: return;  //not an arithmetic op
    }
    //0.7.5: kind-immediate family — compound assigns carry the LHS
    //type's kind (enum≡int32 normalized). Every numeric kind rides the
    //same tables as the binary operator, % included (ModInt template
    //covers the 8/16/64-bit integer rows, ModFloat is fmod).
    NodeKind numKind = BinNumericKindOf(
        lhsType ? lhsType->Kind() : NK_Int32);
    EmitBinOp(emitter, opc, numKind, dst, src);
}

//Phase 11: namespace-qualified stdlib call (math.sqrt(x), io.print(s)).
//Same shape as the string.equals emission minus the receiver: args stage
//in an evalArea claim (nested calls in later args would clobber
//callParamBase), then bulk-copy to callParamBase reading from slot 0 —
//the namespace intrinsic ABI has no this (see StdLib.h). The MemberExpr
//walker reserves 1+argCount for this shape; claiming only argCount
//over-reserves by one slot, which is the safe direction.
void VmBackend::EmitStdLibCall(const StdLibEntry& entry,
        SnInvokeExpr& invoke, BytecodeEmitter& emitter,
        uint16_t resultOffset) {
    uint16_t argCount = 0;
    for (auto& p : invoke.Params()) ++argCount;
    EvalAreaClaim claim(*this, argCount);
    uint16_t claimBase = claim.base();
    uint16_t paramIdx = 0;
    for (auto& param : invoke.Params()) {
        EmitExpression(param, emitter,
            claimBase + paramIdx * kFrameSlotBytes);
        //io.print coercion (see EmitStdLibArgToString).
        if (entry.coerceToString) {
            EmitStdLibArgToString(param, emitter,
                claimBase + paramIdx * kFrameSlotBytes);
        }
        ++paramIdx;
    }
    CopyClaimToCallParams(claimBase, argCount, emitter);
    emitter.Emit(OpCode::OP_CallIntrinsic);
    emitter.EmitUint16(entry.intrinsicId);
    emitter.EmitUint16(m_currFunc->callParamBase);
    //Void entries (io.print) have nothing to assign — the InvokeStmt
    //handler already staged a throwaway claim slot as resultOffset.
    if (static_cast<StdLibReturnType>(entry.returnType) != SLRT_Void) {
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
    }
    emitter.Emit(OpCode::OP_ParaEnd);
}

//io.print coercion for one already-emitted stdlib argument: scalar
//args convert to string in their claim slot right after being emitted
//(registry-driven OP_Prim_to_str — every current and future scalar
//rides the same instruction); arrays and func handles keep their
//dedicated conversions.
//pResult discipline — the to_str opcodes rewrite the accumulator in
//place, so the sequence must be load-slot -> convert -> store-slot
//(the cast_f2i / string-pool-dedup bug family otherwise).
//String args pass through (nothing emitted).
void VmBackend::EmitStdLibArgToString(SnExpression& param,
        BytecodeEmitter& emitter, uint16_t slot) {
    auto* pArgType = param.EvalDataType();
    NodeKind kind = pArgType ? pArgType->Kind() : NK_Void;
    bool scalar = ScalarPrimIndexOf(kind) >= 0;
    bool array = kind == NK_ArrayTypeToken;
    bool func = kind == NK_ClassDecl
        && static_cast<SnClassDecl*>(pArgType)->IsFuncType();
    if (!scalar && !array && !func)
        return;
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(slot);
    if (scalar)
        EmitPrimToStr(emitter, kind);
    else if (array)
        emitter.Emit(OpCode::OP_Array_to_str);
    else
        emitter.Emit(OpCode::OP_Func_to_str);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(slot);
}

//Bulk-copy evalArea claim → callParamBase just before the call.
//OP_VarLocal reads from claimBase+i*kFrameSlotBytes, OP_Assign writes
//to callParamBase+i*kFrameSlotBytes. This preserves any tagged Value
//representation (boxed heap idx, string handle, etc.) since both
//opcodes copy the whole uniform frame cell. Shared by EmitStdLibCall
//and EmitCallArgs.
void VmBackend::CopyClaimToCallParams(uint16_t claimBase, uint16_t slotCount,
                                      BytecodeEmitter& emitter) {
    for (uint16_t i = 0; i < slotCount; ++i) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(claimBase + i * kFrameSlotBytes);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->callParamBase + i * kFrameSlotBytes);
    }
}

//B_Default arm of EmitBinding: emit the default expression under the
//earlier-formals override scope, then normalize the staged slot. The
//default expression carries its own literal kind (an unsuffixed float
//default is double since the 0.7.5 literal tiering; `float x = 0.5`
//passed the declaration gate via constant-fit) — the in-place
//PrimCast re-stages it at the FORMAL's kind so the callee reads the
//declared width. Caller-expr args never land here — they convert via
//FixupParamTypesWithBindings at the call site.
void VmBackend::EmitDefaultBinding(const FormalBinding* pBindings,
                                     size_t bindingIdx, uint16_t slotIdx,
                                     size_t slotBase, uint16_t base,
                                     uint16_t paramOffset,
                                     BytecodeEmitter& emitter,
                                     uint16_t thisSlot)
{
    const auto& b = pBindings[bindingIdx];
    assert(b.pFormal && b.pFormal->Value());
    OverrideScope scope(*this);
    for (size_t j = 0; j < bindingIdx; ++j) {
        scope.Add(pBindings[j].pFormal->Name(),
                  base + (static_cast<uint16_t>(j + slotBase)) * kFrameSlotBytes);
    }
    if (thisSlot != UINT16_MAX) {
        scope.BindThis(thisSlot);
    }
    EmitExpression(*b.pFormal->Value(), emitter, paramOffset);
    auto* pFormalType = b.pFormal->EvalDataType();
    auto* pDefaultType = b.pFormal->Value()->EvalDataType();
    if (pFormalType && pDefaultType)
        EmitScalarSlotCast(pDefaultType->Kind(), pFormalType->Kind(),
                           paramOffset, emitter);
}

void VmBackend::EmitBinding(const FormalBinding* pBindings, size_t bindingIdx,
                              uint16_t slotIdx, size_t slotBase,
                              BytecodeEmitter& emitter, uint16_t thisSlot,
                              uint16_t claimBase)
{
    const auto& b = pBindings[bindingIdx];
    //claimBase is the evalArea claim slice base. Always provided by
    //EmitCallArgs (the only caller). Bindings emit into the claim slice;
    //a bulk-copy loop in EmitCallArgs then moves them to callParamBase.
    uint16_t base = claimBase;
    uint16_t paramOffset = base + slotIdx * kFrameSlotBytes;
    auto* pFormalType = b.pFormal->EvalDataType();

    if (b.kind == FormalBinding::B_Default) {
        EmitDefaultBinding(pBindings, bindingIdx, slotIdx, slotBase, base,
                           paramOffset, emitter, thisSlot);
    } else {
        assert(b.pCallerExpr);
        EmitExpression(*b.pCallerExpr, emitter, paramOffset);
    }

    //Struct deep-copy: if the formal is a struct type, copy the heap
    //subtree so the callee gets its own. Array-typed formals carry their
    //interned array token in EvalDataType (0.7.3 B), which
    //RuntimeTypeKind files as RTK_Array — so arrays pass by reference
    //regardless of element kind, with no separate IsArrayType guard
    //(Phase 9d-3 dispatch invariant, now derived from the token).
    if (pFormalType && RuntimeTypeKind(pFormalType) == RTK_Struct) {
        int structIdx = m_compiledModule.FindStruct(pFormalType->Name());
        emitter.Emit(OpCode::OP_CopyStruct);
        emitter.EmitUint16(m_currFunc->tempSlot);
        emitter.EmitUint16(paramOffset);
        emitter.EmitUint16(structIdx >= 0
            ? static_cast<uint16_t>(structIdx) : 0);
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(m_currFunc->tempSlot);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(paramOffset);
    }
}

void VmBackend::EmitOutSpills(const std::vector<OutSpill>& spills,
                              BytecodeEmitter& emitter)
{
    for (const auto& s : spills) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(m_currFunc->callParamBase
                           + s.slotIdx * kFrameSlotBytes);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(s.localOffset);
    }
}

uint32_t VmBackend::BuildOutMask(const std::vector<OutSpill>& spills)
{
    uint32_t mask = 0;
    for (const auto& s : spills) {
        if (s.slotIdx >= 32)
            throw std::runtime_error(
                "NLang backend: out parameter slot >= 32 is unsupported");
        mask |= 1u << s.slotIdx;
    }
    return mask;
}

//Optional per-arg boxing application (built-in generic class methods):
//box the staged argument in place when a plan marks its slot.
void VmBackend::ApplyArgBoxPlan(uint16_t slotIdx, uint16_t claimBase,
        BytecodeEmitter& emitter,
        const std::map<uint16_t, ArgBoxPlan>* pArgPlans) {
    if (!pArgPlans) return;
    auto it = pArgPlans->find(slotIdx);
    if (it == pArgPlans->end() || !it->second.needsBox) return;
    uint16_t paramOffset = claimBase + slotIdx * kFrameSlotBytes;
    EmitPResultRefresh(emitter, paramOffset);
    emitter.Emit(OpCode::OP_Box);
    emitter.EmitByte(it->second.tag);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(paramOffset);
}

//For method calls (slotBase=1), copy the receiver to claim[0] BEFORE
//emitting bindings. Default-param expressions referencing `this` need
//it available via OverrideScope/ThisOverrideStack.
void VmBackend::StageReceiverInClaim(size_t slotBase, uint16_t thisSlot,
                                     uint16_t claimBase,
                                     BytecodeEmitter& emitter) {
    if (slotBase == 1 && thisSlot != UINT16_MAX) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(thisSlot);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(claimBase);
    }
}

//Unresolved-invoke arm of EmitCallArgs (pCallee == null): legacy
//positional emit, plus out-argument spill recording.
void VmBackend::EmitUnresolvedInvokeArgs(const SnInvokeExpr& invoke,
        BytecodeEmitter& emitter, size_t slotBase, uint16_t claimBase,
        std::vector<OutSpill>* pOutSpills,
        const std::function<void(uint16_t)>& applyBox) {
    //Unresolved invoke — fall back to legacy positional emit.
    //Phase 13 Step 2: out arguments in this path are delegate calls
    //binding to a Func signature (the resolver already checked the
    //out markers). The callee fills the slot and the executor's
    //outMask write-back refreshes it — emit nothing here, just
    //record the spill so the post-call write-back reaches the
    //caller's local (mirrors the binding-path OutSpill fill).
    uint16_t paramIdx = static_cast<uint16_t>(slotBase);
    for (auto& param : invoke.Params()) {
        if (param.Kind() == NK_OutArgExpr && pOutSpills) {
            auto& outArg = static_cast<SnOutArgExpr&>(param);
            if (!outArg.Inner()
                || outArg.Inner()->Kind() != NK_IdentifierExpr)
                throw std::runtime_error(
                    "NLang backend: out argument is not a local variable");
            auto target = ResolveBareIdentifier(
                static_cast<SnIdentifierExpr&>(*outArg.Inner()).Field());
            if (target.kind != BareIdTarget::Local)
                throw std::runtime_error(
                    "NLang backend: out argument is not a local variable");
            pOutSpills->push_back({paramIdx, target.localOffset});
        } else {
            uint16_t paramOffset = claimBase + paramIdx * kFrameSlotBytes;
            EmitExpression(param, emitter, paramOffset);
            applyBox(paramIdx);
        }
        ++paramIdx;
    }
}

//Legacy positional arm of EmitCallArgs (no resolver bindings available).
void VmBackend::EmitLegacyPositionalArgs(const SnInvokeExpr& invoke,
        BytecodeEmitter& emitter, size_t slotBase, uint16_t claimBase,
        const std::function<void(uint16_t)>& applyBox) {
    //Legacy path: caller didn't go through Phase 9c resolver.
    uint16_t paramIdx = static_cast<uint16_t>(slotBase);
    for (auto& param : invoke.Params()) {
        uint16_t paramOffset = claimBase + paramIdx * kFrameSlotBytes;
        EmitExpression(param, emitter, paramOffset);
        applyBox(paramIdx);
        ++paramIdx;
    }
}

//Resolved-binding arm of EmitCallArgs (Phase 9c): sanity ceiling, the
//default-emission recursion guard, then per-binding emission.
//Returns true when the kMaxFuncParams ceiling tripwire fired and the
//call was not emitted — the caller must abort emission as well.
bool VmBackend::EmitFormalBindingArgs(const SnInvokeExpr& invoke,
        SnFunction* pCallee, BytecodeEmitter& emitter, size_t slotBase,
        uint16_t claimBase, uint16_t thisSlot,
        std::vector<OutSpill>* pOutSpills,
        const std::function<void(uint16_t)>& applyBox) {
    const auto& bindings = invoke.Bindings();
    if (bindings.size() + slotBase > kMaxFuncParams) {
        assert(false && "function parameters exceed kMaxFuncParams sanity ceiling");
        return true;
    }

    //Recursion guard (round-9, finding 3): a default expression may
    //contain a call that itself needs this callee's defaults — the
    //expansion recurses forever and overflows the compile stack
    //(0xC00000FD). Track functions whose defaults are mid-emission;
    //re-entry is a source error. Plain body recursion never lands here
    //(all arguments supplied → no default emission → no guard entry),
    //and sibling calls using the same defaults are sequential, not
    //nested, so both stay legal.
    bool emitsDefaults = false;
    for (const auto& b : bindings) {
        if (b.kind == FormalBinding::B_Default) { emitsDefaults = true; break; }
    }
    if (emitsDefaults && !m_defaultEmitting.insert(pCallee).second) {
        throw std::runtime_error(
            "recursive default parameter in call to '" + pCallee->Name()
            + "': the default expression requires the same default again");
    }
    //RAII: erases on every exit, normal or unwinding — a throw from a
    //deeper emission runs this destructor while propagating, so the set
    //never leaks an entry even on a failed build.
    struct DefaultEmitGuard {
        VmBackend* pB; SnFunction* pF; bool armed;
        ~DefaultEmitGuard() { if (armed) pB->m_defaultEmitting.erase(pF); }
    } defGuard{this, pCallee, emitsDefaults};

    for (size_t i = 0; i < bindings.size(); ++i) {
        EmitOneFormalBinding(bindings.data(), i, slotBase, emitter,
                             thisSlot, claimBase, pOutSpills, applyBox);
    }
    return false;
}

//One iteration of the resolved-binding loop: emit the binding, apply the
//optional boxing plan, and record out-argument spill targets (Phase 9e).
void VmBackend::EmitOneFormalBinding(const FormalBinding* pBindings,
        size_t bindingIdx, size_t slotBase, BytecodeEmitter& emitter,
        uint16_t thisSlot, uint16_t claimBase,
        std::vector<OutSpill>* pOutSpills,
        const std::function<void(uint16_t)>& applyBox) {
    uint16_t slotIdx = static_cast<uint16_t>(bindingIdx + slotBase);
    EmitBinding(pBindings, bindingIdx, slotIdx, slotBase, emitter,
                thisSlot, claimBase);
    applyBox(slotIdx);
    //Phase 9e: out binding — record the caller local for the
    //post-call spill. The resolver guarantees pCallerExpr is a
    //plain identifier bound to a caller-frame slot (local var or
    //formal param), so the spill target is a plain frame offset.
    if (pBindings[bindingIdx].bIsOut) {
        auto& idExpr = static_cast<SnIdentifierExpr&>(
            *pBindings[bindingIdx].pCallerExpr);
        auto target = ResolveBareIdentifier(idExpr.Field());
        if (target.kind != BareIdTarget::Local)
            throw std::runtime_error(
                "NLang backend: out argument is not a local variable");
        if (pOutSpills)
            pOutSpills->push_back({slotIdx, target.localOffset});
    }
}

void VmBackend::EmitCallArgs(const SnInvokeExpr& invoke, SnFunction* pCallee,
                              BytecodeEmitter& emitter, size_t slotBase,
                              const std::map<uint16_t, ArgBoxPlan>* pArgPlans,
                              uint16_t thisSlot,
                              std::vector<OutSpill>* pOutSpills) {
    //Determine total claim size (slotBase + arg count).
    const auto& bindings = invoke.Bindings();
    size_t argCount;
    if (!bindings.empty()) {
        argCount = bindings.size();
    } else {
        argCount = 0;
        for (auto& p : invoke.Params()) ++argCount;
    }
    uint16_t n = static_cast<uint16_t>(argCount + slotBase);

    //Claim a slice of the evalArea for this call's bindings.
    //Nested calls claim deeper slices, so inner bindings never overwrite
    //outer bindings. The RAII guard releases the claim on return.
    EvalAreaClaim claim(*this, n);
    uint16_t claimBase = claim.base();

    //Optional per-arg boxing application (built-in generic class methods).
    auto applyBox = [&](uint16_t slotIdx) {
        ApplyArgBoxPlan(slotIdx, claimBase, emitter, pArgPlans);
    };

    StageReceiverInClaim(slotBase, thisSlot, claimBase, emitter);

    //Emit each binding into the claimed evalArea slice.
    if (!pCallee) {
        EmitUnresolvedInvokeArgs(invoke, emitter, slotBase, claimBase,
                                 pOutSpills, applyBox);
    } else if (bindings.empty()) {
        EmitLegacyPositionalArgs(invoke, emitter, slotBase, claimBase,
                                 applyBox);
    } else if (EmitFormalBindingArgs(invoke, pCallee, emitter, slotBase,
                                     claimBase, thisSlot, pOutSpills,
                                     applyBox)) {
        return;
    }

    CopyClaimToCallParams(claimBase, n, emitter);
    //EvalAreaClaim destructor releases the claim automatically.
}


} //namespace nlang
