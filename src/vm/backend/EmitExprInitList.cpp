/*---
    EmitExprInitList.cpp — 集合初始化器发射：bare `[...]` / `new T{...}`
    的数组(ND见EmitExprInitListND)/List/Dict/类/struct 目标形态。
---*/
#include "VmBackend.h"
#include <nlang/compiler/BuildEnvironment.h>
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

namespace nlang {

    //Phase 8e-6: collection initializer dispatch on resolved
    //EvalDataType — array token (rank-1 here, ND in the ND file) /
    //List "add" / Dict "set" / Channel / user class / struct.
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

    //---- Array form (0.9.13 D-new-1: the interned token) ----
    if (initList.IsArrayValued()) {
        //Stage-2 task-5: rank>=2 targets take the rectangular-literal
        //lowering (the resolver validated the shape and stamped it).
        if (pTarget && pTarget->Kind() == NK_ArrayTypeToken
            && static_cast<SnArrayTypeToken*>(pTarget)->Rank() >= 2) {
            EmitInitListArrayND(initList,
                *static_cast<SnArrayTypeToken*>(pTarget), emitter,
                resultOffset);
            return;
        }
        EmitInitListArray(initList, pTarget, emitter, resultOffset);
        return;
    }

    //---- Class-shaped forms dispatch on the base name ----
    if (pTarget->Kind() == NK_ClassDecl) {
        EmitInitListClassTarget(initList,
                                *static_cast<SnClassDecl*>(pTarget),
                                emitter, resultOffset);
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

//Arm: array form (IsArrayValued). For `int[] arr = [..]`, the resolver
//stored the interned array token in EvalDataType (0.9.13 D-new-1):
//RegisterArrayType below consumes the ELEMENT, peeled here — passing the
//token itself would register "array of array".
void VmBackend::EmitInitListArray(SnInitListExpr& initList, SnField* pArrayToken,
                                  BytecodeEmitter& emitter,
                                  uint16_t resultOffset) {
    //Peel the element once: both the type registration and the per-entry
    //struct deep-copy gate below consume the element, never the token.
    SnField* pElemField =
        static_cast<SnArrayTypeToken*>(pArrayToken)->ElemTypeOf();
    uint16_t arrayTypeIdx = RegisterArrayType(pElemField);
    int32_t n = static_cast<int32_t>(initList.Entries().size());

    //Alloc array with size = n.
    emitter.Emit(OpCode::OP_ConstInt32);
    emitter.EmitInt32(n);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(m_currFunc->tempSlot);
    emitter.Emit(OpCode::OP_AllocArrayN);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(arrayTypeIdx);
    emitter.EmitUint16(1);  // rank
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
    emitter.Emit(OpCode::OP_StoreElementN);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.EmitUint16(valueSlot);
    emitter.EmitUint16(1);  // rank — initializer targets are rank-1 here
}

//Shared by the class-shaped init-list arms: OP_New into resultOffset,
//then the no-arg ctor call when one is available. 0.9.11 T6 (D10):
//pCtorFn is the zero-formal ctor the resolver selected for the init
//list (CheckClassInitListForm) — registered → its function slot; null
//(List/Dict arms, or a class whose ctor comes only from the table) →
//classDecl feeds CtorSlotFor, a cross-unit placeholder carries no table
//ctor; a selected-but-unregistered ctor allocates without a call.
//pExtraConstSlot: a Channel ctor stub is (this, capacity) and the frame
//push copies stub-paramCount cells from the caller's callParamBase, so
//the no-arg init-list form stages an explicit constant (0 = unbounded)
//into the given slot — an unstaged slot would copy garbage.
void VmBackend::EmitNewObjectAndNoArgCtor(SnClassDecl& classDecl,
                                          SnFunction* pCtorFn,
                                          uint16_t classIdx,
                                          BytecodeEmitter& emitter,
                                          uint16_t resultOffset,
                                          const int32_t* pExtraConstSlot) {
    emitter.Emit(OpCode::OP_New);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(classIdx);
    uint16_t ctorIdx = 0xFFFF;
    if (SnFunction* pRegistered = RegisteredCtor(pCtorFn))
        ctorIdx = static_cast<uint16_t>(FunctionSlotFor(*pRegistered));
    else if (!pCtorFn)
        ctorIdx = CtorSlotFor(classDecl, classIdx);
    if (ctorIdx != 0xFFFF) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(resultOffset);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->callParamBase);
        if (pExtraConstSlot) {
            //Full-cell stage: OP_ConstInt32 writes 4 bytes but OP_Assign
            //copies the whole 8-byte slot — zero the cell first so no
            //stale upper half rides into the capacity.
            emitter.Emit(OpCode::OP_ConstZero);
            emitter.Emit(OpCode::OP_ConstInt32);
            emitter.EmitInt32(*pExtraConstSlot);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(static_cast<uint16_t>(
                m_currFunc->callParamBase + kFrameSlotBytes));
        }
        emitter.Emit(OpCode::OP_CallMethodDirect);
        emitter.EmitUint16(ctorIdx);
        emitter.EmitUint16(m_currFunc->callParamBase);
        emitter.Emit(OpCode::OP_ParaEnd);
    }
}

