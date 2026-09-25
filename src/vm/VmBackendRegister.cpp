/*---
    VmBackendRegister.cpp — 类型与函数注册族（structs/classes/arrays/enums/functions）。
    从 VmBackend.cpp 抽取（2026-09-25 可维护性重构，零行为变化）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/compiler/ScriptLocation.h>
#include <nlang/compiler/TranslationUnit.h>
#include <nlang/runtime/NodeConsts.h>

namespace nlang {

//Build one CompiledStruct from its AST declaration and append the
//parallel field-name / field-type side lists.
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
    for (auto& member : root.Members()) {
        if (member.Kind() == NK_StructDecl) {
            if (member.IsImported()) continue;  //Phase 9c R3-F: skip stubs
            RegisterStructDecl(static_cast<SnStructDecl&>(member));
        } else if (CanBeFuncParentEx(member.Kind())) {
            for (auto& child : static_cast<SnFunctionParentField&>(member).Members()) {
                if (child.Kind() == NK_StructDecl) {
                    if (child.IsImported()) continue;  //Phase 9c R3-F
                    RegisterStructDecl(static_cast<SnStructDecl&>(child));
                }
            }
        }
    }
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

//Names of the built-in Exception family whose synthetic declarations
//carry no AST fields but a fixed two-field runtime layout.
static bool IsBuiltinExceptionName(const std::string& name) {
    return name == "Exception" || name == "NullPointerException"
        || name == "DivByZeroException"
        || name == "IndexOutOfBoundsException"
        || name == "AssertionException" || name == "IOException";
}

//Append the synthesized Exception-family layout: message (RTK_String)
//and backtrace (heap ref into List).
void VmBackend::AppendBuiltinExceptionFields(CompiledClass& cc) {
    cc.fieldNames.push_back("message");
    cc.fieldTypeKinds.push_back(RTK_String);
    cc.fieldStructIndices.push_back(0xFFFF);
    cc.fieldClassIndices.push_back(0xFFFF);
    cc.fieldAccess.push_back(0);
    cc.fieldNames.push_back("backtrace");
    cc.fieldTypeKinds.push_back(RTK_Class);
    cc.fieldStructIndices.push_back(0xFFFF);
    cc.fieldClassIndices.push_back(
        static_cast<uint16_t>(m_listClassIdx));
    cc.fieldAccess.push_back(0);
}

//Collect inherited fields: walk from root ancestor to direct parent,
//collecting each ancestor's own fields (not their inherited fields).
void VmBackend::CollectInheritedClassFields(SnClassDecl& sn, CompiledClass& cc) {
    std::vector<SnClassDecl*> ancestors;
    auto* pSuper = sn.SuperClass();
    while (pSuper) {
        ancestors.push_back(pSuper);
        pSuper = pSuper->SuperClass();
    }
    //Add fields from root ancestor first (reversed order)
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        //Phase 9d: a built-in Exception ancestor's synthetic decl has
        //no Members(), but its runtime layout carries 2 fields
        //(message=slot[1], backtrace=slot[2]). Inject them so user
        //subclasses of Exception get the correct flattened layout
        //(matching FindClassFieldOffset and the ctor intrinsic).
        if ((*it)->IsBuiltinClass()) {
            if (IsBuiltinExceptionName((*it)->Name())) {
                AppendBuiltinExceptionFields(cc);
                continue;
            }
        }
        for (auto& member : (*it)->Members()) {
            if (member.Kind() == NK_ClassField) {
                auto& cf = static_cast<SnClassField&>(member);
                cc.fieldNames.push_back(cf.Name());
                //0.7.3 B: array fields carry their interned token in
                //EvalDataType — RuntimeTypeKind files both arrays and
                //scalars (mirrors RegisterStructs).
                cc.fieldTypeKinds.push_back(
                    RuntimeTypeKind(cf.EvalDataType()));
                cc.fieldStructIndices.push_back(0xFFFF);
                cc.fieldClassIndices.push_back(0xFFFF);
                cc.fieldAccess.push_back(static_cast<uint8_t>(cf.AccessType()));
            }
        }
    }
}

void VmBackend::CollectOwnClassFields(SnClassDecl& sn, CompiledClass& cc) {
    //Collect own fields
    for (auto& member : sn.Members()) {
        if (member.Kind() == NK_ClassField) {
            auto& cf = static_cast<SnClassField&>(member);
            cc.fieldNames.push_back(cf.Name());
            //0.7.3 B: array fields carry their interned token in
            //EvalDataType — RuntimeTypeKind files both arrays and
            //scalars (mirrors RegisterStructs).
            cc.fieldTypeKinds.push_back(
                RuntimeTypeKind(cf.EvalDataType()));
            cc.fieldStructIndices.push_back(0xFFFF);
            cc.fieldClassIndices.push_back(0xFFFF);
            cc.fieldAccess.push_back(static_cast<uint8_t>(cf.AccessType()));
        }
    }
}

//Build one CompiledClass: inherited fields (ancestor chain), own fields,
//then register in declMap for the post-registration resolution pass.
void VmBackend::RegisterClassDecl(SnClassDecl& sn,
    std::unordered_map<std::string, SnClassDecl*>& declMap) {
    CompiledClass cc;
    cc.name = sn.Name();
    cc.superClassIdx = -1;
    CollectInheritedClassFields(sn, cc);
    CollectOwnClassFields(sn, cc);
    cc.fieldCount = static_cast<uint16_t>(cc.fieldNames.size());
    declMap[sn.Name()] = &sn;
    m_compiledModule.classes.push_back(std::move(cc));
}

//Resolve each field's struct/class table index through the AST field's
//resolved type.
void VmBackend::ResolveClassFieldRefs(SnClassDecl* pDecl, CompiledClass& cc) {
    for (size_t i = 0; i < cc.fieldNames.size(); ++i) {
        auto* pField = pDecl->FindField(cc.fieldNames[i]);
        if (pField && pField->Kind() == NK_ClassField) {
            auto* ft = pField->EvalDataType();
            if (ft) {
                if (cc.fieldTypeKinds[i] == RTK_Class) {
                    int idx = m_compiledModule.FindClass(ft->Name());
                    if (idx >= 0)
                        cc.fieldClassIndices[i] = static_cast<uint16_t>(idx);
                } else if (cc.fieldTypeKinds[i] == RTK_Struct) {
                    int idx = m_compiledModule.FindStruct(ft->Name());
                    if (idx >= 0)
                        cc.fieldStructIndices[i] = static_cast<uint16_t>(idx);
                }
            }
        }
    }
}

//v1.12: field type descriptors piggyback on the resolution loop
//(all classes are registered, so BuildTypeDesc's name lookups
//succeed). Inherited fields resolve through pDecl's ancestor
//chain; the synthesized Exception-family fields (message /
//backtrace) have no AST node and degrade from the recorded kind
//byte + class index. Phase-A-merged imported classes never reach
//here (absent from declMap) and keep their copied descriptors.
void VmBackend::BuildClassFieldTypeDescs(SnClassDecl* pDecl, CompiledClass& cc) {
    for (size_t i = 0; i < cc.fieldNames.size(); ++i) {
        auto* pField = pDecl->FindField(cc.fieldNames[i]);
        if (pField && pField->Kind() == NK_ClassField) {
            cc.fieldTypeDescs.push_back(
                BuildTypeDesc(pField->EvalDataType(), m_compiledModule));
            continue;
        }
        TypeDesc td;
        if (cc.fieldTypeKinds[i] == RTK_String)
            td.kind = RTK_String;
        else if (cc.fieldTypeKinds[i] == RTK_Class
            && cc.fieldClassIndices[i] != 0xFFFF) {
            td.kind = RTK_Class;
            td.typeIdx = cc.fieldClassIndices[i];
        }
        cc.fieldTypeDescs.push_back(td);  //default = NonSerialized
    }
}

//Resolve superClassIdx and fieldClassIndices (requires all classes registered).
void VmBackend::ResolveClassMetadata(
    std::unordered_map<std::string, SnClassDecl*>& declMap) {
    for (auto& cc : m_compiledModule.classes) {
        auto it = declMap.find(cc.name);
        if (it == declMap.end()) continue;
        auto* pDecl = it->second;
        if (pDecl->SuperClass()) {
            int idx = m_compiledModule.FindClass(pDecl->SuperClass()->Name());
            cc.superClassIdx = (idx >= 0) ? static_cast<int16_t>(idx) : -1;
        }
        ResolveClassFieldRefs(pDecl, cc);
        BuildClassFieldTypeDescs(pDecl, cc);
    }
}

//Phase 8e-1: implicit Object inheritance. Every user class with no explicit
//parent inherits from Object. The only class that keeps superClassIdx==-1
//is Object itself (already registered in RegisterBuiltinClasses).
//ByteStream/FileStream (built-in) get Object as their parent too.
void VmBackend::ApplyImplicitObjectInheritance() {
    if (m_objectClassIdx >= 0) {
        for (auto& cc : m_compiledModule.classes) {
            if (cc.superClassIdx == -1 && cc.name != "Object") {
                cc.superClassIdx = m_objectClassIdx;
            }
        }
    }
}

void VmBackend::RegisterClasses(SnNamespace& root) {
    //Map from class name to SnClassDecl* for post-registration resolution.
    std::unordered_map<std::string, SnClassDecl*> declMap;
    for (auto& member : root.Members()) {
        if (member.Kind() == NK_ClassDecl) {
            if (member.IsImported()) continue;  //Phase 9c R3-F: skip stubs
            RegisterClassDecl(static_cast<SnClassDecl&>(member), declMap);
        } else if (CanBeFuncParentEx(member.Kind())) {
            for (auto& child : static_cast<SnFunctionParentField&>(member).Members()) {
                if (child.Kind() == NK_ClassDecl) {
                    if (child.IsImported()) continue;  //Phase 9c R3-F
                    RegisterClassDecl(static_cast<SnClassDecl&>(child), declMap);
                }
            }
        }
    }
    ResolveClassMetadata(declMap);
    ApplyImplicitObjectInheritance();
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
    for (auto& member : root.Members()) {
        WalkArrayTypeNode(member);
        if (CanBeFuncParentEx(member.Kind())) {
            for (auto& child : static_cast<SnFunctionParentField&>(member).Members())
                WalkArrayTypeNode(child);
        }
    }
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
    for (auto& member : root.Members()) {
        if (member.Kind() == NK_EnumDecl) {
            if (member.IsImported()) continue;  //Phase 9c R3-F: skip stubs
            registerEnum(static_cast<SnEnumDecl&>(member));
        } else if (CanBeFuncParentEx(member.Kind())) {
            for (auto& child : static_cast<SnFunctionParentField&>(member).Members()) {
                if (child.Kind() == NK_EnumDecl) {
                    if (child.IsImported()) continue;  //Phase 9c R3-F
                    registerEnum(static_cast<SnEnumDecl&>(child));
                }
            }
        }
    }
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
    for (auto& member : root.Members()) {
        if (member.Kind() == NK_Function) {
            auto& func = static_cast<SnFunction&>(member);
            //Phase 9f: native declarations register like normal
            //functions (the record carries isNative + param signature).
            if (!func.Body() && !func.ContainFlags(NF_Native))
                continue;
            CompiledFunction cf;
            cf.name = func.Name();
            cf.sourceFile = SourceFilePathOf(func);
            m_compiledModule.functions.push_back(std::move(cf));
            m_funcIndexMap[&func] = m_compiledModule.functions.size() - 1;
        } else if (member.Kind() == NK_EnumDecl) {
            //Phase 12: enum methods. SnEnumDecl is not a
            //SnFunctionParentField — methods live in a separate
            //kind-filtered child list.
            for (auto& method : static_cast<SnEnumDecl&>(member).Methods()) {
                //Body-less methods were rejected by the resolver (D4);
                //skipping here only mirrors the class path's defense.
                if (!method.Body() && !method.ContainFlags(NF_Native))
                    continue;
                CompiledFunction cf;
                cf.name = method.Name();
                cf.sourceFile = SourceFilePathOf(method);
                m_compiledModule.functions.push_back(std::move(cf));
                m_funcIndexMap[&method] =
                    m_compiledModule.functions.size() - 1;
            }
        } else if (CanBeFuncParentEx(member.Kind())) {
            for (auto& child : static_cast<SnFunctionParentField&>(member).Members()) {
                if (child.Kind() == NK_Function) {
                    auto& func = static_cast<SnFunction&>(child);
                    if (!func.Body() && !func.ContainFlags(NF_Native))
                        continue;
                    CompiledFunction cf;
                    cf.name = func.Name();
                    cf.sourceFile = SourceFilePathOf(func);
                    m_compiledModule.functions.push_back(std::move(cf));
                    m_funcIndexMap[&func] = m_compiledModule.functions.size() - 1;
                }
            }
        }
    }
}

void VmBackend::PopulateClassMethods(SnNamespace& root) {
    for (auto& member : root.Members()) {
        if (member.Kind() == NK_ClassDecl) {
            if (member.IsImported()) continue;  //Phase 9c R6-1: stub has no AST methods; merged cc.methodIndices from Phase B must be preserved
            auto& sn = static_cast<SnClassDecl&>(member);
            int ccIdx = m_compiledModule.FindClass(sn.Name());
            if (ccIdx < 0) continue;
            auto& cc = m_compiledModule.classes[static_cast<size_t>(ccIdx)];
            cc.methodIndices.clear();
            cc.constructorIdx = 0xFFFF;
            for (auto& child : sn.Members()) {
                if (child.Kind() == NK_Function) {
                    auto it = m_funcIndexMap.find(static_cast<SnFunction*>(&child));
                    if (it != m_funcIndexMap.end()) {
                        uint16_t funcIdx = static_cast<uint16_t>(it->second);
                        cc.methodIndices.push_back(funcIdx);
                        if (child.Name() == sn.Name())
                            cc.constructorIdx = funcIdx;
                    }
                }
            }
        }
    }
}

} //namespace nlang
