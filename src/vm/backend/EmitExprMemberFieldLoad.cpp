/*---
    EmitExprMemberFieldLoad.cpp — 字段读取尾部族：sealed 字段拷贝（v0.12.4 D5 copy-on-read）与 struct/class 字段装载。
    从 EmitExprMember.cpp 抽取（source-size split，零行为变化）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/runtime/BuiltinGenericNames.h>
#include <stdexcept>

namespace nlang
{

//v0.12.4 D5: copy-on-read seal — called right after a field load. The
//verdict reads the field's DECLARING aggregate (field->Parent()), the
//same source the Task-3 store gate uses, so a field is either fully
//sealed (stores rejected, reads copied) or fully ordinary. The copy
//lands only on List/Dict instantiations — the only field surfaces that
//can mutate through a shared handle; const-aggregate-typed fields are
//already write-rejected everywhere. Declaring-ctor stores never pass
//through here (OP_StoreField path), so seeding keeps the real field.
void VmBackend::EmitSealedFieldCopyIfNeeded(SnField* pField,
                                            BytecodeEmitter& emitter,
                                            uint16_t resultOffset) {
    if (!pField || pField->IsArrayType())
        return;
    auto* pOwner = pField->Parent();
    if (!pOwner)
        return;
    bool bSealed;
    if (pOwner->Kind() == NK_ClassDecl)
        bSealed = static_cast<SnClassDecl*>(pOwner)->IsConstAggregate();
    else if (pOwner->Kind() == NK_StructDecl)
        bSealed = static_cast<SnStructDecl*>(pOwner)->IsConstAggregate();
    else
        return;
    if (!bSealed)
        return;
    auto* pType = pField->EvalDataType();
    if (!pType || pType->Kind() != NK_ClassDecl)
        return;
    auto* pInst = static_cast<SnClassDecl*>(pType);
    if (!pInst->IsGenericInstantiation())
        return;
    uint16_t copyId;
    if (pInst->BaseName() == kBuiltinListTypeName)
        copyId = INTR_List_Copy;
    else if (pInst->BaseName() == kBuiltinDictTypeName)
        copyId = INTR_Dict_Copy;
    else
        return;
    //Zero-arg receiver intrinsic (string getHashCode shape): the loaded
    //handle rides resultOffset into callParamBase slot 0, the intrinsic
    //mints the copy, the result lands back in resultOffset. Uses no
    //eval-area claim, so the walkers need no mirror.
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(resultOffset);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.Emit(OpCode::OP_CallIntrinsic);
    emitter.EmitUint16(copyId);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    emitter.Emit(OpCode::OP_ParaEnd);
}

//Struct field read tail: OP_LoadField of `fieldName` into resultOffset
//(the receiver heap index is already emitted).
void VmBackend::EmitStructFieldLoad(SnStructDecl& structDecl,
                                    const std::string& fieldName,
                                    BytecodeEmitter& emitter,
                                    uint16_t resultOffset) {
    int off = FindFieldOffset(structDecl, fieldName);
    if (off < 0) {
        //Round-12: field offset not found — internal error after
        //type resolution should have caught this.
        throw std::runtime_error(
            "NLang backend: struct field offset not found: "
            + fieldName);
    }
    emitter.Emit(OpCode::OP_LoadField);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(static_cast<uint16_t>(off));
    EmitSealedFieldCopyIfNeeded(structDecl.FindField(fieldName),
                                emitter, resultOffset);
}

//Class field read tail: flattened OP_LoadField of `fieldName` into
//resultOffset (the receiver heap index is already emitted).
void VmBackend::EmitClassFieldLoad(SnClassDecl& classDecl,
                                   const std::string& fieldName,
                                   BytecodeEmitter& emitter,
                                   uint16_t resultOffset) {
    int off = FindClassFieldOffset(classDecl, fieldName);
    if (off < 0) {
        throw std::runtime_error(
            "NLang backend: class field offset not found: "
            + fieldName);
    }
    emitter.Emit(OpCode::OP_LoadField);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(static_cast<uint16_t>(off));
    EmitSealedFieldCopyIfNeeded(classDecl.FindField(fieldName),
                                emitter, resultOffset);
}

} //namespace nlang
