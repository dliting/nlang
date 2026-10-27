/*---
    ExprResolverInitListForm.cpp — 集合初始化器条目形态门
    从 ExprResolverInitList.cpp 抽取（2026-10-23 source-size 拆分，
    0.10.9 p23 条目键形态门批次）；0.10.10 统一槽位模型下键改为表达式，
    形态谓词按键节点形状判定。
---*/
#include "ExprResolver.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"

namespace nlang
{

//0.10.9 p23: the entry key form a container target accepts. kFormNone
//targets carry no rule here — class targets are covered by
//CheckClassInitListForm, channels by RejectNonEmptyChannelInit, and
//array targets by the bIsArray arm of RejectInitListEntryKeyForm.
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

//Each lowering reads exactly one key form: array and List store
//positional elements, Dict stores key/value pairs (the key is any
//expression typed against K — 0.10.10), and struct addresses fields
//by name or by declaration order.
static bool InitListEntryKeyAllowed(InitListEntryForm form,
    const InitEntry &entry)
{
    switch (form)
    {
    case kFormList:   return KeyIsAbsent(entry);
    case kFormDict:   return !KeyIsAbsent(entry);
    case kFormStruct: return KeyIsAbsent(entry) || KeyIsIdentifier(entry);
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
        env.Log(CLL_Error, pLoc,
            "a Dict initializer requires a key in every entry; "
            "positional entries are only valid for List, struct, "
            "and array initializers");
        break;
    case kFormStruct:
        env.Log(CLL_Error, pLoc,
            "struct initializer \"%s{...}\" keys are field names; an "
            "expression key is only valid for Dict initializers",
            static_cast<SnStructDecl *>(pTargetField)->Name().c_str());
        break;
    default:
        break;
    }
}

//0.10.9 p23 / 0.10.10: the entry key form must match the target shape.
//Pre-gate, a mismatched entry was silently rewritten: a keyed List
//entry stored its value as an element (the key dropped), a keyless
//Dict entry was skipped at codegen, and a string-keyed struct entry
//fell back to positional. True = rejected; the first offending entry
//logs the named diagnostic.
bool ExprResolveAccessor::RejectInitListEntryKeyForm(SnInitListExpr &sn,
    SnField *pTargetField, bool bIsArray)
{
    //0.10.10: the bracket form shares the unified entry list, so an
    //array target can now see keyed entries — arrays stay positional.
    if (bIsArray)
    {
        for (auto &entry : sn.Entries())
        {
            if (!KeyIsAbsent(entry))
            {
                m_Env.Log(CLL_Error,
                    entry.pValue ? entry.pValue->Location() : sn.Location(),
                    "an array initializer takes positional entries; the "
                    "keyed form is only valid for Dict, struct, and "
                    "class initializers");
                return true;
            }
        }
        return false;
    }
    const InitListEntryForm form = InitListEntryFormOf(pTargetField);
    if (form == kFormNone)
        return false;
    for (auto &entry : sn.Entries())
    {
        if (InitListEntryKeyAllowed(form, entry))
            continue;
        LogInitListEntryKeyMismatch(m_Env, sn, entry, form, pTargetField);
        return true;
    }
    return false;
}

} //namespace nlang
