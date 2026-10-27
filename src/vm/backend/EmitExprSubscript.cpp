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
    //Stage-2: N indices stage after the receiver in one exclusive
    //claim (rank-1 subscripts hold exactly one — same shape as before).
    const size_t rank = sub.Indices().size();
    EvalAreaClaim claim(*this, rank + 1);
    uint16_t claimBase = claim.base();
    EmitExpression(*sub.Array(), emitter, claimBase);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(claimBase);
    for (size_t d = 0; d < rank; ++d)
        EmitExpression(*sub.Indices()[d], emitter,
            claimBase + (d + 1) * kFrameSlotBytes);
    emitter.Emit(OpCode::OP_LoadElementN);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(claimBase);
    emitter.EmitUint16(claimBase + kFrameSlotBytes);  //idxBase
    emitter.EmitUint16(static_cast<uint16_t>(rank));
}


} //namespace nlang
