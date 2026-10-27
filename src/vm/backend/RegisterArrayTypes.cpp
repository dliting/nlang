/*---
    RegisterArrayTypes.cpp — 数组类型发现与注册族：声明点收集
    （struct/class 字段、形参、局部声明）＋ 惰性注册原语。
    从 Register.cpp 抽取（2026-10-02 尺寸守卫触发的拆分，零行为变化）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/SyntaxTree.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/runtime/NodeConsts.h>
#include "builder/ModuleRegistry.h"
#include "BuildEnvironment.h"
#include <vector>

namespace nlang {

uint16_t VmBackend::RegisterArrayType(SnField* pElemType, uint8_t rank) {
    uint8_t elemKind = RuntimeTypeKind(pElemType);
    uint16_t elemTypeIdx = 0xFFFF;
    //Per-unit: named element types slot through the placeholder-aware
    //helpers — a cross-unit element lands as an import slot for nlink
    //to resolve. Kind-gated so the casts are safe; generic
    //instantiations are ownerless and resolve to the built-in entry.
    if (elemKind == RTK_Struct && pElemType
        && pElemType->Kind() == NK_StructDecl) {
        elemTypeIdx = static_cast<uint16_t>(
            StructSlotFor(static_cast<SnStructDecl&>(*pElemType)));
    } else if (elemKind == RTK_Class && pElemType
        && pElemType->Kind() == NK_ClassDecl) {
        elemTypeIdx = static_cast<uint16_t>(
            ClassSlotFor(static_cast<SnClassDecl&>(*pElemType)));
    }
    int existing = m_compiledModule.FindArray(elemKind, elemTypeIdx, rank);
    if (existing >= 0)
        return static_cast<uint16_t>(existing);
    CompiledArrayType at;
    at.elemKind = elemKind;
    at.elemTypeIdx = elemTypeIdx;
    at.rank = rank;
    m_compiledModule.arrayTypes.push_back(at);
    return static_cast<uint16_t>(m_compiledModule.arrayTypes.size() - 1);
}

//Collect the array types used by declarations: struct/class fields,
//formal params, and local declarations. Array-returning functions and
//array-valued expressions register lazily at codegen time.
//0.7.3 B: a resolved array type expression binds Field() to its interned
//array token (alias uses included — the alias pre-pass splices the target
//type in before resolve), so the element type is one ElemTypeOf() away
//and the syntactic shape walk is gone.
void VmBackend::RegisterArrayTypes(SnNamespace& root) {
    ForEachDeclNode(root, [&](SnField& node) { WalkArrayTypeNode(node); });
}

//If the type expression resolves to an interned array token, register its
//element type.
void VmBackend::RegisterArrayTypeExpr(SnFieldExpr* pTypeExpr) {
    auto* pField = pTypeExpr ? pTypeExpr->Field() : nullptr;
    if (pField && pField->Kind() == NK_ArrayTypeToken)
    {
        auto* pToken = static_cast<SnArrayTypeToken*>(pField);
        //Stage-2: the token carries the rank — T[] and T[,] are
        //distinct table entries keyed (elemKind, elemTypeIdx, rank).
        RegisterArrayType(pToken->ElemTypeOf(), pToken->Rank());
    }
}

void VmBackend::WalkArrayTypeField(SnField& f) {
    if (f.Kind() == NK_StructField) {
        auto& sf = static_cast<SnStructField&>(f);
        if (sf.Type())
            RegisterArrayTypeExpr(sf.Type());
    }
    if (f.Kind() == NK_ClassField) {
        auto& cf = static_cast<SnClassField&>(f);
        if (cf.Type())
            RegisterArrayTypeExpr(cf.Type());
    }
    if (f.Kind() == NK_FormalParam) {
        auto& fp = static_cast<SnFormalParam&>(f);
        if (fp.Type())
            RegisterArrayTypeExpr(fp.Type());
    }
}

void VmBackend::WalkArrayTypeStmt(SnStatement& s) {
    if (s.Kind() == NK_Paragraph) {
        for (auto& child : static_cast<SnParagraph&>(s).Statements())
            WalkArrayTypeStmt(child);
    } else if (s.Kind() == NK_LocalDeclStmt) {
        auto& decl = static_cast<SnLocalDeclStmt&>(s);
        RegisterArrayTypeExpr(decl.Type());
        //0.10.7 D2: sized declarators register their element type from
        //the head field — the statement type stays the head type, so
        //the interned-token check above misses them.
        auto* pHeadField = decl.Type()->Field();
        if (pHeadField)
            for (auto& d : decl.Decls())
                if (d.bArrayType)
                {
                    //Sized declarators are rank-1 (comma declarator
                    //sizes are not supported in stage 2).
                    const uint8_t rank = (pHeadField->Kind()
                        == NK_ArrayTypeToken)
                        ? static_cast<SnArrayTypeToken*>(
                            pHeadField)->Rank() : 1;
                    RegisterArrayType(pHeadField, rank);
                }
    } else if (s.Kind() == NK_IfStmt) {
        auto& ifStmt = static_cast<SnIfStmt&>(s);
        if (ifStmt.ThenStmt()) WalkArrayTypeStmt(*ifStmt.ThenStmt());
        if (ifStmt.ElseStmt()) WalkArrayTypeStmt(*ifStmt.ElseStmt());
    } else if (s.Kind() == NK_WhileStmt) {
        WalkArrayTypeStmt(*static_cast<SnWhileStmt&>(s).Body());
    } else if (s.Kind() == NK_DoStmt) {
        WalkArrayTypeStmt(*static_cast<SnDoStmt&>(s).Body());
    } else if (s.Kind() == NK_ForStmt) {
        auto& forStmt = static_cast<SnForStmt&>(s);
        if (forStmt.Init()) WalkArrayTypeStmt(*forStmt.Init());
        if (forStmt.Body()) WalkArrayTypeStmt(*forStmt.Body());
    }
}

void VmBackend::WalkArrayTypeNode(SnField& n) {
    //Phase 9c R3-F: stub trees are skipped. Per-unit: a foreign unit's
    //array types belong to its own image; a shared element type slots
    //through StructSlotFor/ClassSlotFor at this unit's use sites.
    if (n.IsImported() || !IsOwnUnit(n)) return;
    if (n.Kind() == NK_StructDecl) {
        for (auto& member : static_cast<SnStructDecl&>(n).Fields())
            WalkArrayTypeField(member);
    } else if (n.Kind() == NK_ClassDecl) {
        for (auto& member : static_cast<SnClassDecl&>(n).Members())
            WalkArrayTypeField(member);
    } else if (n.Kind() == NK_Function) {
        auto& func = static_cast<SnFunction&>(n);
        for (auto& param : func.Params())
            WalkArrayTypeField(param);
        //A signature is a declaration: the return array type registers
        //eagerly too, not lazily at call sites — a native returning
        //T[] must find its scalar array type registered even when no
        //caller consumes the result (the newArray contract).
        if (func.HasReturn() && func.ReturnType())
            RegisterArrayTypeExpr(func.ReturnType());
        if (func.Body())
            WalkArrayTypeStmt(*func.Body());
    }
}

} //namespace nlang
