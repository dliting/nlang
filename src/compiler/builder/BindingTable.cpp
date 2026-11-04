/*---
	BindingTable.cpp — the intrinsic binding table (builtin-decls
	source-of-truth design §4.5) and its package/name projections.
	Split from VmBackend.cpp when the table landed (Task 1); the
	builtin-class clusters append their binding rows here as their
	declarations land in stdlib. Moved from src/vm/backend/ to the
	compiler (concurrency import redesign): the table is compile-time
	declaration metadata with no vm-library dependency, and the
	resolver's D10/D11 predicates link against it from pure-compiler
	targets.
---*/
#include "BindingTable.h"
#include "BuiltinNames.h"
#include "ModuleRegistry.h"
#include <nlang/vm/Intrinsics.h>
#include <string>
#include <vector>

namespace nlang {

//Rows: VM-implemented declarations by owning package + key. The thread
//rows are the FindThreadIntrinsicId table migrated into the general
//binding table (Task 1 of the builtin-decls source-of-truth plan). A
//declaration outside the table keeps host-native dispatch (the
//ordinary native channel).
//noReceiver marks the STATIC-face ABI: no `this` rides slot 0 — the
//intrinsic's arguments start at slot 0 (every receiver-dispatched row
//defaults to false: slot 0 = the instance).
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
	//Cluster 1 (Mutex/Semaphore): declared in stdlib/concurrent.n
	//(concurrency import redesign). The type-member stamping arm binds
	//the declared native records onto the intrinsic channel; ctor keys
	//are arity-aware (zero-formal = bare name, N-formal = "Class/N").
	{"concurrent", "Mutex",                INTR_Mutex_Ctor},
	{"concurrent", "Mutex.lock",           INTR_Mutex_Lock},
	{"concurrent", "Mutex.unlock",         INTR_Mutex_Unlock},
	{"concurrent", "Mutex.tryLock",        INTR_Mutex_TryLock},
	{"concurrent", "Semaphore/1",          INTR_Semaphore_Ctor},
	{"concurrent", "Semaphore.acquire",    INTR_Semaphore_Acquire},
	{"concurrent", "Semaphore.release",    INTR_Semaphore_Release},
	{"concurrent", "Semaphore.tryAcquire", INTR_Semaphore_TryAcquire},
	//0.10.21: ReentrantMutex — the declared reentrant companion.
	{"concurrent", "ReentrantMutex",         INTR_ReentrantMutex_Ctor},
	{"concurrent", "ReentrantMutex.lock",    INTR_ReentrantMutex_Lock},
	{"concurrent", "ReentrantMutex.unlock",  INTR_ReentrantMutex_Unlock},
	{"concurrent", "ReentrantMutex.tryLock", INTR_ReentrantMutex_TryLock},
	//0.10.22: AtomicInt — the declared atomic-integer family. One ctor
	//id: the declaration's default parameter stages the omitted arg (the
	///1 key follows the arity-aware ctor rule).
	{"concurrent", "AtomicInt/1",                 INTR_AtomicInt_Ctor},
	{"concurrent", "AtomicInt.load",              INTR_AtomicInt_Load},
	{"concurrent", "AtomicInt.store",             INTR_AtomicInt_Store},
	{"concurrent", "AtomicInt.getAndSet",         INTR_AtomicInt_GetAndSet},
	{"concurrent", "AtomicInt.compareExchange",   INTR_AtomicInt_CompareExchange},
	{"concurrent", "AtomicInt.getAndAdd",         INTR_AtomicInt_GetAndAdd},
	{"concurrent", "AtomicInt.addAndGet",         INTR_AtomicInt_AddAndGet},
	{"concurrent", "AtomicInt.incrementAndGet",   INTR_AtomicInt_IncrementAndGet},
	{"concurrent", "AtomicInt.getAndIncrement",   INTR_AtomicInt_GetAndIncrement},
	//Cluster 2 (Exception family): declared in stdlib/lang.n, one
	//ctor row each — except ChannelClosedException, which moved to
	//stdlib/concurrent.n with the channel (concurrency import
	//redesign). The VM raise paths keep their by-name class caches;
	//only the ctor call channel is stamped here. v0.12.4 D7: every ctor
	//declares (message, cause = null), so the arity-aware key is /2 —
	//one-argument calls ride the default-parameter staging onto the
	//same row (the AtomicInt/1 rule).
	{"lang", "Exception/2",                 INTR_Exception_Ctor},
	{"lang", "NullPointerException/2",      INTR_NullPointerException_Ctor},
	{"lang", "DivByZeroException/2",        INTR_DivByZeroException_Ctor},
	{"lang", "IndexOutOfBoundsException/2", INTR_IndexOutOfBoundsException_Ctor},
	{"lang", "AssertionException/2",        INTR_AssertionException_Ctor},
	{"lang", "IOException/2",               INTR_IOException_Ctor},
	{"lang", "InvalidCastException/2",      INTR_InvalidCastException_Ctor},
	{"lang", "InitializationException/2",  INTR_InitializationException_Ctor},
	{"concurrent", "ChannelClosedException/2", INTR_ChannelClosedException_Ctor},
	//Cluster 4 (Object): the three protocol methods. Declared virtual so
	//override propagation marks user overrides and call sites dispatch by
	//name; no ctor row (Object is never constructed by user code).
	{"lang", "Object.equals",      INTR_Object_Equals},
	{"lang", "Object.getHashCode", INTR_Object_GetHashCode},
	{"lang", "Object.toString",    INTR_Object_toString},
	//Cluster 5 (Promise): declared generic template in stdlib/thread.n
	//(concurrency import redesign); the erased instance's members stamp
	//through the same key shape (erasure naming keys them bare
	//"Promise"/"Promise.future").
	{"thread", "Promise",        INTR_Promise_Ctor},
	{"thread", "Promise.future", INTR_Promise_Future},
	{"thread", "Promise.complete", INTR_Promise_Complete},
	{"thread", "Promise.completeExceptionally",
	                           INTR_Promise_CompleteExceptionally},
	//Cluster 6 (generic containers + futures): List/Dict declared in
	//stdlib/containers.n (the prelude split); Channel moved to stdlib/concurrent.n
	//and Future to stdlib/thread.n (concurrency import redesign).
	//Method keys are "Class.method"; ctor keys follow the arity-aware
	//rule. Erasure naming keys the instances bare ("List"/"List.add" —
	//same shape as Promise).
	{"containers", "List",            INTR_List_Ctor},
	{"containers", "List.add",        INTR_List_Add},
	{"containers", "List.get",        INTR_List_Get},
	{"containers", "List.set",        INTR_List_Set},
	{"containers", "List.length",     INTR_List_Length},
	{"containers", "List.removeAt",   INTR_List_RemoveAt},
	{"containers", "List.indexOf",    INTR_List_IndexOf},
	{"containers", "List.contains",   INTR_List_Contains},
	{"containers", "List.clear",      INTR_List_Clear},
	{"containers", "List.copy",       INTR_List_Copy},
	{"containers", "List.toString",   INTR_List_toString},
	{"containers", "Dict",            INTR_Dict_Ctor},
	{"containers", "Dict.set",        INTR_Dict_Set},
	{"containers", "Dict.get",        INTR_Dict_Get},
	{"containers", "Dict.containsKey", INTR_Dict_ContainsKey},
	{"containers", "Dict.remove",     INTR_Dict_Remove},
	{"containers", "Dict.clear",      INTR_Dict_Clear},
	{"containers", "Dict.count",      INTR_Dict_Count},
	{"containers", "Dict.keys",       INTR_Dict_Keys},
	{"containers", "Dict.copy",       INTR_Dict_Copy},
	{"containers", "Dict.toString",   INTR_Dict_toString},
	{"concurrent", "Channel",         INTR_Channel_CtorUnbounded},
	{"concurrent", "Channel/1",       INTR_Channel_Ctor},
	{"concurrent", "Channel.send",    INTR_Channel_Send},
	{"concurrent", "Channel.recv",    INTR_Channel_Recv},
	{"concurrent", "Channel.close",   INTR_Channel_Close},
	{"thread", "Future.get",      INTR_Future_Get},
	{"thread", "Future.isDone",   INTR_Future_IsDone},
	//v0.12.2: String face — declared native methods stamp onto the
	//existing frozen intrinsic ids. 20 rows = the 18 string methods
	//(INTR_String_Substring..INTR_String_ToBool) + equals +
	//getHashCode; String.length (OP_StrLen) and String.toString
	//(identity fold) carry no id, so they have no row. Both substring
	//overloads key the one "String.substring" row. v0.12.7 Task 5: the
	//Array face's protocol methods — the virtual dispatch ids for an
	//Object-receiver protocol call on an array (ReceiverClassIndex maps
	//RTK_Array to the face row). toString formats through the shared
	//array formatter; equals/getHashCode ride the Object identity
	//intrinsics. length/rank/shape keep zero rows: compile-time-static
	//emissions that can never reach a virtual dispatch (the Object
	//protocol is exactly these three methods).
	{"lang", "Array.equals",      INTR_Object_Equals},
	{"lang", "Array.getHashCode", INTR_Object_GetHashCode},
	{"lang", "Array.toString",    INTR_Array_toString},
	{"lang", "String.equals",      INTR_String_Equals},
	{"lang", "String.getHashCode", INTR_String_GetHashCode},
	{"lang", "String.substring",   INTR_String_Substring},
	{"lang", "String.indexOf",     INTR_String_IndexOf},
	{"lang", "String.startsWith",  INTR_String_StartsWith},
	{"lang", "String.endsWith",    INTR_String_EndsWith},
	{"lang", "String.contains",    INTR_String_Contains},
	{"lang", "String.toUpper",     INTR_String_ToUpper},
	{"lang", "String.toLower",     INTR_String_ToLower},
	{"lang", "String.trim",        INTR_String_Trim},
	{"lang", "String.split",       INTR_String_Split},
	{"lang", "String.replace",     INTR_String_Replace},
	{"lang", "String.toInt",       INTR_String_ToInt},
	{"lang", "String.toFloat",     INTR_String_ToFloat},
	{"lang", "String.charAt",      INTR_String_CharAt},
	{"lang", "String.charCount",   INTR_String_CharCount},
	{"lang", "String.toChar",      INTR_String_ToChar},
	{"lang", "String.toLong",      INTR_String_ToLong},
	{"lang", "String.toDouble",    INTR_String_ToDouble},
	{"lang", "String.toBool",      INTR_String_ToBool},
};

