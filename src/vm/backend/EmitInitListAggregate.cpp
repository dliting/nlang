/*---
    EmitInitListAggregate.cpp - user aggregate initializer emission:
    the class/struct brace target forms (allocate + per-field fill).
    Split from EmitExprInitList.cpp (v0.12.6 D14 source-size guard
    split, zero behavior change).
---*/
#include "VmBackend.h"
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnExtraTypes.h>
#include <nlang/runtime/NodeConsts.h>
#include <string>

namespace nlang {

//Arm: user class init form — allocate via OP_New, then per-field
//OP_StoreField. No-arg ctor is invoked if present.
void VmBackend::EmitInitListClassForm(SnInitListExpr& initList,
                                      SnClassDecl& classDecl,
                                      BytecodeEmitter& emitter,
                                      uint16_t resultOffset) {
    //Per-unit: own/builtin classes resolve through the table lookup;
    //a cross-unit class becomes an import placeholder slot.
    const uint16_t classIdx = static_cast<uint16_t>(
        ClassSlotFor(classDecl));
    //0.9.11 T6: the resolver picked the zero-formal ctor for the init
    //list (CheckClassInitListForm); null keeps the table path.
    EmitNewObjectAndNoArgCtor(classDecl, initList.CtorFunction(), classIdx,
                              emitter, resultOffset);
    //Per-field store. Entry values stage in an evalArea claim,
    //never a temp — same clobber family as the array form
    //(binary LEFT operand parked in the staging slot vs. the
    //EmitBinding struct deep-copy scratch on tempSlot).
    //ExprPeakDepth's InitListExpr case tracks claimSize=1.
    EvalAreaClaim valueClaim(*this, 1);
    uint16_t valueSlot = valueClaim.base();
    for (auto& entry : initList.Entries()) {
        if (!KeyIsIdentifier(entry))
            continue;
        if (!entry.pValue) continue;
        const std::string keyName =
            static_cast<SnIdentifierExpr*>(entry.pKey)->Name();
        int off = FindClassFieldOffset(classDecl, keyName);
        if (off < 0) continue;
        EmitExpression(*entry.pValue, emitter, valueSlot);
        //v0.12.7: boundary rank check on the staged value.
        {
            auto* pFillDecl = FindClassFieldDecl(classDecl, keyName);
            EmitArrayRankBoundary(
                pFillDecl ? pFillDecl->EvalDataType() : nullptr,
                entry.pValue, valueSlot, emitter);
            //v0.12.6 D14: a struct value filling a field is deep-copied —
            //the brace fill is a field assignment (manual 4.7.4/4.7.5).
            EmitStructAwareStoreField(pFillDecl,
                resultOffset, static_cast<uint16_t>(off), valueSlot,
                emitter);
        }
    }
}

//Arm: struct init form — allocate a fresh struct via OP_AllocStruct,
//then per-field OP_StoreField using the struct-specific FindFieldOffset
//(linear scan, no inheritance).
void VmBackend::EmitInitListStructForm(SnInitListExpr& initList,
                                       SnStructDecl& structDecl,
                                       BytecodeEmitter& emitter,
                                       uint16_t resultOffset) {
    //Per-unit: a cross-unit struct slots as an import placeholder
    //(StructSlotFor). The fieldCount operand comes from the AST — a
    //placeholder record carries no metadata (the executor allocates
    //from the linked table; the operand is a disassembly aid,
    //identical to the table for own entries).
    const uint16_t structIdx = static_cast<uint16_t>(
        StructSlotFor(structDecl));
    emitter.Emit(OpCode::OP_AllocStruct);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(structIdx);
    emitter.EmitUint16(static_cast<uint16_t>(structDecl.FieldCount()));
    EmitInitListStructEntries(initList, structDecl, emitter, resultOffset);
}

//Struct-form per-field store. FindFieldOffset returns byte offset within the
//struct's data area (no classIdx slot like classes have).
//Identifier keys address by name; value-only entries (the
//`{ v1, v2 }` form) fill fields in DECLARATION ORDER — skipping
//them silently zeroed every field (Phase 8e-6 declared
//declaration-order support; the arg-position probe
//`sum(new Point{ 3 })` exposed it).
//Entry values stage in an evalArea claim, never a temp (same
//clobber family as the array/class forms above). The struct
//branch of a plain assign happens to route through tempSlot2
//today, which dodges the scratch by accident — the claim makes
//that independence a guarantee instead of luck.
//ExprPeakDepth's InitListExpr case tracks claimSize=1.
void VmBackend::EmitInitListStructEntries(SnInitListExpr& initList,
                                          SnStructDecl& structDecl,
                                          BytecodeEmitter& emitter,
                                          uint16_t resultOffset) {
    EvalAreaClaim valueClaim(*this, 1);
    uint16_t valueSlot = valueClaim.base();
    size_t ordinal = 0;
    for (auto& entry : initList.Entries()) {
        if (!entry.pValue) continue;
        int off = -1;
        SnField* fieldType = nullptr;
        if (KeyIsIdentifier(entry)) {
            const std::string keyName =
                static_cast<SnIdentifierExpr*>(entry.pKey)->Name();
            off = FindFieldOffset(structDecl, keyName);
            for (auto& sf : structDecl.Fields()) {
                if (sf.Name() == keyName) {
                    fieldType = sf.EvalDataType();
                    break;
                }
            }
        } else if (ordinal < structDecl.Fields().size()) {
            //Declaration-order positional entry: field ordinal i sits at
            //byte offset i * kHeapFieldStrideBytes under the uniform
            //2-cell heap field stride (Step A2).
            off = static_cast<int>(ordinal * kHeapFieldStrideBytes);
            size_t fi = 0;
            for (auto& sf : structDecl.Fields()) {
                if (fi == ordinal) {
                    fieldType = sf.EvalDataType();
                    break;
                }
                ++fi;
            }
        }
        ++ordinal;
        if (off < 0) continue;
        EmitExpression(*entry.pValue, emitter, valueSlot);
        //v0.12.7: boundary rank check on the staged value.
        EmitArrayRankBoundary(fieldType, entry.pValue, valueSlot, emitter);
        //v0.12.6 D14: a struct value filling a field is deep-copied —
        //the brace fill is a field assignment (manual 4.7.4/4.7.5).
        EmitStructAwareStoreField(fieldType, resultOffset,
            static_cast<uint16_t>(off), valueSlot, emitter);
    }
}

} //namespace nlang
