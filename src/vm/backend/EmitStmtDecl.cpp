/*---
    EmitStmtDecl.cpp — 局部声明与断言语句发射（赋值族在 EmitStmtAssign.cpp）。
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

void VmBackend::Access(SnLocalDeclStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        auto& decl = static_cast<SnLocalDeclStmt&>(stmt);
        //0.7.3 B: an array type expression binds Field() to its interned
        //array token (alias uses included — the alias pre-pass splices the
        //target type in before resolve), so one Field() read plus
        //RuntimeTypeKind files every local, arrays included. The separate
        //IsArrayType shape branch and element walk are gone.
        SnField* evalType = decl.Type()->Field();
        uint8_t typeKind = RuntimeTypeKind(evalType);
        for (auto& local : decl.Decls()) {
            uint16_t offset = AllocLocal(local.name, kFrameSlotBytes, typeKind, false);
            //For struct types, emit OP_AllocStruct to allocate on heap.
            if (typeKind == RTK_Struct && evalType) {
                int structIdx = m_compiledModule.FindStruct(evalType->Name());
                if (structIdx >= 0) {
                    auto& cs = m_compiledModule.structs[structIdx];
                    emitter.Emit(OpCode::OP_AllocStruct);
                    emitter.EmitUint16(offset);
                    emitter.EmitUint16(static_cast<uint16_t>(structIdx));
                    emitter.EmitUint16(cs.fieldCount);
                }
            }
            //Class and array types start as null (0) — no allocation needed.
        }
        return;
}

    //Assert statement: assert(cond); - exit(1) on failure.
    //Codegen pattern: evaluate condition, OP_JumpIfNot to fail block,
    //fail block emits OP_AssertFail which throws (caught by main → exit 1).
void VmBackend::Access(SnAssertStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        auto& as = static_cast<SnAssertStmt&>(stmt);
        //Condition staging: EvalAreaClaim, never tempSlot (round-9 — a
        //binary cond parks its LEFT operand in the staging slot across
        //the nested RIGHT emission; the EmitBinding struct deep-copy
        //scratch on tempSlot corrupted it and flipped the verdict).
        //StmtPeakDepth's AssertStmt case tracks the claim=1; scope ends
        //at the JumpIfNot operand.
        size_t jumpToFail;
        {
            EvalAreaClaim condClaim(*this, 1);
            uint16_t condSlot = condClaim.base();
            EmitExpression(*as.Cond(), emitter, condSlot);
            emitter.Emit(OpCode::OP_JumpIfNot);
            jumpToFail = emitter.CurrentOffset();
            emitter.EmitUint16(0);  //placeholder
            emitter.EmitUint16(condSlot);
        }
        //Success path: jump over fail block
        emitter.Emit(OpCode::OP_Jump);
        size_t jumpToEnd = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder
        //Fail block
        size_t failStart = emitter.CurrentOffset();
        emitter.Emit(OpCode::OP_AssertFail);
        //Optional message: use the assertion's source location text.
        //For Phase 9a we emit an empty message idx (0) — VmExecutor
        //prints "assertion failed" alone when msg is empty.
        emitter.EmitUint16(0);
        //End
        size_t endPos = emitter.CurrentOffset();
        emitter.PatchUint16(jumpToFail, static_cast<uint16_t>(failStart));
        emitter.PatchUint16(jumpToEnd, static_cast<uint16_t>(endPos));
        return;
}
} //namespace nlang
