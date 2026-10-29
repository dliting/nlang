/*---
    BindingTable.cpp — the intrinsic binding table (builtin-decls
    source-of-truth design §4.5) and the prelude shadowed-name pass
    (§6.8). Split from VmBackend.cpp when the table landed (Task 1);
    the builtin-class clusters append their binding rows here as their
    declarations land in stdlib/builtins.n.
---*/
#include "VmBackend.h"
#include "builder/ModuleRegistry.h"
#include "builder/BuiltinNames.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/GenericInstanceRegistry.h>
#include <nlang/vm/Intrinsics.h>
#include <cassert>
#include <set>
#include <string>
#include <vector>

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
    std::string packagePath;
    std::string key;
    uint16_t intrinsicId;
    bool noReceiver = false;
};

//The literal declared-source rows. The static-face rows are DERIVED
//from the face table in BindingRows() below — they never appear here.
static const BindingRow s_LiteralRows[] = {
    {"thread", "thread.spawn", INTR_Thread_Spawn},
    {"thread", "thread.join",  INTR_Thread_Join},
    {"thread", "thread.yield", INTR_Thread_Yield},
    {"thread", "thread.id",    INTR_Thread_Id},
    {"thread", "thread.sleep", INTR_Thread_Sleep},
    //P2-2: spawn-failure observability probes (non-blocking).
    {"thread", "thread.isFailed", INTR_Thread_IsFailed},
    {"thread", "thread.failure",  INTR_Thread_Failure},
    //Cluster 1 (Mutex/Semaphore): declared in stdlib/builtins.n. The
    //type-member stamping arm binds the declared native records onto
    //the intrinsic channel; ctor keys are arity-aware (zero-formal =
    //bare name, N-formal = "Class/N").
    {"builtins", "Mutex",                INTR_Mutex_Ctor},
    {"builtins", "Mutex.lock",           INTR_Mutex_Lock},
    {"builtins", "Mutex.unlock",         INTR_Mutex_Unlock},
    {"builtins", "Mutex.tryLock",        INTR_Mutex_TryLock},
    {"builtins", "Semaphore/1",          INTR_Semaphore_Ctor},
    {"builtins", "Semaphore.acquire",    INTR_Semaphore_Acquire},
    {"builtins", "Semaphore.release",    INTR_Semaphore_Release},
    {"builtins", "Semaphore.tryAcquire", INTR_Semaphore_TryAcquire},
    //0.10.21: ReentrantMutex — the declared reentrant companion.
    {"builtins", "ReentrantMutex",         INTR_ReentrantMutex_Ctor},
    {"builtins", "ReentrantMutex.lock",    INTR_ReentrantMutex_Lock},
    {"builtins", "ReentrantMutex.unlock",  INTR_ReentrantMutex_Unlock},
    {"builtins", "ReentrantMutex.tryLock", INTR_ReentrantMutex_TryLock},
    //0.10.22: AtomicInt — the declared atomic-integer family. One ctor
    //id: the declaration's default parameter stages the omitted arg (the
    ///1 key follows the arity-aware ctor rule).
    {"builtins", "AtomicInt/1",                 INTR_AtomicInt_Ctor},
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
    {"builtins", "Exception/1",                 INTR_Exception_Ctor},
    {"builtins", "NullPointerException/1",      INTR_NullPointerException_Ctor},
    {"builtins", "DivByZeroException/1",        INTR_DivByZeroException_Ctor},
    {"builtins", "IndexOutOfBoundsException/1", INTR_IndexOutOfBoundsException_Ctor},
    {"builtins", "AssertionException/1",        INTR_AssertionException_Ctor},
    {"builtins", "IOException/1",               INTR_IOException_Ctor},
    {"builtins", "InvalidCastException/1",      INTR_InvalidCastException_Ctor},
    {"builtins", "ChannelClosedException/1",    INTR_ChannelClosedException_Ctor},
    //Cluster 4 (Object): the three protocol methods. Declared virtual so
    //override propagation marks user overrides and call sites dispatch by
    //name; no ctor row (Object is never constructed by user code).
    {"builtins", "Object.equals",      INTR_Object_Equals},
    {"builtins", "Object.getHashCode", INTR_Object_GetHashCode},
    {"builtins", "Object.toString",    INTR_Object_toString},
    //Cluster 5 (Promise): declared generic template in stdlib/builtins.n;
    //the erased instance's members stamp through the same key shape
    //(erasure naming keys them bare "Promise"/"Promise.future").
    {"builtins", "Promise",        INTR_Promise_Ctor},
    {"builtins", "Promise.future", INTR_Promise_Future},
    {"builtins", "Promise.complete", INTR_Promise_Complete},
    {"builtins", "Promise.completeExceptionally",
                               INTR_Promise_CompleteExceptionally},
    //Cluster 6 (generic containers + futures): declared generic
    //templates in stdlib/builtins.n (phase 2). Method keys are
    //"Class.method"; ctor keys follow the arity-aware rule. Erasure
    //naming keys the instances bare ("List"/"List.add" — same shape as
    //Promise).
    {"builtins", "List",            INTR_List_Ctor},
    {"builtins", "List.add",        INTR_List_Add},
    {"builtins", "List.get",        INTR_List_Get},
    {"builtins", "List.set",        INTR_List_Set},
    {"builtins", "List.length",     INTR_List_Length},
    {"builtins", "List.removeAt",   INTR_List_RemoveAt},
    {"builtins", "List.indexOf",    INTR_List_IndexOf},
    {"builtins", "List.contains",   INTR_List_Contains},
    {"builtins", "List.clear",      INTR_List_Clear},
    {"builtins", "List.toString",   INTR_List_toString},
    {"builtins", "Dict",            INTR_Dict_Ctor},
    {"builtins", "Dict.set",        INTR_Dict_Set},
    {"builtins", "Dict.get",        INTR_Dict_Get},
    {"builtins", "Dict.containsKey", INTR_Dict_ContainsKey},
    {"builtins", "Dict.remove",     INTR_Dict_Remove},
    {"builtins", "Dict.clear",      INTR_Dict_Clear},
    {"builtins", "Dict.count",      INTR_Dict_Count},
    {"builtins", "Dict.keys",       INTR_Dict_Keys},
    {"builtins", "Dict.toString",   INTR_Dict_toString},
    {"builtins", "Channel",         INTR_Channel_CtorUnbounded},
    {"builtins", "Channel/1",       INTR_Channel_Ctor},
    {"builtins", "Channel.send",    INTR_Channel_Send},
    {"builtins", "Channel.recv",    INTR_Channel_Recv},
    {"builtins", "Channel.close",   INTR_Channel_Close},
    {"builtins", "Future.get",      INTR_Future_Get},
    {"builtins", "Future.isDone",   INTR_Future_IsDone},
    //v0.12.2: String face — declared native methods stamp onto the
    //existing frozen intrinsic ids. 20 rows = the 18 table methods
    //(INTR_String_Substring..INTR_String_ToBool) + equals +
    //getHashCode; String.length (OP_StrLen) and String.toString
    //(identity fold) carry no id, so they have no row. Both substring
    //overloads key the one "String.substring" row. Array's four
    //methods are compile-time-static emissions with no intrinsic id —
    //zero rows (kStringFaceBindingRows below pins this block's size).
    {"builtins", "String.equals",      INTR_String_Equals},
    {"builtins", "String.getHashCode", INTR_String_GetHashCode},
    {"builtins", "String.substring",   INTR_String_Substring},
    {"builtins", "String.indexOf",     INTR_String_IndexOf},
    {"builtins", "String.startsWith",  INTR_String_StartsWith},
    {"builtins", "String.endsWith",    INTR_String_EndsWith},
    {"builtins", "String.contains",    INTR_String_Contains},
    {"builtins", "String.toUpper",     INTR_String_ToUpper},
    {"builtins", "String.toLower",     INTR_String_ToLower},
    {"builtins", "String.trim",        INTR_String_Trim},
    {"builtins", "String.split",       INTR_String_Split},
    {"builtins", "String.replace",     INTR_String_Replace},
    {"builtins", "String.toInt",       INTR_String_ToInt},
    {"builtins", "String.toFloat",     INTR_String_ToFloat},
    {"builtins", "String.charAt",      INTR_String_CharAt},
    {"builtins", "String.charCount",   INTR_String_CharCount},
    {"builtins", "String.toChar",      INTR_String_ToChar},
    {"builtins", "String.toLong",      INTR_String_ToLong},
    {"builtins", "String.toDouble",    INTR_String_ToDouble},
    {"builtins", "String.toBool",      INTR_String_ToBool},
};

