/*-----------------------------------------------------------------------------
	ncomp/builder/ThreadNamespace.h
	K1 (v0.10.0): the built-in `thread` namespace call surface
	(thread.spawn/join/yield/id/sleep). One table and one singleton
	factory serve both sides: the resolver (signature checks against the
	synthetic SnFunction formals) and VmBackend (module-level intrinsic
	stub registration keyed by the very same singleton pointers — nlang_vm
	links nlang_compiler privately and includes builder headers, the
	same arrangement SymbolSlots.hpp uses).
	The formals' type expressions are minted UNbound; binding an
	expression's Field() is an ExprResolveAccessor-only operation, so the
	resolver binds them lazily on first use (BindThreadNsFormalTypes).
-----------------------------------------------------------------------------*/
#pragma once
#include <cstddef>
#include <string>
#include <nlang/runtime/NodeConsts.h>
#include <nlang/vm/Intrinsics.h>

namespace nlang
{

class SnNamespace;
class SnFunction;
struct ThreadNsFuncEntry;

//The reserved namespace/package name (O1: RegisterUnit rejects a user
//package under this path so it can never shadow the built-in surface).
inline constexpr const char* kThreadNamespaceName = "thread";

//Parameter/return kinds the table can describe. The K1 surface carries
//at most one parameter per function; TNP_Void doubles as "no value" for
//the return column.
enum ThreadNsParamKind : unsigned char {
	TNP_Void,      //void return / zero-parameter function
	TNP_Int32,     //int value or return
	TNP_ThreadObj, //Thread instance (class reference)
	TNP_FuncVoid   //func<void> delegate (spawn's parameter)
};

struct ThreadNsFuncEntry {
	const char* memberName;
	uint16_t intrinsicId;
	unsigned char paramKind;   //ThreadNsParamKind of the single parameter
	unsigned char returnKind;  //ThreadNsParamKind of the return value
};

//The five-entry K1 surface. Order is table order — registration order of
//the backend stubs derives from it.
const ThreadNsFuncEntry* ThreadNsFuncTable();
size_t ThreadNsFuncCount();
const ThreadNsFuncEntry* FindThreadNsFunc(const std::string& memberName);

//Fixed call shape of one Mutex/Channel method (the resolver side's
//classification result; Channel.recv arrives in Slice 9 with its
//element-type decision — until then it is simply not a member).
struct SyncMethodShape {
	int paramCount;
	NodeKind returnType;   //NK_Void = no result value on the member
};

//Singleton decls (lazily minted, never freed — process-lifetime
//built-ins, the GetBuiltinClassDecl arrangement). The callee factory
//is index-based: tableIndex must address ThreadNsFuncTable().
SnNamespace* GetThreadNamespaceDecl();
SnFunction* GetThreadNsCallee(size_t tableIndex);

} //namespace nlang