//Class-shaped arm dispatcher: the List<T>/Dict<K,V>/Channel<T> generic
//instantiations by base name (erasure — the runtime class is the bare
//name); every other class-shaped target is the user class init form.
void VmBackend::EmitInitListClassTarget(SnInitListExpr& initList,
                                        SnClassDecl& classDecl,
                                        BytecodeEmitter& emitter,
                                        uint16_t resultOffset) {
    const std::string& baseName = classDecl.BaseName();
    if (baseName == kBuiltinListTypeName) {
        EmitInitListListForm(initList, classDecl, emitter, resultOffset);
        return;
    }
    if (baseName == kBuiltinDictTypeName) {
        EmitInitListDictForm(initList, classDecl, emitter, resultOffset);
        return;
    }
    if (baseName == kBuiltinChannelTypeName) {
        //Channel<T> (K1 Slice 9): construction only — the resolver
        //rejects any entry (values enter through send()).
        EmitInitListChannelForm(initList, classDecl, emitter, resultOffset);
        return;
    }
    //User class init form: new ClassName{field1:v1, ...}
    EmitInitListClassForm(initList, classDecl, emitter, resultOffset);
}

//Arm: List<T> form — pTarget is the resolved SnClassDecl for List<T>
//(generic instantiation; erasure: runtime class name is "List").
//Allocates the List, calls its no-arg ctor when present, then per-entry
//"add" with boxing for primitive T.
void VmBackend::EmitInitListListForm(SnInitListExpr& initList,
                                     SnClassDecl& classDecl,
                                     BytecodeEmitter& emitter,
                                     uint16_t resultOffset) {
    int classIdx = m_compiledModule.FindClass(kBuiltinListTypeName);
    if (classIdx < 0) {
        //Round-12: List is a built-in class — always registered. (The
        //bare literal is the erasure key: builtins carry no owner tag.)
        throw std::runtime_error(
            "NLang backend: List class not registered in module");
    }
    //The List ctor is the builtin table entry — no AST selection.
    EmitNewObjectAndNoArgCtor(classDecl, nullptr,
                              static_cast<uint16_t>(classIdx),
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

//0.10.10: stage one entry slot (a Dict key or a List/Dict value) and
//box it in place when its declared kind is primitive — the pResult
//refresh mirrors the assignment path: the box must read the staged
//slot, not a stale accumulator left behind by the expression emission.
void VmBackend::EmitInitListStageBoxed(SnExpression& expr, uint16_t off,
                                       const BoxingTagResult& box,
                                       BytecodeEmitter& emitter) {
    EmitExpression(expr, emitter, off);
    if (box.isPrimitive) {
        EmitPResultRefresh(emitter, off);
        emitter.Emit(OpCode::OP_Box);
        emitter.EmitByte(box.tag);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(off);
    }
}

//List-form per-entry: stage {this, value} in an evalArea claim, box a
//primitive value in place, then OP_CallMethod "add" via callParamBase.
//The entry key is meaningless here — the resolver guarantees List
//entries are positional (RejectInitListEntryKeyForm).
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
    EmitInitListStageBoxed(*entry.pValue, valOff, tBox, emitter);
    EmitInitListMethodCallTail(claimBase, 2, addNameIdx, resultOffset,
                               emitter);
}

//Arm: Dict<K,V> form — allocates the Dict, calls its no-arg ctor when
//present, then per-entry "set" with key/value boxing.
void VmBackend::EmitInitListDictForm(SnInitListExpr& initList,
                                     SnClassDecl& classDecl,
                                     BytecodeEmitter& emitter,
                                     uint16_t resultOffset) {
    int classIdx = m_compiledModule.FindClass(kBuiltinDictTypeName);
    if (classIdx < 0) {
        //Round-12: Dict is a built-in class — always registered. (The
        //bare literal is the erasure key: builtins carry no owner tag.)
        throw std::runtime_error(
            "NLang backend: Dict class not registered in module");
    }
    //The Dict ctor is the builtin table entry — no AST selection.
    EmitNewObjectAndNoArgCtor(classDecl, nullptr,
                              static_cast<uint16_t>(classIdx),
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
//0.10.10: the key is an expression like the value — the resolver
//guarantees every Dict entry carries one (RejectInitListEntryKeyForm);
//the key-slot cast already adjudicated it against K.
void VmBackend::EmitInitListEntrySet(const InitEntry& entry,
                                     BoxingTagResult kBox,
                                     BoxingTagResult vBox, uint16_t setNameIdx,
                                     uint16_t resultOffset,
                                     BytecodeEmitter& emitter) {
    if (!entry.pValue || !entry.pKey) return;
    //Phase 9c follow-up: route through EvalAreaClaim so the key
    //and value live in evalArea, immune to nested-call bulk-copies
    //to callParamBase. Pre-fix, a value like `helper(5,3)` would
    //bulk-copy to callParamBase[0..2], clobbering the key at [1].
    EvalAreaClaim claim(*this, 3);  // this, key, value
    uint16_t claimBase = claim.base();
    uint16_t keyOff = claimBase + 1 * kFrameSlotBytes;
    uint16_t valOff = claimBase + 2 * kFrameSlotBytes;
    //Key
    EmitInitListStageBoxed(*entry.pKey, keyOff, kBox, emitter);
    //Value
    EmitInitListStageBoxed(*entry.pValue, valOff, vBox, emitter);
    EmitInitListMethodCallTail(claimBase, 3, setNameIdx, resultOffset,
                               emitter);
}

//Arm: Channel<T> form (K1 Slice 9) — allocate the erased "Channel"
//instance and run its constructor (the side-table slot materializer).
//The resolver guarantees an empty entry list, so there is no per-entry
//emission (unlike the List/Dict forms).
void VmBackend::EmitInitListChannelForm(SnInitListExpr& initList,
                                        SnClassDecl& classDecl,
                                        BytecodeEmitter& emitter,
                                        uint16_t resultOffset) {
    (void)initList;
    int classIdx = m_compiledModule.FindClass(kBuiltinChannelTypeName);
    if (classIdx < 0) {
        //Round-12 shape: Channel is a built-in class — always registered
        //(the bare literal is the erasure key).
        throw std::runtime_error(
            "NLang backend: Channel class not registered in module");
    }
    //Built-in target: the resolver's IsBuiltinClass early-return keeps
    //CtorFunction() null, so the table path (CtorSlotFor) is the ctor
    //source — pass a null selected-ctor to take it (0.9.11 T6 merge).
    EmitNewObjectAndNoArgCtor(classDecl, /*pCtorFn=*/nullptr,
                              static_cast<uint16_t>(classIdx),
                              emitter, resultOffset,
                              //K3 task 4: the ctor stub is (this, capacity)
                              //— stage the unbounded default explicitly.
                              /*pExtraConstSlot=*/&nlang::kChannelUnboundedCapacity);
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
        int off = FindClassFieldOffset(classDecl,
            static_cast<SnIdentifierExpr*>(entry.pKey)->Name());
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
        if (KeyIsIdentifier(entry)) {
            off = FindFieldOffset(structDecl,
                static_cast<SnIdentifierExpr*>(entry.pKey)->Name());
        } else if (ordinal < structDecl.Fields().size()) {
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
