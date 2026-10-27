/*---
    RegisterStructs.cpp — 结构体登记族：RegisterStructDecl/RegisterStructs
    （结构体表填充）与 ResolveStructClassRefs（类/类型描述回填）。
    从 Register.cpp 拆出（master 合并后尺寸守卫触发的拆分，沿用
    一族一 TU 惯例）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SyntaxTree.h>
#include "builder/ModuleRegistry.h"

namespace nlang {

void VmBackend::RegisterStructDecl(SnStructDecl& sn) {
    CompiledStruct cs;
    //Same-package duplicates are stopped by the compiler's
    //DuplicateFieldChecker before codegen runs, so the type tables keep
    //no second gate here; a cross-module same key IS the same type (the
    //load-time linker dedups peer images by qualified name).
    cs.name = KeyOf(sn);
    cs.fieldCount = static_cast<uint16_t>(sn.FieldCount());
    std::vector<SnField*> fieldTypes;
    for (auto& field : sn.Fields()) {
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
        fieldTypes.push_back(fieldType);
        //0.9.11 D4: the field initializer folds through the same
        //ExtractDefaultValue channel as function defaults (nullptr →
        //RTK_Void sentinel; string literals intern into the producer's
        //pool and the import merge remaps them). The declaration-side
        //check (FieldInitializerChecks.hpp) admits only the constant-
        //foldable subset, so no RTK_Expr/RTK_Unfoldable tag can appear
        //here; the vector stays dense (one entry per field). ftk is the
        //declared carrier the materializer reads the slot back by.
        cs.fieldDefaults.push_back(
            ExtractDefaultValue(field.Initializer(),
                                static_cast<uint8_t>(ftk)));
    }
    m_compiledModule.structs.push_back(std::move(cs));
    m_structFieldTypes.push_back(std::move(fieldTypes));
}

void VmBackend::RegisterStructs(SnNamespace& root) {
    ForEachDeclNode(root, [&](SnField& node) {
        //Per-unit: foreign units' structs belong to their own images
        //(IsOwnUnit; stubs were already skipped by IsImported).
        if (node.Kind() == NK_StructDecl && !node.IsImported()
            && IsOwnUnit(node))
            RegisterStructDecl(static_cast<SnStructDecl&>(node));
    });
    //Resolve fieldStructIndices now that all structs are registered.
    //fieldClassIndices are resolved later by ResolveStructClassRefs
    //(after RegisterClasses, since classes are not yet registered here).
    //The node lists are parallel to the OWN struct entries only, so the
    //loop is bounded by them: slot resolution below appends placeholder
    //entries for cross-unit field types, growing structs past the list.
    //Element access is by fresh indexing per statement — StructSlotFor
    //can reallocate m_compiledModule.structs, and a held element
    //reference would dangle across it.
    for (size_t si = 0; si < m_structFieldTypes.size(); ++si) {
        auto& fieldTypes = m_structFieldTypes[si];
        for (size_t i = 0; i < fieldTypes.size(); ++i) {
            auto* ft = fieldTypes[i];
            if (m_compiledModule.structs[si].fieldTypeKinds[i]
                    == RTK_Struct
                && ft && ft->Kind() == NK_StructDecl) {
                uint16_t slot = static_cast<uint16_t>(
                    StructSlotFor(static_cast<SnStructDecl&>(*ft)));
                m_compiledModule.structs[si].fieldStructIndices[i] = slot;
            }
        }
    }
}


void VmBackend::ResolveStructClassRefs() {
    //Own-struct entries only (the node lists are parallel to them);
    //placeholder entries have no field data of their own. Fresh per-
    //statement indexing for the same reallocation reason as
    //RegisterStructs (ClassSlotFor appends to classes, and a struct
    //leaf inside BuildTypeDesc appends to structs).
    for (size_t si = 0; si < m_structFieldTypes.size(); ++si) {
        auto& fieldTypes = m_structFieldTypes[si];
        for (size_t i = 0; i < fieldTypes.size(); ++i) {
            auto* ft = fieldTypes[i];
            if (m_compiledModule.structs[si].fieldTypeKinds[i]
                    == RTK_Class
                && ft && ft->Kind() == NK_ClassDecl) {
                uint16_t slot = static_cast<uint16_t>(
                    ClassSlotFor(static_cast<SnClassDecl&>(*ft)));
                m_compiledModule.structs[si].fieldClassIndices[i] = slot;
            }
        }
        //v1.12: field type descriptors — built here because both the
        //struct and class tables are complete (BuildTypeDesc slots
        //leaves through them). The desc is computed before the
        //push_back statement: a struct leaf slots through
        // LeafSlotResolvers, and the member call's receiver must not be
        //a stale reference from before that append.
        for (size_t i = 0; i < fieldTypes.size(); ++i) {
            TypeDesc td = BuildTypeDesc(fieldTypes[i], LeafSlotResolvers());
            m_compiledModule.structs[si].fieldTypeDescs.push_back(td);
        }
    }
}

} //namespace nlang
