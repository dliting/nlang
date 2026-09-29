/*---
    EmitExprCore.cpp — 核心表达式发射：字面量/标识符/调用/out 拒绝/名字/this 与类型引用拒绝、表达式兜底、字符串常量登记。
    从 VmBackend.cpp 抽取（2026-09-25 可维护性重构，零行为变化）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/compiler/SnExtraTypes.h>
#include <nlang/compiler/ScriptLocation.h>
#include <nlang/runtime/NodeConsts.h>
#include <nlang/runtime/PrimitiveTypes.h>
#include <cassert>
#include <map>
#include <unordered_set>

namespace nlang {

uint16_t VmBackend::AddStringConstant(const std::string& s) {
    auto& pool = m_compiledModule.stringConstants;
    for (uint16_t i = 0; i < static_cast<uint16_t>(pool.size()); ++i) {
        if (pool[i] == s)
            return i;
    }
    assert(pool.size() < UINT16_MAX && "string constant pool overflow");
    pool.push_back(s);
    return static_cast<uint16_t>(pool.size() - 1);
}


void VmBackend::Access(SnArrayTypeExpr& expr) {
        throw std::runtime_error(
            "NLang backend: type-reference expression reached codegen "
            "(compile-time-only node) at "
            + (expr.Location() ? expr.Location()->ToString()
                               : std::string("?")));
}

//Round-13: type-reference expressions (ArrayTypeExpr, GenericTypeExpr)
//are compile-time-only — they carry type information but never produce
//runtime values. Reaching EmitExpression means a caller passed one as a
//value-producing expression (round-13 root cause: SnNewExpr::Args() is
//a view over ALL children and includes the AddChild'ed class name).
//This is an internal invariant break — surface it instead of emitting
//garbage or silently skipping (which masks resolver/AST bugs).
void VmBackend::Access(SnGenericTypeExpr& expr) {
        throw std::runtime_error(
            "NLang backend: type-reference expression reached codegen "
            "(compile-time-only node) at "
            + (expr.Location() ? expr.Location()->ToString()
                               : std::string("?")));
}

//Literal arm: registry-driven scalar emission. Integer rows pick the
//const op by slot width — 8-byte rows (long/ulong) use OP_ConstInt64
//with a kind-exact Variant read (Get<int32_t> on a long literal would
//read only the low 4 bytes); every 4-byte row (int/bool/char carriers)
//rides OP_ConstInt32. The double row is float-category and reserved
//for Task 7's OP_ConstDouble.
void VmBackend::EmitScalarLiteral(SnLiteralExpr& lit, NodeKind typeKind,
                                  BytecodeEmitter& emitter,
                                  uint16_t resultOffset) {
    const auto& row = kScalarPrims[ScalarPrimIndexOf(typeKind)];
    if (row.category == PC_SInt || row.category == PC_UInt) {
        if (row.slotWidth == 8) {
            int64_t v = typeKind == NK_ULong
                ? static_cast<int64_t>(lit.Value().Get<uint64_t>())
                : lit.Value().Get<int64_t>();
            emitter.Emit(OpCode::OP_ConstInt64);
            emitter.EmitInt64(v);
        } else {
            int32_t v = lit.Value().Get<int32_t>();
            emitter.Emit(OpCode::OP_ConstInt32);
            emitter.EmitInt32(v);
        }
    } else if (typeKind == NK_Float) {
        float v = lit.Value().Get<float>();
        emitter.Emit(OpCode::OP_ConstFloat);
        emitter.EmitFloat(v);
    } else if (typeKind == NK_Bool) {
        //Bool rides the int32 carrier (0/1) — same const op.
        int32_t v = lit.Value().Get<int32_t>();
        emitter.Emit(OpCode::OP_ConstInt32);
        emitter.EmitInt32(v);
    } else {
        //Round-12 shape: unhandled scalar literal kind. char
        //literals land with Task 8, double literals Task 7.
        throw std::runtime_error(
            "NLang backend: literal with unhandled type kind: "
            + std::to_string(static_cast<int>(typeKind)));
    }
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
}

void VmBackend::Access(SnLiteralExpr& expr) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
    auto& lit = static_cast<SnLiteralExpr&>(expr);
    auto* evalType = lit.EvalDataType();

    if (!evalType) {
        //Round-12: a literal without a resolved type is an internal error.
        throw std::runtime_error(
            "NLang backend: literal expression without a resolved type");
    }

    NodeKind typeKind = evalType->Kind();
    if (ScalarPrimIndexOf(typeKind) >= 0) {
        EmitScalarLiteral(lit, typeKind, emitter, resultOffset);
    } else if (typeKind == NK_String) {
        auto* pStr = lit.Value().Data().m_String;
        std::string sVal = pStr ? *pStr : "";
        uint16_t poolIdx = AddStringConstant(sVal);
        emitter.Emit(OpCode::OP_ConstString);
        emitter.EmitUint16(poolIdx);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
    } else {
        //Round-12: unknown literal type — internal error.
        throw std::runtime_error(
            "NLang backend: literal with unhandled type kind: "
            + std::to_string(static_cast<int>(typeKind)));
    }
    return;
}

//Identifier arm: default-param binding override — read the formal's
//caller-side callParamBase slot directly (see LookupOverride).
void VmBackend::EmitIdentifierOverrideRead(BytecodeEmitter& emitter,
                                           uint16_t resultOffset,
                                           uint16_t overrideSlot) {
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(overrideSlot);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
}

//Identifier arm: enum member constant — emit the resolved integer value.
void VmBackend::EmitIdentifierEnumMemberRead(BytecodeEmitter& emitter,
                                             uint16_t resultOffset,
                                             SnField* field) {
    auto* pEnumMember = static_cast<SnEnumMember*>(field);
    emitter.Emit(OpCode::OP_ConstInt32);
    emitter.EmitInt32(pEnumMember->Value());
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
}

//Identifier arm: bound function reference in value position — emit a
//static-bound handle (the pending-ref sweep rejects unbound references).
void VmBackend::EmitIdentifierFuncHandleRead(BytecodeEmitter& emitter,
                                             uint16_t resultOffset,
                                             SnField* field) {
    auto it = m_funcIndexMap.find(
        static_cast<SnFunction*>(field));
    if (it == m_funcIndexMap.end())
        throw std::runtime_error(
            "NLang backend: function reference without an index: "
            + field->Name());
    emitter.Emit(OpCode::OP_MakeFunc);
    emitter.EmitUint16(static_cast<uint16_t>(it->second));
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
}

//Identifier arm: resolved-field tail — local frame read or the implicit
//this.<field> member read; no codegen binding is an internal error.
void VmBackend::EmitIdentifierFieldRead(BytecodeEmitter& emitter,
                                        uint16_t resultOffset,
                                        SnField* field) {
    auto target = ResolveBareIdentifier(field);
    if (target.kind == BareIdTarget::Local) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(target.localOffset);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
    } else if (target.kind == BareIdTarget::ThisField) {
        //Implicit this.<field> (bare member read inside a method).
        //Same opcode shape as the MemberExpr class-field read.
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(ImplicitThisSlot());
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
        emitter.Emit(OpCode::OP_NullCheck);
        emitter.EmitUint16(resultOffset);
        emitter.Emit(OpCode::OP_LoadField);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(static_cast<uint16_t>(target.fieldOff));
    } else {
        throw std::runtime_error(
            "NLang backend: identifier has no codegen binding: "
            + field->Name());
    }
}

void VmBackend::Access(SnIdentifierExpr& expr) {
    NodeKind kind = expr.Kind();
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
        auto& idExpr = static_cast<SnIdentifierExpr&>(expr);
        //Phase 9c: binding override — when evaluating a default-param
        //expression, an identifier referring to an earlier formal must
        //read from the caller-side callParamBase slot rather than the
        //callee's local frame. Check the override stack before falling
        //through to normal local/global resolution.
        auto override = LookupOverride(idExpr.Name());
        if (override.first) {
            EmitIdentifierOverrideRead(emitter, resultOffset,
                override.second);
            return;
        }
        auto* field = idExpr.Field();
        if (field && field->Kind() == NK_EnumMember) {
            EmitIdentifierEnumMemberRead(emitter, resultOffset, field);
            return;
        }
        if (field && field->Kind() == NK_Function) {
            EmitIdentifierFuncHandleRead(emitter, resultOffset, field);
            return;
        }
        if (field) {
            EmitIdentifierFieldRead(emitter, resultOffset, field);
        } else {
            //Round-11: an unresolved identifier reaching codegen means the
            //resolver marked something resolved without binding it (the
            //string equals() arg bug did exactly this). Emitting ConstZero
            //here produced silent wrong code; fail the build instead —
            //builder.Build() only reports resolver errors, so codegen runs
            //only when the front-end saw none.
            throw std::runtime_error(
                "NLang backend: identifier reached codegen unresolved: "
                + idExpr.Name());
        }
        return;
}

//Delegate invoke arm: materialize the bound callee handle into a scratch
//evalArea slot (the field form must not disturb callParamBase), then
//dispatch through EmitDelegateDispatch.
void VmBackend::EmitDelegateInvoke(const SnInvokeExpr& invoke,
                                   BytecodeEmitter& emitter,
                                   uint16_t resultOffset,
                                   const std::vector<OutSpill>& outSpills) {
    auto target = ResolveBareIdentifier(invoke.Field());
    EvalAreaClaim calleeClaim(*this, 1);
    uint16_t calleeSlot = calleeClaim.base();
    if (target.kind == BareIdTarget::Local) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(target.localOffset);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(calleeSlot);
    } else if (target.kind == BareIdTarget::ThisField) {
        //Implicit this.<field> — same load shape as the
        //identifier arm's ThisField branch.
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(ImplicitThisSlot());
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(calleeSlot);
        emitter.Emit(OpCode::OP_NullCheck);
        emitter.EmitUint16(calleeSlot);
        emitter.Emit(OpCode::OP_LoadField);
        emitter.EmitUint16(calleeSlot);
        emitter.EmitUint16(calleeSlot);
        emitter.EmitUint16(static_cast<uint16_t>(target.fieldOff));
    } else {
        throw std::runtime_error(
            "NLang backend: delegate callee has no codegen "
            "binding: " + invoke.CalleeName());
    }
    EmitDelegateDispatch(emitter, calleeSlot, resultOffset, outSpills);
    emitter.Emit(OpCode::OP_ParaEnd);
}

//Phase 13 Step 2: out-carrying delegate calls dispatch with
//OP_CallDelegateOut; the executor reverses the bound-handle
//this-shift when copying the marked user-parameter slots back
//to callParamBase. Result first — the spills below clobber
//pResult (same ordering discipline as OP_CallFuncOut).
void VmBackend::EmitDelegateDispatch(BytecodeEmitter& emitter,
                                     uint16_t calleeSlot,
                                     uint16_t resultOffset,
                                     const std::vector<OutSpill>& outSpills) {
    if (!outSpills.empty()) {
        emitter.Emit(OpCode::OP_CallDelegateOut);
        emitter.EmitUint16(calleeSlot);
        emitter.EmitUint16(m_currFunc->callParamBase);
        emitter.EmitInt32(static_cast<int32_t>(
            BuildOutMask(outSpills)));
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
        EmitOutSpills(outSpills, emitter);
    } else {
        emitter.Emit(OpCode::OP_CallDelegate);
        emitter.EmitUint16(calleeSlot);
        emitter.EmitUint16(m_currFunc->callParamBase);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
    }
}

//Free-function call: OP_CallFunc, or OP_CallFuncOut + spills when the
//call carries out arguments.
void VmBackend::EmitFreeFunctionCall(int funcIndex, BytecodeEmitter& emitter,
                                     uint16_t resultOffset,
                                     const std::vector<OutSpill>& outSpills) {
    if (!outSpills.empty()) {
        emitter.Emit(OpCode::OP_CallFuncOut);
        emitter.EmitUint16(static_cast<uint16_t>(funcIndex));
        emitter.EmitUint16(m_currFunc->callParamBase);
        emitter.EmitInt32(static_cast<int32_t>(
            BuildOutMask(outSpills)));
        //Result first: the spills below clobber pResult via
        //OP_VarLocal, so the call's return value must be stored
        //before any writeback.
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
        EmitOutSpills(outSpills, emitter);
    } else {
        emitter.Emit(OpCode::OP_CallFunc);
        emitter.EmitUint16(static_cast<uint16_t>(funcIndex));
        emitter.EmitUint16(m_currFunc->callParamBase);
        // Result is in pResult, store to resultOffset
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
    }
}

void VmBackend::Access(SnInvokeExpr& expr) {
    NodeKind kind = expr.Kind();
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
        auto& invoke = static_cast<SnInvokeExpr&>(expr);
        auto* callee = invoke.Callee();

        //Phase 9c: binding-aware argument emission. Handles positional,
        //named, and default-param bindings via EmitCallArgs.
        //Phase 9e: out arguments are collected here and written back
        //right after the call via the *Out opcode + spills.
        std::vector<OutSpill> outSpills;
        EmitCallArgs(invoke, callee, emitter, /*slotBase=*/0,
                     /*pArgPlans=*/nullptr, /*thisSlot=*/UINT16_MAX,
                     &outSpills);

        //Phase 13: delegate invoke — the resolver bound the callee name to
        //a Func-typed value (local/param, or an implicit this-field)
        //instead of a function declaration (Callee() null, Field() set).
        //Args are already staged at callParamBase by the positional emit
        //above; materialize the handle into a scratch evalArea slot (the
        //field form must not disturb callParamBase) and dispatch.
        if (IsDelegateInvoke(invoke)) {
            EmitDelegateInvoke(invoke, emitter, resultOffset, outSpills);
            return;
        }

        // Find function index
        int funcIndex = -1;
        if (callee) {
            auto it = m_funcIndexMap.find(callee);
            if (it != m_funcIndexMap.end())
                funcIndex = static_cast<int>(it->second);
        }
        //Round-11: an invoke reaching here with no resolvable callee means
        //the resolver marked it resolved without binding (the string
        //equals() arg bug did exactly this — the arg's nested call was
        //silently skipped and equals compared stale memory). Codegen only
        //runs when the front-end saw no errors, so this is an internal
        //inconsistency: fail the build instead of emitting wrong code.
        if (funcIndex < 0) {
            throw std::runtime_error(
                "NLang backend: invoke reached codegen unresolved: "
                + invoke.CalleeName());
        }
        EmitFreeFunctionCall(funcIndex, emitter, resultOffset, outSpills);
        emitter.Emit(OpCode::OP_ParaEnd);
        return;
}

    //Phase 9e: out arguments are consumed by the binding path in
    //EmitCallArgs (the wrapper never emits itself). Reaching this point
    //means the wrapper flowed into a non-binding emit site (ctor call,
    //super(...), or a legacy path) — all rejected by the resolver, so
    //this is an internal error, not a fallback.
