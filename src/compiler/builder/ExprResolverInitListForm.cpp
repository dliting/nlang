/*---
    ExprResolverInitListForm.cpp — 集合初始化器条目形态门
    从 ExprResolverInitList.cpp 抽取（2026-10-23 source-size 拆分，
    0.10.9 p23 条目键形态门批次）。
---*/
#include "ExprResolver.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"

namespace nlang
{

//0.10.9 p23: the entry key form a container target accepts. kFormNone
//targets carry no rule here — class targets are covered by
//CheckClassInitListForm, channels by RejectNonEmptyChannelInit, and
//array targets by the grammar/brace gates.
enum InitListEntryForm { kFormNone, kFormList, kFormDict, kFormStruct };

static InitListEntryForm InitListEntryFormOf(SnField *pTargetField)
{
    if (pTargetField->Kind() == NK_StructDecl)
        return kFormStruct;
    if (pTargetField->Kind() != NK_ClassDecl)
        return kFormNone;
    auto *pGen = static_cast<SnClassDecl *>(pTargetField);
    if (!pGen->IsGenericInstantiation())
        return kFormNone;
    if (pGen->BaseName() == kBuiltinListTypeName)
        return kFormList;
    if (pGen->BaseName() == kBuiltinDictTypeName)
        return kFormDict;
    return kFormNone;
}

//Each lowering reads exactly one key form: List stores positional
//elements, Dict stores string-keyed pairs, and struct addresses fields
//by name or by declaration order.
static bool InitListEntryKeyAllowed(InitListEntryForm form,
    InitEntry::KeyKind key)
{
    switch (form)
    {
    case kFormList:   return key == InitEntry::KeyKind::None;
    case kFormDict:   return key == InitEntry::KeyKind::String;
    case kFormStruct: return key != InitEntry::KeyKind::String;
    default:          return true;
    }
}

//The named diagnostic for one mismatched entry, anchored at the entry's
//value location with the initializer node as fallback
//(CheckClassInitListForm's idiom).
static void LogInitListEntryKeyMismatch(BuildEnvironment &env,
    SnInitListExpr &sn, const InitEntry &entry,
    InitListEntryForm form, SnField *pTargetField)
{
    const ISourceLocation *pLoc =
        entry.pValue ? entry.pValue->Location() : sn.Location();
    switch (form)
    {
    case kFormList:
        env.Log(CLL_Error, pLoc,
            "a List initializer takes positional entries; the keyed "
            "form is only valid for Dict, struct, and class "
            "initializers");
        break;
    case kFormDict:
        if (entry.keyKind == InitEntry::KeyKind::None)
            env.Log(CLL_Error, pLoc,
                "a Dict initializer requires a key in every entry; "
                "positional entries are only valid for List, struct, "
                "and array initializers");
        else
            env.Log(CLL_Error, pLoc,
                "the Dict collection initializer requires string "
                "literal keys; write \"%s\" as the key",
                entry.keyStr.c_str());
        break;
    case kFormStruct:
        env.Log(CLL_Error, pLoc,
            "struct initializer \"%s{...}\" keys are field names; "
            "string keys are only valid for Dict initializers",
            static_cast<SnStructDecl *>(pTargetField)->Name().c_str());
        break;
    default:
        break;
    }
}

//0.10.9 p23: the entry key form must match the target kind. Pre-gate,
//a mismatched entry was silently rewritten: a keyed List entry stored
//its value as an element (the key dropped), a keyless Dict entry was
//skipped at codegen, a string-keyed struct entry fell back to
//positional, and an identifier key on Dict was silently taken as its
//spelled string. True = rejected; the first offending entry logs the
//named diagnostic.
bool ExprResolveAccessor::RejectInitListEntryKeyForm(SnInitListExpr &sn,
    SnField *pTargetField)
{
    const InitListEntryForm form = InitListEntryFormOf(pTargetField);
    if (form == kFormNone)
        return false;
    for (auto &entry : sn.Entries())
    {
        if (InitListEntryKeyAllowed(form, entry.keyKind))
            continue;
        LogInitListEntryKeyMismatch(m_Env, sn, entry, form, pTargetField);
        return true;
    }
    return false;
}

} //namespace nlang
