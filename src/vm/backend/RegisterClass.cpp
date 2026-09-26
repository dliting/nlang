/*---
    RegisterClass.cpp — 类注册族：内建异常字段、继承/自有字段收集、
    类元数据解析、隐式 Object 继承与类方法表填充。
    从 Register.cpp 拆出（2026-09-25 可维护性重构，零行为变化）。
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
