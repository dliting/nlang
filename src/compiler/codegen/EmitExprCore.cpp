/*---
    EmitExprCore.cpp — 核心表达式发射：字面量、类型引用拒绝、表达式兜底、字符串常量登记。
    从 VmBackend.cpp 抽取（2026-09-25 可维护性重构，零行为变化）；
    标识符发射族后拆至 EmitExprIdent.cpp（2026-11-02 尺寸守卫）。
---*/
#include "VmBackend.h"
#include "builder/FieldInitializerChecks.hpp"
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
#include <nlang/runtime/StackDump.h>

namespace nlang {

uint16_t VmBackend::AddStringConstant(const std::string& s) {
    auto& pool = m_compiledModule.stringConstants;
    for (uint16_t i = 0; i < static_cast<uint16_t>(pool.size()); ++i) {
        if (pool[i] == s)
            return i;
    }
    NLANG_ASSERT(pool.size() < UINT16_MAX && "string constant pool overflow");
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

//Phase 4b: a qualified type reference (ns.Type) is likewise compile-time
//only; reaching codegen as a value is an internal invariant break.
void VmBackend::Access(SnQualifiedTypeExpr& expr) {
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
//rides OP_ConstInt32. The float-category rows split by kind: float
//keeps OP_ConstFloat, double rides OP_ConstDouble (both carriers are
//exact — the Variant stores the CppType bit pattern).
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
    } else if (typeKind == NK_Double) {
        double v = lit.Value().Get<double>();
        emitter.Emit(OpCode::OP_ConstDouble);
        emitter.EmitDouble(v);
    } else if (typeKind == NK_Bool) {
        //Bool rides the int32 carrier (0/1) — same const op.
        int32_t v = lit.Value().Get<int32_t>();
        emitter.Emit(OpCode::OP_ConstInt32);
        emitter.EmitInt32(v);
    } else if (typeKind == NK_Char) {
        //char rides the uint32 carrier (m_Int bit alias); the 4 raw
        //bytes ARE the code point — same const op as bool.
        int32_t v = lit.Value().Get<int32_t>();
        emitter.Emit(OpCode::OP_ConstInt32);
        emitter.EmitInt32(v);
    } else {
        //Round-12 shape: unhandled scalar literal kind.
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

//Delegate invoke arm: materialize the bound callee handle into a scratch
//evalArea slot (the field form must not disturb callParamBase), then
//dispatch through EmitDelegateDispatch.
//Task 8: a raw &-return binds the pair itself (binding targets, nested
//ref-calls); a value read normalizes through the key's return deref tag.
void VmBackend::EmitDelegateInvoke(const SnInvokeExpr& invoke,
                                   BytecodeEmitter& emitter,
                                   uint16_t resultOffset,
                                   bool bRawRefResult) {
    auto target = ResolveBareIdentifier(invoke.Field());
    EvalAreaClaim calleeClaim(*this, 1);
    uint16_t calleeSlot = calleeClaim.base();
    EmitDelegateCalleeLoad(invoke, target, calleeSlot, emitter);
    const uint8_t refDerefKind = bRawRefResult ? kNoRefDeref
        : DelegateReturnDerefKind(DelegateFuncTypeOfField(invoke.Field()));
    EmitDelegateDispatch(emitter, calleeSlot, resultOffset,
        refDerefKind);
    emitter.Emit(OpCode::OP_ParaEnd);
}

//The delegate callee's handle load, one arm per resolved storage kind.
//The capture arm carries its own rejection (a flavored capture's storage
//is not a raw handle — ledger ruling); the fall-through throw is the
//invariant break for any unmatched kind.
void VmBackend::EmitDelegateCalleeLoad(const SnInvokeExpr& invoke,
                                       const BareIdTarget& target,
                                       uint16_t calleeSlot,
                                       BytecodeEmitter& emitter) {
    if (target.kind == BareIdTarget::Local) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(target.localOffset);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(calleeSlot);
    } else if (target.kind == BareIdTarget::DisplayField
        || target.kind == BareIdTarget::CaptureField) {
        //0.13.10 Task 6: a func-typed capture as delegate callee. The
        //delegate handle rides the display field cell: chase the record,
        //load the cell.
        auto* pField = invoke.Field();
        if (pField && (pField->ContainFlags(NF_Promoted)
            || pField->ContainFlags(NF_Ref)))
            throw std::runtime_error(
                "NLang backend: promoted/reference capture as a "
                "delegate callee is not supported: "
                + invoke.CalleeName());
        EmitCaptureCellLoad(calleeSlot, target, pField, emitter);
        emitter.Emit(OpCode::OP_LoadField);
        emitter.EmitUint16(calleeSlot);
        emitter.EmitUint16(calleeSlot);
        emitter.EmitUint16(static_cast<uint16_t>(target.fieldOff));
    } else if (target.kind == BareIdTarget::StaticSlot) {
        //I3 tail: a func-typed STATIC field as delegate callee (the bare
        //in-class spelling of Cfg.dbl) — gate the declaring class, load
        //the handle from the module slot; no receiver exists.
        EmitStaticFieldGate(invoke.Field(), emitter);
        emitter.Emit(OpCode::OP_LoadStatic);
        emitter.EmitUint16(calleeSlot);
        emitter.EmitUint16(target.slotIdx);
    } else if (target.kind == BareIdTarget::ThisField) {
        //Implicit this.<field> — same load shape as the
        //identifier arm's ThisField branch.
        EmitReceiverLoad(calleeSlot, emitter);
        emitter.Emit(OpCode::OP_LoadField);
        emitter.EmitUint16(calleeSlot);
        emitter.EmitUint16(calleeSlot);
        emitter.EmitUint16(static_cast<uint16_t>(target.fieldOff));
    } else {
        throw std::runtime_error(
            "NLang backend: delegate callee has no codegen "
            "binding: " + invoke.CalleeName());
    }
}

//Delegate dispatch: OP_CallDelegate. A ref-returning key parks a raw
//pair — deref in place for a value consumer.
void VmBackend::EmitDelegateDispatch(BytecodeEmitter& emitter,
                                     uint16_t calleeSlot,
                                     uint16_t resultOffset,
                                     uint8_t refDerefKind) {
    emitter.Emit(OpCode::OP_CallDelegate);
    emitter.EmitUint16(calleeSlot);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    if (refDerefKind != kNoRefDeref)
        EmitRefDerefInPlace(refDerefKind, resultOffset, emitter);
}

//Free-function (receiverless) call: OP_CallStatic. The 0.13.1a
//descriptor operand rides the form — StaticCallDescIdxFor decides
//none vs caller-forward.
void VmBackend::EmitFreeFunctionCall(int funcIndex, BytecodeEmitter& emitter,
                                     uint16_t resultOffset,
                                     uint16_t descIdx,
                                     uint8_t refDerefKind) {
    emitter.Emit(OpCode::OP_CallStatic);
    emitter.EmitUint16(static_cast<uint16_t>(funcIndex));
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.EmitUint16(descIdx);
    //Result is in pResult, store to resultOffset. A &-returning
    //callee parks a raw pair: deref the payload cell in place
    //(Task 6, value semantics at the read site).
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    if (refDerefKind != kNoRefDeref)
        EmitRefDerefInPlace(refDerefKind, resultOffset, emitter);
}

//0.13.1a: the descriptor operand of a receiverless call. A callee that
//belongs to the same generic class row the emitted function belongs to
//(m_pCurrClass — the caller-side owner) shares the caller's
//instantiation — recursion and same-template bare statics forward the
//caller's frame cell (kInstDescForwardCaller). Everything else emits
//none: non-generic callees pay nothing, and cross-context generic
//callees await their inference/spelling channels (a T read in an
//unfilled cell degrades to the named runtime error).
uint16_t VmBackend::StaticCallDescIdxFor(const SnFunction& callee) const {
    auto* pCalleeOwner = callee.Parent();
    if (!pCalleeOwner || pCalleeOwner != m_pCurrClass
        || !static_cast<SnClassDecl*>(pCalleeOwner)->IsGenericDeclaration())
        return kInstDescNone;
    return kInstDescForwardCaller;
}

void VmBackend::Access(SnInvokeExpr& expr) {
    NodeKind kind = expr.Kind();
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
        auto& invoke = static_cast<SnInvokeExpr&>(expr);

        //0.9.0: a ctor-carrying invoke (`Foo(args)` without new) shares
        //the NewExpr emission; dispatched first — the delegate predicate
        //in the dispatch tail would misread the bound class as a func
        //value.
        //v0.12.7: the Array face's dynamic-rank ctor — the Array<E> record
        //is not a class object; route to the array-alloc emission.
        if (invoke.ArrayDynCtor()) {
            EmitArrayDynCtor(*invoke.ArrayDynCtor(), invoke.Params(),
                             resultOffset);
            return;
        }
        if (invoke.CtorClass()) {
            EmitNewObject(invoke.CtorClass(), invoke.CtorFunction(),
                          invoke.Params(), nullptr, resultOffset);
            return;
        }
        //0.9.11 D2: the struct twin (`S(args)` / `m.S(args)` — bare or
        //module-qualified ctor invoke; exactly one of the two type slots
        //is bound by the resolver).
        if (invoke.CtorStruct()) {
            EmitStructCtor(*invoke.CtorStruct(), invoke.CtorFunction(),
                           invoke.Params(), nullptr, resultOffset);
            return;
        }

        EmitBareInvokeShape(invoke, resultOffset,
                            /*bRawRefResult=*/false);
}

//Shared bare-invoke staging (value reads and the MakeRef &-return
//pass-through both land here): the static-field gate, binding-aware
//argument emission, then dispatch. bRawRefResult suppresses the
//value-side return normalization so a &-returning callee's raw
//(record, cell) pair reaches the caller untouched (Task 6).
void VmBackend::EmitBareInvokeShape(SnInvokeExpr& invoke,
                                    uint16_t resultOffset,
                                    bool bRawRefResult) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    //v0.12.6 D2: a bare static-method call (the in-class spelling)
    //is a first active use too — gate the callee's declaring class
    //before any argument evaluation. Free functions carry no owner
    //class and the gate is a no-op.
    auto* callee = invoke.Callee();
    EmitStaticFieldGate(callee, emitter);
    //Phase 9c: binding-aware argument emission. Handles positional,
    //named, and default-param bindings via EmitCallArgs.
    EmitCallArgs(invoke, callee, emitter, /*slotBase=*/0,
                 /*pArgPlans=*/nullptr, /*thisSlot=*/UINT16_MAX);
    EmitInvokeDispatch(invoke, callee, emitter, resultOffset,
                       bRawRefResult);
}

//Staged-invoke dispatch tail (split out of Access for the 50-line
//function budget). bRawRefResult (Task 6): suppress the value-side
//deref so a &-returning callee's raw pair reaches the caller.
void VmBackend::EmitInvokeDispatch(SnInvokeExpr& invoke, SnFunction* callee,
                                   BytecodeEmitter& emitter,
                                   uint16_t resultOffset,
                                   bool bRawRefResult) {
    //Phase 13: delegate invoke — the resolver bound the callee name to
    //a func-typed value (local/param, or an implicit this-field)
    //instead of a function declaration (Callee() null, Field() set).
    //Args are already staged at callParamBase by the positional emit
    //above; materialize the handle into a scratch evalArea slot (the
    //field form must not disturb callParamBase) and dispatch.
    if (IsDelegateInvoke(invoke)) {
        EmitDelegateInvoke(invoke, emitter, resultOffset,
            bRawRefResult);
        return;
    }

    // Find function index
    if (!callee) {
        //Round-11: an invoke reaching here with no resolvable callee
        //means the resolver marked it resolved without binding (the
        //string equals() arg bug did exactly this — the arg's nested
        //call was silently skipped and equals compared stale memory).
        //Codegen only runs when the front-end saw no errors, so this
        //is an internal inconsistency: fail the build instead of
        //emitting wrong code.
        throw std::runtime_error(
            "NLang backend: invoke reached codegen unresolved: "
            + invoke.CalleeName());
    }
    //Per-unit: cross-unit callees slot as import placeholders
    //(FunctionSlotFor); own ones resolve through m_funcIndexMap.
    //0.13.1a Task 4: a generic free function's frame cell comes from the
    //call site's inferred-argument row (D4: the template record serves
    //every call; the row is what varies). Ordinary callees keep the
    //same-generic-row forward / none decision.
    NLANG_ASSERT(!callee->IsGenericDeclaration()
        || !invoke.InferredTypeArgs().empty());
    uint16_t descIdx = callee->IsGenericDeclaration()
        ? FreeCallDescriptorIndexFor(
            DescriptorSpellingOf(*m_pRegistry, *callee),
            invoke.InferredTypeArgs())
        : StaticCallDescIdxFor(*callee);
    EmitFreeFunctionCall(
        static_cast<int>(FunctionSlotFor(*callee)),
        emitter, resultOffset, descIdx,
        bRawRefResult ? kNoRefDeref : RefReturnDerefKind(callee));
    emitter.Emit(OpCode::OP_ParaEnd);
}

