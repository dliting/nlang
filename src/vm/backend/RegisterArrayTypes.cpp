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

uint16_t VmBackend::RegisterArrayType(SnField* pElemType) {
    uint8_t elemKind = RuntimeTypeKind(pElemType);
    uint16_t elemTypeIdx = 0xFFFF;
    if (elemKind == RTK_Struct && pElemType) {
        int idx = m_compiledModule.FindStruct(KeyOf(*pElemType));
        if (idx >= 0)
            elemTypeIdx = static_cast<uint16_t>(idx);
    } else if (elemKind == RTK_Class && pElemType) {
        int idx = m_compiledModule.FindClass(KeyOf(*pElemType));
        if (idx >= 0)
            elemTypeIdx = static_cast<uint16_t>(idx);
    }
    int existing = m_compiledModule.FindArray(elemKind, elemTypeIdx);
    if (existing >= 0)
        return static_cast<uint16_t>(existing);
    CompiledArrayType at;
    at.elemKind = elemKind;
    at.elemTypeIdx = elemTypeIdx;
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
        RegisterArrayType(static_cast<SnArrayTypeToken*>(
            pField)->ElemTypeOf());
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
        RegisterArrayTypeExpr(static_cast<SnLocalDeclStmt&>(s).Type());
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

void VmBackend::WalkArrayTypeNode(SyntaxNode& n) {
    if (n.IsImported()) return;  //Phase 9c R3-F: skip stub trees
    if (n.Kind() == NK_StructDecl) {
        for (auto& member : static_cast<SnStructDecl&>(n).Members())
            WalkArrayTypeField(member);
    } else if (n.Kind() == NK_ClassDecl) {
        for (auto& member : static_cast<SnClassDecl&>(n).Members())
            WalkArrayTypeField(member);
    } else if (n.Kind() == NK_Function) {
        auto& func = static_cast<SnFunction&>(n);
        for (auto& param : func.Params())
            WalkArrayTypeField(param);
        if (func.Body())
            WalkArrayTypeStmt(*func.Body());
    }
}

} //namespace nlang