//Pins the String face block above independently of unrelated packages'
//rows: 18 table methods + equals + getHashCode. Keep in sync with the
//comment block when a face row changes (the face itself is frozen).
constexpr size_t kStringFaceBindingRows = 20;
static_assert(kStringFaceBindingRows == 18 + 2,
    "String face rows = 18 table methods + equals + getHashCode");

//The literal rows plus the DERIVED static-face rows. A face's row comes
//from the face table (BuiltinNames.h) — the single hand-maintained
//source — so a second static face cannot drift between the two tables:
//the intrinsic id matches by construction and noReceiver is true by
//definition of a static face (the type name qualifies the call, no
//instance rides slot 0). The derived rows stay inert until the builtins
//source-of-truth migration declares a static native member that stamps
//onto this channel.
const std::vector<BindingRow>& BindingRows()
{
    static const std::vector<BindingRow> s_rows = [] {
        std::vector<BindingRow> rows(std::begin(s_LiteralRows),
                                     std::end(s_LiteralRows));
        size_t faceCount = 0;
        const BuiltinStaticFace* pFaces = BuiltinStaticFaces(faceCount);
        for (size_t i = 0; i < faceCount; ++i)
            rows.push_back({"builtins",
                std::string(pFaces[i].typeName) + "."
                    + pFaces[i].methodName,
                pFaces[i].intrinsicId, /*noReceiver=*/true});
        return rows;
    }();
    return s_rows;
}

bool IsStampedBindingPackage(const std::string& packagePath)
{
    for (const auto& row : BindingRows())
        if (packagePath == row.packagePath)
            return true;
    return false;
}

uint16_t LookupIntrinsicId(const std::string& packagePath,
    const std::string& key)
{
    for (const auto& row : BindingRows())
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
