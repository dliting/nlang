/*---
    RegisterClass.cpp — 类注册族：自有字段收集、类元数据解析、
    隐式 Object 继承与类方法表（含访问级别/虚标志）填充。
    从 Register.cpp 拆出（2026-09-25 可维护性重构，零行为变化）。
    0.9.12 (v2.7)：字段列表改为仅自有字段，继承字段由运行期沿
    superClassIdx 链拼接（VmClassLayout），线格式不再重复携带。
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

//0.9.12 (v2.7): the class record carries OWN instance fields only.
//Inherited fields are no longer flattened here — the runtime view
//(VmClassLayout) joins the superClassIdx chain base-first, and the
//compile-time offset walk (FindClassFieldOffset) already reads the
//ancestor AST. The built-in Exception family's message/backtrace pair
//lives on the Exception base record (BuiltinClasses.cpp), so user
//subclasses see it through the chain instead of a per-subclass copy.
void VmBackend::CollectOwnClassFields(SnClassDecl& sn, CompiledClass& cc) {
    //A marked prelude class's record leads with the hidden __handle —
    //the fixed cell[1] slot the intrinsic stores and ctors write
    //(source-of-truth design §6.4). Not an AST member: FindField misses
    //it, so the post-registration passes keep the default (non-
    //serialized) descriptor, and the runtime layout view (VmClassLayout)
    //joins the slot through these very arrays.
    if (IsHandleClassDecl(sn)) {
        cc.fieldNames.push_back("__handle");
        cc.fieldTypeKinds.push_back(RTK_Int32);
        cc.fieldStructIndices.push_back(0xFFFF);
        cc.fieldClassIndices.push_back(0xFFFF);
        cc.fieldAccess.push_back(0);  //private / accessible via field ref
    }
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

//Build one CompiledClass: own fields only (0.9.12 v2.7 — inherited
//fields ride the superClassIdx chain, not the record), then register
//in declMap for the post-registration resolution pass.
void VmBackend::RegisterClassDecl(SnClassDecl& sn,
    std::unordered_map<std::string, SnClassDecl*>& declMap) {
    CompiledClass cc;
    //Same-package duplicates are stopped by the compiler's
    //DuplicateFieldChecker before codegen runs, so the type tables keep
    //no second gate here; a cross-module same key IS the same type (the
    //load-time linker dedups peer images by qualified name).
    cc.name = KeyOf(sn);
    cc.superClassIdx = -1;
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
//succeed). 0.9.12 (v2.7): records are own-only, so every field name
//resolves to an own AST node (FindField checks own members before
//the ancestor chain); the degrade path that synthesized descriptors
//for inherited/Exception-family entries is gone. Cross-unit classes
//are placeholder records absent from declMap — they never reach
//here; their descriptors live in their own unit's image.
//classIdx instead of CompiledClass& for the same reallocation
//reason as ResolveClassFieldRefs.
void VmBackend::BuildClassFieldTypeDescs(SnClassDecl* pDecl, size_t classIdx) {
    const size_t fieldCount =
        m_compiledModule.classes[classIdx].fieldNames.size();
    for (size_t i = 0; i < fieldCount; ++i) {
        auto* pField = pDecl->FindField(
            m_compiledModule.classes[classIdx].fieldNames[i]);
        //Desc computed before the push_back statement: a leaf slot
        //inside BuildTypeDesc can append placeholders to classes,
        //and a member-call receiver must not predate that append.
        TypeDesc td;  //default = NonSerialized
        if (pField && pField->Kind() == NK_ClassField)
            td = BuildTypeDesc(pField->EvalDataType(),
                               LeafSlotResolvers());
        m_compiledModule.classes[classIdx].fieldTypeDescs.push_back(td);
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
        //A shadowed prelude declaration does not register (spec §6.8 —
        //the same-name user declaration is the visible winner).
        if (node.Kind() == NK_ClassDecl && !node.IsImported()
            && IsOwnUnit(node) && !IsShadowedPreludeDecl(node))
            RegisterClassDecl(static_cast<SnClassDecl&>(node), declMap);
    });
    ResolveClassMetadata(declMap);
    ApplyImplicitObjectInheritance();
}

//PopulateClassMethods worker: one class's method/ctor tables from its AST
//members in declaration order (keys on m_funcIndexMap, hence a member).
void VmBackend::AppendClassMemberMethods(SnClassDecl& sn, CompiledClass& cc) {
    for (auto& child : sn.Members()) {
        if (child.Kind() != NK_Function)
            continue;
        auto it = m_funcIndexMap.find(static_cast<SnFunction*>(&child));
        if (it == m_funcIndexMap.end())
            continue;
        uint16_t funcIdx = static_cast<uint16_t>(it->second);
        //0.9.12 (v2.7): the method table carries every member function —
        //static and instance — with its declared access and virtuality in
        //parallel byte arrays, so a consumer stub rebuilds the same
        //visibility the producer compiled. The v2.4 public-static index
        //block is gone: staticness already rides CompiledFunction
        //.isStatic, and private statics now travel on the wire for the
        //consumer's contract checks (they stay call-invisible through the
        //rebuilt stub's access).
        cc.methodIndices.push_back(funcIdx);
        cc.methodAccess.push_back(
            static_cast<uint8_t>(child.AccessType()));
        cc.methodVirtual.push_back(
            child.ContainFlags(NF_Virtual) ? 1 : 0);
        //0.9.10 D2.4: a same-name static method is not the constructor
        //(declaration order must not matter). 0.9.11 (v2.6): plural
        //record in declaration order — the ordinal within this vector is
        //the cross-module ownerOrdinal selector (overloads share the bare
        //name). 0.9.12 (v2.7): constructorAccess parallels it so the
        //consumer rebuilds each ctor with its declared access.
        if (child.Name() == sn.Name()
            && !child.ContainFlags(NF_Static))
        {
            cc.constructorIndices.push_back(funcIdx);
            cc.constructorAccess.push_back(
                static_cast<uint8_t>(child.AccessType()));
        }
    }
}

void VmBackend::PopulateClassMethods(SnNamespace& root) {
    ForEachDeclNode(root, [&](SnField& node) {
        if (node.Kind() != NK_ClassDecl)
            return;
        //Per-unit: a class stub has no AST methods, and foreign units'
        //classes keep their own image's method table — neither
        //contributes here. A shadowed prelude class skips entirely:
        //this pass CLEAR-AND-REFILLS the tables it touches, so letting a
        //shadowed prelude class through would replace the user class's
        //method table with the (retired) prelude one.
        if (node.IsImported() || !IsOwnUnit(node)
            || IsShadowedPreludeDecl(node))
            return;
        auto& sn = static_cast<SnClassDecl&>(node);
        int ccIdx = m_compiledModule.FindClass(KeyOf(sn));
        if (ccIdx < 0) return;
        auto& cc = m_compiledModule.classes[static_cast<size_t>(ccIdx)];
        cc.methodIndices.clear();
        cc.methodAccess.clear();
        cc.methodVirtual.clear();
        cc.constructorIndices.clear();
        cc.constructorAccess.clear();
        AppendClassMemberMethods(sn, cc);
    });
}

//0.9.11 D2: struct constructors are registered bare-name members (a
//struct declares no general methods), so the declaration-order ordinal
//within constructorIndices is the only stable cross-module selector
//(SymbolImport::ownerOrdinal). Only registered functions — a body or a
//native binding — own a function record; m_funcIndexMap is exactly that
//set, so an unregistered ctor contributes nothing.
void VmBackend::PopulateStructMethods(SnNamespace& root) {
    ForEachDeclNode(root, [&](SnField& node) {
        if (node.Kind() != NK_StructDecl)
            return;
        //Per-unit: a struct stub has no AST members, and foreign units'
        //structs keep their own image's metadata — neither contributes.
        if (node.IsImported() || !IsOwnUnit(node))
            return;
        auto& sn = static_cast<SnStructDecl&>(node);
        int csIdx = m_compiledModule.FindStruct(KeyOf(sn));
        if (csIdx < 0) return;
        auto& cs = m_compiledModule.structs[static_cast<size_t>(csIdx)];
        cs.constructorIndices.clear();
        cs.constructorAccess.clear();
        for (auto& child : sn.Ctors()) {
            auto it = m_funcIndexMap.find(static_cast<SnFunction*>(&child));
            if (it != m_funcIndexMap.end()) {
                cs.constructorIndices.push_back(
                    static_cast<uint16_t>(it->second));
                //0.9.12 (v2.7): declared access parallel to the index
                //vector — the consumer stub rebuilds each ctor with its
                //visibility (a private struct ctor stays private across
                //the module border).
                cs.constructorAccess.push_back(
                    static_cast<uint8_t>(child.AccessType()));
            }
        }
    });
}

} //namespace nlang
