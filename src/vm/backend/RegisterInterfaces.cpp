/*---
    RegisterInterfaces.cpp — 0.9.10 D5：接口静态成员的单元级登记。
    接口静态方法是接口表跨模块导出的唯一内容（实例成员需要接收者，
    消费端桩无法重建），登记形态与 enumKeys 同构：限定键 + 静态函数
    下标。从 RegisterClass.cpp 分文件（一表一 TU，尺寸守卫余量）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/SyntaxTree.h>
#include <nlang/compiler/SnMisc.h>

namespace nlang {

void VmBackend::RegisterInterfaces(SnNamespace& root) {
    m_compiledModule.interfaces.clear();
    ForEachDeclNode(root, [&](SnField& node) {
        //Per-unit: foreign units' interfaces belong to their own images;
        //imported stubs contribute nothing (the consumer rebuilds them
        //from the peer unit's table).
        if (node.Kind() != NK_InterfaceDecl || node.IsImported()
            || !IsOwnUnit(node))
            return;
        auto& sn = static_cast<SnInterfaceDecl&>(node);
        CompiledInterface ci;
        ci.name = KeyOf(sn);
        for (auto& member : sn.Members()) {
            if (member.Kind() != NK_Function)
                continue;
            auto& func = static_cast<SnFunction&>(member);
            //Only statics register function records (abstract instance
            //members are body-less and skipped by RegisterFunctions);
            //the map lookup is the registration filter of record.
            auto it = m_funcIndexMap.find(&func);
            if (it == m_funcIndexMap.end())
                continue;
            ci.staticMethodIndices.push_back(
                static_cast<uint16_t>(it->second));
        }
        m_compiledModule.interfaces.push_back(std::move(ci));
    });
}

} //namespace nlang
