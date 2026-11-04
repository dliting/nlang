/*---
    EmitExprIdent.cpp — 标识符表达式发射族：绑定覆盖/枚举成员/函数句柄/
    局部与隐式this字段尾臂、SnIdentifierExpr 主分派。
    从 EmitExprCore.cpp 抽取（2026-11-02 尺寸守卫触发的拆分，零行为变化）。
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
#include <nlang/runtime/StackDump.h>

namespace nlang {

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
//SlotFor keeps this unit-local: a cross-unit reference becomes an import
//placeholder the load-time linker resolves.
void VmBackend::EmitIdentifierFuncHandleRead(BytecodeEmitter& emitter,
                                             uint16_t resultOffset,
                                             SnField* field) {
    emitter.Emit(OpCode::OP_MakeFunc);
    emitter.EmitUint16(static_cast<uint16_t>(
        FunctionSlotFor(static_cast<SnFunction&>(*field))));
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
}

//v0.12.6 D3: a compile-time-folded static const reads as an inline
//literal — it owns no slot (registration stamped 0xFFFF), so the slot
//dispatch below must never be consulted for it. Same predicate as the
//member form and the backend registration (one definition of "folded").
bool VmBackend::TryEmitFoldedStaticConstRead(SnField* field,
        BytecodeEmitter& emitter, uint16_t resultOffset) {
    if (!(field && field->Kind() == NK_ClassField
          && StaticConstFoldsAtCompileTime(
              *static_cast<SnClassField*>(field))))
        return false;
    EmitFoldedConstantRead(*static_cast<SnClassField*>(field), emitter);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    return true;
}

//Identifier arm: resolved-field tail — local frame read, module static
//slot load, or the implicit this.<field> member read; no codegen
//binding is an internal error.
void VmBackend::EmitIdentifierFieldRead(BytecodeEmitter& emitter,
                                        uint16_t resultOffset,
                                        SnField* field) {
    if (TryEmitFoldedStaticConstRead(field, emitter, resultOffset))
        return;
    auto target = ResolveBareIdentifier(field);
    if (target.kind == BareIdTarget::Local) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(target.localOffset);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
    } else if (target.kind == BareIdTarget::StaticSlot) {
        //Bare static-field read inside the class (D3's qualified-only
        //rule for outsiders is a resolver concern; the bare form is the
        //in-class spelling). No receiver, no null check. First active
        //use gates the declaring class's lazy initializer (D2).
        EmitStaticFieldGate(field, emitter);
        emitter.Emit(OpCode::OP_LoadStatic);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(target.slotIdx);
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
        //v0.12.4 D5: same copy-on-read seal as the explicit member tail.
        EmitSealedFieldCopyIfNeeded(field, emitter, resultOffset);
    } else {
        throw std::runtime_error(
            "NLang backend: identifier has no codegen binding: "
            + field->Name());
    }
}

void VmBackend::Access(SnIdentifierExpr& expr) {
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
            //the front end reports resolver errors before codegen runs.
            throw std::runtime_error(
                "NLang backend: identifier reached codegen unresolved: "
                + idExpr.Name());
        }
        return;
}

} //namespace nlang
