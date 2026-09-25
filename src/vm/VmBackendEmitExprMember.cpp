/*---
    VmBackendEmitExprMember.cpp — 成员表达式发射：字段读取、内建属性、方法调用与容器语法糖（含成员发射专用 static 辅助族）。
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

int VmBackend::FindFieldOffset(SnStructDecl& structDecl, const std::string& fieldName) {
    uint16_t off = 0;
    for (auto& sf : structDecl.Members()) {
        if (sf.Name() == fieldName)
            return off;
        off += VALUE_SIZE;
    }
    return -1;
}

//Returns class field offset in bytes (including +4 for classIdx slot), or -1.
//Object layout: [classIdx, root_ancestor_fields..., parent_fields..., own_fields...]
//Slot[0] = classIdx (4 bytes, used for runtime virtual dispatch via OP_CallMethod).
//Slot[1..N] = data fields (4 bytes each, offset starts at VALUE_SIZE=4).
//Compile-time offset here must match runtime AllocClassOnHeap in VmExecutor.
int VmBackend::FindClassFieldOffset(SnClassDecl& classDecl, const std::string& fieldName) {
    //Phase 9d: built-in Exception classes expose message/backtrace fields
    //that aren't materialized as SnClassDecl members (the synthetic class
    //decl has empty Members()). Runtime layout (VmBackend::RegisterBuiltinClasses):
    //  slot[1] = message  → offset 4
    //  slot[2] = backtrace → offset 8
    //Walk SuperClass chain so user subclasses of Exception also resolve.
    auto isBuiltinExceptionName = [](const std::string& cn) {
        return cn == "Exception" || cn == "NullPointerException"
            || cn == "DivByZeroException" || cn == "IndexOutOfBoundsException"
            || cn == "AssertionException" || cn == "IOException";
    };
    //Phase 9d: direct built-in Exception class — synthetic decl has no
    //Members(); the 2 runtime fields are fixed at slot[1]/slot[2].
    if (classDecl.IsBuiltinClass()
        && isBuiltinExceptionName(classDecl.Name())) {
        if (fieldName == "message")  return VALUE_SIZE;
        if (fieldName == "backtrace") return 2 * VALUE_SIZE;
        return -1;
    }
    //Collect ancestor chain from root to direct parent
    std::vector<SnClassDecl*> ancestors;
    auto* pSuper = classDecl.SuperClass();
    while (pSuper) {
        ancestors.push_back(pSuper);
        pSuper = pSuper->SuperClass();
    }
    //Search from root ancestor to direct parent (reversed order)
    uint16_t off = VALUE_SIZE; //skip classIdx slot
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        //Phase 9d: a built-in Exception ancestor contributes 2 runtime
        //fields (message, backtrace) that aren't in the synthetic
        //Members() list — RegisterClasses injects them into the compiled
        //field list, so account for them here.
        if ((*it)->IsBuiltinClass()
            && isBuiltinExceptionName((*it)->Name())) {
            if (fieldName == "message")  return off;
            if (fieldName == "backtrace") return off + VALUE_SIZE;
            off += 2 * VALUE_SIZE;
            continue;
        }
        for (auto& member : (*it)->Members()) {
            if (member.Kind() == NK_ClassField && member.Name() == fieldName)
                return off;
            if (member.Kind() == NK_ClassField)
                off += VALUE_SIZE;
        }
    }
    //Search in this class (after all parent fields)
    for (auto& member : classDecl.Members()) {
        if (member.Kind() == NK_ClassField && member.Name() == fieldName)
            return off;
        if (member.Kind() == NK_ClassField)
            off += VALUE_SIZE;
    }
    return -1;
}

//Bare identifier that resolved to a class member (implicit this.field,
//e.g. `v = 1;` inside a method): returns the SnClassDecl that owns the
//field node. For inherited fields the field node belongs to the ancestor
//decl, and FindClassFieldOffset walking from that owner yields the same
//flattened offset as from any subclass. Returns null when pField is not
//a class data member (locals, formals, enum members).
SnClassDecl* VmBackend::OwningClassOfMemberField(SnField* pField)
{
    if (!pField || pField->Kind() != NK_ClassField)
        return nullptr;
    auto* pParent = pField->Parent();
    if (pParent && pParent->Kind() == NK_ClassDecl)
        return static_cast<SnClassDecl*>(pParent);
    return nullptr;
}

void VmBackend::Access(SnMemberExpr& expr) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
        auto& member = static_cast<SnMemberExpr&>(expr);
        //Phase 11: namespace-qualified stdlib call (math.sqrt(x)). Must be
        //dispatched before anything that needs member.Field() — the
        //resolver marks these resolved without a field, and the outer
        //identifier (the namespace name) is never emitted at all.
        {
            auto* outer = member.Outer();
            auto* inner = member.Inner();
            if (outer && outer->Kind() == NK_IdentifierExpr
                && inner && inner->Kind() == NK_InvokeExpr)
            {
                auto& outerId = static_cast<SnIdentifierExpr&>(*outer);
                if (IsStdLibNamespaceName(outerId.Name()))
                {
                    auto& invoke = static_cast<SnInvokeExpr&>(*inner);
                    const StdLibEntry* pEntry = FindStdLibFunction(
                        outerId.Name(), invoke.CalleeName());
                    //Resolver guarantees a hit here (unknown functions are
                    //compile errors); fall through defensively if not.
                    if (pEntry)
                    {
                        EmitStdLibCall(*pEntry, invoke, emitter, resultOffset);
                        return;
                    }
                }
            }
        }
        auto* field = member.Field();
        if (field && field->Kind() == NK_EnumMember) {
            //Enum member constant (e.g. Color.Red).
            auto* pEnumMember = static_cast<SnEnumMember*>(field);
            emitter.Emit(OpCode::OP_ConstInt32);
            emitter.EmitInt32(pEnumMember->Value());
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return;
        }
        //Phase 13 Step 2: bound method reference (c.foo / o.run in a
        //value position) — the resolver bound the member's EvalDataType
        //to the Func declaration while Field() stayed the SnFunction
        //(mirrors the bare-name OP_MakeFunc arm). VALUE POSITION ONLY:
        //the inner node must be an identifier (c.foo). A call shape
        //(c.foo(), inner == invoke) resolves to the same Func-typed
        //member but must fall through to the invoke emission below —
        //binding it here would emit a bound-reference opcode over a
        //receiver that no expression ever produced (review C1: a
        //module-qualified Func-returning call crashed with "null
        //receiver in method reference"). Receiver-first: emit the
        //receiver to resultOffset, refresh pResult, then the bind
        //opcode reads the receiver from pResult and writes the handle
        //there. Form selection matches the direct-call codegen decision:
        //virtual methods and interface declarations dispatch by name,
        //everything else binds the static function index. The executor's
        //bind-time null guard covers null receivers.
        if (field && field->Kind() == NK_Function
            && member.Inner()
            && member.Inner()->Kind() == NK_IdentifierExpr
            && member.EvalDataType()
            && member.EvalDataType()->Kind() == NK_ClassDecl
            && static_cast<SnClassDecl*>(
                member.EvalDataType())->IsFuncType()) {
            auto* method = static_cast<SnFunction*>(field);
            bool dispatchesByName = method->ContainFlags(NF_Virtual)
                || (method->Parent()
                    && method->Parent()->Kind() == NK_InterfaceDecl);
            EmitExpression(*member.Outer(), emitter, resultOffset);
            EmitPResultRefresh(emitter, resultOffset);
            if (dispatchesByName) {
                uint16_t nameIdx = AddStringConstant(method->Name());
                emitter.Emit(OpCode::OP_MakeVFunc);
                emitter.EmitUint16(nameIdx);
            } else {
                auto it = m_funcIndexMap.find(method);
                if (it == m_funcIndexMap.end())
                    throw std::runtime_error(
                        "NLang backend: method reference without a body: "
                        + method->Name());
                emitter.Emit(OpCode::OP_MakeBoundFunc);
                emitter.EmitUint16(static_cast<uint16_t>(it->second));
            }
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return;
        }
        //Phase 12: user-defined enum method call (c.rank(),
        //Color.Blue.rank(), this.weight()). Detection is by the resolver
        //binding: invoke.Field() is the SnFunction whose parent is the
        //enum decl — both receiver shapes bind identically. The receiver
        //is the enum VALUE (an int32 slot), so unlike the class path
        //there is no null check and no boxing; the claim shape is the
        //same receiver-first form with thisSlot=resultOffset. The builtin
        //toString dispatch below stays reachable: user methods of that
        //name are rejected at declaration (D5), so a bound enum-method
        //callee is never toString.
        {
            auto* inner = member.Inner();
            if (inner && inner->Kind() == NK_InvokeExpr)
            {
                auto& invoke = static_cast<SnInvokeExpr&>(*inner);
                auto* callee = invoke.Callee();
                if (callee && callee->Parent()
                    && callee->Parent()->Kind() == NK_EnumDecl)
                {
                    //Evaluate the receiver (enum value) into resultOffset.
                    EmitExpression(*member.Outer(), emitter, resultOffset);
                    std::vector<OutSpill> outSpills;
                    //Enum methods reject default and out formals at
                    //declaration (D4) — the binding path emits exactly
                    //the actual arguments.
                    EmitCallArgs(invoke, callee, emitter, /*slotBase=*/1,
                                 /*pArgPlans=*/nullptr,
                                 /*thisSlot=*/resultOffset, &outSpills);
                    if (!outSpills.empty())
                        throw std::runtime_error(
                            "NLang backend: out argument on enum method "
                            "call");
                    auto it = m_funcIndexMap.find(callee);
                    if (it == m_funcIndexMap.end())
                        throw std::runtime_error(
                            "NLang backend: call to enum method without "
                            "a body: " + callee->Name());
                    emitter.Emit(OpCode::OP_CallMethodDirect);
                    emitter.EmitUint16(static_cast<uint16_t>(it->second));
                    emitter.EmitUint16(m_currFunc->callParamBase);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(resultOffset);
                    emitter.Emit(OpCode::OP_ParaEnd);
                    return;
                }
            }
        }
        //Phase 8e-9b: non-class receiver toString() dispatch.
        //For enum/int/float receivers, the resolver accepted the call
        //(EvalDataType=String, NF_Resolved) without setting m_pField.
        //Codegen dispatches based on outer->EvalDataType():
        // - enum (NK_EnumDecl or NK_EnumMember Field()) → OP_Enum_to_str
        // - int (NK_Int32) → OP_Int32_to_str
        // - float (NK_Float) → OP_Float_to_str
        // - string (NK_String) → identity (no opcode)
        //This mirrors the SnCastExpr handler's dispatch but for the
        //MemberExpr+InvokeExpr AST shape that `x.toString()` produces.
        {
            auto* inner = member.Inner();
            if (inner && inner->Kind() == NK_InvokeExpr)
            {
                auto& invoke = static_cast<SnInvokeExpr&>(*inner);
                if (invoke.CalleeName() == "toString")
                {
                    auto* outerType = member.Outer()->EvalDataType();
                    NodeKind outerKind = outerType ? outerType->Kind() : static_cast<NodeKind>(0);
                    //Phase 9b-pre: array receiver — array is a VM primitive,
                    //not a class. MUST be checked BEFORE the int path because
                    //EvalDataType for `int[] arr` returns the element type
                    //(NK_Int32); array-ness is on the SnField via IsArrayType().
                    {
                        auto outerExprKind = member.Outer()->Kind();
                        if (outerExprKind == NK_MemberExpr
                            || outerExprKind == NK_IdentifierExpr)
                        {
                            auto& outerFieldExpr = static_cast<SnFieldExpr&>(
                                *member.Outer());
                            auto* outerField = outerFieldExpr.Field();
                            if (outerField && outerField->IsArrayType())
                            {
                                EmitExpression(*member.Outer(), emitter, resultOffset);
                                //Field loads leave the pResult accumulator
                                //stale (see EmitPResultRefresh); array_to_str
                                //reads the accumulator, so reload first.
                                EmitPResultRefresh(emitter, resultOffset);
                                emitter.Emit(OpCode::OP_Array_to_str);
                                emitter.Emit(OpCode::OP_Assign);
                                emitter.EmitUint16(resultOffset);
                                return;
                            }
                        }
                    }
                    //String.toString() — identity, no opcode.
                    if (outerKind == NK_String)
                    {
                        EmitExpression(*member.Outer(), emitter, resultOffset);
                        return;
                    }
                    //Enum receiver — need to find the SnEnumDecl for enumDefIdx.
                    //Two sub-cases:
                    // (a) outerKind == NK_EnumDecl (typed enum variable)
                    // (b) outerKind == NK_Int32 but outer's Field() is NK_EnumMember
                    //     (enum literal like Color.Green — EvalDataType is NK_Int32)
                    if (outerKind == NK_EnumDecl)
                    {
                        EmitExpression(*member.Outer(), emitter, resultOffset);
                        auto* enumDecl = static_cast<SnEnumDecl*>(outerType);
                        auto it = m_enumIndexMap.find(enumDecl);
                        if (it != m_enumIndexMap.end())
                        {
                            emitter.Emit(OpCode::OP_Enum_to_str);
                            emitter.EmitUint16(static_cast<uint16_t>(it->second));
                            emitter.Emit(OpCode::OP_Assign);
                            emitter.EmitUint16(resultOffset);
                        }
                        return;
                    }
                    if (outerKind == NK_Int32)
                    {
                        //Check if outer is an enum literal (Field() == NK_EnumMember)
                        auto outerExprKind = member.Outer()->Kind();
                        if (outerExprKind == NK_MemberExpr
                            || outerExprKind == NK_IdentifierExpr)
                        {
                            auto& outerFieldExpr = static_cast<SnFieldExpr&>(
                                *member.Outer());
                            auto* outerField = outerFieldExpr.Field();
                            if (outerField
                                && outerField->Kind() == NK_EnumMember)
                            {
                                EmitExpression(*member.Outer(), emitter, resultOffset);
                                auto* enumDecl = static_cast<SnEnumDecl*>(
                                    outerField->Parent());
                                auto it = m_enumIndexMap.find(enumDecl);
                                if (it != m_enumIndexMap.end())
                                {
                                    emitter.Emit(OpCode::OP_Enum_to_str);
                                    emitter.EmitUint16(static_cast<uint16_t>(it->second));
                                    emitter.Emit(OpCode::OP_Assign);
                                    emitter.EmitUint16(resultOffset);
                                }
                                return;
                            }
                        }
                        //Plain int receiver
                        EmitExpression(*member.Outer(), emitter, resultOffset);
                        EmitPResultRefresh(emitter, resultOffset);
                        emitter.Emit(OpCode::OP_Int32_to_str);
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(resultOffset);
                        return;
                    }
                    if (outerKind == NK_Float)
                    {
                        EmitExpression(*member.Outer(), emitter, resultOffset);
                        EmitPResultRefresh(emitter, resultOffset);
                        emitter.Emit(OpCode::OP_Float_to_str);
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(resultOffset);
                        return;
                    }
                    //Fall through to struct/class/interface handling below
                    //for class receivers (which have their own toString path
                    //via OP_CallMethod).
                }
            }
        }
        //Array.length builtin property (e.g. arr.length).
        //Array redesign B: the receiver check is the array-valued
        //property on the outer expression (stamped at resolver binding
        //tails — any shape: identifier, member like li.get(0), or call
        //result like mk()/lib.mk(3)); the emission itself is
        //shape-agnostic (EmitExpression handles any receiver form).
        //MUST be checked BEFORE the struct/class dispatch below: for
        //struct-element arrays (`Point[] b`), EvalDataType returns the
        //ELEMENT type (NK_StructDecl), so the struct branch would match
        //first, fail FindFieldOffset("length"), and silently emit only
        //the receiver. Same dispatch-order hazard as the array toString
        //path above.
        {
            auto* inner = member.Inner();
            if (inner && inner->Kind() == NK_IdentifierExpr
                && static_cast<SnIdentifierExpr*>(inner)->Name() == "length"
                && member.Outer()->IsArrayValued())
            {
                EmitExpression(*member.Outer(), emitter, resultOffset);
                emitter.Emit(OpCode::OP_NullCheck);
                emitter.EmitUint16(resultOffset);
                emitter.Emit(OpCode::OP_ArrayLength);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(resultOffset);
                return;
            }
        }
        //Struct field access (e.g. pt.x, pt.inner.x)
        auto* outerType = member.Outer()->EvalDataType();
        if (outerType && outerType->Kind() == NK_StructDecl) {
            auto* structDecl = static_cast<SnStructDecl*>(outerType);
            //Evaluate outer expression to resultOffset (gets heap index)
            EmitExpression(*member.Outer(), emitter, resultOffset);
            //Find the field's offset within the struct
            auto* inner = member.Inner();
            if (inner && inner->Kind() == NK_IdentifierExpr) {
                auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                int off = FindFieldOffset(*structDecl, fieldName);
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
            }
            return;
        }
        //Class field access (e.g. obj.x, obj.y)
        if (outerType && outerType->Kind() == NK_ClassDecl) {
            auto* classDecl = static_cast<SnClassDecl*>(outerType);
            //Evaluate outer expression to resultOffset (gets heap index)
            EmitExpression(*member.Outer(), emitter, resultOffset);
            //Null check
            emitter.Emit(OpCode::OP_NullCheck);
            emitter.EmitUint16(resultOffset);
            auto* inner = member.Inner();
            if (inner && inner->Kind() == NK_IdentifierExpr) {
                auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                int off = FindClassFieldOffset(*classDecl, fieldName);
                if (off < 0) {
                    throw std::runtime_error(
                        "NLang backend: class field offset not found: "
                        + fieldName);
                }
                emitter.Emit(OpCode::OP_LoadField);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(static_cast<uint16_t>(off));
            } else if (inner && inner->Kind() == NK_InvokeExpr) {
                //Class method call
                auto& invoke = static_cast<SnInvokeExpr&>(*inner);

                //Phase 13: function-handle toString() — the only built-in
                //method on a Func<...> receiver. The receiver is already
                //at resultOffset (loaded by the class-receiver prologue
                //above). Must precede the boxing-plan and method-table
                //machinery: a handle is a VM primitive (like an array),
                //Func has no backing CompiledClass.
                if (classDecl->IsFuncType()
                    && invoke.CalleeName() == "toString") {
                    EmitPResultRefresh(emitter, resultOffset);
                    emitter.Emit(OpCode::OP_Func_to_str);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(resultOffset);
                    return;
                }

                //Phase 13 Step 2: delegate member invoke (obj.cb(x)) —
                //the resolver bound the callee name to a Func-typed FIELD
                //of this class (the invoke's Field() is that field, not
                //an SnFunction; D13). The receiver is already at
                //resultOffset with the null check done. Unlike a method
                //call, staging holds USER args only — slotBase 0, no this
                //at slot 0. The handle is materialized into a scratch
                //claim slot (claimed BEFORE the args so nested emissions
                //cannot clobber it), matching the bare-form delegate arm.
                if (IsDelegateInvoke(invoke)) {
                    int off = FindClassFieldOffset(*classDecl,
                        invoke.CalleeName());
                    if (off < 0) {
                        throw std::runtime_error(
                            "NLang backend: class field offset not found: "
                            + invoke.CalleeName());
                    }
                    EvalAreaClaim calleeClaim(*this, 1);
                    uint16_t calleeSlot = calleeClaim.base();
                    emitter.Emit(OpCode::OP_LoadField);
                    emitter.EmitUint16(calleeSlot);
                    emitter.EmitUint16(resultOffset);
                    emitter.EmitUint16(static_cast<uint16_t>(off));
                    std::vector<OutSpill> delegateOutSpills;
                    EmitCallArgs(invoke, nullptr, emitter, /*slotBase=*/0,
                                 /*pArgPlans=*/nullptr,
                                 /*thisSlot=*/UINT16_MAX,
                                 &delegateOutSpills);
                    if (!delegateOutSpills.empty()) {
                        emitter.Emit(OpCode::OP_CallDelegateOut);
                        emitter.EmitUint16(calleeSlot);
                        emitter.EmitUint16(m_currFunc->callParamBase);
                        emitter.EmitInt32(static_cast<int32_t>(
                            BuildOutMask(delegateOutSpills)));
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(resultOffset);
                        EmitOutSpills(delegateOutSpills, emitter);
                    } else {
                        emitter.Emit(OpCode::OP_CallDelegate);
                        emitter.EmitUint16(calleeSlot);
                        emitter.EmitUint16(m_currFunc->callParamBase);
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(resultOffset);
                    }
                    emitter.Emit(OpCode::OP_ParaEnd);
                    return;
                }

                //Phase 8e-4: per-method boxing plan for built-in generic
                //classes (List<T>, Dict<K,V>). For primitive type arguments,
                //the corresponding param slots must be boxed (OP_Box) before
                //OP_CallMethod, and Get()/index returns must be unboxed
                //(OP_Unbox) after. argPlans maps paramIdx → {tag,needsBox};
                //returnsBoxed/returnTag handle the return-side unbox.
                //For class/struct type args, BoxingTagFor returns 0 → no
                //boxing; values pass as heap idxs directly.
                std::map<uint16_t, ArgBoxPlan> argPlans;
                bool returnsBoxed = false;
                uint8_t returnTag = 0;
                if (classDecl->IsGenericInstantiation()) {
                    const auto& typeArgs = classDecl->GenericTypeArgs();
                    const auto& methodName = invoke.CalleeName();
                    const auto& baseName = classDecl->BaseName();
                    //0.7.3 B: an array-typed slot IS the interned token —
                    //BoxingTagFor's default ({0,false}, not primitive)
                    //already flows it as a raw handle, class-style, so
                    //boxing stays reserved for true primitives.
                    if (baseName == "List") {
                        auto t = BoxingTagFor(
                            typeArgs.empty() ? nullptr : typeArgs[0]);
                        if (t.isPrimitive) {
                            //List<T> method signatures:
                            //  Add(T)         — T at paramIdx 1
                            //  Set(int, T)    — T at paramIdx 2
                            //  IndexOf(T)     — T at paramIdx 1
                            //  Contains(T)    — T at paramIdx 1
                            //  Get(int) → T   — return unboxed
                            if (methodName == "add"
                                || methodName == "indexOf"
                                || methodName == "contains") {
                                argPlans[1] = {t.tag, true};
                            } else if (methodName == "set") {
                                argPlans[2] = {t.tag, true};
                            } else if (methodName == "get") {
                                returnsBoxed = true; returnTag = t.tag;
                            }
                        }
                    } else if (baseName == "Dict") {
                        auto k = BoxingTagFor(
                            typeArgs.empty() ? nullptr : typeArgs[0]);
                        auto v = (typeArgs.size() > 1)
                            ? BoxingTagFor(typeArgs[1]) : BoxingTagResult{0, false};
                        if (methodName == "set") {
                            if (k.isPrimitive) argPlans[1] = {k.tag, true};
                            if (v.isPrimitive) argPlans[2] = {v.tag, true};
                        } else if (methodName == "get") {
                            if (k.isPrimitive) argPlans[1] = {k.tag, true};
                            if (v.isPrimitive) { returnsBoxed = true; returnTag = v.tag; }
                        } else if (methodName == "containsKey"
                            || methodName == "remove") {
                            if (k.isPrimitive) argPlans[1] = {k.tag, true};
                        }
                    }
                }

                auto* callee = invoke.Callee();
                //Phase 9e: out-argument spills collected by EmitCallArgs.
                std::vector<OutSpill> outSpills;
                //Phase 9c: evaluate args via the shared binding-aware
                //helper. Slot 0 is reserved for `this` (slotBase=1).
                //Boxing plans (for built-in generic class methods like
                //List<int>.add) are applied per-arg inside EmitCallArgs.
                //thisSlot=resultOffset: default-param expressions that
                //reference `this.field` resolve `this` to resultOffset
                //(the slot holding the receiver object). This avoids the
                //callParamBase nested-call clobber bug — copying `this`
                //to callParamBase[0] before emitting args would be
                //overwritten by any nested call inside the args.
                EmitCallArgs(invoke, callee, emitter, /*slotBase=*/1,
                             &argPlans, /*thisSlot=*/resultOffset,
                             &outSpills);
                //EmitCallArgs now copies `this` to claim[0] and bulk-copies
                //to callParamBase[0] — no separate this-copy needed here.
                bool isVirtual = callee && callee->ContainFlags(NF_Virtual);
                if (isVirtual) {
                    //Virtual method dispatch — name-based lookup at runtime
                    //Phase 9e: out args are resolver-rejected on virtual
                    //methods; spills here would be silently lost.
                    if (!outSpills.empty())
                        throw std::runtime_error(
                            "NLang backend: out argument on virtual call");
                    uint16_t nameIdx = AddStringConstant(callee->Name());
                    emitter.Emit(OpCode::OP_CallMethod);
                    emitter.EmitUint16(nameIdx);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                } else if (callee) {
                    //Non-virtual (final) method — direct call by function index
                    //Round-12: a miss on the function map means the resolver
                    //bound a method that never got registered — today that is
                    //a body-less declaration (the backend skips functions with
                    //nothing to compile). Silently skipping the call made the
                    //expression read stale frame memory; fail the build
                    //instead so the user gets a real diagnostic.
                    auto it = m_funcIndexMap.find(callee);
                    if (it == m_funcIndexMap.end())
                        throw std::runtime_error(
                            "NLang backend: call to method without a body: "
                            + callee->Name());
                    if (!outSpills.empty()) {
                        emitter.Emit(OpCode::OP_CallMethodDirectOut);
                        emitter.EmitUint16(static_cast<uint16_t>(it->second));
                        emitter.EmitUint16(m_currFunc->callParamBase);
                        emitter.EmitInt32(static_cast<int32_t>(
                            BuildOutMask(outSpills)));
                    } else {
                        emitter.Emit(OpCode::OP_CallMethodDirect);
                        emitter.EmitUint16(static_cast<uint16_t>(it->second));
                        emitter.EmitUint16(m_currFunc->callParamBase);
                    }
                } else {
                    //Phase 8e-1: no AST callee. Covers two cases:
                    //  (a) Builtin class methods (ByteStream/FileStream/List/Dict) —
                    //      the synthesized SnClassDecl has no method members.
                    //  (b) User-class calls to inherited Object protocol methods
                    //      (Equals/GetHashCode) — no AST override exists, so
                    //      callee is null. The VM walks superClassIdx to find
                    //      Object's intrinsic stub and short-circuits to
                    //      ExecuteIntrinsic.
                    //Both paths dispatch by name via OP_CallMethod. Phase 9e:
                    //out args cannot bind here (no formals) — internal error.
                    if (!outSpills.empty())
                        throw std::runtime_error(
                            "NLang backend: out argument on builtin call");
                    uint16_t nameIdx = AddStringConstant(invoke.CalleeName());
                    emitter.Emit(OpCode::OP_CallMethod);
                    emitter.EmitUint16(nameIdx);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                }
                //Phase 8e-3/8e-4: unbox Get() return for primitive T/V.
                if (returnsBoxed) {
                    emitter.Emit(OpCode::OP_Unbox);
                    emitter.EmitByte(returnTag);
                }
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(resultOffset);
                //Phase 9e: out spills LAST — OP_VarLocal clobbers pResult,
                //so the return value (and any unbox) must complete first.
                if (!outSpills.empty())
                    EmitOutSpills(outSpills, emitter);
                emitter.Emit(OpCode::OP_ParaEnd);
            }
            return;
        }
        //Interface method call (e.g. p.Print()).
        //At runtime, p holds a heap reference whose actual class is unknown
        //at compile time. Always dispatch virtually by method name — the
        //VM walks the runtime class's methodIndices via superClassIdx.
        if (outerType && outerType->Kind() == NK_InterfaceDecl) {
            //Evaluate outer expression to resultOffset (gets heap index)
            EmitExpression(*member.Outer(), emitter, resultOffset);
            //Null check
            emitter.Emit(OpCode::OP_NullCheck);
            emitter.EmitUint16(resultOffset);
            auto* inner = member.Inner();
            if (inner && inner->Kind() == NK_InvokeExpr) {
                auto& invoke = static_cast<SnInvokeExpr&>(*inner);
                //Phase 9c: binding-aware arg emit; slot 0 reserved for `this`.
                //thisSlot=resultOffset so default-param `this.field` reads
                //the receiver from the outer-expression result slot.
                std::vector<OutSpill> outSpills;
                EmitCallArgs(invoke, invoke.Callee(), emitter, /*slotBase=*/1,
                             /*pArgPlans=*/nullptr, /*thisSlot=*/resultOffset,
                             &outSpills);
                //EmitCallArgs now copies `this` to claim[0] and bulk-copies
                //to callParamBase[0] — no separate this-copy needed here.
                //Phase 9e: interface dispatch is name-based (virtual) —
                //out args are resolver-rejected; spills here are internal.
                if (!outSpills.empty())
                    throw std::runtime_error(
                        "NLang backend: out argument on interface call");
                //Always virtual dispatch by name.
                auto* callee = invoke.Callee();
                //Round-12: no callee means the resolver resolved the call
                //without binding a method — emitting nothing here would read
                //stale frame memory as the "result". Fail the build instead.
                if (!callee)
                    throw std::runtime_error(
                        "NLang backend: interface call without a bound method: "
                        + invoke.CalleeName());
                uint16_t nameIdx = AddStringConstant(callee->Name());
                emitter.Emit(OpCode::OP_CallMethod);
                emitter.EmitUint16(nameIdx);
                emitter.EmitUint16(m_currFunc->callParamBase);
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(resultOffset);
                emitter.Emit(OpCode::OP_ParaEnd);
            }
            return;
        }
        auto* inner = member.Inner();
        //Array element field access: arr[i].field (struct element)
        if (member.Outer()->Kind() == NK_SubscriptExpr) {
            auto& sub = static_cast<SnSubscriptExpr&>(*member.Outer());
            auto* subElemType = sub.EvalDataType();
            if (subElemType && subElemType->Kind() == NK_StructDecl) {
                auto* structDecl = static_cast<SnStructDecl*>(subElemType);
                //Evaluate array ref + index, load element heap index
                EmitExpression(sub, emitter, resultOffset);
                if (inner && inner->Kind() == NK_IdentifierExpr) {
                    auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                    int off = FindFieldOffset(*structDecl, fieldName);
                    if (off < 0) {
                        throw std::runtime_error(
                            "NLang backend: struct field offset not found: "
                            + fieldName);
                    }
                    emitter.Emit(OpCode::OP_LoadField);
                    emitter.EmitUint16(resultOffset);
                    emitter.EmitUint16(resultOffset);
                    emitter.EmitUint16(static_cast<uint16_t>(off));
                }
                return;
            }
            //Class element field access: arr[i].field (class element)
            if (subElemType && subElemType->Kind() == NK_ClassDecl) {
                auto* classDecl = static_cast<SnClassDecl*>(subElemType);
                EmitExpression(sub, emitter, resultOffset);
                emitter.Emit(OpCode::OP_NullCheck);
                emitter.EmitUint16(resultOffset);
                if (inner && inner->Kind() == NK_IdentifierExpr) {
                    auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                    int off = FindClassFieldOffset(*classDecl, fieldName);
                    if (off < 0) {
                        throw std::runtime_error(
                            "NLang backend: class field offset not found: "
                            + fieldName);
                    }
                    emitter.Emit(OpCode::OP_LoadField);
                    emitter.EmitUint16(resultOffset);
                    emitter.EmitUint16(resultOffset);
                    emitter.EmitUint16(static_cast<uint16_t>(off));
                }
                return;
            }
        }
        //String builtin methods: s.length(), s.GetHashCode(), s.Equals(other)
        //Phase 8e-1: GetHashCode/Equals dispatch to intrinsics for value semantics.
        //Strings are primitives (pool idx), not classes — these intrinsics are the
        //only way to invoke the protocol on them. Parallel to s.length() shortcut.
        if (inner && inner->Kind() == NK_InvokeExpr) {
            auto& invoke = static_cast<SnInvokeExpr&>(*inner);
            if (outerType && outerType->Kind() == NK_String)
            {
                const auto& methName = invoke.CalleeName();
                if (methName == "length")
                {
                    EmitExpression(*member.Outer(), emitter, resultOffset);
                    emitter.Emit(OpCode::OP_StrLen);
                    emitter.EmitUint16(resultOffset);
                    emitter.EmitUint16(resultOffset);
                    return;
                }
                if (methName == "getHashCode")
                {
                    //Round-10 receiver-first: emit the receiver to
                    //resultOffset, then copy straight to callParamBase — no
                    //evalArea claim needed (nothing is emitted between the
                    //copy and the intrinsic call, so nothing can clobber
                    //callParamBase). The old shape claimed the slot BEFORE
                    //emitting the receiver, stacking the receiver's nested
                    //claims on top while the MemberExpr walker is max-shaped
                    //(max(receiver, 1)) — a receiver containing calls
                    //drifted past the reserved frame.
                    EmitExpression(*member.Outer(), emitter, resultOffset);
                    emitter.Emit(OpCode::OP_VarLocal);
                    emitter.EmitUint16(resultOffset);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                    emitter.Emit(OpCode::OP_CallIntrinsic);
                    emitter.EmitUint16(INTR_String_GetHashCode);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(resultOffset);
                    emitter.Emit(OpCode::OP_ParaEnd);
                    return;
                }
                if (methName == "equals")
                {
                    //Phase 9c follow-up + round-10 receiver-first: args stage
                    //in the {this, args...} claim (nested calls in later args
                    //would clobber callParamBase), but the RECEIVER is
                    //emitted to resultOffset before the claim — resultOffset
                    //is a user local / parent claim slot, out of reach of
                    //nested emissions. Claiming before the receiver emission
                    //stacked the receiver's nested claims above the slice
                    //while the MemberExpr walker is max-shaped
                    //(max(receiver, claimSize + argDepth)).
                    uint16_t argCount = 0;
                    for (auto& p : invoke.Params()) ++argCount;
                    uint16_t n = static_cast<uint16_t>(1 + argCount);
                    EmitExpression(*member.Outer(), emitter, resultOffset);
                    EvalAreaClaim claim(*this, n);
                    uint16_t claimBase = claim.base();
                    emitter.Emit(OpCode::OP_VarLocal);
                    emitter.EmitUint16(resultOffset);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(claimBase);
                    uint16_t paramIdx = 1;
                    for (auto& param : invoke.Params()) {
                        EmitExpression(param, emitter,
                            claimBase + paramIdx * VALUE_SIZE);
                        ++paramIdx;
                    }
                    for (uint16_t i = 0; i < n; ++i) {
                        emitter.Emit(OpCode::OP_VarLocal);
                        emitter.EmitUint16(claimBase + i * VALUE_SIZE);
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(m_currFunc->callParamBase + i * VALUE_SIZE);
                    }
                    emitter.Emit(OpCode::OP_CallIntrinsic);
                    emitter.EmitUint16(INTR_String_Equals);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(resultOffset);
                    emitter.Emit(OpCode::OP_ParaEnd);
                    return;
                }
                //Phase 11 Step 3: table-driven string methods — the exact
                //equals shape above (receiver to resultOffset first, then
                //the {this, args...} claim, bulk copy, intrinsic, assign
                //back). All 12 return a value, so OP_Assign is always
                //emitted. The walker mirrors the claim size (incl. the
                //synthesized trailing arg) via the same table — see the
                //ExprPeakDepth MemberExpr branch.
                if (const StringMethodEntry* pMethod
                    = FindStringMethod(methName))
                {
                    uint16_t argCount = 0;
                    for (auto& p : invoke.Params()) ++argCount;
                    //OP_CallIntrinsic carries no argument count: a call
                    //shorter than maxArgs must stage the missing trailing
                    //argument synthetically or the intrinsic reads stale
                    //memory (substring's end = receiver.length(), filled
                    //via OP_StrLen on the receiver copy in claim slot 0).
                    uint16_t stagedArgs = argCount;
                    if (pMethod->trailingDefault == STD_ReceiverLength
                        && argCount < pMethod->maxArgs)
                        stagedArgs = pMethod->maxArgs;
                    uint16_t n = static_cast<uint16_t>(1 + stagedArgs);
                    EmitExpression(*member.Outer(), emitter, resultOffset);
                    EvalAreaClaim claim(*this, n);
                    uint16_t claimBase = claim.base();
                    emitter.Emit(OpCode::OP_VarLocal);
                    emitter.EmitUint16(resultOffset);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(claimBase);
                    uint16_t paramIdx = 1;
                    for (auto& param : invoke.Params()) {
                        EmitExpression(param, emitter,
                            claimBase + paramIdx * VALUE_SIZE);
                        ++paramIdx;
                    }
                    if (stagedArgs > argCount) {
                        //dst = StrLen(receiver copy at claim slot 0)
                        emitter.Emit(OpCode::OP_StrLen);
                        emitter.EmitUint16(static_cast<uint16_t>(
                            claimBase + stagedArgs * VALUE_SIZE));
                        emitter.EmitUint16(claimBase);
                    }
                    for (uint16_t i = 0; i < n; ++i) {
                        emitter.Emit(OpCode::OP_VarLocal);
                        emitter.EmitUint16(claimBase + i * VALUE_SIZE);
                        emitter.Emit(OpCode::OP_Assign);
                        emitter.EmitUint16(m_currFunc->callParamBase + i * VALUE_SIZE);
                    }
                    emitter.Emit(OpCode::OP_CallIntrinsic);
                    emitter.EmitUint16(pMethod->intrinsicId);
                    emitter.EmitUint16(m_currFunc->callParamBase);
                    emitter.Emit(OpCode::OP_Assign);
                    emitter.EmitUint16(resultOffset);
                    emitter.Emit(OpCode::OP_ParaEnd);
                    return;
                }
            }
            EmitExpression(*static_cast<SnExpression*>(inner), emitter, resultOffset);
        }
        return;
    }

    // Name expression - delegate

} //namespace nlang
