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
#include <nlang/runtime/PrimitiveTypes.h>
#include <cassert>
#include <map>
#include <unordered_set>

namespace nlang {

//Compile-time field-offset solving (FindFieldOffset/FindClassFieldOffset/
//ClassAncestorFieldOffset) lives in backend/FieldOffsets.cpp (0.9.12
//source-size split, zero behavior change).

//Bare identifier that resolved to a type member (implicit this.field,
//e.g. `v = 1;` inside a method or a 0.9.11 D2 struct constructor):
//returns the owning type decl (SnClassDecl or SnStructDecl). For
//inherited fields the field node belongs to the ancestor decl, and
//FindClassFieldOffset walking from that owner yields the same flattened
//offset as from any subclass. Returns null when pField is not a class
//or struct data member (locals, formals, enum members).
SnField* VmBackend::OwningTypeOfMemberField(SnField* pField)
{
    if (!pField)
        return nullptr;
    auto* pParent = pField->Parent();
    if (!pParent)
        return nullptr;
    if (pField->Kind() == NK_ClassField
        && pParent->Kind() == NK_ClassDecl)
        return static_cast<SnClassDecl*>(pParent);
    //0.9.11 D2: struct constructors get the same bare-field binding.
    if (pField->Kind() == NK_StructField
        && pParent->Kind() == NK_StructDecl)
        return static_cast<SnStructDecl*>(pParent);
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
    //0.9.10: type-name-qualified static call — the resolver bound the
    //inner invoke to an NF_Static callee; no receiver value exists
    //(evaluating the outer identifier would hit the no-codegen-binding
    //throw), no null check, args stage from slot 0.
    if (EmitMemberStaticCall(member, emitter, resultOffset))
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

//Shared shape of a function reference in value position: a function
//field whose inner node is the plain identifier and whose type is the
//expected func declaration (c.foo / lib.seven).
static bool IsValueFuncRefShape(SnField& field, SnMemberExpr& member) {
    return field.Kind() == NK_Function
        && member.Inner()
        && member.Inner()->Kind() == NK_IdentifierExpr
        && member.EvalDataType()
        && member.EvalDataType()->Kind() == NK_ClassDecl
        && static_cast<SnClassDecl*>(
            member.EvalDataType())->IsFuncType();
}

//v2.2: module-qualified free-function reference (`lib.seven`): the
//function is owned by a namespace — or by nothing at all for a detached
//imported stub. These carry no receiver, unlike the bound-method form.
static bool IsReceiverlessFuncRefTarget(SnField& field) {
    auto* owner = field.Parent();
    return owner == nullptr || owner->Kind() == NK_Namespace;
}

//Compile-time constant read phase: an enum member constant (Color.Red)
//and an interface constant (I.NAME) both emit an inline literal. The
//receiver names a type, so no receiver value is emitted.
bool VmBackend::EmitMemberConstantRead(SnField* field,
                                       BytecodeEmitter& emitter,
                                       uint16_t resultOffset) {
    if (field && field->Kind() == NK_EnumMember) {
        auto* pEnumMember = static_cast<SnEnumMember*>(field);
        emitter.Emit(OpCode::OP_ConstInt32);
        emitter.EmitInt32(pEnumMember->Value());
    } else if (field && field->Kind() == NK_ClassField
               && IsInterfaceConstantField(*field)) {
        EmitInterfaceConstantRead(*static_cast<SnClassField*>(field),
                                  emitter);
    } else {
        return false;
    }
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    return true;
}

//Access head phase: namespace-qualified stdlib call, compile-time
//constants, Phase 13 bound method reference, Phase 12 enum method call.
//Returns true when one of these families emitted the expression.
bool VmBackend::EmitMemberHeaderDispatch(SnMemberExpr& member, SnField* field,
                                         BytecodeEmitter& emitter,
                                         uint16_t resultOffset) {
    //Library calls (stdlib or third-party, native or NLang) resolve to an
    //inlined AST function. A namespace-qualified free call emits ONLY its
    //inner invoke (the outer namespace name is never emitted): the invoke
    //binds an inlined function and dispatches through OP_CallFunc.
    {
        SnField* outerField = nullptr;
        if (member.Outer()
            && member.Outer()->Kind() == NK_IdentifierExpr)
            outerField = static_cast<SnFieldExpr*>(
                member.Outer())->Field();
        if (outerField && outerField->Kind() == NK_Namespace
            && member.Inner()
            && member.Inner()->Kind() == NK_InvokeExpr) {
            EmitExpression(*member.Inner(), emitter, resultOffset);
            return true;
        }
    }
    if (EmitMemberConstantRead(field, emitter, resultOffset))
        return true;
    //Function references in value position: the module form first
    //(lib.seven, v2.2 — receiver-less static OP_MakeFunc), then the
    //bound-method form (c.foo, Phase 13 Step 2). The full shape notes
    //travel with the emission helpers.
    if (field && IsValueFuncRefShape(*field, member)) {
        if (IsReceiverlessFuncRefTarget(*field))
            EmitReceiverlessFuncHandleRef(*field, emitter, resultOffset);
        else
            EmitMemberFuncHandleRef(member, field, emitter, resultOffset);
        return true;
    }
    if (EmitMemberEnumMethodCall(member, emitter, resultOffset))
        return true;
    return false;
}

//The folded payload in the channel matching its literal kind. Integer
//kinds stay integral (a floating bridge would drop bits above 2^53);
//the float-category kinds widen through double.
static int64_t FoldedSignedValue(const DefaultValueDesc& dv) {
    switch (dv.tag) {
        case RTK_Bool:
        case RTK_Char:
        case RTK_Int32:  return static_cast<int32_t>(dv.intValue);
        case RTK_Long:   return dv.longValue;
        case RTK_ULong:  return static_cast<int64_t>(dv.longValue);
        case RTK_Float:  return static_cast<int64_t>(dv.floatValue);
        case RTK_Double: return static_cast<int64_t>(dv.doubleValue);
        default:
            throw std::runtime_error(
                "NLang backend: interface constant folded tag "
                + std::to_string(static_cast<int>(dv.tag))
                + " has no integer value");
    }
}

static double FoldedFloatingValue(const DefaultValueDesc& dv) {
    switch (dv.tag) {
        case RTK_Int32:  return static_cast<int32_t>(dv.intValue);
        case RTK_Long:   return static_cast<double>(dv.longValue);
        case RTK_ULong:
            return static_cast<double>(
                static_cast<uint64_t>(dv.longValue));
        case RTK_Float:  return dv.floatValue;
        case RTK_Double: return dv.doubleValue;
        default:
            throw std::runtime_error(
                "NLang backend: interface constant folded tag "
                + std::to_string(static_cast<int>(dv.tag))
                + " has no floating value");
    }
}

//0.9.10 D3.5: interface constant use site. The declaration contract
//guarantees the initializer folds and the declared type is scalar or
//string, so emission is a plain inline literal (the caller's constant
//phase assigns it to the result slot).
void VmBackend::EmitInterfaceConstantRead(SnClassField& field,
                                          BytecodeEmitter& emitter) {
    DefaultValueDesc dv = ExtractDefaultValue(
        field.Initializer(), RuntimeTypeKind(field.EvalDataType()));
    if (dv.tag == RTK_Void) {
        //Declaration contract violation — an internal invariant break.
        throw std::runtime_error(
            "NLang backend: interface constant without a folded value: "
            + field.Name());
    }
    SnField* pDeclared = field.EvalDataType();
    const NodeKind declaredKind = pDeclared ? pDeclared->Kind() : NK_Int32;
    if (dv.tag == RTK_Null) {
        //No payload in any declared type; zero the whole uniform cell
        //so an 8-byte slot reads 0 too.
        emitter.Emit(OpCode::OP_ConstZero);
    } else if (declaredKind == NK_String) {
        emitter.Emit(OpCode::OP_ConstString);
        emitter.EmitUint16(static_cast<uint16_t>(dv.stringIdx));
    } else if (declaredKind == NK_Float) {
        emitter.Emit(OpCode::OP_ConstFloat);
        emitter.EmitFloat(static_cast<float>(FoldedFloatingValue(dv)));
    } else if (declaredKind == NK_Double) {
        emitter.Emit(OpCode::OP_ConstDouble);
        emitter.EmitDouble(FoldedFloatingValue(dv));
    } else {
        const int primIdx = ScalarPrimIndexOf(declaredKind);
        const int64_t value = FoldedSignedValue(dv);
        if (primIdx >= 0 && kScalarPrims[primIdx].slotWidth == 8) {
            emitter.Emit(OpCode::OP_ConstInt64);
            emitter.EmitInt64(value);
        } else {
            emitter.Emit(OpCode::OP_ConstInt32);
            emitter.EmitInt32(static_cast<int32_t>(value));
        }
    }
}

//Builtin phase: non-class receiver toString() dispatch, then the array
//length() method. Returns true when either emitted the expression.
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
    if (EmitMemberArrayLengthMethod(member, emitter, resultOffset))
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

//Array length() builtin method (e.g. arr.length()). The receiver
//check is the array-valued property on the outer expression (stamped
//at resolver binding tails — any shape: identifier, member like
//li.get(0), or call result like mk()/lib.mk(3)); the emission itself
//is shape-agnostic (EmitExpression handles any receiver form). The
//resolver consumed the call shape (TryResolveArrayLengthMethod); the
//zero-argument check here is defensive depth. Checked BEFORE the
//struct/class dispatch in EmitMemberTypedReceiverDispatch as defense
//in depth: array values carry the interned array token in
//EvalDataType (IsArrayValued is that derived check), so the
//struct/class branches do not match array receivers today — the
//order just guarantees a future typed-receiver change cannot
//silently swallow the builtin.
//Returns true when the method call was emitted.
bool VmBackend::EmitMemberArrayLengthMethod(SnMemberExpr& member,
                                            BytecodeEmitter& emitter,
                                            uint16_t resultOffset) {
    auto* inner = member.Inner();
    if (!(inner && inner->Kind() == NK_InvokeExpr
        && static_cast<SnInvokeExpr*>(inner)->CalleeName() == "length"
        && static_cast<SnInvokeExpr*>(inner)->Params().begin()
            == static_cast<SnInvokeExpr*>(inner)->Params().end()
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
