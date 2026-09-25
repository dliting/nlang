/*---
    VmBackendEmitExprNew.cpp — 构造表达式发射：new/new array/初始化列表。
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

void VmBackend::Access(SnNewExpr& expr) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
        auto& newExpr = static_cast<SnNewExpr&>(expr);
        auto* pClassDecl = newExpr.ClassDecl();
        if (!pClassDecl) {
            //Round-12: the resolver failed to bind a class declaration to
            //this new-expression. Codegen only runs when the front-end saw
            //no errors, so this is an internal invariant break.
            throw std::runtime_error(
                "NLang backend: new expression without a bound class declaration");
        }
        //Phase 8e-3: generic instantiations (List<int>) share one CompiledClass
        //named "List" at runtime (erasure). BaseName() returns the unqualified
        //"List" for generic instances, or the full name for ordinary classes.
        const std::string& className = pClassDecl->BaseName();
        int classIdx = m_compiledModule.FindClass(className);
        if (classIdx < 0) {
            //Round-12: the class was resolved by the front-end but never
            //registered in the compiled module. This means the class has
            //no compiled representation — fail the build instead of
            //silently emitting zero (which would make `new Foo()` return
            //null at runtime, a subtle wrong-code bug).
            throw std::runtime_error(
                "NLang backend: new expression for unregistered class: "
                + className);
        }
        uint16_t ctorIdx = m_compiledModule.classes[classIdx].constructorIdx;
        //Phase 9c follow-up: route ctor args through evalArea claim so
        //nested calls inside ctor args don't clobber each other (parallel
        //to EmitCallArgs). claim layout: [0]=this, [1..N]=args.
        uint16_t allocSlot = resultOffset;
        if (ctorIdx != 0xFFFF) {
            //Count actual ctor args. Args() is a view over ALL children and
            //its LAST element is the AddChild'ed class-name node (SnNewExpr
            //ctor appends it after the ctor args) — skip it by identity.
            //The resolver rejects named/out args and validates arity, so
            //every remaining entry is a positional value expression.
            size_t argCount = 0;
            for (auto& param : newExpr.Args()) {
                if (&param == newExpr.ClassName()) continue;
                ++argCount;
            }
            uint16_t n = static_cast<uint16_t>(1 + argCount);  //this + args

            EvalAreaClaim claim(*this, n);
            uint16_t claimBase = claim.base();

            //Emit args to claim[1..N]. Nested calls in args claim deeper
            //slices — they bulk-copy to callParamBase but that's fine;
            //our this/args live in evalArea, not callParamBase.
            uint16_t paramIdx = 1;
            for (auto& param : newExpr.Args()) {
                if (&param == newExpr.ClassName()) continue;
                uint16_t paramOffset = claimBase + paramIdx * VALUE_SIZE;
                EmitExpression(param, emitter, paramOffset);
                ++paramIdx;
            }
            //Allocate to resultOffset (no conflict with args — they're in evalArea).
            allocSlot = resultOffset;

            emitter.Emit(OpCode::OP_New);
            emitter.EmitUint16(allocSlot);
            emitter.EmitUint16(static_cast<uint16_t>(classIdx));

            //Copy this (new object heap index) to claim[0].
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(allocSlot);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(claimBase);

            //Bulk-copy evalArea claim → callParamBase just before the call.
            for (uint16_t i = 0; i < n; ++i) {
                emitter.Emit(OpCode::OP_VarLocal);
                emitter.EmitUint16(claimBase + i * VALUE_SIZE);
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(m_currFunc->callParamBase + i * VALUE_SIZE);
            }
            emitter.Emit(OpCode::OP_CallMethodDirect);
            emitter.EmitUint16(ctorIdx);
            emitter.EmitUint16(m_currFunc->callParamBase);
            emitter.Emit(OpCode::OP_ParaEnd);
            //claim releases on scope exit.
            return;
        }

        emitter.Emit(OpCode::OP_New);
        emitter.EmitUint16(allocSlot);
        emitter.EmitUint16(static_cast<uint16_t>(classIdx));
        return;
}

    // This expression - reads the implicit first parameter
void VmBackend::Access(SnNewArrayExpr& expr) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
        auto& newArr = static_cast<SnNewArrayExpr&>(expr);
        //Register the array type (idempotent)
        uint16_t arrayTypeIdx = RegisterArrayType(
            newArr.ElementType()->Field());
        //Size staging: EvalAreaClaim, never a temp. A temp staging slot
        //lets a nested binary (`1 + use(make(1))`) park its LEFT operand
        //there while a struct-by-value argument deep-copies through
        //tempSlot (EmitBinding CopyStruct scratch) — silently corrupting
        //the parked value. Claim slots are exclusive to this expression.
        //ExprPeakDepth's NewArrayExpr case tracks the claim=1.
        EvalAreaClaim sizeClaim(*this, 1);
        uint16_t sizeSlot = sizeClaim.base();
        EmitExpression(*newArr.Size(), emitter, sizeSlot);
        emitter.Emit(OpCode::OP_AllocArray);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(arrayTypeIdx);
        emitter.EmitUint16(sizeSlot);
        return;
}

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
        //For `int[] arr = [..]`, the resolver set TargetIsArray=true and
        //stored the ELEMENT type field in EvalDataType — the init-list is
        //deliberately not tokenized (0.7.3 B): RegisterArrayType below
        //consumes it as the element directly, and a token here would
        //register "array of array".
        if (initList.TargetIsArray()) {
            SnField* pElemField = pTarget;
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
                EmitExpression(*entry.pValue, emitter, valueSlot);
                //Struct entries deep-copy before the store, mirroring
                //the subscript-store path: a bare store would alias
                //the source struct value (later mutation of the source
                //would change the stored element).
                if (RuntimeTypeKind(pElemField) == RTK_Struct) {
                    int structIdx =
                        m_compiledModule.FindStruct(pElemField->Name());
                    emitter.Emit(OpCode::OP_CopyStruct);
                    emitter.EmitUint16(valueSlot);
                    emitter.EmitUint16(valueSlot);
                    emitter.EmitUint16(structIdx >= 0
                        ? static_cast<uint16_t>(structIdx) : 0);
                }
                //Index constant to callParamBase (avoids tempSlot/valueSlot).
                emitter.Emit(OpCode::OP_ConstInt32);
                emitter.EmitInt32(i);
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(m_currFunc->callParamBase);
                emitter.Emit(OpCode::OP_StoreElement);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(m_currFunc->callParamBase);
                emitter.EmitUint16(valueSlot);
            }
            return;
        }

        //---- List<T> form ----
        //pTarget is the resolved SnClassDecl for List<T> (generic
        //instantiation). Erasure: runtime class name is "List".
        if (pTarget->Kind() == NK_ClassDecl) {
            auto* pClassDecl = static_cast<SnClassDecl*>(pTarget);
            const std::string& baseName = pClassDecl->BaseName();
            if (baseName == "List") {
                int classIdx = m_compiledModule.FindClass("List");
                if (classIdx < 0) {
                    //Round-12: List is a built-in class — always registered.
                    throw std::runtime_error(
                        "NLang backend: List class not registered in module");
                }
                //Allocate List instance.
                emitter.Emit(OpCode::OP_New);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(static_cast<uint16_t>(classIdx));
                //Call no-arg ctor if present.
                uint16_t ctorIdx = m_compiledModule.classes[classIdx].constructorIdx;
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
                //Boxing plan for primitive T. An array-typed T is the
                //interned token — BoxingTagFor's default (not primitive)
                //flows raw handles, no box.
                const auto& typeArgs = pClassDecl->GenericTypeArgs();
                auto t = BoxingTagFor(
                    typeArgs.empty() ? nullptr : typeArgs[0]);
                uint16_t addNameIdx = AddStringConstant("add");
                //Phase 10 audit C2: stage entry values in evalArea via
                //EvalAreaClaim, immune to nested-call bulk-copies into
                //callParamBase. Pre-fix, the value was emitted straight to
                //callParamBase+1; a binary entry like `1 + helper(0,0,5)`
                //parked the left operand there and the call's arg bulk-copy
                //overwrote it (stored 5 instead of 6). Mirrors the Dict form
                //below (Phase 9c follow-up). ExprPeakDepth's InitListExpr
                //case must track the matching claimSize=2.
                for (auto& entry : initList.Entries()) {
                    if (!entry.pValue) continue;
                    EvalAreaClaim claim(*this, 2);  // this, value
                    uint16_t claimBase = claim.base();
                    uint16_t valOff = claimBase + 1 * VALUE_SIZE;
                    EmitExpression(*entry.pValue, emitter, valOff);
                    if (t.isPrimitive) {
                        EmitPResultRefresh(emitter, valOff);
                        emitter.Emit(OpCode::OP_Box);
                        emitter.EmitByte(t.tag);
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(valOff);
                    }
                    //this = resultOffset → claim[0]
                    emitter.Emit(OpCode::OP_VarLocal);
                    emitter.EmitUint16(resultOffset);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(claimBase);
                    //Bulk-copy claim → callParamBase
                    for (uint16_t i = 0; i < 2; ++i) {
                        emitter.Emit(OpCode::OP_VarLocal);
                        emitter.EmitUint16(claimBase + i * VALUE_SIZE);
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(m_currFunc->callParamBase + i * VALUE_SIZE);
                    }
                    emitter.Emit(OpCode::OP_CallMethod);
                    emitter.EmitUint16(addNameIdx);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                    emitter.Emit(OpCode::OP_ParaEnd);
                }
                return;
            }
            if (baseName == "Dict") {
                //---- Dict<K,V> form ----
                int classIdx = m_compiledModule.FindClass("Dict");
                if (classIdx < 0) {
                    //Round-12: Dict is a built-in class — always registered.
                    throw std::runtime_error(
                        "NLang backend: Dict class not registered in module");
                }
                emitter.Emit(OpCode::OP_New);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(static_cast<uint16_t>(classIdx));
                uint16_t ctorIdx = m_compiledModule.classes[classIdx].constructorIdx;
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
                const auto& typeArgs = pClassDecl->GenericTypeArgs();
                //Array-typed K/V slots are interned tokens — raw handles,
                //no box (BoxingTagFor default).
                auto kBox = BoxingTagFor(
                    typeArgs.empty() ? nullptr : typeArgs[0]);
                auto vBox = (typeArgs.size() > 1)
                    ? BoxingTagFor(typeArgs[1]) : BoxingTagResult{0, false};
                uint16_t setNameIdx = AddStringConstant("set");
                //Phase 9c follow-up: route through EvalAreaClaim so the key
                //and value live in evalArea, immune to nested-call bulk-copies
                //to callParamBase. Pre-fix, a value like `helper(5,3)` would
                //bulk-copy to callParamBase[0..2], clobbering the key at [1].
                for (auto& entry : initList.Entries()) {
                    if (!entry.pValue) continue;
                    //Key: dict requires String key form. Identifier keys are
                    //accepted for struct init only — for dict they would be
                    //a resolver error. Value-only entries are also invalid.
                    if (entry.keyKind != InitEntry::KeyKind::String
                        && entry.keyKind != InitEntry::KeyKind::Identifier) {
                        continue;
                    }
                    EvalAreaClaim claim(*this, 3);  // this, key, value
                    uint16_t claimBase = claim.base();
                    uint16_t keyOff = claimBase + 1 * VALUE_SIZE;
                    uint16_t valOff = claimBase + 2 * VALUE_SIZE;
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
                    //this = resultOffset → claim[0]
                    emitter.Emit(OpCode::OP_VarLocal);
                    emitter.EmitUint16(resultOffset);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(claimBase);
                    //Bulk-copy claim → callParamBase
                    for (uint16_t i = 0; i < 3; ++i) {
                        emitter.Emit(OpCode::OP_VarLocal);
                        emitter.EmitUint16(claimBase + i * VALUE_SIZE);
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(m_currFunc->callParamBase + i * VALUE_SIZE);
                    }
                    emitter.Emit(OpCode::OP_CallMethod);
                    emitter.EmitUint16(setNameIdx);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                    emitter.Emit(OpCode::OP_ParaEnd);
                }
                return;
            }
            //---- User class init form: new ClassName{field1:v1, ...} ----
            //Allocate via OP_New, then per-field OP_StoreField. No-arg ctor
            //is invoked if present.
            {
                const std::string& className = pClassDecl->BaseName().empty()
                    ? pClassDecl->Name() : pClassDecl->BaseName();
                int classIdx = m_compiledModule.FindClass(className);
                if (classIdx < 0) {
                    //Round-12: class was resolved but not registered.
                    throw std::runtime_error(
                        "NLang backend: init-list class not registered: "
                        + className);
                }
                emitter.Emit(OpCode::OP_New);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(static_cast<uint16_t>(classIdx));
                uint16_t ctorIdx = m_compiledModule.classes[classIdx].constructorIdx;
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
                    int off = FindClassFieldOffset(*pClassDecl, entry.keyStr);
                    if (off < 0) continue;
                    EmitExpression(*entry.pValue, emitter, valueSlot);
                    emitter.Emit(OpCode::OP_StoreField);
                    emitter.EmitUint16(resultOffset);
                    emitter.EmitUint16(static_cast<uint16_t>(off));
                    emitter.EmitUint16(valueSlot);
                }
                return;
            }
        }

        //---- Struct init form: new StructName{field1:v1, ...} ----
        //Allocate a fresh struct via OP_AllocStruct, then per-field OP_StoreField
        //using the struct-specific FindFieldOffset (linear scan, no inheritance).
        if (pTarget->Kind() == NK_StructDecl) {
            auto* pStructDecl = static_cast<SnStructDecl*>(pTarget);
            const std::string& structName = pStructDecl->Name();
            int structIdx = m_compiledModule.FindStruct(structName);
            if (structIdx < 0) {
                //Round-12: struct was resolved but not registered.
                throw std::runtime_error(
                    "NLang backend: init-list struct not registered: "
                    + structName);
            }
            auto& cs = m_compiledModule.structs[structIdx];
            emitter.Emit(OpCode::OP_AllocStruct);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(static_cast<uint16_t>(structIdx));
            emitter.EmitUint16(cs.fieldCount);
            //Per-field store. FindFieldOffset returns byte offset within the
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
            EvalAreaClaim valueClaim(*this, 1);
            uint16_t valueSlot = valueClaim.base();
            size_t ordinal = 0;
            for (auto& entry : initList.Entries()) {
                if (!entry.pValue) continue;
                int off = -1;
                if (entry.keyKind == InitEntry::KeyKind::Identifier) {
                    off = FindFieldOffset(*pStructDecl, entry.keyStr);
                } else if (ordinal < pStructDecl->Members().size()) {
                    off = static_cast<int>(ordinal * VALUE_SIZE);
                }
                ++ordinal;
                if (off < 0) continue;
                EmitExpression(*entry.pValue, emitter, valueSlot);
                emitter.Emit(OpCode::OP_StoreField);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(static_cast<uint16_t>(off));
                emitter.EmitUint16(valueSlot);
            }
            return;
        }

        //Unknown target kind — no codegen path yet.
        assert(false && "NK_InitListExpr: unsupported target kind");
        return;
}

    // Subscript expression: arr[index]

} //namespace nlang
