/*---
    EmitExprCast.cpp — 转型表达式发射：cast与as。
    从 VmBackend.cpp 抽取（2026-09-25 可维护性重构，零行为变化）；
    下标表达式已拆至EmitContainerSubscript.cpp（2026-11-09）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/BuildEnvironment.h>
#include "EmitPrimOps.h"
#include <nlang/runtime/BuiltinGenericNames.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/compiler/SnExtraTypes.h>
#include <nlang/compiler/ScriptLocation.h>
#include <nlang/runtime/NodeConsts.h>
#include <cassert>
#include <map>
#include <unordered_set>

namespace nlang {


//Cast arm: primitive → Object implicit boxing — OP_Box with the source's
//type tag so the VM knows what to wrap.
void VmBackend::EmitCastBoxOp(SnCastExpr& cast, BytecodeEmitter& emitter,
                              uint16_t resultOffset) {
    //0.7.5: registry-driven tag (string/enum/int32-carrier/scalar rows).
    //0xFF would mean a non-boxable kind reached boxing — the executor
    //raises a named error on it rather than boxing garbage.
    auto* sourceType = cast.Source()->EvalDataType();
    uint8_t typeTag = sourceType ? BoxTypeTagOfKind(sourceType->Kind())
                                 : RTK_Int32;
    EmitPResultRefresh(emitter, resultOffset);
    emitter.Emit(OpCode::OP_Box);
    emitter.EmitByte(typeTag);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
}

//Cast arm: enum → string emits OP_Enum_to_str (preserves enum identity
//for name lookup). Returns true when it fired.
bool VmBackend::EmitCastEnumToString(SnCastExpr& cast, NodeKind srcKind,
    SnField* sourceType, BytecodeEmitter& emitter, uint16_t resultOffset) {
    //Two detection paths:
    // (a) srcKind == NK_EnumDecl — explicit enum-typed variable cast
    //     (e.g. `(Color) c` where c is some int).
    // (b) Source expression's Field() is SnEnumMember — enum literal
    //     access like Color.Red wrapped by binary strengthening.
    //     EvalDataType is NK_Int32 here (SnEnumMember::EvalDataType
    //     returns NK_Int32), so srcKind is NK_Int32 — we must walk
    //     the Field() chain to discover the enum decl.
    SnEnumDecl* pEnumDecl = nullptr;
    if (srcKind == NK_EnumDecl) {
        pEnumDecl = static_cast<SnEnumDecl*>(sourceType);
    } else {
        //Try Field() chain on source expression.
        auto srcExprKind = cast.Source()->Kind();
        if (srcExprKind == NK_MemberExpr
            || srcExprKind == NK_IdentifierExpr)
        {
            auto& srcFieldExpr = static_cast<SnFieldExpr&>(
                *cast.Source());
            auto* srcField = srcFieldExpr.Field();
            if (srcField
                && srcField->Kind() == NK_EnumMember)
            {
                pEnumDecl = static_cast<SnEnumDecl*>(
                    srcField->Parent());
            }
        }
    }
    if (pEnumDecl) {
        //EnumSlotFor: own enums resolve through the table (a miss throws
        //— the resolver bound an enum the registration never saw), a
        //cross-unit enum lands as a placeholder the linker resolves.
        //The old direct m_enumIndexMap probe degraded foreign enums to
        //the numeric int->string spelling.
        emitter.Emit(OpCode::OP_Enum_to_str);
        emitter.EmitUint16(static_cast<uint16_t>(
            EnumSlotFor(*pEnumDecl)));
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
        return true;
    }
    return false;
}

//Cast arm: func → string renders the handle via OP_Func_to_str.
void VmBackend::EmitCastFuncToString(BytecodeEmitter& emitter,
                                     uint16_t resultOffset) {
    EmitPResultRefresh(emitter, resultOffset);
    emitter.Emit(OpCode::OP_Func_to_str);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
}

//Cast arm: type parameter → string converts the accumulator's value
//through the receiver's instantiation kind (v0.13.1 D6) — same
//pResult shape as the func arm.
void VmBackend::EmitCastTypeArgToString(BytecodeEmitter& emitter,
                                        uint16_t resultOffset,
                                        uint8_t ordinal) {
    EmitPResultRefresh(emitter, resultOffset);
    emitter.Emit(OpCode::OP_TypeArg_to_str);
    emitter.EmitByte(ordinal);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
}

//Cast arm: class → string emits a virtual toString() call — copy
//resultOffset → callParamBase[0], OP_CallMethod by name, copy back.
void VmBackend::EmitCastClassToString(BytecodeEmitter& emitter,
                                      uint16_t resultOffset) {
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(resultOffset);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(m_currFunc->callParamBase);
    uint16_t nameIdx = AddStringConstant("toString");
    emitter.Emit(OpCode::OP_CallMethod);
    emitter.EmitUint16(nameIdx);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
}

//Cast arm: array → string emits OP_Array_to_str (Array is a VM primitive,
//not a class, so OP_CallMethod doesn't apply).
void VmBackend::EmitCastArrayToString(BytecodeEmitter& emitter,
                                      uint16_t resultOffset) {
    //A member-source emit ends in OP_LoadField, which
    //writes the slot but leaves the accumulator stale.
    EmitPResultRefresh(emitter, resultOffset);
    emitter.Emit(OpCode::OP_Array_to_str);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
}

//Cast arm: NK_ClassDecl sources to string, in the mandatory precedence
//order — func handle render first (a handle's slot[0] is a function
//index the virtual toString dispatch would misread as a class index),
//then the v0.13.1 D6 type-parameter kind dispatch (a placeholder is an
//NK_ClassDecl with no virtual toString; the kind decides at runtime
//between the scalar registry render, the string itself, and the heap
//value walk), then the Phase 8e-9b virtual toString() call.
bool VmBackend::EmitCastClassSourceToString(SnCastExpr& cast,
                                            SnField* sourceType,
                                            BytecodeEmitter& emitter,
                                            uint16_t resultOffset) {
    if (static_cast<SnClassDecl*>(
            cast.Source()->EvalDataType())->IsFuncType()) {
        EmitCastFuncToString(emitter, resultOffset);
        return true;
    }
    if (static_cast<SnClassDecl*>(sourceType)->IsTypeParamPlaceholder()) {
        EmitCastTypeArgToString(emitter, resultOffset,
            TypeParamOrdinalOfType(*sourceType));
        return true;
    }
    EmitCastClassToString(emitter, resultOffset);
    return true;
}

//Cast arm: to-string special cases (enum/class-family/array), each
//reporting whether it emitted so the caller returns before the enum→int
//collapse in EmitCastNumericOrStringOp.
bool VmBackend::EmitCastToStringOp(SnCastExpr& cast, NodeKind srcKind,
                                   NodeKind dstKind, SnField* sourceType,
                                   BytecodeEmitter& emitter,
                                   uint16_t resultOffset) {
    //Enum → string MUST be checked before collapsing enum to int below,
    //otherwise OP_Prim_to_str would fire and produce a numeric string
    //instead of the value name.
    if (dstKind == NK_String) {
        if (EmitCastEnumToString(cast, srcKind, sourceType, emitter,
                                 resultOffset))
            return true;
        if (srcKind == NK_ClassDecl
            && EmitCastClassSourceToString(cast, sourceType, emitter,
                                           resultOffset))
            return true;
        //Phase 9b-pre: array → string emits OP_Array_to_str (Array is a
        //VM primitive, not a class, so OP_CallMethod doesn't apply).
        //Array-ness keys on the source expression's array-valued
        //property (the resolver's single channel) — identifier and
        //member sources, invoke results, new-array expressions and
        //container element reads alike. The old Kind/Field() test
        //missed every non-lvalue shape, which then fell through to
        //the Int32→String arm and printed the raw handle index.
        if (cast.Source()->IsArrayValued()) {
            EmitCastArrayToString(emitter, resultOffset);
            return true;
        }
    }
    return false;
}

//Cast arm: numeric and primitive→string conversions after the enum
//kinds have collapsed to int. 0.7.5: both shapes ride the
//kind-immediate family — OP_PrimCast for EVERY scalar pair the
//resolver lets through implicitly (the int32↔float special case
//generalized: CommonNumericType widenings like int32→long and
//constant-fit narrowings like int32→byte land here too), OP_Prim_to_str
//for the →string coercion.
void VmBackend::EmitCastNumericOrStringOp(NodeKind srcKind, NodeKind dstKind,
                                          BytecodeEmitter& emitter,
                                          uint16_t resultOffset) {
    if (dstKind == NK_String) {
        //Phase 8e-9a: scalar → string coercion for `int + string` etc.
        EmitPResultRefresh(emitter, resultOffset);
        EmitPrimToStr(emitter, srcKind);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
    } else if (ScalarPrimIndexOf(srcKind) >= 0
        && ScalarPrimIndexOf(dstKind) >= 0) {
        EmitPResultRefresh(emitter, resultOffset);
        EmitPrimCast(emitter, srcKind, dstKind);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
    }
}

void VmBackend::Access(SnCastExpr& expr) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
        auto& cast = static_cast<SnCastExpr&>(expr);
        EmitExpression(*cast.Source(), emitter, resultOffset);

        //Phase 8e-1: TCK_Box — primitive → Object implicit boxing.
        //The cast kind was computed by CastInfo when source was primitive
        //and target was Object. Emit OP_Box with the source's type tag
        //(RTK_Int32/RTK_Float/RTK_String) so the VM knows what to wrap.
        if (cast.CastKind() == TCK_Box) {
            EmitCastBoxOp(cast, emitter, resultOffset);
            return;
        }

        auto* targetType = cast.Target();
        auto* sourceType = cast.Source()->EvalDataType();
        if (sourceType && targetType) {
            NodeKind srcKind = sourceType->Kind();
            NodeKind dstKind = targetType->Kind();
            if (EmitCastToStringOp(cast, srcKind, dstKind, sourceType,
                                   emitter, resultOffset)) {
                return;
            }
            if (srcKind == NK_EnumDecl) srcKind = NK_Int32;
            if (dstKind == NK_EnumDecl) dstKind = NK_Int32;
            EmitCastNumericOrStringOp(srcKind, dstKind, emitter, resultOffset);
        }
        return;
}

//As arm: TCK_Box — primitive → Object boxing, symmetric to NK_CastExpr's
//TCK_Box path. The null literal is exempt — boxing it would allocate a
//boxed 0 and destroy the null identity downstream
//(`oa[0] = null as Object` then compares unequal to null).
//Access(SnAsExpr) propagates NF_NullLiteral onto the as-expr
//for exactly this test. Same exemption as FixupExprType's
//null-literal skip and the element-store boxing guards.
void VmBackend::EmitAsBoxOp(SnAsExpr& asExpr, BytecodeEmitter& emitter,
                            uint16_t resultOffset) {
    if (asExpr.ContainFlags(NF_NullLiteral))
        return;
    //0.7.5: registry-driven tag (see EmitCastBoxOp).
    auto* sourceType = asExpr.Operand()->EvalDataType();
    uint8_t typeTag = sourceType ? BoxTypeTagOfKind(sourceType->Kind())
                                 : RTK_Int32;
    EmitPResultRefresh(emitter, resultOffset);
    emitter.Emit(OpCode::OP_Box);
    emitter.EmitByte(typeTag);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
}

//As arm: TCK_Unbox — target type is primitive, derive RTK_* from target.
void VmBackend::EmitAsUnboxOp(SnAsExpr& asExpr, BytecodeEmitter& emitter,
                              uint16_t resultOffset) {
    //0.7.5: registry-driven tag (see EmitCastBoxOp).
    auto* targetType = asExpr.ResolvedTarget();
    uint8_t typeTag = targetType ? BoxTypeTagOfKind(targetType->Kind())
                                 : RTK_Int32;
    EmitPResultRefresh(emitter, resultOffset);
    emitter.Emit(OpCode::OP_Unbox);
    emitter.EmitByte(typeTag);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
}

//As arm: TCK_Downcast — target is a subclass, emit OP_CheckCast classIdx.
//v0.13.1: a generic-instantiation target also names its descriptor row
//(the row check alone cannot tell Box<int> from Box<string> — one erased
//row per template); non-instantiations carry the 0xFFFF sentinel, and
//recipe rows compose against the receiver at run time like OP_New.
void VmBackend::EmitAsDowncastOp(SnAsExpr& asExpr, BytecodeEmitter& emitter,
                                 uint16_t resultOffset) {
    auto* targetType = asExpr.ResolvedTarget();
    uint16_t classIdx = 0;
    uint16_t descIdx = kNoInstDescriptor;
    if (targetType && targetType->Kind() == NK_ClassDecl) {
        //Per-unit: own/builtin classes resolve through the table lookup;
        //a cross-unit class becomes an import placeholder slot.
        classIdx = static_cast<uint16_t>(
            ClassSlotFor(static_cast<SnClassDecl&>(*targetType)));
        descIdx = DescriptorIndexFor(
            static_cast<SnClassDecl&>(*targetType));
    }
    emitter.Emit(OpCode::OP_CheckCast);
    emitter.EmitUint16(classIdx);
    emitter.EmitUint16(descIdx);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
}

//As arm: TCK_Explicit — 0.7.5 narrowing scalar conversion via the
//kind-immediate OP_PrimCast (C# unchecked semantics: truncation for
//float→int, wrap for wide→narrow integers). Only scalar pairs whose
//cast-table cell is non-null reach codegen; bool is excluded by the
//table itself. False = operand/target are not both scalars (the
//caller falls through to the invariant throw).
bool VmBackend::EmitAsExplicitNarrowOp(SnAsExpr& asExpr,
                                       BytecodeEmitter& emitter,
                                       uint16_t resultOffset) {
    auto* srcType = asExpr.Operand()->EvalDataType();
    auto* tgtType = asExpr.ResolvedTarget();
    if (!srcType || !tgtType
        || ScalarPrimIndexOf(srcType->Kind()) < 0
        || ScalarPrimIndexOf(tgtType->Kind()) < 0) {
        return false;
    }
    EmitPResultRefresh(emitter, resultOffset);
    EmitPrimCast(emitter, srcType->Kind(), tgtType->Kind());
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    return true;
}

//As arm: `f as string` — the one resolver-approved TCK_Auto form.
//Returns true when OP_Func_to_str was emitted.
bool VmBackend::EmitAsFuncToString(SnAsExpr& asExpr, BytecodeEmitter& emitter,
                                   uint16_t resultOffset) {
    auto* srcType = asExpr.Operand()->EvalDataType();
    if (srcType && srcType->Kind() == NK_ClassDecl
        && static_cast<SnClassDecl*>(srcType)->IsFuncType()) {
        EmitPResultRefresh(emitter, resultOffset);
        emitter.Emit(OpCode::OP_Func_to_str);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
        return true;
    }
    return false;
}

    //Phase 8e-1.5: `expr as T` runtime-checked cast. Valid kinds:
    //TCK_Same (no-op; v0.12.7 adds the array rank boundary), TCK_Box,
    //TCK_Unbox, TCK_Downcast (ancestor→subclass), TCK_Explicit.
void VmBackend::Access(SnAsExpr& expr) {
    //The body's only `kind` is the TypeCastKind local below.
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
        auto& asExpr = static_cast<SnAsExpr&>(expr);
        //Evaluate operand to resultOffset (heap idx for ref types).
        EmitExpression(*asExpr.Operand(), emitter, resultOffset);

        auto kind = asExpr.CastKind();
        if (kind == TCK_Same) {
            //v0.12.7: the boundary rank check; null passes through.
            EmitArrayRankBoundary(asExpr.ResolvedTarget(),
                                  asExpr.Operand(), resultOffset, emitter);
            return;
        }
        if (kind == TCK_Box) {
            EmitAsBoxOp(asExpr, emitter, resultOffset);
            return;
        }
        if (kind == TCK_Unbox) {
            EmitAsUnboxOp(asExpr, emitter, resultOffset);
            return;
        }
        if (kind == TCK_Downcast) {
            //v0.12.7 Task 5: array token target checks type identity.
            if (EmitAsArrayDowncastOp(asExpr.ResolvedTarget(),
                                      resultOffset, emitter))
                return;
            EmitAsDowncastOp(asExpr, emitter, resultOffset);
            return;
        }
        if (kind == TCK_Explicit) {
            if (EmitAsExplicitNarrowOp(asExpr, emitter, resultOffset))
                return;
            //Uncovered kind pair — fall through to the invariant throw.
        }
        //Phase 13: `f as string` — function handles render as "func <name>".
        if (kind == TCK_Auto) {
            if (EmitAsFuncToString(asExpr, emitter, resultOffset))
                return;
        }
        //Other kinds are resolver-rejected — invariant break if reached
        //(round-13: used to silently leave a stale value).
        throw std::runtime_error(
            "NLang backend: unhandled `as` cast kind in codegen: "
            + std::to_string(static_cast<int>(kind)) + " at "
            + (expr.Location() ? expr.Location()->ToString()
                               : std::string("?")));
}

} //namespace nlang
