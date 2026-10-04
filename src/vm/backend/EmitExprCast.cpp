/*---
    EmitExprCast.cpp — 转型与下标表达式发射：cast、as、下标读取。
    从 VmBackend.cpp 抽取（2026-09-25 可维护性重构，零行为变化）。
---*/
#include "VmBackend.h"
#include "EmitPrimOps.h"
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

//Cast arm: Func → string renders the handle via OP_Func_to_str.
void VmBackend::EmitCastFuncToString(BytecodeEmitter& emitter,
                                     uint16_t resultOffset) {
    EmitPResultRefresh(emitter, resultOffset);
    emitter.Emit(OpCode::OP_Func_to_str);
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

//Cast arm: to-string special cases (enum/func/class/array), each
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
    }
    //Phase 13: Func → string renders the handle ("func <name>")
    //via OP_Func_to_str. MUST precede the class→string branch —
    //a handle's slot[0] is a function index, and the virtual
    //toString dispatch would read it as a class index.
    if (srcKind == NK_ClassDecl && dstKind == NK_String
        && static_cast<SnClassDecl*>(
            cast.Source()->EvalDataType())->IsFuncType()) {
        EmitCastFuncToString(emitter, resultOffset);
        return true;
    }
    //Phase 8e-9b: class → string emits a virtual toString() call.
    if (srcKind == NK_ClassDecl && dstKind == NK_String) {
        EmitCastClassToString(emitter, resultOffset);
        return true;
    }
    //Phase 9b-pre: array → string emits OP_Array_to_str (Array is a
    //VM primitive, not a class, so OP_CallMethod doesn't apply).
    //Array-ness keys on the source expression's array-valued
    //property (the resolver's single channel) — identifier and
    //member sources, invoke results, new-array expressions and
    //container element reads alike. The old Kind/Field() test
    //missed every non-lvalue shape, which then fell through to
    //the Int32→String arm and printed the raw handle index.
    if (dstKind == NK_String && cast.Source()->IsArrayValued()) {
        EmitCastArrayToString(emitter, resultOffset);
        return true;
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
void VmBackend::EmitAsDowncastOp(SnAsExpr& asExpr, BytecodeEmitter& emitter,
                                 uint16_t resultOffset) {
    auto* targetType = asExpr.ResolvedTarget();
    uint16_t classIdx = 0;
    if (targetType && targetType->Kind() == NK_ClassDecl) {
        //Per-unit: own/builtin classes resolve through the table lookup;
        //a cross-unit class becomes an import placeholder slot.
        classIdx = static_cast<uint16_t>(
            ClassSlotFor(static_cast<SnClassDecl&>(*targetType)));
    }
    emitter.Emit(OpCode::OP_CheckCast);
    emitter.EmitUint16(classIdx);
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

    //Phase 8e-1.5: `expr as T` runtime-checked cast.
    //Valid kinds: TCK_Same (no-op), TCK_Box (primitive→Object), TCK_Unbox
    //(Object→primitive), TCK_Downcast (ancestor→subclass), TCK_Explicit
    //(0.7.5 narrowing scalar conversion, e.g. float→int).
void VmBackend::Access(SnAsExpr& expr) {
    //No `NodeKind kind` snapshot: this body's only `kind` is its own
    //TypeCastKind local below (the pre-refactor branch shadowed the chain
    //variable with it).
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
        auto& asExpr = static_cast<SnAsExpr&>(expr);
        //Evaluate operand to resultOffset. After this, pResult holds the
        //value (heap idx for ref types) per EmitExpression convention.
        EmitExpression(*asExpr.Operand(), emitter, resultOffset);

        auto kind = asExpr.CastKind();
        if (kind == TCK_Same) {
            //Already the right type — no opcode needed.
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
            EmitAsDowncastOp(asExpr, emitter, resultOffset);
            return;
        }
        if (kind == TCK_Explicit) {
            if (EmitAsExplicitNarrowOp(asExpr, emitter, resultOffset))
                return;
            //Uncovered kind pair — fall through to the invariant throw.
        }
        //Phase 13: `f as string` is the one resolver-approved TCK_Auto
        //`as` form — function handles render as "func <name>".
        if (kind == TCK_Auto) {
            if (EmitAsFuncToString(asExpr, emitter, resultOffset))
                return;
        }
        //Other kinds (TCK_Auto, TCK_Dynamic, TCK_None, and TCK_Explicit
        //pairs outside the transitional opcode coverage) are rejected by
        //ExprResolver.Access(SnAsExpr&) before codegen — reaching here is
        //an internal invariant break. Round-13: this used to silently
        //return, leaving resultOffset unwritten (stale/garbage value).
        throw std::runtime_error(
            "NLang backend: unhandled `as` cast kind in codegen: "
            + std::to_string(static_cast<int>(kind)) + " at "
            + (expr.Location() ? expr.Location()->ToString()
                               : std::string("?")));
}

//Subscript arm: List<T>/Dict<K,V> get() sugar — classify the generic
//instantiation (element/key/value boxing tags), then emit the call.
void VmBackend::EmitContainerSubscriptGet(SnSubscriptExpr& sub,
                                          BytecodeEmitter& emitter,
                                          uint16_t resultOffset) {
    auto* pGenClass = static_cast<SnClassDecl*>(
        sub.Array()->EvalDataType());
    const auto& baseName = pGenClass->BaseName();
    const auto& typeArgs = pGenClass->GenericTypeArgs();
    bool isList = (baseName == "List" && !typeArgs.empty());
    bool isDict = (baseName == "Dict" && typeArgs.size() > 1);
    //Array-typed slots are interned tokens — raw
    //handles, no box/unbox (BoxingTagFor default).
    //Dict keys box when primitive; List's index is int.
    auto keyBox = isDict
        ? BoxingTagFor(typeArgs[0])
        : BoxingTagResult{0, false};
    SnField* pElem = isList ? typeArgs[0]
        : (typeArgs.size() > 1 ? typeArgs[1] : nullptr);
    auto valBox = BoxingTagFor(pElem);
    EmitContainerGetCall(sub, keyBox, valBox, emitter, resultOffset);
}

//Subscript arm: the get() call itself — emits through EvalAreaClaim so
//nested calls (e.g. foo(li[j])) cannot clobber an outer call's
//callParamBase slice. Unboxes the get() return for primitive T/V,
//mirroring the foreach element load and the member-call boxing plans.
void VmBackend::EmitContainerGetCall(SnSubscriptExpr& sub,
                                     BoxingTagResult keyBox,
                                     BoxingTagResult valBox,
                                     BytecodeEmitter& emitter,
                                     uint16_t resultOffset) {
    EvalAreaClaim claim(*this, 2);
    uint16_t claimBase = claim.base();
    //Receiver → claim[0] directly (round-4: NOT via
    //resultOffset — a self-referential read `i = li[i]`
    //would overwrite the index's source slot with the
    //List handle before the index is emitted). Same
    //shape as the array path below; resultOffset is
    //written only by the final get() store.
    EmitExpression(*sub.Array(), emitter, claimBase);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(claimBase);
    //arg0 = index (box primitive Dict keys).
    uint16_t keyOffset = claimBase + kFrameSlotBytes;
    EmitExpression(*sub.Index(), emitter, keyOffset);
    if (keyBox.isPrimitive) {
        EmitPResultRefresh(emitter, keyOffset);
        emitter.Emit(OpCode::OP_Box);
        emitter.EmitByte(keyBox.tag);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(keyOffset);
    }
    //Bulk-copy claim → callParamBase (raw 4-byte moves
    //preserve tagged representations).
    for (uint16_t i = 0; i < 2; ++i) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(claimBase + i * kFrameSlotBytes);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(
            m_currFunc->callParamBase + i * kFrameSlotBytes);
    }
    uint16_t nameIdx = AddStringConstant("get");
    emitter.Emit(OpCode::OP_CallMethod);
    emitter.EmitUint16(nameIdx);
    emitter.EmitUint16(m_currFunc->callParamBase);
    if (valBox.isPrimitive) {
        emitter.Emit(OpCode::OP_Unbox);
        emitter.EmitByte(valBox.tag);
    }
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    emitter.Emit(OpCode::OP_ParaEnd);
}

    // Member expression - struct field access or delegate to inner
void VmBackend::Access(SnSubscriptExpr& expr) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
        auto& sub = static_cast<SnSubscriptExpr&>(expr);
        //0.7.5 string subscript: byte read on a string base.
        if (sub.Array()->IsResolved() && sub.Array()->EvalDataType()
            && sub.Array()->EvalDataType()->Kind() == NK_String) {
            EmitStringByteAt(sub, emitter, resultOffset);
            return;
        }
        //List<T>/Dict<K,V> subscript sugar: li[i] == li.get(i),
        //d[k] == d.get(k). Dispatch on the base's resolved type being a
        //generic instantiation (arrays take the OP_LoadElement path below).
        if (IsContainerSubscript(*sub.Array())) {
            EmitContainerSubscriptGet(sub, emitter, resultOffset);
            return;
        }
        //Phase 10 audit round-3: park receiver AND index in an exclusive
        //EvalAreaClaim(2), mirroring the container get() shape. The old
        //chain (receiver → resultOffset, index → PickTempSlot(resultOffset))
        //broke at nesting depth 5 — PickTempSlot wraps tempSlot4 back to
        //tempSlot, so `a[b[c[d[e[0]]]]]` clobbered an outer parked value —
        //and parked the receiver in resultOffset, clobberable whenever a
        //non-temp exclude slot fell through to the same temp. Claim slots
        //stack per nesting level, so read depth is now unbounded. Same
        //instruction count as the old shape; only the slot numbers change.
        EvalAreaClaim claim(*this, 2);
        uint16_t claimBase = claim.base();
        EmitExpression(*sub.Array(), emitter, claimBase);
        emitter.Emit(OpCode::OP_NullCheck);
        emitter.EmitUint16(claimBase);
        uint16_t indexSlot = claimBase + kFrameSlotBytes;
        EmitExpression(*sub.Index(), emitter, indexSlot);
        emitter.Emit(OpCode::OP_LoadElement);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(claimBase);
        emitter.EmitUint16(indexSlot);
        //Phase 9d-3: no defensive CopyStruct for struct element types here.
        //Value copies belong at assignment/store boundaries (AssignStmt and
        //SubscriptAssign both emit their own OP_CopyStruct). Copying on read
        //broke write-through receivers — `arr[i].f = v` stored into a
        //discarded copy — and caused a redundant double copy for
        //`Point p = arr[i]`.
        return;
}

} //namespace nlang
