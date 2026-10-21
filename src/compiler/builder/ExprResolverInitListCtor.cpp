/*---
    ExprResolverInitListCtor.cpp — 类初始化器的构造器契约检查
    从 ExprResolverInitList.cpp 抽取（2026-10-21 source-size 拆分）；
    0.9.12 D7（D-new-5）的构造器访问门落在本文件的 CheckClassCtorForInitList。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "ScriptLocation.h"
#include "BuildEnvironment.h"
#include <vector>

namespace nlang
{

//Class init lists lower to `new C()` + per-field stores; the implicit
//ctor call must not require arguments (spec: class init requires a
//no-arg constructor, explicit or implicit). Without this check the
//codegen emits the ctor call anyway and the VM copies garbage/reads
//past the frame for the missing params. Class initializers are also
//identifier-keyed only: the codegen per-field store dispatches on the
//key name, and declaration order is meaningless with inherited fields
//(layout is root-ancestor-first). Value-only entries used to be
//silently skipped, leaving every field at its ctor default — reject
//them instead (struct targets keep the ordinal form).
void ExprResolveAccessor::CheckClassInitListForm(SnInitListExpr &sn,
    SnClassDecl *pClassDecl)
{
    if (pClassDecl->IsBuiltinClass())
        return;
    for (auto& entry : sn.Entries())
    {
        if (entry.keyKind != InitEntry::KeyKind::Identifier)
        {
            m_Env.Log(CLL_Error,
                entry.pValue ? entry.pValue->Location() : sn.Location(),
                "class initializer \"%s{...}\" requires "
                "field:value entries; the positional form is "
                "only valid for struct/array targets",
                pClassDecl->Name().c_str());
            break;
        }
    }
    CheckClassCtorForInitList(sn, pClassDecl);
}

//0.9.11 T6 (D10): select the zero-formal ctor among the candidates —
//the init list calls through it (codegen no longer scans for the
//first same-name member). With no zero-formal candidate the text
//names the first non-zero-formal one's arity (declaration order);
//with no candidates at all the implicit/table shape stays legal.
void ExprResolveAccessor::CheckClassCtorForInitList(SnInitListExpr &sn,
    SnClassDecl *pClassDecl)
{
    //0.9.12 D-new-5: only caller-reachable ctors are candidates; with
    //none reachable but some existing, report the accessibility failure.
    SnFunction *pNoArgCtor = nullptr;
    SnFunction *pFirstCtor = nullptr;
    bool bAnyInaccessible = false;
    for (auto& field : pClassDecl->Members())
    {
        //0.9.10 D2.4: a same-name static is not the constructor — skip it
        //and keep looking (declaration order must not matter).
        if (field.Kind() == NK_Function
            && field.Name() == pClassDecl->Name()
            && !field.ContainFlags(NF_Static))
        {
            auto &ctor = static_cast<SnFunction&>(field);
            if (!ctor.AllowAccess(*m_pAccessor))
            {
                bAnyInaccessible = true;
                continue;
            }
            if (!pFirstCtor)
                pFirstCtor = &ctor;
            if (!pNoArgCtor && ctor.Params().size() == 0)
                pNoArgCtor = &ctor;
        }
    }
    if (pNoArgCtor)
    {
        sn.CtorFunction(pNoArgCtor);
        return;
    }
    if (pFirstCtor)
    {
        m_Env.Log(CLL_Error, sn.Location(),
            "class initializer \"%s{...}\" requires a "
            "no-arg constructor; \"%s\" takes %zu "
            "argument(s)",
            pClassDecl->Name().c_str(),
            pClassDecl->Name().c_str(),
            pFirstCtor->Params().size());
    }
    else if (bAnyInaccessible)
    {
        m_Env.Log(CLL_Error, sn.Location(),
            "constructor of class \"%s\" is not accessible",
            pClassDecl->Name().c_str());
    }
}

} //namespace nlang
