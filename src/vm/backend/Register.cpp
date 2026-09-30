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
#include <nlang/compiler/Logger.h>
#include <nlang/runtime/NodeConsts.h>
#include "builder/ModuleRegistry.h"
#include "BuildEnvironment.h"
#include <cassert>
#include <vector>

namespace nlang {
namespace {

//v1.9 (debugger): source file path recorded per function. Imported
//stubs carry a location whose TransUnit() is null and are normally
//filtered by the body-less check inside RegisterFunctions; the null
//guards are defensive cover for anything that slips through. Hoisted
//here (phase 5) so the entry-point scan can name candidate files too.
static std::string SourceFilePathOf(const SnFunction& func) {
    auto* pLoc = func.Location();
    if (!pLoc) return std::string();
    auto* pTu = pLoc->TransUnit();
    return pTu ? pTu->FilePath() : std::string();
}

} // namespace

//Canonical VM key of a declaration: "<package>.<name>", or the bare name
//when the node carries no owner tag. The package comes from the compile-
//time registry (path-derived), NOT from an AST namespace walk — a walk can
//only name a unit that literally wrote `namespace <path>`, and project
//members hang off the root, so every project key would stay bare.
//A CLASS/INTERFACE/ENUM METHOD always keeps a bare name, even when its
//owning type is package-nested: methods dispatch by name through their
//receiver (VmExecutor::FindMethodByName compares the bare name), so a
//package prefix would make every lookup fail.
//A synthetic generic instantiation keeps its ERASED builtin key ("List"
//for List<int>): the backing CompiledClass is registered once under the
//base name, and the instantiation node is ownerless, so the registry
//would otherwise degrade to its display name ("List<int>") — not a key.
//File-local to Register.cpp; VmBackend::KeyOf is the only exported
//spelling.
static std::string QualifiedName(const ModuleRegistry& reg, const SnField& field) {
    if (field.Kind() == NK_ClassDecl) {
        auto& cls = static_cast<const SnClassDecl&>(field);
        if (cls.IsGenericInstantiation())
            return cls.BaseName();
    }
    if (const SyntaxNode* pParent = field.Parent();
        pParent && (pParent->Kind() == NK_ClassDecl
            || pParent->Kind() == NK_InterfaceDecl
            || pParent->Kind() == NK_EnumDecl))
        return field.Name();
    return reg.QualifiedName(field);
}

//Codegen-side accessor: every name written into or looked up in the VM
//tables goes through here, so the spelling cannot drift from what the
//resolver reports. Requires the registry injected by ModuleBuilder.
std::string VmBackend::KeyOf(const SnField& field) const {
    assert(m_pRegistry && "VmBackend::KeyOf before SetModuleRegistry");
    return QualifiedName(*m_pRegistry, field);
}

//Phase 5 D5, called at the FillNativeFunctionRecord call side: the host
//DLL is the first-dot segment (nlang_<seg>.dll), so a native in a
//multi-segment package cannot name one yet (phase 6 lifts the limit).
//Returns true (after logging) when the declaration must be refused.
bool VmBackend::RejectMultiSegmentNativePackage(SnFunction& func) {
    const std::string pkg = m_pRegistry->PackageOf(func);
    if (pkg.find('.') == std::string::npos)
        return false;
    m_pEnv->Log(CLL_Error,
        "native function '%s': multi-segment package '%s' cannot name a "
        "host DLL yet.", KeyOf(func).c_str(), pkg.c_str());
    return true;
}

void VmBackend::RegisterStructDecl(SnStructDecl& sn) {
    CompiledStruct cs;
    //Same-package duplicates are stopped by the compiler's
    //DuplicateFieldChecker before codegen runs, so the type tables keep
    //no second gate here; a cross-module same key IS the same type (see
    //MergeImportedTypeTables in Import.cpp).
    cs.name = KeyOf(sn);
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
            typeNames.push_back(KeyOf(*fieldType));
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
                BuildTypeDesc(fieldTypes[i], m_compiledModule, *m_pRegistry));
    }
}

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

//v1.9 (debugger): the source-file spelling lives in SourceFilePathOf
//(anonymous namespace, top of file) — shared with the entry-point scan.

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
        cf.name = KeyOf(func);
        cf.sourceFile = SourceFilePathOf(func);
        m_compiledModule.functions.push_back(std::move(cf));
        m_funcIndexMap[&func] = m_compiledModule.functions.size() - 1;
    });
}
//Entry = a root-level (non-method) `main` declared by a PROJECT unit.
//Libraries never provide the entry; a module with zero candidates keeps
//entryPoint = -1 and nvm reports "no entry point" instead of today's
//"no 'main' function found". Two candidates is a build error, not a
//first-wins tie-break: the keys are `a.main` and `b.main`, both legal,
//so only the source paths can tell the user which file they meant.
void VmBackend::ResolveEntryPoint(SnNamespace& root) {
    m_compiledModule.entryPoint = -1;
    std::vector<SnFunction*> candidates;
    for (auto& member : root.Members()) {
        if (member.Kind() != NK_Function)
            continue;
        auto& func = static_cast<SnFunction&>(member);
        if (func.Name() != "main")
            continue;
        //A method named main is never the entry: methods keep bare names
        //and dispatch through their receiver. (Direct root members cannot
        //be methods; the check keeps the rule explicit for a shell child.)
        auto* pParent = func.Parent();
        if (pParent && (pParent->Kind() == NK_ClassDecl
            || pParent->Kind() == NK_InterfaceDecl
            || pParent->Kind() == NK_EnumDecl))
            continue;
        //Libraries never provide the entry; an untagged node has no unit
        //to be a project member of.
        const uint32_t owner = m_pRegistry->OwnerOf(func);
        if (owner == ModuleRegistry::NO_OWNER
            || m_pRegistry->IsLibraryModule(owner))
            continue;
        candidates.push_back(&func);
    }
    if (candidates.empty())
        return;   //entryPoint stays -1: no entry point in this module
    if (candidates.size() > 1) {
        std::string msg = "entry point is ambiguous:";
        for (auto* pFunc : candidates) {
            msg += " '" + KeyOf(*pFunc) + "' ("
                + SourceFilePathOf(*pFunc) + ");";
        }
        msg += " keep exactly one main() in the project.";
        m_pEnv->Log(CLL_Error, "%s", msg.c_str());
        return;
    }
    SnFunction& entry = *candidates.front();
    if (entry.ContainFlags(NF_Native)) {
        //The native record exists, but executing the entry must not
        //silently dispatch through the host table: name the key.
        m_pEnv->Log(CLL_Error,
            "entry point '%s' is native: main must be a compiled function,"
            " not a host-table declaration.", KeyOf(entry).c_str());
        return;
    }
    m_compiledModule.entryPoint = m_compiledModule.FindFunction(KeyOf(entry));
}

} //namespace nlang
