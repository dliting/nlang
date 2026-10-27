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
#include <nlang/compiler/GenericInstanceRegistry.h>
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
//noReceiver marks the STATIC-face ABI: no `this` rides slot 0 — the
//intrinsic's arguments start at slot 0 (every receiver-dispatched row
//defaults to false: slot 0 = the instance).
struct BindingRow {
    const char* packagePath;
    const char* key;
    uint16_t intrinsicId;
    bool noReceiver = false;
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
    //0.10.21: ReentrantMutex — the declared reentrant companion.
    {"builtins", "ReentrantMutex",         INTR_ReentrantMutex_Ctor},
    {"builtins", "ReentrantMutex.lock",    INTR_ReentrantMutex_Lock},
    {"builtins", "ReentrantMutex.unlock",  INTR_ReentrantMutex_Unlock},
    {"builtins", "ReentrantMutex.tryLock", INTR_ReentrantMutex_TryLock},
    //0.10.22: AtomicInt — the declared atomic-integer family. One ctor
    //id: the declaration's default parameter stages the omitted arg.
    {"builtins", "AtomicInt",                   INTR_AtomicInt_Ctor},
    {"builtins", "AtomicInt.load",              INTR_AtomicInt_Load},
    {"builtins", "AtomicInt.store",             INTR_AtomicInt_Store},
    {"builtins", "AtomicInt.getAndSet",         INTR_AtomicInt_GetAndSet},
    {"builtins", "AtomicInt.compareExchange",   INTR_AtomicInt_CompareExchange},
    {"builtins", "AtomicInt.getAndAdd",         INTR_AtomicInt_GetAndAdd},
    {"builtins", "AtomicInt.addAndGet",         INTR_AtomicInt_AddAndGet},
    {"builtins", "AtomicInt.incrementAndGet",   INTR_AtomicInt_IncrementAndGet},
    {"builtins", "AtomicInt.getAndIncrement",   INTR_AtomicInt_GetAndIncrement},
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
    //Cluster 4 (Object): the three protocol methods. Declared virtual so
    //override propagation marks user overrides and call sites dispatch by
    //name; no ctor row (Object is never constructed by user code).
    {"builtins", "Object.equals",      INTR_Object_Equals},
    {"builtins", "Object.getHashCode", INTR_Object_GetHashCode},
    {"builtins", "Object.toString",    INTR_Object_toString},
    //Cluster 5 (Promise): declared generic template in stdlib/builtins.n;
    //the erased instance's members stamp through the same key shape
    //(erasure naming keys them bare "Promise"/"Promise.future"). The
    //completion intrinsics bind with Task 4 (complete /
    //completeExceptionally).
    {"builtins", "Promise",        INTR_Promise_Ctor},
    {"builtins", "Promise.future", INTR_Promise_Future},
    {"builtins", "Promise.complete", INTR_Promise_Complete},
    {"builtins", "Promise.completeExceptionally",
                                   INTR_Promise_CompleteExceptionally},
    //The intrinsic channel's first NO-RECEIVER row: Future.selectAny
    //is a static face on the type name, so no instance rides slot 0 —
    //the wait-set array is the sole argument. Today the compiler emits
    //the OP_CallIntrinsic directly (Future is still a synthetic class,
    //nothing declares this member); the row records the ABI for the
    //builtins source-of-truth migration, when a declared static native
    //Future.selectAny stamps onto this channel.
    {"builtins", "Future.selectAny", INTR_Future_SelectAny,
                                   /*noReceiver=*/true},
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
    "Mutex", "Channel", "Future", "Semaphore", "Promise",
    "ReentrantMutex",
    "AtomicInt",
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
