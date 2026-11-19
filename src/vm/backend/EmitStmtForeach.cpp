/*---
    EmitStmtForeach.cpp — foreach 语句发射（数组/List/Dict 三路索引式展开）。
    从 VmBackend.cpp 抽取（2026-09-25 可维护性重构，零行为变化）。
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
#include <nlang/runtime/StackDump.h>

namespace nlang {


//Foreach iterable classification: exactly one of isArray/isList/isDict/
//isString fires, with elemType = the field the loop-var slot kind keys
//on (arrays: the ELEMENT, token peeled; List/Dict: the first type
//argument; string: the builtin char singleton — code points).
struct ForeachIterableKind {
    bool isArray = false;
    bool isList = false;
    bool isDict = false;
    bool isString = false;
    SnField* elemType = nullptr;
    //0.13.1a Task 5: the erased-container boundary plan for the
    //element — the dynamic variant fires when the element type
    //argument is this generic body's own placeholder.
    VmBackend::ArgBoxPlan elemBox;
};

//Declaration-side arm: identifier or member lvalue (`a` / `b.a` /
//`this.a`) whose FIELD is declared array-typed — the authoritative
//declaration signal. Pre-token the member's own EvalDataType degraded
//to the element type (the dispatch-order trap) and mis-routed this
//into the List path; post-flip it carries the token and the value-form
//arm below routes the same shapes as backstop.
static bool DetectDeclaredArraySource(SnExpression* pIter,
                                      SnField*& elemType) {
    SnField* pField = nullptr;
    if (pIter->Kind() == NK_IdentifierExpr) {
        pField = static_cast<SnIdentifierExpr*>(pIter)->Field();
    } else if (pIter->Kind() == NK_MemberExpr) {
        auto* pInner = static_cast<SnMemberExpr*>(pIter)->Inner();
        if (pInner && pInner->Kind() == NK_IdentifierExpr)
            pField = static_cast<SnIdentifierExpr*>(pInner)->Field();
    }
    if (!(pField && pField->IsArrayType()))
        return false;
    //The loop-var slot kind keys on the ELEMENT row, either spelling
    //of the declared array (the interned token or the Array<E>
    //instantiation row) — leaving the class row here keyed the slot
    //RTK_Array and broke struct materialization (Phase 9d-3).
    elemType = ArrayElementRowOf(pField->EvalDataType());
    return true;
}

static ForeachIterableKind ClassifyForeachIterable(
        SnExpression* pIter) {
    ForeachIterableKind k;
    if (DetectDeclaredArraySource(pIter, k.elemType)) {
        //Stage-2: array sources of ANY rank lower to the flat walk —
        //rank-1 flat order is the linear order, unchanged.
        k.isArray = true;
        return k;
    }
    auto* pIterType = pIter->EvalDataType();
    //0.7.3 B D10 value-form arm: every array-valued source — get()
    //and subscript reads out of List<T[]>/Dict<K,V[]>, array-returning
    //calls, bound method calls, new-array expressions — carries an
    //array row in EvalDataType. The declaration-side arm above only
    //recognizes identifier/member lvalue shapes; this arm routes the
    //value forms through the canonical IsArrayType() predicate, which
    //covers both spellings: the interned token AND the Array<E>
    //instantiation row (the rank-generic tensor primitives declare
    //their returns that way, so `foreach (v in a + b)` lands here).
    if (pIterType && pIterType->IsArrayType()) {
        k.isArray = true;
        k.elemType = ArrayElementRowOf(pIterType);
        return k;
    }
    //0.7.5 char bridge: a string source iterates Unicode scalar values;
    //elemType = char drives RTK_Char for the loop-var slot.
    if (pIterType && pIterType->Kind() == NK_String) {
        k.isString = true;
        k.elemType = SnBuiltinDataType::InstanceOf(NK_Char);
        return k;
    }
    if (pIterType && pIterType->Kind() == NK_ClassDecl) {
        auto* pClass = static_cast<SnClassDecl*>(pIterType);
        if (pClass->IsGenericInstantiation()) {
            const auto& baseName = pClass->BaseName();
            const auto& typeArgs = pClass->GenericTypeArgs();
            if (baseName == kBuiltinListTypeName) {
                k.isList = true;
                k.elemType = typeArgs.empty() ? nullptr : typeArgs[0];
            } else if (baseName == kBuiltinDictTypeName) {
                k.isDict = true;
                k.elemType = typeArgs.empty() ? nullptr : typeArgs[0];
            }
        }
    }
    return k;
}

//Task 7 (D6): the & mark is effective for the bindable element family
//— scalar/string cells and struct records. Dict/string sources were
//rejected at the resolver; class/array elements degrade to value form
//(spec 3.1).
static bool ForeachRefForm(SnForeachStmt& fe, bool bRefForbidden,
        uint8_t elemKind) {
    const bool bRefMarked = fe.VarType()
        && fe.VarType()->ContainFlags(NF_Ref);
    NLANG_ASSERT(!(bRefMarked && bRefForbidden)
        && "reference-form Dict/string foreach must be rejected at the resolver");
    return bRefMarked
        && (elemKind == RTK_Struct || elemKind == RTK_String
            || ScalarPrimIndexOfRtk(elemKind) >= 0);
}

void VmBackend::Access(SnForeachStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        auto& fe = static_cast<SnForeachStmt&>(stmt);
        ForeachIterableKind iter = ClassifyForeachIterable(fe.Iterable());
        //0.13.1a Task 5: the container element boundary plan (static
        //tag or the frame-dispatched dynamic variant).
        if (iter.isList || iter.isDict)
            iter.elemBox = PlanBoundaryBox(iter.elemType,
                static_cast<SnClassDecl*>(
                    fe.Iterable()->EvalDataType()));
        //0.7.5 char bridge: string sources take the dedicated
        //code-point expansion (no length/get() calls — OP_StrForeachStep).
        if (iter.isString) {
            EmitStringForeach(fe, emitter);
            return;
        }
        NLANG_ASSERT((iter.isArray || iter.isList || iter.isDict)
            && "foreach iterable must be Array, List<T>, Dict<K,V>, or string");
        EmitForeachIndexed(fe, iter.isArray, iter.isDict, iter.elemType,
            iter.elemBox, emitter);
        return;
}

//The index-based expansion for Array/List/Dict sources (the string arm
//has its own expansion; Access classifies and dispatches). Task 12:
//arrays probe their length once — an owned record's length never
//changes. Containers (List, and Dict-after-Keys) re-probe EVERY
//iteration inside the loop head (spec §5 removal-during-iteration): a
//removal inside the body shortens the walk instead of reading past
//the shrunk store (the cached probe was an IndexOutOfBoundsException),
//and an append extends it to the fresh tail.
void VmBackend::EmitForeachIndexed(SnForeachStmt& fe, bool isArray,
        bool isDict, SnField* pElemType, const ArgBoxPlan& elemBox,
        BytecodeEmitter& emitter) {
        //0.7.3 B: an array-typed element (List<int[]> / Dict<K[],V> key
        //iteration) IS the interned token, and RuntimeTypeKind maps the
        //token to RTK_Array through the IsArrayType() override — so one
        //channel tags the loop-var slot, and the GC traces the handle.
        uint8_t elemKind = RuntimeTypeKind(pElemType);
        const bool bRefLoop = ForeachRefForm(fe, isDict, elemKind);
        //Body-level & binding of a value-form loop var promotes it
        //(MarkPromotedBindTarget flags the statement); the slot then
        //holds a box handle and every iteration store wraps the raw
        //element value (EmitPromotedValueWrap).
        const bool bPromotedVar = fe.ContainFlags(NF_Promoted);
        ForeachSlots slots = AllocForeachLocals(fe, elemKind,
            isArray, bRefLoop);
        //Evaluate the iterable into iterSlot.
        EmitExpression(*fe.Iterable(), emitter, slots.iterSlot);
        if (isDict)
            EmitForeachDictKeysPrelude(slots.iterSlot, emitter);
        if (isArray)
            EmitForeachLength(true, slots.iterSlot, slots.nSlot,
                emitter);
        //i = 0
        emitter.Emit(OpCode::OP_ConstInt32);
        emitter.EmitInt32(0);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(slots.iSlot);
        size_t loopStart = EmitForeachLoopHead(slots.iSlot, slots.nSlot,
            emitter, !isArray, slots.iterSlot);
        EmitForeachLoadElement(isArray, pElemType, elemKind,
            elemBox, bRefLoop, bPromotedVar, slots, emitter);
        EmitStatement(*fe.Body(), emitter);
        EmitForeachLoopTail(loopStart, slots.iSlot, emitter);
        return;
}

//Allocate hidden locals BEFORE the LoopContext push. AllocLocal dedupes
//by name, so uniquify hidden locals via per-function counter (nested
//foreach would otherwise collide on __foreach_iter etc.).
VmBackend::ForeachSlots VmBackend::AllocForeachLocals(SnForeachStmt& fe,
        uint8_t elemKind, bool isArray, bool bRefLoop) {
    ForeachSlots slots;
    uint16_t counter = m_currFunc->foreachCounter++;
    //Effective reference form: the loop-var slot holds a (record, cell)
    //pair — a GC-rooted byref slot, not the element value. Task 11: the
    //cell kind for scalar/string element loops (the pair always names a
    //payload cell), the aggregate kind for struct element loops (struct
    //pairs read as plain record handles at offset 0).
    uint8_t refVarKind = RTK_Byref;
    if (ScalarPrimIndexOfRtk(elemKind) >= 0 || elemKind == RTK_String)
        refVarKind = RTK_ByrefCell;
    //A promoted loop var (body-level & binding) takes the promoted
    //slot kind — the box handle rides the ESC_HeapIndex GC band and
    //the debug display renders the payload; mutually exclusive with
    //the & form (reference targets never promote).
    const uint8_t userVarKind = bRefLoop ? refVarKind
        : (fe.ContainFlags(NF_Promoted)
            ? static_cast<uint8_t>(RTK_Promoted) : elemKind);
    slots.userVarSlot = AllocLocal(fe.VarName(), kFrameSlotBytes,
        userVarKind,
        false);
    //Array iter is RTK_Array; List and Dict-via-Keys are RTK_Class.
    uint8_t iterKind = isArray
        ? static_cast<uint8_t>(RTK_Array)
        : static_cast<uint8_t>(RTK_Class);
    slots.iterSlot = AllocLocal(
        "__foreach_iter_" + std::to_string(counter),
        kFrameSlotBytes, iterKind, false);
    slots.iSlot = AllocLocal(
        "__foreach_i_" + std::to_string(counter),
        kFrameSlotBytes, RTK_Int32, false);
    slots.nSlot = AllocLocal(
        "__foreach_n_" + std::to_string(counter),
        kFrameSlotBytes, RTK_Int32, false);
    return slots;
}

//Dict: inline dict.Keys() — replace iterSlot's dict heap idx with a
//fresh List<K> heap idx from the Keys() intrinsic. From here on, the
//lowering is identical to List<T> iteration (element type = K).
void VmBackend::EmitForeachDictKeysPrelude(uint16_t iterSlot,
        BytecodeEmitter& emitter) {
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(iterSlot);
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(iterSlot);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(m_currFunc->callParamBase);
    uint16_t keysIdx = AddStringConstant("keys");
    emitter.Emit(OpCode::OP_CallMethod);
    emitter.EmitUint16(keysIdx);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(iterSlot);
    emitter.Emit(OpCode::OP_ParaEnd);
}

//Compute the iteration count into nSlot. Array: OP_ArrayLength
//<dst=arr> <src=arr> on the heap idx (NullCheck first). List (and
//Dict-after-Keys: List<K>): length() via the method-call shape.
void VmBackend::EmitForeachLength(bool isArray, uint16_t iterSlot,
        uint16_t nSlot, BytecodeEmitter& emitter) {
    if (isArray) {
        emitter.Emit(OpCode::OP_NullCheck);
        emitter.EmitUint16(iterSlot);
        emitter.Emit(OpCode::OP_ArrayLength);
        emitter.EmitUint16(nSlot);
        emitter.EmitUint16(iterSlot);
        return;
    }
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(iterSlot);
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(iterSlot);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(m_currFunc->callParamBase);
    uint16_t nameIdx = AddStringConstant("length");
    emitter.Emit(OpCode::OP_CallMethod);
    emitter.EmitUint16(nameIdx);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(nSlot);
    emitter.Emit(OpCode::OP_ParaEnd);
}

//Loop head: mark the loop start, enter the LoopContext (break/continue
//reuse the loop machinery), then the condition i < n → jumpToEnd if
//not. tempSlot = i; tempSlot2 = n; OP_Cmp <i32> Less writes 1/0 into
//tempSlot. The miss-jump placeholder doubles as this loop's break
//target. Returns the loop-start offset for the back-jump.
//probeLength (container loops): re-probe n from the iterable at the
//top of EVERY iteration — the back-jump lands on the probe, so the
//bound tracks the container the body mutates (Task 12; arrays pass
//false, their length is record-fixed).
size_t VmBackend::EmitForeachLoopHead(uint16_t iSlot, uint16_t nSlot,
        BytecodeEmitter& emitter, bool probeLength, uint16_t iterSlot) {
    size_t loopStart = emitter.CurrentOffset();
    PushLoopContext();
    if (probeLength)
        EmitForeachLength(false, iterSlot, nSlot, emitter);
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(iSlot);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(m_currFunc->tempSlot);
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(nSlot);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(m_currFunc->tempSlot2);
    //Counter/length hidden locals are fixed int32 (0.7.5 kind-immediate).
    EmitCmp(emitter, NK_Int32, kCmpLess,
            m_currFunc->tempSlot, m_currFunc->tempSlot2);
    emitter.Emit(OpCode::OP_JumpIfNot);
    size_t jumpToEnd = emitter.CurrentOffset();
    emitter.EmitUint16(0);  //placeholder, patched by the loop tail
    emitter.EmitUint16(m_currFunc->tempSlot);
    m_loopStack.back().breakJumps.push_back(jumpToEnd);
    return loopStart;
}

//Body-prelude: load element i into the user variable slot. Array:
//OP_LoadElement, plus struct deep-copy on read (value semantics).
//Reference form (Task 7 D6): bind the element in place instead of
//copying it out — array cells via OP_RefOfElement, List elements via
//the parked raw handle (RefOfPromoted for boxed scalar/string
//payloads, RefOfLocal for struct records).
//List (and Dict-after-Keys): get(i) — callParamBase[1] = i,
//callParamBase[0] = this, OP_CallMethod, then unbox primitive T.
void VmBackend::EmitForeachLoadElement(bool isArray, SnField* pElemType,
        uint8_t elemKind, const ArgBoxPlan& elemBox, bool bRefLoop,
        bool bPromotedVar, const ForeachSlots& slots,
        BytecodeEmitter& emitter) {
    if (isArray) {
        if (bRefLoop)
            EmitForeachArrayRefBind(slots, emitter);
        else
            EmitForeachArrayLoadElement(pElemType, elemKind,
                bPromotedVar, slots, emitter);
        return;
    }
    uint16_t paramOffset = m_currFunc->callParamBase + 1 * kFrameSlotBytes;
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(slots.iSlot);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(paramOffset);
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(slots.iterSlot);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(m_currFunc->callParamBase);
    uint16_t nameIdx = AddStringConstant(kBuiltinGetMethodName);
    emitter.Emit(OpCode::OP_CallMethod);
    emitter.EmitUint16(nameIdx);
    emitter.EmitUint16(m_currFunc->callParamBase);
    if (elemBox.needsBox && !bRefLoop)
        EmitBoundaryUnbox(elemBox, emitter);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(slots.userVarSlot);
    if (bRefLoop)
        EmitForeachListRefPair(elemBox, slots, emitter);
    emitter.Emit(OpCode::OP_ParaEnd);
    //Promoted loop var: after the parameter-staging fence, wrap the
    //raw element value into the stable box (Box may collect; the raw
    //value rides the slot's heap band until the Assign lands).
    if (bPromotedVar)
        EmitPromotedValueWrap(slots.userVarSlot, pElemType->Kind(),
            emitter);
}

//Array value-form element load: OP_LoadElementFlat, the struct
//deep-copy on read, and the promoted-loop-var box wrap.
void VmBackend::EmitForeachArrayLoadElement(SnField* pElemType,
        uint8_t elemKind, bool bPromotedVar, const ForeachSlots& slots,
        BytecodeEmitter& emitter) {
    //Stage-2: OP_LoadElementFlat <dst> <arr> <flatIdx> — the payload
    //is contiguous row-major, so the linear index walks rank-1 and
    //rank-N arrays identically.
    emitter.Emit(OpCode::OP_LoadElementFlat);
    emitter.EmitUint16(slots.userVarSlot);
    emitter.EmitUint16(slots.iterSlot);
    emitter.EmitUint16(slots.iSlot);
    //Struct element types need deep-copy on read (value semantics).
    //Per-unit: a cross-unit element struct slots as an import
    //placeholder (the old silent-0 fallback could not).
    if (elemKind == RTK_Struct && pElemType
        && pElemType->Kind() == NK_StructDecl) {
        EmitStructDeepCopy(slots.userVarSlot, slots.userVarSlot,
            static_cast<SnStructDecl&>(*pElemType), emitter);
    }
    //Promoted loop var: the raw element value just landed in the
    //slot — wrap it into the stable box before any body read.
    if (bPromotedVar)
        EmitPromotedValueWrap(slots.userVarSlot,
            pElemType->Kind(), emitter);
}

//Array & bind (Task 7 D6): the & twin of LoadElementFlat — bind the
//(record, cell) pair; scalar cells bind the cell address, struct cells
//deref to the element record (OP_RefOfElement).
void VmBackend::EmitForeachArrayRefBind(const ForeachSlots& slots,
        BytecodeEmitter& emitter) {
    emitter.Emit(OpCode::OP_RefOfElement);
    emitter.EmitUint16(slots.userVarSlot);
    emitter.EmitUint16(slots.iterSlot);
    emitter.EmitUint16(slots.iSlot);
}

//List & bind tail (Task 7 D6): park the raw element handle, then build
//the pair in place — no allocation sits between the park and the pair,
//so no GC window opens. Boxed scalar/string elements bind the boxing
//record's payload cell; struct records bind themselves.
void VmBackend::EmitForeachListRefPair(const ArgBoxPlan& elemBox,
        const ForeachSlots& slots, BytecodeEmitter& emitter) {
    emitter.Emit(elemBox.needsBox ? OpCode::OP_RefOfPromoted
                                  : OpCode::OP_RefOfLocal);
    emitter.EmitUint16(slots.userVarSlot);
    emitter.EmitUint16(slots.userVarSlot);
}

//Loop tail: continue target (i = i + 1), back-jump to the loop start,
//then patch this loop's break/continue jumps and pop the LoopContext.
void VmBackend::EmitForeachLoopTail(size_t loopStart, uint16_t iSlot,
        BytecodeEmitter& emitter) {
    size_t continueTarget = emitter.CurrentOffset();
    emitter.Emit(OpCode::OP_ConstInt32);
    emitter.EmitInt32(1);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(m_currFunc->tempSlot2);
    //OP_Add <i32> <dst> <src>: locals[dst] += locals[src] (counter int32).
    EmitBinOp(emitter, OpCode::OP_Add, NK_Int32,
              iSlot, m_currFunc->tempSlot2);
    //Jump back to loop start
    emitter.Emit(OpCode::OP_Jump);
    emitter.EmitUint16(static_cast<uint16_t>(loopStart));
    size_t loopEnd = emitter.CurrentOffset();
    auto& ctx = m_loopStack.back();
    for (size_t pos : ctx.breakJumps)
        emitter.PatchUint16(pos, static_cast<uint16_t>(loopEnd));
    for (size_t pos : ctx.continueJumps)
        emitter.PatchUint16(pos, static_cast<uint16_t>(continueTarget));
    m_loopStack.pop_back();
}

} //namespace nlang
