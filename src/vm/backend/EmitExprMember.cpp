/*---
    EmitExprMember.cpp — 成员表达式发射（读取侧）：Access 分派器、字段读取与内建属性；含成员发射专用 static 辅助族。
    从 VmBackend.cpp 抽取；方法调用族拆至 EmitExprMemberCall.cpp，字符串族拆至 EmitExprMemberString.cpp（2026-09-25 可维护性重构，零行为变化）。
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

//Phase 9d: the built-in Exception class names whose runtime layout carries
//the message/backtrace fields outside Members().
static bool IsBuiltinExceptionName(const std::string& cn) {
    return cn == "Exception" || cn == "NullPointerException"
        || cn == "DivByZeroException" || cn == "IndexOutOfBoundsException"
        || cn == "AssertionException" || cn == "IOException";
}

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
    if (classDecl.IsBuiltinClass()
        && IsBuiltinExceptionName(classDecl.Name())) {
        //Phase 9d: direct built-in Exception class — synthetic decl has no
        //Members(); the 2 runtime fields are fixed at slot[1]/slot[2].
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
        int found = ClassAncestorFieldOffset(**it, fieldName, off);
        if (found >= 0)
            return found;
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

//FindClassFieldOffset arm: one ancestor in the root→parent walk. Returns
//the field offset when the field lives in this ancestor, else -1; `off`
//always advances past the ancestor's data fields.
int VmBackend::ClassAncestorFieldOffset(SnClassDecl& ancestor,
                                        const std::string& fieldName,
                                        uint16_t& off) {
    //Phase 9d: a built-in Exception ancestor contributes 2 runtime
    //fields (message, backtrace) that aren't in the synthetic
    //Members() list — RegisterClasses injects them into the compiled
    //field list, so account for them here.
    if (ancestor.IsBuiltinClass()
        && IsBuiltinExceptionName(ancestor.Name())) {
        if (fieldName == "message")  return off;
        if (fieldName == "backtrace") return off + VALUE_SIZE;
        off += 2 * VALUE_SIZE;
        return -1;
    }
    for (auto& member : ancestor.Members()) {
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

//Member-expression entry: snapshots the emission context, then walks the
//dispatch ladder in the original evaluation order. Each phase helper
//consumes one arm family and reports whether it emitted the expression;
//the snapshots are passed down explicitly because nested emission
//rewrites m_pCurrEmitter/m_resultOffset mid-flight.
void VmBackend::Access(SnMemberExpr& expr) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
    auto& member = static_cast<SnMemberExpr&>(expr);
    auto* field = member.Field();
    if (EmitMemberHeaderDispatch(member, field, emitter, resultOffset))
        return;
    if (EmitMemberBuiltinDispatch(member, emitter, resultOffset))
        return;
    auto* outerType = member.Outer()->EvalDataType();
    if (EmitMemberTypedReceiverDispatch(member, outerType, emitter,
                                        resultOffset))
        return;
    auto* inner = member.Inner();
    if (EmitMemberSubscriptElementRead(member, inner, emitter, resultOffset))
        return;
    EmitMemberStringMethodTail(member, inner, outerType, emitter,
                               resultOffset);
}

//Access head phase: namespace-qualified stdlib call, enum member constant,
//Phase 13 bound method reference, Phase 12 enum method call. Returns true
//when one of these families emitted the expression.
bool VmBackend::EmitMemberHeaderDispatch(SnMemberExpr& member, SnField* field,
                                         BytecodeEmitter& emitter,
                                         uint16_t resultOffset) {
    if (EmitMemberStdlibCall(member, emitter, resultOffset))
        return true;
    if (field && field->Kind() == NK_EnumMember) {
        //Enum member constant (e.g. Color.Red).
        auto* pEnumMember = static_cast<SnEnumMember*>(field);
        emitter.Emit(OpCode::OP_ConstInt32);
        emitter.EmitInt32(pEnumMember->Value());
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
        return true;
    }
    //Phase 13 Step 2: bound method reference (c.foo in value position) —
    //the full shape note travels with the emission in EmitMemberFuncHandleRef.
    if (field && field->Kind() == NK_Function
        && member.Inner()
        && member.Inner()->Kind() == NK_IdentifierExpr
        && member.EvalDataType()
        && member.EvalDataType()->Kind() == NK_ClassDecl
        && static_cast<SnClassDecl*>(
            member.EvalDataType())->IsFuncType()) {
        EmitMemberFuncHandleRef(member, field, emitter, resultOffset);
        return true;
    }
    if (EmitMemberEnumMethodCall(member, emitter, resultOffset))
        return true;
    return false;
}

//Builtin phase: non-class receiver toString() dispatch, then the array
//.length property. Returns true when either emitted the expression.
bool VmBackend::EmitMemberBuiltinDispatch(SnMemberExpr& member,
                                          BytecodeEmitter& emitter,
                                          uint16_t resultOffset) {
    auto* inner = member.Inner();
    if (inner && inner->Kind() == NK_InvokeExpr)
    {
        auto& invoke = static_cast<SnInvokeExpr&>(*inner);
        if (invoke.CalleeName() == "toString")
        {
            if (EmitMemberToStringNonClass(member, emitter, resultOffset))
                return true;
            //Fall through to struct/class/interface handling below
            //for class receivers (which have their own toString path
            //via OP_CallMethod).
        }
    }
    if (EmitMemberArrayLengthProperty(member, emitter, resultOffset))
        return true;
    return false;
}

//Typed-receiver phase: struct field read, class field read or method call,
//interface method call. Returns true when the receiver type matched.
bool VmBackend::EmitMemberTypedReceiverDispatch(SnMemberExpr& member,
                                                SnField* outerType,
                                                BytecodeEmitter& emitter,
                                                uint16_t resultOffset) {
    //Struct field access (e.g. pt.x, pt.inner.x)
    if (outerType && outerType->Kind() == NK_StructDecl) {
        auto* structDecl = static_cast<SnStructDecl*>(outerType);
        //Evaluate outer expression to resultOffset (gets heap index)
        EmitExpression(*member.Outer(), emitter, resultOffset);
        //Find the field's offset within the struct
        auto* inner = member.Inner();
        if (inner && inner->Kind() == NK_IdentifierExpr) {
            auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
            EmitStructFieldLoad(*structDecl, fieldName, emitter, resultOffset);
        }
        return true;
    }
    //Class field access / method call (e.g. obj.x, obj.foo())
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
            EmitClassFieldLoad(*classDecl, fieldName, emitter, resultOffset);
        } else if (inner && inner->Kind() == NK_InvokeExpr) {
            auto& invoke = static_cast<SnInvokeExpr&>(*inner);
            EmitMemberClassMethodCall(invoke, *classDecl, emitter,
                                      resultOffset);
        }
        return true;
    }
    //Interface dispatch — always virtual by name (see the helper).
    if (outerType && outerType->Kind() == NK_InterfaceDecl) {
        EmitMemberInterfaceCall(member, emitter, resultOffset);
        return true;
    }
    return false;
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
}

//Array element field access: arr[i].field for struct/class element types.
//Returns true when the outer was a subscript of either element kind.
bool VmBackend::EmitMemberSubscriptElementRead(SnMemberExpr& member,
                                               SnFieldExpr* inner,
                                               BytecodeEmitter& emitter,
                                               uint16_t resultOffset) {
    if (member.Outer()->Kind() == NK_SubscriptExpr) {
        auto& sub = static_cast<SnSubscriptExpr&>(*member.Outer());
        auto* subElemType = sub.EvalDataType();
        if (subElemType && subElemType->Kind() == NK_StructDecl) {
            auto* structDecl = static_cast<SnStructDecl*>(subElemType);
            //Evaluate array ref + index, load element heap index
            EmitExpression(sub, emitter, resultOffset);
            if (inner && inner->Kind() == NK_IdentifierExpr) {
                auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                EmitStructFieldLoad(*structDecl, fieldName, emitter,
                                    resultOffset);
            }
            return true;
        }
        //Class element field access: arr[i].field (class element)
        if (subElemType && subElemType->Kind() == NK_ClassDecl) {
            auto* classDecl = static_cast<SnClassDecl*>(subElemType);
            EmitExpression(sub, emitter, resultOffset);
            emitter.Emit(OpCode::OP_NullCheck);
            emitter.EmitUint16(resultOffset);
            if (inner && inner->Kind() == NK_IdentifierExpr) {
                auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
                EmitClassFieldLoad(*classDecl, fieldName, emitter,
                                   resultOffset);
            }
            return true;
        }
    }
    return false;
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
//Returns true when the property was emitted.
bool VmBackend::EmitMemberArrayLengthProperty(SnMemberExpr& member,
                                              BytecodeEmitter& emitter,
                                              uint16_t resultOffset) {
    auto* inner = member.Inner();
    if (!(inner && inner->Kind() == NK_IdentifierExpr
        && static_cast<SnIdentifierExpr*>(inner)->Name() == "length"
        && member.Outer()->IsArrayValued()))
        return false;
    EmitExpression(*member.Outer(), emitter, resultOffset);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(resultOffset);
    emitter.Emit(OpCode::OP_ArrayLength);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(resultOffset);
    return true;
}

} //namespace nlang
