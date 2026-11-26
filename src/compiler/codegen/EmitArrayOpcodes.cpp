/*---
    EmitArrayOpcodes.cpp — v0.13.1 D5: the array opcode emission choke
    points. Each helper owns its opcode's COMPLETE operand layout — the
    call sites pass semantic parameters (element type field, dims base)
    and can never hand-write a stale sequence when a layout word
    changes. The element descriptor row is derived here from the
    element field, so an allocation site cannot forget it: the erased
    arrayTypes row cannot tell Box<int>[] from Box<string>[] and the
    record's stamped element descriptor is what does.
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnArrayTypeToken.h>

namespace nlang {

//OP_AllocArrayN: dst, typeIdx, rank, dimsBase, descIdx.
void VmBackend::EmitAllocArrayN(uint16_t resultOffset, uint16_t arrayTypeIdx,
                                uint8_t rank, uint16_t dimsBase,
                                SnField* pElemField,
                                BytecodeEmitter& emitter) {
    emitter.Emit(OpCode::OP_AllocArrayN);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(arrayTypeIdx);
    emitter.EmitUint16(rank);
    emitter.EmitUint16(dimsBase);
    emitter.EmitUint16(ElemDescriptorIndexFor(pElemField));
}

//OP_AssembleArrayN: dst, typeIdx, elemsBase, count, descIdx.
void VmBackend::EmitAssembleArrayN(uint16_t resultOffset,
                                   uint16_t arrayTypeIdx, uint16_t rowsBase,
                                   uint16_t rowCount, SnField* pElemField,
                                   BytecodeEmitter& emitter) {
    emitter.Emit(OpCode::OP_AssembleArrayN);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(arrayTypeIdx);
    emitter.EmitUint16(rowsBase);
    emitter.EmitUint16(rowCount);
    emitter.EmitUint16(ElemDescriptorIndexFor(pElemField));
}

//OP_AllocArrayDyn: dst, typeIdx, dimsSlot, descIdx (the Array(dims)
//dynamic-rank construction stamps its element like every other form).
void VmBackend::EmitAllocArrayDyn(uint16_t resultOffset,
                                  uint16_t arrayTypeIdx, uint16_t dimsSlot,
                                  SnField* pElemField,
                                  BytecodeEmitter& emitter) {
    emitter.Emit(OpCode::OP_AllocArrayDyn);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(arrayTypeIdx);
    emitter.EmitUint16(dimsSlot);
    emitter.EmitUint16(ElemDescriptorIndexFor(pElemField));
}

//OP_CheckArrayType: dst, src, arrayTypeIdx, descIdx (`o as Box<int>[]`).
void VmBackend::EmitCheckArrayTypeOp(uint16_t slot, uint16_t arrayTypeIdx,
                                     SnField* pElemField,
                                     BytecodeEmitter& emitter) {
    emitter.Emit(OpCode::OP_CheckArrayType);
    emitter.EmitUint16(slot);
    emitter.EmitUint16(slot);
    emitter.EmitUint16(arrayTypeIdx);
    emitter.EmitUint16(ElemDescriptorIndexFor(pElemField));
}

} //namespace nlang
