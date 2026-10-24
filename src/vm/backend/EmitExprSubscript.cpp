/*---
EmitExprSubscript.cpp — the plain array element read (OP_LoadElement
family emitter), split from EmitExprCast.cpp when that file reached the
source-size budget. The container-get and string-byte arms stay with
the cast dispatch; this file hosts the array path only.
---*/
#include "VmBackend.h"
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/SnArrayTypeToken.h>

namespace nlang {

void VmBackend::EmitArraySubscriptRead(SnSubscriptExpr& sub,
                                       BytecodeEmitter& emitter,
                                       uint16_t resultOffset) {
    //Stage-2 task-2 gate: ND subscript emission lands in task 4.
    if (EmitGateRankOf(*sub.Array()) >= 2)
    {
        m_pEnv->Log(CLL_Error, sub.Location(),
            "multi-dimensional arrays are not supported yet.");
        return;
    }
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
}


} //namespace nlang
