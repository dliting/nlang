/*---
    Register.cpp — 类型与函数注册族：structs/arrays/enums/functions（类族在 RegisterClass.cpp）。
    从 VmBackend.cpp 抽取（2026-09-25 可维护性重构，零行为变化）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/SyntaxTree.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/compiler/ScriptLocation.h>
#include <nlang/compiler/TranslationUnit.h>
#include <nlang/runtime/NodeConsts.h>

namespace nlang {
namespace {

//Qualified name of a function nested in one or more non-root namespaces,
//e.g. "io.print" / "math.ext.hypot3". Top-level free functions keep a bare
//name. A CLASS/INTERFACE/ENUM METHOD always keeps a bare name too, even when
//its owning type is nested inside a namespace: methods dispatch by name
//through their receiver (VmExecutor::FindMethodByName compares the bare
//name), so a namespace prefix would make every lookup fail.
std::string QualifiedFunctionName(const SnFunction& func) {
    if (func.Parent()
        && (func.Parent()->Kind() == NK_ClassDecl
            || func.Parent()->Kind() == NK_InterfaceDecl
            || func.Parent()->Kind() == NK_EnumDecl))
        return func.Name();

    SnNamespace* const pRoot = TheAST().Root();
    std::string prefix;
    for (SyntaxNode* pNode = func.Parent(); pNode != nullptr;
        pNode = pNode->Parent()) {
        if (pNode->Kind() == NK_Namespace && pNode != pRoot) {
            const auto* pNs = static_cast<const SnNamespace*>(pNode);
            prefix = pNs->Name() + (prefix.empty() ? "" : ".") + prefix;
        }
    }
    return prefix.empty() ? func.Name() : prefix + "." + func.Name();
}

} // namespace

void VmBackend::RegisterStructDecl(SnStructDecl& sn) {
    CompiledStruct cs;
    cs.name = sn.Name();
    cs.fieldCount = static_cast<uint16_t>(sn.FieldCount());
    std::vector<std::string> typeNames;
    std::vector<SnField*> fieldTypes;
    for (auto& field : sn.Members()) {
        cs.fieldNames.push_back(field.Name());
        auto* fieldType = field.EvalDataType();
        //0.7.3 B: array fields bind EvalDataType to their interned
        //array token, so RuntimeTypeKind alone files them as
        //RTK_Array. (Pre-token, EvalDataType degraded to the element
        //type and mis-filed `int[]` as RTK_Int32 in writeStruct.)
        uint16_t ftk = RuntimeTypeKind(fieldType);
        cs.fieldTypeKinds.push_back(ftk);
        cs.fieldStructIndices.push_back(0xFFFF);
        cs.fieldClassIndices.push_back(0xFFFF);
        if ((ftk == RTK_Struct || ftk == RTK_Class) && fieldType)
            typeNames.push_back(fieldType->Name());
        else
            typeNames.push_back("");
        fieldTypes.push_back(fieldType);
    }
    m_compiledModule.structs.push_back(std::move(cs));
    m_structFieldTypeNames.push_back(std::move(typeNames));
    m_structFieldTypes.push_back(std::move(fieldTypes));
}

void VmBackend::RegisterStructs(SnNamespace& root) {
    ForEachDeclNode(root, [&](SnField& node) {
        if (node.Kind() == NK_StructDecl && !node.IsImported())
            RegisterStructDecl(static_cast<SnStructDecl&>(node));
    });
    //Resolve fieldStructIndices now that all structs are registered.
    //fieldClassIndices are resolved later by ResolveStructClassRefs
    //(after RegisterClasses, since classes are not yet registered here).
    for (size_t si = 0; si < m_compiledModule.structs.size(); ++si) {
        auto& cs = m_compiledModule.structs[si];
        auto& typeNames = m_structFieldTypeNames[si];
        for (size_t i = 0; i < typeNames.size(); ++i) {
            if (!typeNames[i].empty() && cs.fieldTypeKinds[i] == RTK_Struct) {
                int idx = m_compiledModule.FindStruct(typeNames[i]);
                if (idx >= 0)
                    cs.fieldStructIndices[i] = static_cast<uint16_t>(idx);
            }
        }
    }
}


void VmBackend::ResolveStructClassRefs() {
    for (size_t si = 0; si < m_compiledModule.structs.size(); ++si) {
        auto& cs = m_compiledModule.structs[si];
        auto& typeNames = m_structFieldTypeNames[si];
        for (size_t i = 0; i < typeNames.size(); ++i) {
            if (!typeNames[i].empty() && cs.fieldTypeKinds[i] == RTK_Class) {
                int idx = m_compiledModule.FindClass(typeNames[i]);
                if (idx >= 0)
                    cs.fieldClassIndices[i] = static_cast<uint16_t>(idx);
            }
        }
        //v1.12: field type descriptors — built here because both the
        //struct and class tables are complete (BuildTypeDesc resolves
        //names through them). Phase-A-merged imported structs have an
        //empty parallel list, so they keep the copied+remapped
        //descriptors untouched.
        auto& fieldTypes = m_structFieldTypes[si];
        for (size_t i = 0; i < fieldTypes.size(); ++i)
            cs.fieldTypeDescs.push_back(
                BuildTypeDesc(fieldTypes[i], m_compiledModule));
    }
}

uint16_t VmBackend::RegisterArrayType(SnField* pElemType) {
    uint8_t elemKind = RuntimeTypeKind(pElemType);
    uint16_t elemTypeIdx = 0xFFFF;
    if (elemKind == RTK_Struct && pElemType) {
        int idx = m_compiledModule.FindStruct(pElemType->Name());
        if (idx >= 0)
            elemTypeIdx = static_cast<uint16_t>(idx);
    } else if (elemKind == RTK_Class && pElemType) {
        int idx = m_compiledModule.FindClass(pElemType->Name());
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

//Phase 8e-9b: collect every enum declaration's value-name table into
//m_compiledModule.enumNames, and remember each SnEnumDecl*'s defIdx for
//later codegen (SnCastExpr src-type lookup). Enum declarations may live
//at namespace top level or nested inside class/struct scopes (handled by
//CanBeFuncParentEx, same pattern as RegisterStructs/RegisterClasses).
void VmBackend::RegisterEnums(SnNamespace& root) {
    m_enumIndexMap.clear();
    m_compiledModule.enumNames.clear();
    auto registerEnum = [&](SnEnumDecl& sn) {
        auto defIdx = m_compiledModule.enumNames.size();
        std::vector<std::string> names;
        for (auto& member : sn.Members()) {
            if (member.Kind() == NK_EnumMember) {
                auto& em = static_cast<SnEnumMember&>(member);
                //Enum values are sequential starting at 0; if user provides
                //explicit values that skip numbers, the corresponding slots
                //are filled with empty strings (OP_Enum_to_str will throw
                //"enum value out of range" at runtime if hit). NLang grammar
                //currently only supports implicit sequential values.
                while (names.size() <= static_cast<size_t>(em.Value()))
                    names.push_back(std::string());
                names[static_cast<size_t>(em.Value())] = em.Name();
            }
        }
        m_compiledModule.enumNames.push_back(std::move(names));
        m_enumIndexMap[&sn] = defIdx;
    };
    ForEachDeclNode(root, [&](SnField& node) {
        if (node.Kind() == NK_EnumDecl && !node.IsImported())
            registerEnum(static_cast<SnEnumDecl&>(node));
    });
}

//v1.9 (debugger): source file path recorded per function. Imported
//stubs carry a location whose TransUnit() is null and are normally
//filtered by the body-less check inside RegisterFunctions; the null
//guards are defensive cover for anything that slips through.
static std::string SourceFilePathOf(const SnFunction& func) {
    auto* pLoc = func.Location();
    if (!pLoc) return std::string();
    auto* pTu = pLoc->TransUnit();
    return pTu ? pTu->FilePath() : std::string();
}

void VmBackend::RegisterFunctions(SnNamespace& root) {
    m_funcIndexMap.clear();
    ForEachDeclNode(root, [&](SnField& node) {
        if (node.Kind() != NK_Function)
            return;
        auto& func = static_cast<SnFunction&>(node);
        //Phase 9f: native declarations register like normal functions
        //(the record carries isNative + param signature); a body-less
        //non-native declaration gets no record.
        if (!func.Body() && !func.ContainFlags(NF_Native))
            return;
        CompiledFunction cf;
        cf.name = QualifiedFunctionName(func);
        cf.sourceFile = SourceFilePathOf(func);
        m_compiledModule.functions.push_back(std::move(cf));
        m_funcIndexMap[&func] = m_compiledModule.functions.size() - 1;
    });
}
} //namespace nlang