    //v0.13.9 phase 4: the resolver logs the retirement diagnostic and
    //leaves the node unresolved, and codegen never runs on an errored
    //module — this arm is the internal-error tripwire if that invariant
    //breaks (the wrapper has no legal emission of its own).
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
        //0.13.10: inside a synthetic capture frame, `this` is the anchor
        //display's "$this" field (the override logic is meaningless
        //there — $lambda_N bodies carry no default params).
        if (InSyntheticCaptureFrame()) {
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(0);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            NLANG_ASSERT(m_pCurrClass);
            const int thisOff = FindClassFieldOffset(*m_pCurrClass, "$this");
            NLANG_ASSERT(thisOff >= 0);
            emitter.Emit(OpCode::OP_LoadField);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(static_cast<uint16_t>(thisOff));
            return;
        }
        auto thisOverride = LookupThisOverride();
        uint16_t thisSlot = thisOverride.first ? thisOverride.second : 0;
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(thisSlot);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
        return;
}

//v0.13.1 D4: typeName(Type) — three emission forms, decided by what
//the resolver recorded. A concrete operand folds at resolve time and
//rides the ordinary string-constant path; a bare type parameter reads
//the receiver descriptor's argument (OP_TypeNameArg, the ordinal is
//the declared parameter position); anything else containing a type
//parameter (an instantiation like Box<T>, or an array T[]) composes
//from a composition record row (OP_TypeNameCompose).
void VmBackend::Access(SnTypeNameExpr& expr) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    const uint16_t resultOffset = m_resultOffset;
    if (!expr.FoldedSpelling().empty()) {
        const uint16_t poolIdx = AddStringConstant(expr.FoldedSpelling());
        emitter.Emit(OpCode::OP_ConstString);
        emitter.EmitUint16(poolIdx);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
        return;
    }
    if (expr.TypeParamOrdinal() != SnTypeNameExpr::kNoTypeParamOrdinal) {
        emitter.Emit(OpCode::OP_TypeNameArg);
        emitter.EmitUint16(resultOffset);
        emitter.EmitByte(expr.TypeParamOrdinal());
        return;
    }
    const uint16_t compositionIdx = CompositionIndexFor(*expr.TypeRef()->Field());
    NLANG_ASSERT(compositionIdx != kNoInstDescriptor);
    emitter.Emit(OpCode::OP_TypeNameCompose);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(compositionIdx);
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
