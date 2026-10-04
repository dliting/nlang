/*---
    EmitExprInitList.cpp — 集合初始化器发射：bare `[...]` / `new T{...}`
    的数组/List/Dict/类/struct 五路目标形态。
    从 EmitExprNew.cpp 拆出（2026-09-25 可维护性重构，零行为变化）。
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

    //Phase 8e-6: collection initializer `[...]` / `new T{...}`.
    //Dispatches on resolved EvalDataType:
    //  - Array (target->IsArrayType()): OP_AllocArray + per-element OP_StoreElement
    //  - List<T> (SnClassDecl, BaseName "List"): OP_New + per-entry OP_CallMethod "add" with boxing
    //  - Dict/Struct/Class: handled in Phase D (falls through to assert for now).
void VmBackend::Access(SnInitListExpr& expr) {
    NodeKind kind = expr.Kind();
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
    auto& initList = static_cast<SnInitListExpr&>(expr);
    SnField* pTarget = initList.EvalDataType();
    if (!pTarget) {
        //Round-12: init list without a resolved target type.
        throw std::runtime_error(
            "NLang backend: init list expression without a resolved target type");
    }

    //---- Array form: target is T[] ----
    if (initList.TargetIsArray()) {
        EmitInitListArray(initList, pTarget, emitter, resultOffset);
        return;
    }

    //---- List<T> form ----
    //pTarget is the resolved SnClassDecl for List<T> (generic
    //instantiation). Erasure: runtime class name is "List".
    if (pTarget->Kind() == NK_ClassDecl) {
        auto* pClassDecl = static_cast<SnClassDecl*>(pTarget);
        const std::string& baseName = pClassDecl->BaseName();
        if (baseName == "List") {
            EmitInitListListForm(initList, *pClassDecl, emitter, resultOffset);
            return;
        }
        if (baseName == "Dict") {
            //---- Dict<K,V> form ----
            EmitInitListDictForm(initList, *pClassDecl, emitter, resultOffset);
            return;
        }
        //---- User class init form: new ClassName{field1:v1, ...} ----
        EmitInitListClassForm(initList, *pClassDecl, emitter, resultOffset);
        return;
    }

    //---- Struct init form: new StructName{field1:v1, ...} ----
    if (pTarget->Kind() == NK_StructDecl) {
        auto* pStructDecl = static_cast<SnStructDecl*>(pTarget);
        EmitInitListStructForm(initList, *pStructDecl, emitter, resultOffset);
        return;
    }

    //Unknown target kind — no codegen path yet.
    assert(false && "NK_InitListExpr: unsupported target kind");
    return;
}

//Arm: array form (TargetIsArray). For `int[] arr = [..]`, the resolver set
//TargetIsArray=true and stored the ELEMENT type field in EvalDataType — the
//init-list is deliberately not tokenized (0.7.3 B): RegisterArrayType below
//consumes it as the element directly, and a token here would
//register "array of array".
void VmBackend::EmitInitListArray(SnInitListExpr& initList, SnField* pElemField,
                                  BytecodeEmitter& emitter,
                                  uint16_t resultOffset) {
    uint16_t arrayTypeIdx = RegisterArrayType(pElemField);
    int32_t n = static_cast<int32_t>(initList.Entries().size());

    //Alloc array with size = n.
    emitter.Emit(OpCode::OP_ConstInt32);
    emitter.EmitInt32(n);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(m_currFunc->tempSlot);
    emitter.Emit(OpCode::OP_AllocArray);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(arrayTypeIdx);
    emitter.EmitUint16(m_currFunc->tempSlot);

    //Stage entry values in an evalArea claim, never a temp: a
    //binary entry (`1 + use(make(5))`) parks its LEFT operand in
    //the staging slot, and a struct-by-value argument's deep-copy
    //scratch (EmitBinding, tempSlot) must not be able to touch it.
    //One claim spans the loop: each entry is consumed immediately
    //(StoreElement) and nested emissions claim fresh slots above.
    //ExprPeakDepth's InitListExpr case tracks claimSize=1.
    EvalAreaClaim valueClaim(*this, 1);
    uint16_t valueSlot = valueClaim.base();
    //Store each entry: arr[i] = entries[i].pValue.
    for (int32_t i = 0; i < n; ++i) {
        auto& entry = initList.Entries()[i];
        if (!entry.pValue) continue;
        EmitInitListArrayEntry(entry, i, pElemField, emitter,
                               resultOffset, valueSlot);
    }
}

//Array-form per-entry: emit the entry value into the staging slot
//(struct entries deep-copy first), then arr[entryIndex] = staged value.
void VmBackend::EmitInitListArrayEntry(const InitEntry& entry,
                                       int32_t entryIndex,
                                       SnField* pElemField,
                                       BytecodeEmitter& emitter,
                                       uint16_t resultOffset,
                                       uint16_t valueSlot) {
    EmitExpression(*entry.pValue, emitter, valueSlot);
    //Struct entries deep-copy before the store, mirroring
    //the subscript-store path: a bare store would alias
    //the source struct value (later mutation of the source
    //would change the stored element).
    if (RuntimeTypeKind(pElemField) == RTK_Struct && pElemField
        && pElemField->Kind() == NK_StructDecl) {
        //Per-unit: slot resolution covers cross-unit element structs
        //too (the old silent-0 fallback could not).
        EmitStructDeepCopy(valueSlot, valueSlot,
            static_cast<SnStructDecl&>(*pElemField), emitter);
    }
    //Index constant to callParamBase (avoids tempSlot/valueSlot).
    emitter.Emit(OpCode::OP_ConstInt32);
    emitter.EmitInt32(entryIndex);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.Emit(OpCode::OP_StoreElement);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.EmitUint16(valueSlot);
}

//Shared by the class-shaped init-list arms: OP_New into resultOffset,
//then the no-arg ctor call when the class declares one. classDecl feeds
//CtorSlotFor — a cross-unit placeholder carries no table ctor.
void VmBackend::EmitNewObjectAndNoArgCtor(SnClassDecl& classDecl,
                                          uint16_t classIdx,
                                          BytecodeEmitter& emitter,
                                          uint16_t resultOffset) {
    emitter.Emit(OpCode::OP_New);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(classIdx);
    uint16_t ctorIdx = CtorSlotFor(classDecl, classIdx);
    if (ctorIdx != 0xFFFF) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(resultOffset);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->callParamBase);
        emitter.Emit(OpCode::OP_CallMethodDirect);
        emitter.EmitUint16(ctorIdx);
        emitter.EmitUint16(m_currFunc->callParamBase);
        emitter.Emit(OpCode::OP_ParaEnd);
    }
}

//Arm: List<T> form — pTarget is the resolved SnClassDecl for List<T>
//(generic instantiation; erasure: runtime class name is "List").
//Allocates the List, calls its no-arg ctor when present, then per-entry
//"add" with boxing for primitive T.
void VmBackend::EmitInitListListForm(SnInitListExpr& initList,
                                     SnClassDecl& classDecl,
                                     BytecodeEmitter& emitter,
                                     uint16_t resultOffset) {
    int classIdx = m_compiledModule.FindClass("List");
    if (classIdx < 0) {
        //Round-12: List is a built-in class — always registered. (The
        //bare literal is the erasure key: builtins carry no owner tag.)
        throw std::runtime_error(
            "NLang backend: List class not registered in module");
    }
    EmitNewObjectAndNoArgCtor(classDecl, static_cast<uint16_t>(classIdx),
                              emitter, resultOffset);
    //Boxing plan for primitive T. An array-typed T is the
    //interned token — BoxingTagFor's default (not primitive)
    //flows raw handles, no box.
    const auto& typeArgs = classDecl.GenericTypeArgs();
    auto tBox = BoxingTagFor(typeArgs.empty() ? nullptr : typeArgs[0]);
    uint16_t addNameIdx = AddStringConstant("add");
    for (auto& entry : initList.Entries())
        EmitInitListEntryAdd(entry, tBox, addNameIdx, resultOffset, emitter);
}

//List-form per-entry: stage {this, value} in an evalArea claim, box a
//primitive value in place, then OP_CallMethod "add" via callParamBase.
void VmBackend::EmitInitListEntryAdd(const InitEntry& entry,
                                     BoxingTagResult tBox,
                                     uint16_t addNameIdx, uint16_t resultOffset,
                                     BytecodeEmitter& emitter) {
    if (!entry.pValue) return;
    //Phase 10 audit C2: stage entry values in evalArea via
    //EvalAreaClaim, immune to nested-call bulk-copies into
    //callParamBase. Pre-fix, the value was emitted straight to
    //callParamBase+1; a binary entry like `1 + helper(0,0,5)`
    //parked the left operand there and the call's arg bulk-copy
    //overwrote it (stored 5 instead of 6). Mirrors the Dict form
    //(Phase 9c follow-up). ExprPeakDepth's InitListExpr
    //case must track the matching claimSize=2.
    EvalAreaClaim claim(*this, 2);  // this, value
    uint16_t claimBase = claim.base();
    uint16_t valOff = claimBase + 1 * kFrameSlotBytes;
    EmitExpression(*entry.pValue, emitter, valOff);
    if (tBox.isPrimitive) {
        EmitPResultRefresh(emitter, valOff);
        emitter.Emit(OpCode::OP_Box);
        emitter.EmitByte(tBox.tag);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(valOff);
    }
    EmitInitListMethodCallTail(claimBase, 2, addNameIdx, resultOffset,
                               emitter);
}

//Arm: Dict<K,V> form — allocates the Dict, calls its no-arg ctor when
//present, then per-entry "set" with key/value boxing.
void VmBackend::EmitInitListDictForm(SnInitListExpr& initList,
                                     SnClassDecl& classDecl,
                                     BytecodeEmitter& emitter,
                                     uint16_t resultOffset) {
    int classIdx = m_compiledModule.FindClass("Dict");
    if (classIdx < 0) {
        //Round-12: Dict is a built-in class — always registered. (The
        //bare literal is the erasure key: builtins carry no owner tag.)
        throw std::runtime_error(
            "NLang backend: Dict class not registered in module");
    }
    EmitNewObjectAndNoArgCtor(classDecl, static_cast<uint16_t>(classIdx),
                              emitter, resultOffset);
    const auto& typeArgs = classDecl.GenericTypeArgs();
    //Array-typed K/V slots are interned tokens — raw handles,
    //no box (BoxingTagFor default).
    auto kBox = BoxingTagFor(typeArgs.empty() ? nullptr : typeArgs[0]);
    auto vBox = (typeArgs.size() > 1)
        ? BoxingTagFor(typeArgs[1]) : BoxingTagResult{0, false};
    uint16_t setNameIdx = AddStringConstant("set");
    for (auto& entry : initList.Entries())
        EmitInitListEntrySet(entry, kBox, vBox, setNameIdx, resultOffset,
                             emitter);
}

//Dict-form per-entry: stage {this, key, value} in an evalArea claim,
//box primitive key/value, then OP_CallMethod "set" via callParamBase.
void VmBackend::EmitInitListEntrySet(const InitEntry& entry,
                                     BoxingTagResult kBox,
                                     BoxingTagResult vBox, uint16_t setNameIdx,
                                     uint16_t resultOffset,
                                     BytecodeEmitter& emitter) {
    if (!entry.pValue) return;
    //Key: dict requires String key form. Identifier keys are
    //accepted for struct init only — for dict they would be
    //a resolver error. Value-only entries are also invalid.
    if (entry.keyKind != InitEntry::KeyKind::String
        && entry.keyKind != InitEntry::KeyKind::Identifier) {
        return;
    }
    //Phase 9c follow-up: route through EvalAreaClaim so the key
    //and value live in evalArea, immune to nested-call bulk-copies
    //to callParamBase. Pre-fix, a value like `helper(5,3)` would
    //bulk-copy to callParamBase[0..2], clobbering the key at [1].
    EvalAreaClaim claim(*this, 3);  // this, key, value
    uint16_t claimBase = claim.base();
    uint16_t keyOff = claimBase + 1 * kFrameSlotBytes;
    uint16_t valOff = claimBase + 2 * kFrameSlotBytes;
    uint16_t keyPoolIdx = AddStringConstant(entry.keyStr);
    emitter.Emit(OpCode::OP_ConstString);
    emitter.EmitUint16(keyPoolIdx);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(keyOff);
    if (kBox.isPrimitive) {
        emitter.Emit(OpCode::OP_Box);
        emitter.EmitByte(kBox.tag);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(keyOff);
    }
    //Value
    EmitExpression(*entry.pValue, emitter, valOff);
    if (vBox.isPrimitive) {
        EmitPResultRefresh(emitter, valOff);
        emitter.Emit(OpCode::OP_Box);
        emitter.EmitByte(vBox.tag);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(valOff);
    }
    EmitInitListMethodCallTail(claimBase, 3, setNameIdx, resultOffset,
                               emitter);
}

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
    EmitNewObjectAndNoArgCtor(classDecl, classIdx,
                              emitter, resultOffset);
    //Per-field store. Entry values stage in an evalArea claim,
    //never a temp — same clobber family as the array form
    //(binary LEFT operand parked in the staging slot vs. the
    //EmitBinding struct deep-copy scratch on tempSlot).
    //ExprPeakDepth's InitListExpr case tracks claimSize=1.
    EvalAreaClaim valueClaim(*this, 1);
    uint16_t valueSlot = valueClaim.base();
    for (auto& entry : initList.Entries()) {
        if (entry.keyKind != InitEntry::KeyKind::Identifier)
            continue;
        if (!entry.pValue) continue;
        int off = FindClassFieldOffset(classDecl, entry.keyStr);
        if (off < 0) continue;
        EmitExpression(*entry.pValue, emitter, valueSlot);
        emitter.Emit(OpCode::OP_StoreField);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(static_cast<uint16_t>(off));
        emitter.EmitUint16(valueSlot);
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
        if (entry.keyKind == InitEntry::KeyKind::Identifier) {
            off = FindFieldOffset(structDecl, entry.keyStr);
        } else if (ordinal < structDecl.Members().size()) {
            //Declaration-order positional entry: field ordinal i sits at
            //byte offset i * kHeapFieldStrideBytes under the uniform
            //2-cell heap field stride (Step A2).
            off = static_cast<int>(ordinal * kHeapFieldStrideBytes);
        }
        ++ordinal;
        if (off < 0) continue;
        EmitExpression(*entry.pValue, emitter, valueSlot);
        emitter.Emit(OpCode::OP_StoreField);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(static_cast<uint16_t>(off));
        emitter.EmitUint16(valueSlot);
    }
}

//Per-entry call tail shared by the List "add" and Dict "set" emitters:
//this (resultOffset) into claim slot 0, bulk-copy the claim to
//callParamBase, then OP_CallMethod methodName.
void VmBackend::EmitInitListMethodCallTail(uint16_t claimBase,
                                           uint16_t slotCount,
                                           uint16_t methodNameIdx,
                                           uint16_t resultOffset,
                                           BytecodeEmitter& emitter) {
    //this = resultOffset → claim[0]
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(resultOffset);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(claimBase);
    //Bulk-copy claim → callParamBase
    for (uint16_t i = 0; i < slotCount; ++i) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(claimBase + i * kFrameSlotBytes);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->callParamBase + i * kFrameSlotBytes);
    }
    emitter.Emit(OpCode::OP_CallMethod);
    emitter.EmitUint16(methodNameIdx);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.Emit(OpCode::OP_ParaEnd);
}
} //namespace nlang
