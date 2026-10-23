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

//0.10.10: the slot types init-list entries bind against — the array
//element type / the List value slot / the Dict key+value slots
//(`List<func<int,int>> l = [bar];`, `Dict<string,int> d = {k: 1};`).
//A null pair = the target carries no per-slot type (a user class/struct
//target is per-field); the func-ref binding and the slot casts then
//address the declared field instead.
InitListSlotTypes ExprResolveAccessor::ResolveInitListSlotTypes(
    SnField *pTargetField, bool bIsArray)
{
    InitListSlotTypes slots;
    if (bIsArray)
    {
        slots.pValueType = pTargetField;
        return slots;
    }
    if (pTargetField->Kind() == NK_ClassDecl)
    {
        auto *pGen = static_cast<SnClassDecl*>(pTargetField);
        const auto& baseName = pGen->BaseName();
        if (pGen->IsGenericInstantiation()
            && (baseName == kBuiltinListTypeName
                || baseName == kBuiltinDictTypeName))
        {
            auto typeArgs = GetGenericTypeArgs(pGen);
            if (baseName == kBuiltinDictTypeName && !typeArgs.empty())
                slots.pKeyType = typeArgs[0];
            const size_t valIdx =
                (baseName == kBuiltinListTypeName) ? 0 : 1;
            if (typeArgs.size() > valIdx)
                slots.pValueType = typeArgs[valIdx];
        }
    }
    return slots;
}

//Phase 13: bind each entry's pending function reference to its slot
//type (both the bare and the receiver-bound member form). 0.10.10: the
//key slot is covered symmetrically; its branch stays unreachable until
//`Func<...>` is a legal generic type argument.
static void BindEntrySlotFuncRef(BuildEnvironment &env,
    SnExpression *pExpr, SnField *pSlotType)
{
    if (!pExpr || !pSlotType)
        return;
    if (IsUnboundFuncRef(*pExpr))
    {
        BindFuncRefToExpected(env,
            *static_cast<SnIdentifierExpr*>(pExpr), pSlotType);
    }
    else if (IsUnboundMemberFuncRef(*pExpr))
    {
        BindMemberFuncRefToExpected(env,
            *static_cast<SnMemberExpr*>(pExpr), pSlotType);
    }
}

void ExprResolveAccessor::BindInitListFuncRefs(SnInitListExpr &sn,
    const InitListSlotTypes &slots)
{
    for (auto &entry : sn.Entries())
    {
        BindEntrySlotFuncRef(m_Env, entry.pKey, slots.pKeyType);
        BindEntrySlotFuncRef(m_Env, entry.pValue, slots.pValueType);
    }
}

