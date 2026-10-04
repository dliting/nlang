/*---
    EmitStmtForeach.cpp — foreach 语句发射（数组/List/Dict 三路索引式展开）。
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
    elemType = pField->EvalDataType();
    //0.7.3 B token path: an array-typed local carries the interned
    //token; the loop-var slot kind keys on the ELEMENT.
    if (elemType && elemType->Kind() == NK_ArrayTypeToken)
        elemType = static_cast<SnArrayTypeToken*>(
            elemType)->ElemTypeOf();
    return true;
}

static ForeachIterableKind ClassifyForeachIterable(
        SnExpression* pIter) {
    ForeachIterableKind k;
    if (DetectDeclaredArraySource(pIter, k.elemType)) {
        k.isArray = true;
        return k;
    }
    auto* pIterType = pIter->EvalDataType();
    //0.7.3 B D10 value-form arm: every array-valued source — get()
    //and subscript reads out of List<T[]>/Dict<K,V[]>, array-returning
    //calls, bound method calls, new-array expressions — carries the
    //interned array token in EvalDataType. The declaration-side arm
    //above only recognizes identifier/member lvalue shapes; this arm
    //routes the value forms (accepted since D10; the masquerade-era
    //resolve-time rejection existed because the degraded
    //EvalDataType carried no array identity for this dispatch).
    if (pIterType && pIterType->Kind() == NK_ArrayTypeToken) {
        k.isArray = true;
        k.elemType = static_cast<SnArrayTypeToken*>(
            pIterType)->ElemTypeOf();
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
            if (baseName == "List") {
                k.isList = true;
                k.elemType = typeArgs.empty() ? nullptr : typeArgs[0];
            } else if (baseName == "Dict") {
                k.isDict = true;
                k.elemType = typeArgs.empty() ? nullptr : typeArgs[0];
            }
        }
    }
    return k;
}

void VmBackend::Access(SnForeachStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        auto& fe = static_cast<SnForeachStmt&>(stmt);
        ForeachIterableKind iter = ClassifyForeachIterable(fe.Iterable());
        //0.7.5 char bridge: string sources take the dedicated
        //code-point expansion (no length/get() calls — OP_StrForeachStep).
        if (iter.isString) {
            EmitStringForeach(fe, emitter);
            return;
        }
        //Phase C: Array + List. Phase D: Dict (the Keys() prelude
        //materializes a List<K> into iterSlot, then the rest mirrors
        //the List path).
        assert((iter.isArray || iter.isList || iter.isDict)
            && "foreach iterable must be Array, List<T>, Dict<K,V>, or string");
        //0.7.3 B: an array-typed element (List<int[]> / Dict<K[],V> key
        //iteration) IS the interned token, and RuntimeTypeKind maps the
        //token to RTK_Array through the IsArrayType() override — so one
        //channel tags the loop-var slot, and the GC traces the handle.
        uint8_t elemKind = RuntimeTypeKind(iter.elemType);
        ForeachSlots slots = AllocForeachLocals(fe, elemKind,
            iter.isArray);
        //Evaluate the iterable into iterSlot.
        EmitExpression(*fe.Iterable(), emitter, slots.iterSlot);
        if (iter.isDict)
            EmitForeachDictKeysPrelude(slots.iterSlot, emitter);
        EmitForeachLength(iter.isArray, slots.iterSlot, slots.nSlot,
            emitter);
        //i = 0
        emitter.Emit(OpCode::OP_ConstInt32);
        emitter.EmitInt32(0);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(slots.iSlot);
        size_t loopStart = EmitForeachLoopHead(slots.iSlot, slots.nSlot,
            emitter);
        EmitForeachLoadElement(iter.isArray, iter.elemType, elemKind,
            slots, emitter);
        EmitStatement(*fe.Body(), emitter);
        EmitForeachLoopTail(loopStart, slots.iSlot, emitter);
        return;
}

//Allocate hidden locals BEFORE the LoopContext push. AllocLocal dedupes
//by name, so uniquify hidden locals via per-function counter (nested
//foreach would otherwise collide on __foreach_iter etc.).
VmBackend::ForeachSlots VmBackend::AllocForeachLocals(SnForeachStmt& fe,
        uint8_t elemKind, bool isArray) {
    ForeachSlots slots;
    uint16_t counter = m_currFunc->foreachCounter++;
    slots.userVarSlot = AllocLocal(fe.VarName(), kFrameSlotBytes, elemKind,
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
size_t VmBackend::EmitForeachLoopHead(uint16_t iSlot, uint16_t nSlot,
        BytecodeEmitter& emitter) {
    size_t loopStart = emitter.CurrentOffset();
    PushLoopContext();
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
//List (and Dict-after-Keys): get(i) — callParamBase[1] = i,
//callParamBase[0] = this, OP_CallMethod, then unbox primitive T.
void VmBackend::EmitForeachLoadElement(bool isArray, SnField* pElemType,
        uint8_t elemKind, const ForeachSlots& slots,
        BytecodeEmitter& emitter) {
    if (isArray) {
        //OP_LoadElement <dst> <arr> <index>
        emitter.Emit(OpCode::OP_LoadElement);
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
    uint16_t nameIdx = AddStringConstant("get");
    emitter.Emit(OpCode::OP_CallMethod);
    emitter.EmitUint16(nameIdx);
    emitter.EmitUint16(m_currFunc->callParamBase);
    auto t = BoxingTagFor(pElemType);
    if (t.isPrimitive) {
        emitter.Emit(OpCode::OP_Unbox);
        emitter.EmitByte(t.tag);
    }
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(slots.userVarSlot);
    emitter.Emit(OpCode::OP_ParaEnd);
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

//0.7.5 string arm: code-point iteration. Hidden locals mirror the
//index-based expansion in kind role (string handle / byte offset /
//continue flag — the counter-and-length pair collapses into the
//offset, since OP_StrForeachStep detects end-of-string itself).
//The loop head IS the step (decode + advance + continue flag in one
//op): continue jumps to the head (the step is the increment), break
//to the loop end via the miss-jump. No NullCheck — StrVal(0) reads
//as "" and the first step clears the flag; no EvalAreaClaim — the
//step touches only these locals (the walker's 2-slot foreach
//reservation stays a harmless over-estimate).
void VmBackend::EmitStringForeach(SnForeachStmt& fe,
                                  BytecodeEmitter& emitter) {
    uint16_t counter = m_currFunc->foreachCounter++;
    uint16_t userVarSlot = AllocLocal(fe.VarName(), kFrameSlotBytes,
                                      RTK_Char, false);
    uint16_t iterSlot = AllocLocal(
        "__foreach_iter_" + std::to_string(counter),
        kFrameSlotBytes, RTK_String, false);
    uint16_t offSlot = AllocLocal(
        "__foreach_off_" + std::to_string(counter),
        kFrameSlotBytes, RTK_Int32, false);
    uint16_t condSlot = AllocLocal(
        "__foreach_cond_" + std::to_string(counter),
        kFrameSlotBytes, RTK_Int32, false);
    //Evaluate the source once; the byte offset starts at 0.
    EmitExpression(*fe.Iterable(), emitter, iterSlot);
    emitter.Emit(OpCode::OP_ConstInt32);
    emitter.EmitInt32(0);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(offSlot);
    //Loop head = the step. Miss-jump doubles as the break target.
    size_t loopStart = emitter.CurrentOffset();
    PushLoopContext();
    emitter.Emit(OpCode::OP_StrForeachStep);
    emitter.EmitUint16(iterSlot);
    emitter.EmitUint16(offSlot);
    emitter.EmitUint16(condSlot);
    emitter.EmitUint16(userVarSlot);
    emitter.Emit(OpCode::OP_JumpIfNot);
    size_t jumpToEnd = emitter.CurrentOffset();
    emitter.EmitUint16(0);  //placeholder, patched below
    emitter.EmitUint16(condSlot);
    m_loopStack.back().breakJumps.push_back(jumpToEnd);
    EmitStatement(*fe.Body(), emitter);
    emitter.Emit(OpCode::OP_Jump);
    emitter.EmitUint16(static_cast<uint16_t>(loopStart));
    //continue target = the head (the step advances the offset).
    size_t loopEnd = emitter.CurrentOffset();
    auto& ctx = m_loopStack.back();
    for (size_t pos : ctx.breakJumps)
        emitter.PatchUint16(pos, static_cast<uint16_t>(loopEnd));
    for (size_t pos : ctx.continueJumps)
        emitter.PatchUint16(pos, static_cast<uint16_t>(loopStart));
    m_loopStack.pop_back();
}

} //namespace nlang
