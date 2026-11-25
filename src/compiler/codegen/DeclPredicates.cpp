/*---
    DeclPredicates.cpp — VmBackend's owner-gated declaration
    predicates: the handle-class certification (builtin-decls
    source-of-truth design §6.4; record insertion and the
    FieldOffsets mirror consult the same gate so the layouts cannot
    diverge) and the prelude shadowed-name pass (§6.8). Split from
    BindingTable.cpp when the binding table moved to the compiler
    (concurrency import redesign); both consult the table through
    builder/BindingTable.h.
---*/
#include "VmBackend.h"
#include "builder/ModuleRegistry.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/GenericInstanceRegistry.h>
#include <set>
#include <string>

namespace nlang {

//Owner-gated marker check (record insertion and the FieldOffsets mirror
//must consult the SAME predicate or the layouts diverge): a user class
//that merely shares the name after shadowing is not the builtin, so
//neither its record nor its offset walk reserves the handle slot.
//A declared-generic instantiation is a detached node — no owner tag of
//its own (OwnerOf is a pointer map) — so its handle-ness resolves
//through the template's owner: a prelude generic template's erased
//instances carry the hidden slot; a user shadow template's do not.
bool VmBackend::IsHandleClassDecl(const SnField& decl) const
{
    if (!IsHandleClassName(decl.Name()))
        return false;
    const SnField* pOwnerDecl = &decl;
    if (auto* pClass = dynamic_cast<const SnClassDecl*>(&decl))
    {
        if (pClass->IsGenericInstantiation())
        {
            pOwnerDecl = m_pRegistry->DeclaredGenerics().InstanceOf(pClass);
            if (!pOwnerDecl)
                return false;
        }
    }
    const uint32_t owner = m_pRegistry->OwnerOf(*pOwnerDecl);
    return IsBuiltinHandleDeclName(*m_pRegistry, owner, decl.Name());
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
        //Only a BARE-keyed foreign declaration collides with the
        //prelude's bare key (the main unit's root members key bare). A
        //packaged declaration keys qualified — "alib.Object" — and
        //coexists with the prelude root by design (source-of-truth
        //design §8, the object_key pin): counting it here would
        //suppress the prelude record and strand every SuperClass()
        //reference to it (ClassSlotFor throw).
        else if (m_pRegistry->QualifiedName(member) == member.Name())
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