void VmBackend::Access(SnOutArgExpr& expr) {
        throw std::runtime_error(
            "NLang backend: out argument in unsupported call form");
}

void VmBackend::Access(SnNameExpr& expr) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
        auto& nameExpr = static_cast<SnNameExpr&>(expr);
        if (nameExpr.Expr()) {
            EmitExpression(*nameExpr.Expr(), emitter, resultOffset);
        }
        return;
}

    // New expression - object instantiation
void VmBackend::Access(SnThisExpr& expr) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
        //Inside a method-call default-param expression, `this` resolves
        //to the caller-side callParamBase slot holding the receiver.
        //Otherwise (inside a method body), `this` is local 0.
        auto thisOverride = LookupThisOverride();
        uint16_t thisSlot = thisOverride.first ? thisOverride.second : 0;
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(thisSlot);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
        return;
}

    // New array expression: new T[size]
void VmBackend::Access(SnExpression& expr) {
    NodeKind kind = expr.Kind();
    throw std::runtime_error(
        "NLang backend: unhandled expression kind in codegen: "
        + std::to_string(static_cast<int>(kind)) + " at "
        + (expr.Location() ? expr.Location()->ToString() : std::string("?")));
}

//Line anchor shared by the statement prologue and the loop emitters.
//Loop statements skip the prologue and call this at their per-iteration
//pass point instead (loop head for while/for — where the back edge
//lands; tail condition re-check for do-while), so the loop line fires
//every iteration (first pass falls through — gdb break-on-loop-line
//semantics). Anchor strategy lives only here.

} //namespace nlang
