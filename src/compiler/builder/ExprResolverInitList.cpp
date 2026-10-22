/*---
    ExprResolverInitList.cpp — 集合初始化器解析家族
    从 ExprResolverNew.cpp 抽取（2026-09-29 source-size 拆分，零行为变化）。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnArrayTypeToken.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include "ModuleRegistry.h"
#include <nlang/vm/StdLib.h>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

//0.9.0: the bare `{...}` node of an expression whose target type is not
//set yet — the shared entry point of target-type inference (deferred
//call arguments, nested entries, statement-side propagation). See
//ExprResolver.h.
SnInitListExpr *UntypedInitListOf(SnExpression &expr)
{
	if (expr.Kind() != NK_InitListExpr || expr.IsResolved())
		return nullptr;
	auto &initList = static_cast<SnInitListExpr&>(expr);
	if (initList.ExplicitType() || initList.InferredTarget())
		return nullptr;
	return &initList;
}

//0.9.0: the target shapes a collection initializer lowers to — array
//tokens, structs, List/Dict instantiations and non-builtin user
//classes (exactly what the init-list resolver and its emitter write).
//Scalars, string, func types, builtin classes, interfaces and enums
//would resolve the list but leave the target slot unstaged (the
//pre-0.9.0 `[1]` hole). Shared with the bare-brace argument admission
//(AdmitsInitListArg in ExprResolverBinding.cpp).
bool IsInitListTargetShape(SnField *pType)
{
    if (!pType)
        return false;
    switch (pType->Kind())
    {
    case NK_ArrayTypeToken:
    case NK_StructDecl:
        return true;
    case NK_ClassDecl:
        break;
    default:
        return false;
    }
    auto *pClass = static_cast<SnClassDecl*>(pType);
    if (IsFuncTypeDecl(pType))
        return false;
    if (pClass->IsGenericInstantiation())
        return pClass->BaseName() == kBuiltinListTypeName
            || pClass->BaseName() == kBuiltinDictTypeName
            //K1 Slice 9: `Channel<T> c = {};` constructs an empty
            //channel (the only legal entry count — see the reject
            //gate in Access(SnInitListExpr&)).
            || pClass->BaseName() == kBuiltinChannelTypeName;
    return !pClass->IsBuiltinClass();
}

//Target determination of the collection initializer (Phase 8e-6):
//- Explicit form `new Type{...}`: ExplicitType() carries the type
//  expression. Resolve it like a normal type, its resolved Field is the
//  target.
//- Bare form `[...]`: ExplicitType() is null. The parent AssignStmt
//  resolver must have populated InferredTarget() from the LHS variable's
//  type.
//False = no target could be derived (diagnostic logged).
bool ExprResolveAccessor::ResolveInitListTarget(SnInitListExpr &sn,
    SnField* &pTargetField, bool &bIsArray)
{
    pTargetField = nullptr;
    bIsArray = false;
    if (auto *pExplicit = sn.ExplicitType())
    {
        //Resolve the explicit type expression (NameExpr/GenericTypeExpr).
        pExplicit->Accept(*m_pVisitor);
        if (!pExplicit->IsResolved())
            return false;
        pTargetField = pExplicit->Field();
        bIsArray = pExplicit->IsArrayType();
    }
    else if (auto *pInferred = sn.InferredTarget())
    {
        //Bare form: parent populated the LHS variable. bIsArray keeps the
        //declaration-side signal (IsArrayType()); the type slot carries
        //the interned token for array variables, peeled to the element
        //below.
        bIsArray = pInferred->IsArrayType();
        pTargetField = pInferred->EvalDataType();
        //0.7.3 B token path: an
        //array-typed LHS carries the interned token; the init-list's
        //per-entry element gate consumes the ELEMENT (the node itself
        //keeps the interned token — Access stores it at the mint point,
        //0.9.13 D-new-1).
        if (pTargetField
            && pTargetField->Kind() == NK_ArrayTypeToken)
            pTargetField = static_cast<SnArrayTypeToken*>(
                pTargetField)->ElemTypeOf();
    }
    if (pTargetField)
        return true;
    m_Env.Log(CLL_Error, sn.Location(),
        "Collection initializer requires an explicit type or an LHS "
        "context to infer the target type.");
    return false;
}

//0.9.0: target-shape gate of Access(SnInitListExpr&) — only collection
//targets lower; anything else is a clean reject before the resolver
//flags the node. Array targets skip the element-shape test (the target
//IS the collection). True = rejected.
bool ExprResolveAccessor::RejectNonCollectionInitTarget(SnInitListExpr &sn,
    SnField *pTargetField, bool bIsArray)
{
    if (bIsArray || IsInitListTargetShape(pTargetField))
        return false;
    m_Env.Log(CLL_Error, sn.Location(),
        "cannot initialize \"%s\" from a collection initializer.",
        pTargetField->ToString().c_str());
    return true;
}

//0.9.11 T7 (D6): array initializers use the bracket form; the brace
//form is reserved for collection, struct, and class initializers.
//True = rejected.
bool ExprResolveAccessor::RejectBraceArrayInitTarget(SnInitListExpr &sn,
    bool bIsArray)
{
    if (bIsArray && !sn.IsArrayForm())
    {
        m_Env.Log(CLL_Error, sn.Location(),
            "array initializers use '[...]'; the brace form '{...}' is "
            "reserved for collection, struct, and class initializers");
        return true;
    }
    return false;
}

//CheckClassInitListForm / CheckClassCtorForInitList live in
//ExprResolverInitListCtor.cpp (source-size split).

//Phase 13: the element type init-list entries bind against — the array
//element type, or the value slot of a generic container
//(`List<func<int,int>> l = [bar];`). 0.7.5: restricted to the built-in
//List/Dict instantiations and returned as T for List / V for Dict
//(entries are key:value; keys are literal strings, values are V — the
//old elemArgs[0] made Dict values bind against K). Null = the target
//carries no element type (a user class/struct target is per-field);
//the func-ref binding is skipped.
SnField *ExprResolveAccessor::ResolveInitListElemType(SnField *pTargetField,
    bool bIsArray)
{
    if (bIsArray)
        return pTargetField;
    if (pTargetField->Kind() == NK_ClassDecl)
    {
        auto *pGen = static_cast<SnClassDecl*>(pTargetField);
        const auto& baseName = pGen->BaseName();
        if (!pGen->IsGenericInstantiation()
            || (baseName != kBuiltinListTypeName
                && baseName != kBuiltinDictTypeName))
            return nullptr;
        const size_t elemIdx = (baseName == kBuiltinListTypeName) ? 0 : 1;
        auto elemArgs = GetGenericTypeArgs(pGen);
        if (elemArgs.size() > elemIdx)
            return elemArgs[elemIdx];
    }
    return nullptr;
}

//Phase 13: bind each entry's pending function reference to the element
//type (both the bare and the receiver-bound member form).
void ExprResolveAccessor::BindInitListFuncRefs(SnInitListExpr &sn,
    SnField *pElemType)
{
    for (auto &entry : sn.Entries())
    {
        if (!entry.pValue)
            continue;
        if (IsUnboundFuncRef(*entry.pValue))
        {
            BindFuncRefToExpected(m_Env,
                *static_cast<SnIdentifierExpr*>(entry.pValue),
                pElemType);
        }
        else if (IsUnboundMemberFuncRef(*entry.pValue))
        {
            BindMemberFuncRefToExpected(m_Env,
                *static_cast<SnMemberExpr*>(entry.pValue),
                pElemType);
        }
    }
}

//Array-form element-type gate: every entry is an element store, so
//it takes the same conversion checks as `arr[i] = v` — the cast
//table adjudicates (0.7.3 B): a mismatched entry is rejected by
//FixupExprType (named array diagnostic for cross-element array
//values), and the wrap makes the codegen's OP_StoreElement emit the
//box/coercion. 0.7.5: container forms (List/Dict) run the same gate —
//their per-method boxing plans stage each entry at the element kind's
//tag width, and without the wrap an int entry into List<float> boxed
//raw int bits and read back as denormals.
void ExprResolveAccessor::ApplyInitListElemCasts(SnInitListExpr &sn,
    SnField *pElemType)
{
    const auto &entries = sn.Entries();
    for (size_t nIdx = 0; nIdx < entries.size(); ++nIdx)
    {
        auto *pValue = entries[nIdx].pValue;
        if (!pValue || !pValue->IsResolved()
            || !pValue->EvalDataType())
            continue;
        TypeCastInfo castInfo(pValue->EvalDataType(), pElemType);
        auto iExpr = sn.Children().find(pValue);
        if (iExpr == sn.Children().end())
            continue;
        if (FixupExprType(iExpr, castInfo))
            sn.SetEntryValue(nIdx,
                &static_cast<SnCastExpr &>(*iExpr));
    }
}

//One class decl's arm of the init-list field lookup: data fields only.
static SnField *ClassFieldByName(SnClassDecl &classDecl,
    const std::string &name)
{
    for (auto &member : classDecl.Members())
        if (member.Kind() == NK_ClassField && member.Name() == name)
            return member.EvalDataType();
    return nullptr;
}

//The declared type an init-list entry stores into. Identifier keys
//address fields by name — the class arm walks the SuperClass chain
//ROOT-FIRST so it resolves the same field the backend's
//FindClassFieldOffset gives the store slot to (layout is
//root-ancestor-first; a shadowed name belongs to the ancestor slot).
//Positional entries address declaration order (struct form; the
//emitter counts every entry's index, keyed or not). Null = not
//resolvable here (synthetic built-in fields, out-of-range ordinal,
//non-identifier class entries).
SnField *ExprResolveAccessor::InitListEntryFieldType(SnField &targetDecl,
    const SnInitListExpr &sn, size_t entryIdx)
{
    const auto &entries = sn.Entries();
    if (entryIdx >= entries.size())
        return nullptr;
    const auto &entry = entries[entryIdx];
    if (targetDecl.Kind() == NK_StructDecl)
    {
        size_t idx = 0;
        for (auto &sf :
            static_cast<SnStructDecl &>(targetDecl).Fields())
        {
            if (entry.keyKind == InitEntry::KeyKind::Identifier)
            {
                if (sf.Name() == entry.keyStr)
                    return sf.EvalDataType();
            }
            else if (idx == entryIdx)
                return sf.EvalDataType();
            ++idx;
        }
        return nullptr;
    }
    if (targetDecl.Kind() == NK_ClassDecl)
    {
        if (entry.keyKind != InitEntry::KeyKind::Identifier)
            return nullptr;
        auto *pClass = static_cast<SnClassDecl *>(&targetDecl);
        std::vector<SnClassDecl *> ancestors;
        auto *pSuper = pClass->SuperClass();
        while (pSuper)
        {
            ancestors.push_back(pSuper);
            pSuper = pSuper->SuperClass();
        }
        for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it)
            if (auto *pType = ClassFieldByName(**it, entry.keyStr))
                return pType;
        return ClassFieldByName(*pClass, entry.keyStr);
    }
    return nullptr;
}

//Struct/class init form: entries address declared fields, and the
//lowering's OP_StoreField copies the full 2-cell field slot — a
//4-byte entry into an 8-byte field (long/ulong; double from Task 7)
//carried the staging claim's stale upper bytes (0.7.5: `new P{id:1}`
//after any method call read 4294967297). Wrap each resolvable entry
//against its field's declared type exactly like `p.field = v` does
//(the member-assign gate); constant-fit and lossy warnings follow.
//Entries this walk cannot resolve (synthetic built-in fields) keep
//the unwrap status quo.
void ExprResolveAccessor::ApplyInitListFieldCasts(SnInitListExpr &sn,
    SnField *pTargetField)
{
    const auto &entries = sn.Entries();
    for (size_t nIdx = 0; nIdx < entries.size(); ++nIdx)
    {
        auto *pValue = entries[nIdx].pValue;
        if (!pValue || !pValue->IsResolved()
            || !pValue->EvalDataType())
            continue;
        SnField *pFieldType =
            InitListEntryFieldType(*pTargetField, sn, nIdx);
        if (!pFieldType)
            continue;
        TypeCastInfo castInfo(pValue->EvalDataType(), pFieldType);
        auto iExpr = sn.Children().find(pValue);
        if (iExpr == sn.Children().end())
            continue;
        if (FixupExprType(iExpr, castInfo))
            sn.SetEntryValue(nIdx,
                &static_cast<SnCastExpr &>(*iExpr));
    }
}

//2026-09-29 decomposition of Access(SnInitListExpr&): the 0.7.5 Dict
//key-form gate. Dict initializer keys are string literals by grammar;
//the lowering emits every key as a string constant and boxes it with
//K's tag — a non-string K stored the string HANDLE bits as the key
//(silently unreachable entries). Explicit set() covers other key
//types; an empty initializer stays legal. True = rejected.
bool ExprResolveAccessor::RejectNonStringDictInitKeys(SnInitListExpr &sn,
    SnField *pTargetField)
{
    if (pTargetField->Kind() != NK_ClassDecl)
        return false;
    auto *pGen = static_cast<SnClassDecl *>(pTargetField);
    if (!pGen->IsGenericInstantiation()
        || pGen->BaseName() != kBuiltinDictTypeName)
        return false;
    auto typeArgs = GetGenericTypeArgs(pGen);
    if (typeArgs.empty() || !typeArgs[0]
        || typeArgs[0]->Kind() == NK_String || sn.Entries().empty())
        return false;
    m_Env.Log(CLL_Error, sn.Location(),
        "the Dict collection initializer requires string keys; use "
        "set() with an explicit '%s' key",
        typeArgs[0]->ToString().c_str());
    return true;
}

//K1 Slice 9: gate of Access(SnInitListExpr&) — a channel initializer
//must be empty. Values enter a channel through send() alone (the
//synchronized surface); an initializer entry would be a second,
//unsynchronized insertion path, and the lowering has no entry form for
//it. True = rejected.
bool ExprResolveAccessor::RejectNonEmptyChannelInit(SnInitListExpr &sn,
    SnField *pTargetField)
{
    if (pTargetField->Kind() != NK_ClassDecl)
        return false;
    auto *pGen = static_cast<SnClassDecl *>(pTargetField);
    if (!pGen->IsGenericInstantiation()
        || pGen->BaseName() != kBuiltinChannelTypeName
        || sn.Entries().empty())
        return false;
    m_Env.Log(CLL_Error, sn.Location(),
        "a channel initializer must be empty; values enter through "
        "send()");
    return true;
}

//0.9.0: nested bare brace entries inherit the slot type they store into
//— the element type for array/List/Dict targets, or the addressed
//field's declared type for struct/class targets (InitListEntryFieldType).
void ExprResolveAccessor::PropagateNestedInitListTargets(SnInitListExpr &sn,
    SnField *pTargetField, SnField *pElemType, bool bIsArray)
{
    const auto &entries = sn.Entries();
    for (size_t nIdx = 0; nIdx < entries.size(); ++nIdx)
    {
        auto *pValue = entries[nIdx].pValue;
        auto *pNested = pValue ? UntypedInitListOf(*pValue) : nullptr;
        if (!pNested)
            continue;
        SnField *pNestedTarget = pElemType;
        if (!pNestedTarget && !bIsArray)
            pNestedTarget = InitListEntryFieldType(*pTargetField, sn, nIdx);
        if (pNestedTarget)
            pNested->InferredTarget(pNestedTarget);
    }
}

//Target-shape phase of Access(SnInitListExpr&) (split out for size):
//the three form gates, then stamping the resolved type token and the
//class-form ctor contract. True = rejected (the firing gate logged
//its diagnostic).
bool ExprResolveAccessor::PrepareInitListShape(SnInitListExpr &sn,
    SnField *pTargetField, bool bIsArray)
{
    //0.9.0: the target must be a shape the lowering can write.
    if (RejectNonCollectionInitTarget(sn, pTargetField, bIsArray))
        return true;

    //0.9.11 T7 (D6): arrays use the bracket form.
    if (RejectBraceArrayInitTarget(sn, bIsArray)) return true;

    //K1 Slice 9: channel construction is the empty initializer only.
    if (RejectNonEmptyChannelInit(sn, pTargetField))
        return true;

    //0.9.13 D-new-1: array targets mint the interned array token.
    sn.EvalDataType(bIsArray ? m_Env.InternArrayTypeToken(pTargetField)
        : pTargetField);
    sn.AddFlags(NF_Resolved);

    if (!bIsArray && pTargetField->Kind() == NK_ClassDecl)
        CheckClassInitListForm(sn,
            static_cast<SnClassDecl*>(pTargetField));
    return false;
}

//Phase 8e-6: Collection initializer resolver — target determination,
//class-form checks, entry resolution, func-ref binding and the
//entry-type gates, in that order. Entry values are resolved via
//direct Accept (children inherit no expected type for now — they
//resolve via their normal paths); keys (for {...} form) are not
//expressions and need no resolution.
void ExprResolveAccessor::Access(SnInitListExpr &sn)
{
    assert(!sn.IsResolved());

    SnField *pTargetField = nullptr;
    bool bIsArray = false;
    if (!ResolveInitListTarget(sn, pTargetField, bIsArray))
        return;

    if (PrepareInitListShape(sn, pTargetField, bIsArray))
        return;

    //0.9.0: nested bare brace entries infer their target from the slot
    //they store into, before the entry walk resolves them.
    SnField *pElemType = ResolveInitListElemType(pTargetField, bIsArray);
    PropagateNestedInitListTargets(sn, pTargetField, pElemType, bIsArray);

    for (auto &entry : sn.Entries())
    {
        if (entry.pValue)
            entry.pValue->Accept(*m_pVisitor);
    }

    if (!bIsArray && RejectNonStringDictInitKeys(sn, pTargetField))
        return;

    if (pElemType)
        BindInitListFuncRefs(sn, pElemType);
    //0.7.5: every entry must cross at its slot's declared width — the
    //array/container lowerings read the staged entry at the element
    //kind's width (element casts), and struct/class field stores copy
    //the full 2-cell slot (per-field casts).
    if (pElemType)
        ApplyInitListElemCasts(sn, pElemType);
    else if (!bIsArray)
        ApplyInitListFieldCasts(sn, pTargetField);
}

} //namespace nlang
