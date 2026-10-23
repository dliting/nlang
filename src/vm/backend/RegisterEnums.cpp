/*---
    RegisterEnums.cpp — enum 表的单元级登记：名称表/限定键（v2.0）与
    成员方法下标/访问表（0.10.14 v2.9）。从 Register.cpp 分文件
    （一表一 TU，源尺寸守卫拆分，零行为变化）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/SyntaxTree.h>
#include <nlang/compiler/SnMisc.h>

namespace nlang {

//Phase 8e-9b: collect every enum declaration's value-name table into
//m_compiledModule.enumNames, and remember each SnEnumDecl*'s defIdx for
//later codegen (SnCastExpr src-type lookup). Enum declarations may live
//at namespace top level or nested inside class/struct scopes (handled by
//CanBeFuncParentEx, same pattern as RegisterStructs/RegisterClasses).
void VmBackend::RegisterEnums(SnNamespace& root) {
    m_enumIndexMap.clear();
    m_compiledModule.enumNames.clear();
    m_compiledModule.enumKeys.clear();
    //Placeholder slots are appended AFTER registration (codegen), so
    //clearing the import table here can never wipe a live placeholder —
    //it only keeps a premature one from leaving an orphan import entry.
    m_compiledModule.enumImports.clear();
    auto registerEnum = [&](SnEnumDecl& sn) {
        auto defIdx = m_compiledModule.enumNames.size();
        std::vector<std::string> names;
        for (auto& member : sn.Members()) {
            if (member.Kind() == NK_EnumMember) {
                auto& em = static_cast<SnEnumMember&>(member);
                //Enum values are sequential starting at 0; if user provides
                //explicit values that skip numbers, the corresponding slots
                //are filled with empty strings (OP_Enum_to_str will throw
                //"enum value out of range" at runtime if hit). Negative
                //values are rejected at resolve time; the guard keeps a
                //slipped-through negative from wrapping to SIZE_MAX and
                //hanging the table fill (defense, not a semantic path).
                if (em.Value() < 0)
                    continue;
                while (names.size() <= static_cast<size_t>(em.Value()))
                    names.push_back(std::string());
                names[static_cast<size_t>(em.Value())] = em.Name();
            }
        }
        m_compiledModule.enumNames.push_back(std::move(names));
        //v2.0: parallel qualified key (nlink name-addresses enum slots).
        m_compiledModule.enumKeys.push_back(KeyOf(sn));
        m_enumIndexMap[&sn] = defIdx;
    };
    ForEachDeclNode(root, [&](SnField& node) {
        //Per-unit: foreign units' enums belong to their own images.
        if (node.Kind() == NK_EnumDecl && !node.IsImported()
            && IsOwnUnit(node))
            registerEnum(static_cast<SnEnumDecl&>(node));
    });
}

//0.10.14 (v2.9): per-enum member-method index/access tables, parallel
//to enumNames/enumKeys. Mirrors PopulateStructMethods: only registered
//functions (a body or a native binding) own a function record, so
//m_funcIndexMap is the registration filter; the access byte rides the
//declared modifier (nlang.y EnumMethod carries one). Same walk + filter
//+ push order as RegisterEnums, so the four vectors stay parallel by
//construction.
void VmBackend::RegisterEnumMethods(SnNamespace& root) {
    m_compiledModule.enumMethodIndices.clear();
    m_compiledModule.enumMethodAccess.clear();
    ForEachDeclNode(root, [&](SnField& node) {
        if (node.Kind() != NK_EnumDecl || node.IsImported()
            || !IsOwnUnit(node))
            return;
        auto& sn = static_cast<SnEnumDecl&>(node);
        std::vector<uint16_t> indices;
        std::vector<uint8_t> access;
        for (auto& method : sn.Methods()) {
            auto it = m_funcIndexMap.find(&method);
            if (it == m_funcIndexMap.end())
                continue;
            indices.push_back(static_cast<uint16_t>(it->second));
            access.push_back(static_cast<uint8_t>(method.AccessType()));
        }
        m_compiledModule.enumMethodIndices.push_back(std::move(indices));
        m_compiledModule.enumMethodAccess.push_back(std::move(access));
    });
}

} // namespace nlang
