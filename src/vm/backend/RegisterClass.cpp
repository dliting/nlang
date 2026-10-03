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
    //Same-package duplicates are stopped by the compiler's
    //DuplicateFieldChecker before codegen runs, so the type tables keep
    //no second gate here; a cross-module same key IS the same type (the
    //load-time linker dedups peer images by qualified name).
    cc.name = KeyOf(sn);
    cc.superClassIdx = -1;
    CollectInheritedClassFields(sn, cc);
    CollectOwnClassFields(sn, cc);
    cc.fieldCount = static_cast<uint16_t>(cc.fieldNames.size());
    declMap[cc.name] = &sn;   //key = the qualified name just written
    m_compiledModule.classes.push_back(std::move(cc));
}

//Resolve each field's struct/class table index through the AST field's
//resolved type. classIdx (not a CompiledClass&): slot resolution appends
//import placeholders to m_compiledModule.classes mid-loop, and a held
//element reference would dangle across the reallocation.
void VmBackend::ResolveClassFieldRefs(SnClassDecl* pDecl, size_t classIdx) {
    //SlotFor appends whole classes; an existing class's field lists are
    //never touched, so the field bound is stable across the loop.
    const size_t fieldCount =
        m_compiledModule.classes[classIdx].fieldNames.size();
    for (size_t i = 0; i < fieldCount; ++i) {
        auto* pField = pDecl->FindField(
            m_compiledModule.classes[classIdx].fieldNames[i]);
        if (pField && pField->Kind() == NK_ClassField) {
            auto* ft = pField->EvalDataType();
            //Per-unit: node-based slot resolution — a cross-unit field
            //type lands as an import placeholder instead of a silent
            //lookup miss. Kind-gated so the casts are safe; generic
            //instantiations are ownerless and resolve through the own
            //branch to the built-in entry. Slot computed before the
            //write statement so no receiver predates the append.
            if (m_compiledModule.classes[classIdx].fieldTypeKinds[i]
                    == RTK_Class
                && ft && ft->Kind() == NK_ClassDecl) {
                uint16_t slot = static_cast<uint16_t>(
                    ClassSlotFor(static_cast<SnClassDecl&>(*ft)));
                m_compiledModule.classes[classIdx]
                    .fieldClassIndices[i] = slot;
            } else if (m_compiledModule.classes[classIdx]
                    .fieldTypeKinds[i] == RTK_Struct
                && ft && ft->Kind() == NK_StructDecl) {
                uint16_t slot = static_cast<uint16_t>(
                    StructSlotFor(static_cast<SnStructDecl&>(*ft)));
                m_compiledModule.classes[classIdx]
                    .fieldStructIndices[i] = slot;
            }
        }
    }
}

//v1.12: field type descriptors piggyback on the resolution loop
//(all classes are registered, so BuildTypeDesc's name lookups
//succeed). Inherited fields resolve through pDecl's ancestor
//chain; the synthesized Exception-family fields (message /
//backtrace) have no AST node and degrade from the recorded kind
//byte + class index. Cross-unit classes are placeholder records
//absent from declMap — they never reach here; their descriptors
//live in their own unit's image.
//classIdx instead of CompiledClass& for the same reallocation
//reason as ResolveClassFieldRefs.
void VmBackend::BuildClassFieldTypeDescs(SnClassDecl* pDecl, size_t classIdx) {
    const size_t fieldCount =
        m_compiledModule.classes[classIdx].fieldNames.size();
    for (size_t i = 0; i < fieldCount; ++i) {
        auto* pField = pDecl->FindField(
            m_compiledModule.classes[classIdx].fieldNames[i]);
        if (pField && pField->Kind() == NK_ClassField) {
            //Desc computed before the push_back statement: a leaf slot
            //inside BuildTypeDesc can append placeholders to classes,
            //and a member-call receiver must not predate that append.
            TypeDesc td = BuildTypeDesc(pField->EvalDataType(),
                                        LeafSlotResolvers());
            m_compiledModule.classes[classIdx].fieldTypeDescs.push_back(td);
            continue;
        }
        TypeDesc td;
        if (m_compiledModule.classes[classIdx].fieldTypeKinds[i]
                == RTK_String)
            td.kind = RTK_String;
        else if (m_compiledModule.classes[classIdx].fieldTypeKinds[i]
                     == RTK_Class
            && m_compiledModule.classes[classIdx]
                     .fieldClassIndices[i] != 0xFFFF) {
            td.kind = RTK_Class;
            td.typeIdx = m_compiledModule.classes[classIdx]
                .fieldClassIndices[i];
        }
        m_compiledModule.classes[classIdx].fieldTypeDescs
            .push_back(td);  //default = NonSerialized
    }
}

