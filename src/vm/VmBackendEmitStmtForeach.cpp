/*---
    VmBackendEmitStmtForeach.cpp — foreach 语句发射（数组/List/Dict 三路索引式展开）。
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
#include <cassert>
#include <map>
#include <unordered_set>

namespace nlang {

static const uint16_t VALUE_SIZE = 4; // int32 and float are both 4 bytes

void VmBackend::Access(SnForeachStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        auto& fe = static_cast<SnForeachStmt&>(stmt);

        //--- Detect iterable kind -----------------------------------------
        SnExpression* pIter = fe.Iterable();
        bool isArray = false;
        bool isList  = false;
        bool isDict  = false;
        SnField* pElemType = nullptr;

        if (pIter->Kind() == NK_IdentifierExpr) {
            auto* pField = static_cast<SnIdentifierExpr*>(pIter)->Field();
            if (pField && pField->IsArrayType()) {
                isArray = true;
                pElemType = pField->EvalDataType();
                //0.7.3 B token path: an array-typed local carries
                //the interned token; the loop-var slot kind keys on
                //the ELEMENT.
                if (pElemType && pElemType->Kind() == NK_ArrayTypeToken)
                    pElemType = static_cast<SnArrayTypeToken*>(
                        pElemType)->ElemTypeOf();
            }
        } else if (pIter->Kind() == NK_MemberExpr) {
            //Member lvalue (`b.a` / `this.a`) with an array-typed field —
            //the same lvalue family as the identifier shape, keyed on the
            //FIELD's declared array-ness (the authoritative declaration
            //signal). Pre-token the member's own EvalDataType degraded to
            //the element type (the dispatch-order trap) and mis-routed
            //this into the List path; post-flip it carries the token and
            //the value-form arm below routes the same shapes as backstop.
            auto* pInner = static_cast<SnMemberExpr*>(pIter)->Inner();
            if (pInner && pInner->Kind() == NK_IdentifierExpr) {
                auto* pField = static_cast<SnIdentifierExpr*>(pInner)->Field();
                if (pField && pField->IsArrayType()) {
                    isArray = true;
                    pElemType = pField->EvalDataType();
                    //0.7.3 B token path: same peel as the identifier
                    //shape above.
                    if (pElemType && pElemType->Kind() == NK_ArrayTypeToken)
                        pElemType = static_cast<SnArrayTypeToken*>(
                            pElemType)->ElemTypeOf();
                }
            }
        }
        //0.7.3 B D10 value-form arm: every array-valued source — get()
        //and subscript reads out of List<T[]>/Dict<K,V[]>, array-returning
        //calls, bound method calls, new-array expressions — carries the
        //interned array token in EvalDataType. The declaration-side arms
        //above only recognize identifier/member lvalue shapes; this arm
        //routes the value forms (accepted since D10; the masquerade-era
        //resolve-time rejection existed because the degraded
        //EvalDataType carried no array identity for this dispatch).
        if (!isArray && pIter->EvalDataType()
            && pIter->EvalDataType()->Kind() == NK_ArrayTypeToken)
        {
            isArray = true;
            pElemType = static_cast<SnArrayTypeToken*>(
                pIter->EvalDataType())->ElemTypeOf();
        }
        if (!isArray) {
            auto* pIterType = pIter->EvalDataType();
            if (pIterType && pIterType->Kind() == NK_ClassDecl) {
                auto* pClass = static_cast<SnClassDecl*>(pIterType);
                if (pClass->IsGenericInstantiation()) {
                    const auto& baseName = pClass->BaseName();
                    const auto& typeArgs = pClass->GenericTypeArgs();
                    if (baseName == "List") {
                        isList = true;
                        pElemType = typeArgs.empty() ? nullptr : typeArgs[0];
                    } else if (baseName == "Dict") {
                        isDict = true;
                        pElemType = typeArgs.empty() ? nullptr : typeArgs[0];
                    }
                }
            }
        }
        //Phase C: Array + List. Phase D: Dict (inline Keys() call materializes
        //a List<K> into iterSlot, then the rest mirrors the List path).
        assert((isArray || isList || isDict)
            && "foreach iterable must be Array, List<T>, or Dict<K,V>");

        //0.7.3 B: an array-typed element (List<int[]> / Dict<K[],V> key
        //iteration) IS the interned token, and RuntimeTypeKind maps the
        //token to RTK_Array through the IsArrayType() override — so one
        //channel tags the loop-var slot, and the GC traces the handle.
        uint8_t elemKind = RuntimeTypeKind(pElemType);

        //--- 1. Allocate hidden locals BEFORE LoopContext push -------------
        //AllocLocal dedupes by name, so uniquify hidden locals via per-function
        //counter (nested foreach would otherwise collide on __foreach_iter etc.).
        uint16_t counter = m_currFunc->foreachCounter++;
        uint16_t userVarSlot = AllocLocal(fe.VarName(),
            VALUE_SIZE, elemKind, false);
        //Array iter is RTK_Array; List and Dict-via-Keys are RTK_Class.
        uint8_t iterKind = isArray
            ? static_cast<uint8_t>(RTK_Array)
            : static_cast<uint8_t>(RTK_Class);
        uint16_t iterSlot = AllocLocal(
            "__foreach_iter_" + std::to_string(counter),
            VALUE_SIZE, iterKind, false);
        uint16_t iSlot = AllocLocal(
            "__foreach_i_" + std::to_string(counter),
            VALUE_SIZE, RTK_Int32, false);
        uint16_t nSlot = AllocLocal(
            "__foreach_n_" + std::to_string(counter),
            VALUE_SIZE, RTK_Int32, false);

        //--- 2. Evaluate iterable into iterSlot ---------------------------
        EmitExpression(*pIter, emitter, iterSlot);

        //--- 2b. Dict: inline dict.Keys() → materialize List<K> into iterSlot
        //For Dict foreach, iterSlot now holds a dict heap idx; we replace it
        //with a fresh List<K> heap idx from the Keys() intrinsic. From here on,
        //the lowering is identical to List<T> iteration (element type = K).
        if (isDict) {
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

        //--- 3. Compute length into nSlot ---------------------------------
        if (isArray) {
            //OP_ArrayLength <dst=arr> <src=arr> — operates on the heap idx in
            //the slot. Mirror the pattern at line 1259 (NullCheck first).
            emitter.Emit(OpCode::OP_NullCheck);
            emitter.EmitUint16(iterSlot);
            emitter.Emit(OpCode::OP_ArrayLength);
            emitter.EmitUint16(nSlot);
            emitter.EmitUint16(iterSlot);
        } else {
            //List<T>.Length() (or Dict-after-Keys: List<K>.Length()).
            //Call shape mirrors line 1155-1200.
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

        //--- 4. i = 0 -----------------------------------------------------
        emitter.Emit(OpCode::OP_ConstInt32);
        emitter.EmitInt32(0);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(iSlot);

        //--- 5. Loop start ------------------------------------------------
        size_t loopStart = emitter.CurrentOffset();

        //--- 6. Enter loop context (reuse LoopContext for break/continue) -
        PushLoopContext();

        //--- 7. Condition: i < n → jumpToEnd if not -----------------------
        //tempSlot = i; tempSlot2 = n; OP_Less_i32 writes 1/0 into tempSlot.
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(iSlot);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->tempSlot);
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(nSlot);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->tempSlot2);
        emitter.Emit(OpCode::OP_Less_i32);
        emitter.EmitUint16(m_currFunc->tempSlot);
        emitter.EmitUint16(m_currFunc->tempSlot2);
        emitter.Emit(OpCode::OP_JumpIfNot);
        size_t jumpToEnd = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder, patched at step 12
        emitter.EmitUint16(m_currFunc->tempSlot);
        m_loopStack.back().breakJumps.push_back(jumpToEnd);

        //--- 8. Body-prelude: load element i into userVarSlot -------------
        if (isArray) {
            //OP_LoadElement <dst> <arr> <index>
            emitter.Emit(OpCode::OP_LoadElement);
            emitter.EmitUint16(userVarSlot);
            emitter.EmitUint16(iterSlot);
            emitter.EmitUint16(iSlot);
            //Struct element types need deep-copy on read (value semantics),
            //parallel to subscript codegen at line 1478-1485.
            if (elemKind == RTK_Struct && pElemType) {
                int structIdx = m_compiledModule.FindStruct(
                    pElemType->Name());
                emitter.Emit(OpCode::OP_CopyStruct);
                emitter.EmitUint16(userVarSlot);
                emitter.EmitUint16(userVarSlot);
                emitter.EmitUint16(structIdx >= 0
                    ? static_cast<uint16_t>(structIdx) : 0);
            }
        } else {
            //List<T>.Get(i). Per-method boxing plan: unbox primitive T.
            //callParamBase[1] = i; callParamBase[0] = this; OP_CallMethod.
            uint16_t paramOffset = m_currFunc->callParamBase + 1 * VALUE_SIZE;
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(iSlot);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(paramOffset);
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(iterSlot);
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
            emitter.EmitUint16(userVarSlot);
            emitter.Emit(OpCode::OP_ParaEnd);
        }

        //--- 9. Body ------------------------------------------------------
        EmitStatement(*fe.Body(), emitter);

        //--- 10. Continue target: i = i + 1 -------------------------------
        size_t continueTarget = emitter.CurrentOffset();
        emitter.Emit(OpCode::OP_ConstInt32);
        emitter.EmitInt32(1);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->tempSlot2);
        //OP_Add_i32 <dst> <src>: locals[dst] += locals[src].
        emitter.Emit(OpCode::OP_Add_i32);
        emitter.EmitUint16(iSlot);
        emitter.EmitUint16(m_currFunc->tempSlot2);

        //--- 11. Jump back to loop start ----------------------------------
        emitter.Emit(OpCode::OP_Jump);
        emitter.EmitUint16(static_cast<uint16_t>(loopStart));

        //--- 12. Patch break/continue; pop LoopContext --------------------
        size_t loopEnd = emitter.CurrentOffset();
        auto& ctx = m_loopStack.back();
        for (size_t pos : ctx.breakJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(loopEnd));
        for (size_t pos : ctx.continueJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(continueTarget));

        m_loopStack.pop_back();
        return;
}

    //Break statement.
    //Break exits the innermost enclosing switch or loop.

} //namespace nlang
