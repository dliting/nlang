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
    //Cluster 1 (Mutex/Semaphore): declared in stdlib/builtins.n. The
    //type-member stamping arm binds the declared native records onto
    //the intrinsic channel; the constructor key is the bare class name.
    {"builtins", "Mutex",                INTR_Mutex_Ctor},
    {"builtins", "Mutex.lock",           INTR_Mutex_Lock},
    {"builtins", "Mutex.unlock",         INTR_Mutex_Unlock},
    {"builtins", "Mutex.tryLock",        INTR_Mutex_TryLock},
    {"builtins", "Semaphore",            INTR_Semaphore_Ctor},
    {"builtins", "Semaphore.acquire",    INTR_Semaphore_Acquire},
    {"builtins", "Semaphore.release",    INTR_Semaphore_Release},
    {"builtins", "Semaphore.tryAcquire", INTR_Semaphore_TryAcquire},
    //Cluster 2 (Exception family): declared in stdlib/builtins.n, one
    //ctor row each. The VM raise paths keep their by-name class caches;
    //only the ctor call channel is stamped here.
    {"builtins", "Exception",                 INTR_Exception_Ctor},
    {"builtins", "NullPointerException",      INTR_NullPointerException_Ctor},
    {"builtins", "DivByZeroException",        INTR_DivByZeroException_Ctor},
    {"builtins", "IndexOutOfBoundsException", INTR_IndexOutOfBoundsException_Ctor},
    {"builtins", "AssertionException",        INTR_AssertionException_Ctor},
    {"builtins", "IOException",               INTR_IOException_Ctor},
    {"builtins", "InvalidCastException",      INTR_InvalidCastException_Ctor},
    {"builtins", "ChannelClosedException",    INTR_ChannelClosedException_Ctor},
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

//Handle-class marker (builtin-decls source-of-truth design §6.4): the
//prelude classes whose CompiledClass record leads with the hidden
//__handle field — the one by-name C++-side list that survives the
//migration. Append-only. Entries whose cluster has not been declared
//yet are inert: the marker fires only for a DECLARED prelude class.
const char* const s_HandleClassNames[] = {
    "ByteStream", "FileStream", "List", "Dict", "Thread",
    "Mutex", "Channel", "Future", "Semaphore",
};

bool IsHandleClassName(const std::string& className)
{
    for (const char* name : s_HandleClassNames)
        if (className == name)
            return true;
    return false;
}

//Owner-gated marker check (record insertion and the FieldOffsets mirror
//must consult the SAME predicate or the layouts diverge): a user class
//that merely shares the name after shadowing is not the builtin, so
//neither its record nor its offset walk reserves the handle slot.
bool VmBackend::IsHandleClassDecl(const SnField& decl) const
{
    if (!IsHandleClassName(decl.Name()))
        return false;
    const uint32_t owner = m_pRegistry->OwnerOf(decl);
    return owner != ModuleRegistry::NO_OWNER
        && m_pRegistry->IsPreludeModule(owner);
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