//Pins the String face block above independently of unrelated packages'
//rows: the 18 string methods + equals + getHashCode. The tie to
//kStringMethodIntrinsicCount breaks the build when the intrinsic block
//grows without matching rows (the face itself is frozen).
constexpr size_t kStringFaceBindingRows = 20;
static_assert(kStringFaceBindingRows == kStringMethodIntrinsicCount + 2,
	"String face rows = string-method intrinsics + equals + getHashCode");

//The literal rows plus the DERIVED static-face rows. A face's row comes
//from the face table (BuiltinNames.h) — the single hand-maintained
//source — so a second static face cannot drift between the two tables:
//the intrinsic id matches by construction and noReceiver is true by
//definition of a static face (the type name qualifies the call, no
//instance rides slot 0). The derived rows stay inert until a prelude
//source file declares a static native member that stamps onto this
//channel.
const std::vector<BindingRow>& BindingRows()
{
	static const std::vector<BindingRow> s_rows = [] {
		std::vector<BindingRow> rows(std::begin(s_LiteralRows),
			std::end(s_LiteralRows));
		size_t faceCount = 0;
		const BuiltinStaticFace* pFaces = BuiltinStaticFaces(faceCount);
		for (size_t i = 0; i < faceCount; ++i)
			rows.push_back({pFaces[i].package,
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

//D10 (concurrency import redesign): reverse lookup for the unimported-
//use note — which stamped package declares this bare name? The four key
//forms the table spells: exact zero-arg ctor key, "/1" arity ctor key,
//a "Name." method-prefix cluster, and the free-function qualified form
//("thread.spawn" answers "spawn" -> "thread"). Method segments alone
//("lock") never match: the note fires on the class/function name the
//user wrote. outIsFreeFuncForm marks a last-form match so the note can
//name the qualified spelling.
bool BindingPackageForName(const std::string& name,
	std::string& outPackage, bool* outIsFreeFuncForm)
{
	const std::string ctorArity = name + "/1";
	const std::string methodPrefix = name + ".";
	for (const auto& row : BindingRows())
	{
		const bool methodForm = row.key.compare(0, methodPrefix.size(),
			methodPrefix) == 0;
		const bool freeFuncForm = row.key == row.packagePath + "."
			+ name;
		if (row.key == name || row.key == ctorArity || methodForm
			|| freeFuncForm)
		{
			outPackage = row.packagePath;
			if (outIsFreeFuncForm)
				*outIsFreeFuncForm = freeFuncForm;
			return true;
		}
	}
	return false;
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

//D11 (concurrency import redesign): prelude ownership alone stops being
//the "this declaration IS the builtin one" test once the concurrency
//classes leave the prelude. The second arm keeps them certified: a
//stamped binding package (thread/concurrent/...) owning a handle-class
//name. Thread/Future have no class binding rows, so this is a name-list
//arm, not a row-presence test (spec §3 D11).
bool IsBuiltinHandleDeclName(const ModuleRegistry& registry,
	uint32_t ownerUnit, const std::string& className)
{
	if (ownerUnit == ModuleRegistry::NO_OWNER)
		return false;
	if (registry.IsPreludeModule(ownerUnit))
		return true;
	return IsStampedBindingPackage(registry.ModulePathOf(ownerUnit))
		&& IsHandleClassName(className);
}

} //namespace nlang