//0.7.3 B / 0.7.5 / 0.10.10: every entry slot must cross at its
//declared width. The array/container lowerings read the staged value
//at the element kind's tag width (the wrap makes the codegen's store
//emit the box/coercion — without it an int entry into List<float>
//boxed raw int bits and read back as denormals), struct/class field
//stores copy the full 2-cell slot (a 4-byte entry into an 8-byte
//field carried the staging claim's stale upper bytes: 0.7.5
//`new P{id:1}` after any method call read 4294967297), and the Dict
//key slot stages at K's tag width exactly like the value slot at V's.
//Each slot runs the same conversion checks as the corresponding
//assignment (`arr[i] = v`, `p.field = v`, `d[k] = v`): the cast table
//adjudicates via FixupExprType (constant-fit and lossy warnings
//follow) and the wrap is written back through SetEntryValue /
//SetEntryKey. Slots this walk cannot resolve (synthetic built-in
//fields) keep the unwrap status quo.
void ExprResolveAccessor::ApplyInitListSlotCasts(SnInitListExpr &sn,
    SnField *pTargetField, const InitListSlotTypes &slots)
{
    const auto &entries = sn.Entries();
    for (size_t nIdx = 0; nIdx < entries.size(); ++nIdx)
    {
        //Key slot: only Dict carries a key type; the null check leaves
        //every other shape to the value path alone.
        if (slots.pKeyType)
        {
            auto *pKey = entries[nIdx].pKey;
            if (pKey && pKey->IsResolved() && pKey->EvalDataType())
            {
                TypeCastInfo keyCast(pKey->EvalDataType(), slots.pKeyType);
                auto iKey = sn.Children().find(pKey);
                if (iKey != sn.Children().end()
                    && FixupExprType(iKey, keyCast))
                    sn.SetEntryKey(nIdx,
                        &static_cast<SnCastExpr &>(*iKey));
            }
        }
        //Value slot: the element/value type for array/List/Dict, or the
//addressed field's declared type for struct/class.
        auto *pValue = entries[nIdx].pValue;
        if (!pValue || !pValue->IsResolved()
            || !pValue->EvalDataType())
            continue;
        SnField *pSlotType = slots.pValueType;
        if (!pSlotType)
            pSlotType = InitListEntryFieldType(*pTargetField, sn, nIdx);
        if (!pSlotType)
            continue;
        TypeCastInfo castInfo(pValue->EvalDataType(), pSlotType);
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
            if (KeyIsIdentifier(entry))
            {
                if (sf.Name() ==
                    static_cast<SnIdentifierExpr *>(entry.pKey)->Name())
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
        if (!KeyIsIdentifier(entry))
            return nullptr;
        const std::string keyName =
            static_cast<SnIdentifierExpr *>(entry.pKey)->Name();
        auto *pClass = static_cast<SnClassDecl *>(&targetDecl);
        std::vector<SnClassDecl *> ancestors;
        auto *pSuper = pClass->SuperClass();
        while (pSuper)
        {
            ancestors.push_back(pSuper);
            pSuper = pSuper->SuperClass();
        }
        for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it)
            if (auto *pType = ClassFieldByName(**it, keyName))
                return pType;
        return ClassFieldByName(*pClass, keyName);
    }
    return nullptr;
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

    //0.10.10: the entry key form must match the target shape (array and
    //List positional, Dict keyed, struct field-name-or-positional). The
    //Dict key TYPE is no longer a gate — any expression key is legal and
    //the key-slot cast below adjudicates it against K; the class form is
    //covered by CheckClassInitListForm after the stamping.
    if (RejectInitListEntryKeyForm(sn, pTargetField, bIsArray))
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
//the form gates (class-form checks and the entry key-form gate, both
//in PrepareInitListShape), entry resolution, func-ref binding and the
//entry-slot casts, in that order. Entry values and (0.10.10) Dict keys
//are expressions resolved via direct Accept (children inherit no
//expected type for now — they resolve via their normal paths; the slot
//casts then adjudicate each slot against its declared type).
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
    InitListSlotTypes slots =
        ResolveInitListSlotTypes(pTargetField, bIsArray);
    PropagateNestedInitListTargets(sn, pTargetField, slots.pValueType,
        bIsArray);

    for (auto &entry : sn.Entries())
    {
        //0.10.10: a Dict key is an expression resolved like a value.
        //struct/class identifier keys are field NAMES (the entry-form
        //gate leaves nothing else) and array/List entries carry no key
        //at all — resolving those identifiers as variables would
        //misreport "Cannot resolve the field".
        if (slots.pKeyType && entry.pKey && !entry.pKey->IsResolved())
            entry.pKey->Accept(*m_pVisitor);
        //0.10.8 D2: the target-less bracket inference already resolved
        //the entries; re-accepting a resolved node trips its assert.
        if (entry.pValue && !entry.pValue->IsResolved())
            entry.pValue->Accept(*m_pVisitor);
    }

    BindInitListFuncRefs(sn, slots);
    //0.7.5: every entry must cross at its slot's declared width — the
    //array/container lowerings read the staged entry at the element
    //kind's width (element casts), and struct/class field stores copy
    //the full 2-cell slot (per-field casts). 0.10.10: Dict keys cross
    //K exactly like values cross V.
    ApplyInitListSlotCasts(sn, pTargetField, slots);
}

} //namespace nlang
