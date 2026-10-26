/*---
    BindingTable.cpp — the intrinsic binding table (builtin-decls
    source-of-truth design §4.5) and the prelude shadowed-name pass
    (§6.8). Split from VmBackend.cpp when the table landed (Task 1);
    the builtin-class clusters append their binding rows here as their
    declarations land in stdlib/builtins.n.
---*/
#include "VmBackend.h"
#include "builder/ModuleRegistry.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/vm/Intrinsics.h>
#include <cassert>
#include <set>

namespace nlang {

//--- intrinsic binding table ------------------------------------------------

//Rows: VM-implemented declarations by owning package + key. The thread
//rows are the FindThreadIntrinsicId table migrated into the general
//binding table (Task 1 of the builtin-decls source-of-truth plan). A
//declaration outside the table keeps host-native dispatch (the
//ordinary native channel).
struct BindingRow {
    const char* packagePath;
    const char* key;
    uint16_t intrinsicId;
};

const BindingRow s_BindingRows[] = {
    {"thread", "thread.spawn", INTR_Thread_Spawn},
    {"thread", "thread.join",  INTR_Thread_Join},
    {"thread", "thread.yield", INTR_Thread_Yield},
    {"thread", "thread.id",    INTR_Thread_Id},
    {"thread", "thread.sleep", INTR_Thread_Sleep},
};

bool IsStampedBindingPackage(const std::string& packagePath)
{
    for (const auto& row : s_BindingRows)
        if (packagePath == row.packagePath)
            return true;
    return false;
}

uint16_t LookupIntrinsicId(const std::string& packagePath,
    const std::string& key)
{
    for (const auto& row : s_BindingRows)
        if (packagePath == row.packagePath && key == row.key)
            return row.intrinsicId;
    return INTR_None;
}

//--- prelude shadowing ------------------------------------------------------

void VmBackend::ComputeShadowedPreludeNames(SnNamespace& root)
{
    m_shadowedPreludeNames.clear();
    std::set<std::string> preludeNames;
    std::set<std::string> foreignNames;
    for (auto& member : root.Members())
    {
        if (member.Kind() != NK_Function && member.Kind() != NK_ClassDecl
            && member.Kind() != NK_StructDecl
            && member.Kind() != NK_EnumDecl
            && member.Kind() != NK_InterfaceDecl)
            continue;
        const uint32_t owner = m_pRegistry->OwnerOf(member);
        if (owner != ModuleRegistry::NO_OWNER
            && m_pRegistry->IsPreludeModule(owner))
            preludeNames.insert(member.Name());
        else
            foreignNames.insert(member.Name());
    }
    for (const auto& name : preludeNames)
    {
        if (foreignNames.count(name) > 0)
            m_shadowedPreludeNames.insert(name);
    }
}

bool VmBackend::IsShadowedPreludeDecl(const SnField& node) const
{
    if (m_shadowedPreludeNames.count(node.Name()) == 0)
        return false;
    const uint32_t owner = m_pRegistry->OwnerOf(node);
    return owner != ModuleRegistry::NO_OWNER
        && m_pRegistry->IsPreludeModule(owner);
}

} //namespace nlang
