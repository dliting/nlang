/*---
    VmBackendEmitCall.cpp — 调用发射辅助族：pResult 刷新、复合赋值运算、
    标准库调用、绑定发射、out 参数溢写、调用实参发射。
    从 VmBackend.cpp 抽取（2026-09-25 可维护性重构，零行为变化）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>

namespace nlang {

static const uint16_t VALUE_SIZE = 4; // int32 and float are both 4 bytes

//Several opcodes read the pResult accumulator (OP_Box/OP_Unbox,
//OP_CastIntToFloat/OP_CastFloatToInt, OP_Int32_to_str/OP_Float_to_str).
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
    bool isFloat = lhsType && lhsType->Kind() == NK_Float;
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
    case SnBinaryExpr::OP_Add: opc = isFloat ? OpCode::OP_Add_f32 : OpCode::OP_Add_i32; break;
    case SnBinaryExpr::OP_Sub: opc = isFloat ? OpCode::OP_Sub_f32 : OpCode::OP_Sub_i32; break;
    case SnBinaryExpr::OP_Mul: opc = isFloat ? OpCode::OP_Mul_f32 : OpCode::OP_Mul_i32; break;
    case SnBinaryExpr::OP_Div: opc = isFloat ? OpCode::OP_Div_f32 : OpCode::OP_Div_i32; break;
    case SnBinaryExpr::OP_Mod: opc = OpCode::OP_Mod_i32; break;
    default: return;  //not an arithmetic op
    }
    emitter.Emit(opc);
    emitter.EmitUint16(dst);
    emitter.EmitUint16(src);
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
            claimBase + paramIdx * VALUE_SIZE);
        //io.print coercion: int/float args convert to string in their
        //claim slot right after being emitted. pResult discipline — the
        //to_str opcodes have no operands and rewrite the accumulator in
        //place, so the sequence must be load-slot -> convert -> store-slot
        //(the cast_f2i / string-pool-dedup bug family otherwise).
        if (entry.coerceToString) {
            auto* pArgType = param.EvalDataType();
            OpCode conv = OpCode::OP_Count;  //string args pass through
            if (pArgType && pArgType->Kind() == NK_Int32)
                conv = OpCode::OP_Int32_to_str;
            else if (pArgType && pArgType->Kind() == NK_Float)
                conv = OpCode::OP_Float_to_str;
            else if (pArgType && pArgType->Kind() == NK_ArrayTypeToken)
                conv = OpCode::OP_Array_to_str;
            else if (pArgType && pArgType->Kind() == NK_ClassDecl
                && static_cast<SnClassDecl*>(pArgType)->IsFuncType())
                conv = OpCode::OP_Func_to_str;
            if (conv != OpCode::OP_Count) {
                const uint16_t slot = claimBase + paramIdx * VALUE_SIZE;
                emitter.Emit(OpCode::OP_VarLocal);
                emitter.EmitUint16(slot);
                emitter.Emit(conv);
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(slot);
            }
        }
        ++paramIdx;
    }
    for (uint16_t i = 0; i < argCount; ++i) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(claimBase + i * VALUE_SIZE);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->callParamBase + i * VALUE_SIZE);
    }
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
    uint16_t paramOffset = base + slotIdx * VALUE_SIZE;

    if (b.kind == FormalBinding::B_Default) {
        assert(b.pFormal && b.pFormal->Value());
        OverrideScope scope(*this);
        for (size_t j = 0; j < bindingIdx; ++j) {
            scope.Add(pBindings[j].pFormal->Name(),
                      base + (static_cast<uint16_t>(j + slotBase)) * VALUE_SIZE);
        }
        if (thisSlot != UINT16_MAX) {
            scope.BindThis(thisSlot);
        }
        EmitExpression(*b.pFormal->Value(), emitter, paramOffset);
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
    auto* pFormalType = b.pFormal->EvalDataType();
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
                           + s.slotIdx * VALUE_SIZE);
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
        if (!pArgPlans) return;
        auto it = pArgPlans->find(slotIdx);
        if (it == pArgPlans->end() || !it->second.needsBox) return;
        uint16_t paramOffset = claimBase + slotIdx * VALUE_SIZE;
        EmitPResultRefresh(emitter, paramOffset);
        emitter.Emit(OpCode::OP_Box);
        emitter.EmitByte(it->second.tag);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(paramOffset);
    };

    //For method calls (slotBase=1), copy the receiver to claim[0] BEFORE
    //emitting bindings. Default-param expressions referencing `this` need
    //it available via OverrideScope/ThisOverrideStack.
    if (slotBase == 1 && thisSlot != UINT16_MAX) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(thisSlot);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(claimBase);
    }

    //Emit each binding into the claimed evalArea slice.
    if (!pCallee) {
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
                uint16_t paramOffset = claimBase + paramIdx * VALUE_SIZE;
                EmitExpression(param, emitter, paramOffset);
                applyBox(paramIdx);
            }
            ++paramIdx;
        }
    } else if (bindings.empty()) {
        //Legacy path: caller didn't go through Phase 9c resolver.
        uint16_t paramIdx = static_cast<uint16_t>(slotBase);
        for (auto& param : invoke.Params()) {
            uint16_t paramOffset = claimBase + paramIdx * VALUE_SIZE;
            EmitExpression(param, emitter, paramOffset);
            applyBox(paramIdx);
            ++paramIdx;
        }
    } else {
        if (bindings.size() + slotBase > kMaxFuncParams) {
            assert(false && "function parameters exceed kMaxFuncParams sanity ceiling");
            return;
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
            uint16_t slotIdx = static_cast<uint16_t>(i + slotBase);
            EmitBinding(bindings.data(), i, slotIdx, slotBase, emitter,
                        thisSlot, claimBase);
            applyBox(slotIdx);
            //Phase 9e: out binding — record the caller local for the
            //post-call spill. The resolver guarantees pCallerExpr is a
            //plain identifier bound to a caller-frame slot (local var or
            //formal param), so the spill target is a plain frame offset.
            if (bindings[i].bIsOut) {
                auto& idExpr = static_cast<SnIdentifierExpr&>(
                    *bindings[i].pCallerExpr);
                auto target = ResolveBareIdentifier(idExpr.Field());
                if (target.kind != BareIdTarget::Local)
                    throw std::runtime_error(
                        "NLang backend: out argument is not a local variable");
                if (pOutSpills)
                    pOutSpills->push_back({slotIdx, target.localOffset});
            }
        }
    }

    //Bulk-copy evalArea claim → callParamBase just before the call.
    //OP_VarLocal reads from claimBase+i*4, OP_Assign writes to
    //callParamBase+i*4. This preserves any tagged Value representation
    //(boxed heap idx, string handle, etc.) since both opcodes copy
    //4 raw bytes.
    for (uint16_t i = 0; i < n; ++i) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(claimBase + i * VALUE_SIZE);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->callParamBase + i * VALUE_SIZE);
    }
    //EvalAreaClaim destructor releases the claim automatically.
}


} //namespace nlang