//Resolve superClassIdx and fieldClassIndices (requires all classes registered).
void VmBackend::ResolveClassMetadata(
    std::unordered_map<std::string, SnClassDecl*>& declMap) {
    //Index loop bounded by the table size at entry: ClassSlotFor below
    //appends import placeholders to m_compiledModule.classes, so a
    //range-for's iterator would dangle and an unbounded index loop would
    //descend into the appended placeholders' (empty) metadata. The
    //snapshot bound also covers builtins — they miss declMap and
    //continue, unchanged semantics.
    const size_t registeredCount = m_compiledModule.classes.size();
    for (size_t ci = 0; ci < registeredCount; ++ci) {
        auto it = declMap.find(m_compiledModule.classes[ci].name);
        if (it == declMap.end()) continue;
        auto* pDecl = it->second;
        if (pDecl->SuperClass()) {
            //Per-unit: a cross-unit parent slots as an import
            //placeholder, keeping the subclass record's parent link
            //unit-local for nlink to resolve. Ownerless parents
            //(built-in Exception family, implicit Object) resolve
            //through the own branch as before.
            int16_t superIdx = static_cast<int16_t>(
                ClassSlotFor(*pDecl->SuperClass()));
            m_compiledModule.classes[ci].superClassIdx = superIdx;
        }
        ResolveClassFieldRefs(pDecl, ci);
        BuildClassFieldTypeDescs(pDecl, ci);
    }
}

//Phase 8e-1: implicit Object inheritance. Every user class with no explicit
//parent inherits from Object. The only class that keeps superClassIdx==-1
//is Object itself (already registered in RegisterBuiltinClasses).
//ByteStream/FileStream (built-in) get Object as their parent too.
void VmBackend::ApplyImplicitObjectInheritance() {
    if (m_objectClassIdx >= 0) {
        for (auto& cc : m_compiledModule.classes) {
            //Bare-name comparison = builtin: the synthesized Object has no
            //owner tag, so its key stays bare "Object"; a user `pkg.Object`
            //is keyed with its package and correctly still gets an
            //implicit Object parent (it is not the builtin root).
            if (cc.superClassIdx == -1 && cc.name != "Object") {
                cc.superClassIdx = m_objectClassIdx;
            }
        }
    }
}

void VmBackend::RegisterClasses(SnNamespace& root) {
    //Map from class name to SnClassDecl* for post-registration resolution.
    std::unordered_map<std::string, SnClassDecl*> declMap;
    ForEachDeclNode(root, [&](SnField& node) {
        //Per-unit: foreign units' classes belong to their own images;
        //a cross-unit reference slots as a placeholder at its use site.
        if (node.Kind() == NK_ClassDecl && !node.IsImported()
            && IsOwnUnit(node))
            RegisterClassDecl(static_cast<SnClassDecl&>(node), declMap);
    });
    ResolveClassMetadata(declMap);
    ApplyImplicitObjectInheritance();
}

void VmBackend::PopulateClassMethods(SnNamespace& root) {
    ForEachDeclNode(root, [&](SnField& node) {
        if (node.Kind() != NK_ClassDecl)
            return;
        //Per-unit: a class stub has no AST methods, and foreign units'
        //classes keep their own image's method table — neither
        //contributes here.
        if (node.IsImported() || !IsOwnUnit(node))
            return;
        auto& sn = static_cast<SnClassDecl&>(node);
        int ccIdx = m_compiledModule.FindClass(KeyOf(sn));
        if (ccIdx < 0) return;
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
    });
}

} //namespace nlang
