#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnArrayTypeToken.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include "ModuleRegistry.h"
#include <nlang/vm/StdLib.h>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

//Canonical SnClassDecl instances for built-in classes. These must be
//singletons so that CalcTypeDistance's pointer identity check works
//across all resolution sites (type names, new expressions, member calls).
static SnClassDecl* s_pByteStreamClass = nullptr;
static SnClassDecl* s_pFileStreamClass = nullptr;
static SnClassDecl* s_pObjectClass = nullptr;  //Phase 8e-1: implicit Object base class
//Phase 9d: built-in Exception hierarchy. Each subclass declares Exception
//as its super so IsExceptionSubclass's chain walk succeeds. These are
//type-system stand-ins — actual ctor/fields live in the CompiledClass
//emitted by VmBackend::RegisterBuiltinClasses.
static SnClassDecl* s_pExceptionClass = nullptr;
static SnClassDecl* s_pNullPtrExcClass = nullptr;
static SnClassDecl* s_pDivZeroExcClass = nullptr;
static SnClassDecl* s_pOobExcClass = nullptr;
static SnClassDecl* s_pAssertExcClass = nullptr;
static SnClassDecl* s_pIoExcClass = nullptr;  //Phase 11: IOException

//Phase 9d: returns true for any name in the built-in Exception hierarchy.
//Phase 13: the name predicates moved to builder/BuiltinNames.h (shared with
//the alias clash check); local statics are no longer needed.

//Phase 10 audit H2: IsBuiltinClassName moved to builder/BuiltinNames.h
//(shared with the Phase 13 alias clash check in DuplicateFieldChecker).

//On-demand minted decls (built-in classes, generic instantiations) take
//the caller's location when one exists. That location is polymorphic —
//a parser-minted ScriptLocation for source nodes, the imported-module
//location for v1.12 stub nodes — so it must flow through the
//ISourceLocation interface (SyntaxNode clones whatever it receives).
//Punning it to ScriptLocation reads past the end of the smaller
//imported-location object.
static const ISourceLocation& DeclLocation(const ISourceLocation* pLoc,
	ScriptLocation& fallback)
{
	return pLoc ? *pLoc : fallback;
}

//Precondition: name passes IsBuiltinClassName. Returns nullptr for any
//other name (defensive — callers skip resolution and the identifier
//surfaces as a normal unresolved-name error).
static SnClassDecl* GetBuiltinClassDecl(const std::string& name,
	const ISourceLocation* pLoc)
{
	if (!IsBuiltinClassName(name))
		return nullptr;
	//Phase 9d: force-create Exception singleton first so subclass chain
	//walk has a target even if the subclass is requested first.
	if (IsBuiltinExceptionClassName(name) && !s_pExceptionClass) {
		auto* pName = new std::string("Exception");
		auto* pMembers = new PtrList<SnField>();
		ScriptLocation fallback;
		s_pExceptionClass = new SnClassDecl(pName, nullptr, pMembers,
			DeclLocation(pLoc, fallback));
		s_pExceptionClass->SetBuiltinClass();
	}
	SnClassDecl*& rpRef = (name == "ByteStream") ? s_pByteStreamClass
		: (name == "FileStream") ? s_pFileStreamClass
		: (name == "Object") ? s_pObjectClass
		: (name == "Exception") ? s_pExceptionClass
		: (name == "NullPointerException") ? s_pNullPtrExcClass
		: (name == "DivByZeroException") ? s_pDivZeroExcClass
		: (name == "IndexOutOfBoundsException") ? s_pOobExcClass
		: (name == "AssertionException") ? s_pAssertExcClass
		: (name == "IOException") ? s_pIoExcClass
		: s_pObjectClass;  //unreachable: IsBuiltinClassName gate above
	if (!rpRef)
	{
		auto* pName = new std::string(name);
		auto* pMembers = new PtrList<SnField>();
		ScriptLocation fallback;
		rpRef = new SnClassDecl(pName, nullptr, pMembers,
			DeclLocation(pLoc, fallback));
		rpRef->SetBuiltinClass();
		//Phase 9d: subclasses point at Exception singleton for chain walk.
		if (name != "Exception" && IsBuiltinExceptionClassName(name))
			rpRef->SuperClass(s_pExceptionClass);
		rpRef->SetBuiltinClass();
	}
	return rpRef;
}

//Phase 8e-3: Built-in generic class instantiation cache.
//Key: (base class name, resolved type-arg SnField* pointers).
//Value: synthetic SnClassDecl representing this instantiation. The same
//key MUST always return the same SnClassDecl* — pointer identity is
//load-bearing for CalcTypeDistance and for VmBackend's class index lookup.
//
//At runtime, all instantiations share a single CompiledClass named "List"
//via VmBackend's name-prefix strip. The synthetic SnClassDecl is a
//compile-time artifact only.
struct GenericInstKey {
	std::string baseName;
	std::vector<SnField*> typeArgs;
	//Phase 13: out markers live on the type-arg expression nodes (NF_Out),
	//not on the canonical fields, so the key needs a parallel flag vector —
	//without it Func<void,int> and Func<void,out int> collapse into one
	//declaration and the first instantiation silently wins.
	//0.7.3 B: array-ness needs no flag vector anymore — an array-typed
	//argument IS the interned SnArrayTypeToken in typeArgs, so pointer
	//identity keeps List<int> and List<int[]> distinct keys.
	std::vector<uint8> outFlags;
	bool operator<(const GenericInstKey& rhs) const {
		if (baseName != rhs.baseName) return baseName < rhs.baseName;
		if (typeArgs.size() != rhs.typeArgs.size())
			return typeArgs.size() < rhs.typeArgs.size();
		if (outFlags != rhs.outFlags) return outFlags < rhs.outFlags;
		for (size_t i = 0; i < typeArgs.size(); ++i) {
			if (typeArgs[i] != rhs.typeArgs[i])
				return typeArgs[i] < rhs.typeArgs[i];
		}
		return false;
	}
};
static std::map<GenericInstKey, SnClassDecl*> s_genericInstances;

//Side table: synthetic class → its type arguments. Used by Access(SnMemberExpr&)
//to compute method return types (e.g., List<int>.Get() returns int = typeArgs[0]).
//Also mirrored on SnClassDecl::GenericTypeArgs() for VmBackend codegen.
static std::map<SnClassDecl*, std::vector<SnField*>> s_genericTypeArgs;

//Phase 13: parallel out-flag side table (same key discipline as
//s_genericTypeArgs). Func's delegate-binding channel reads the signature
//(return type + out-marked params) from these two tables.
static std::map<SnClassDecl*, std::vector<uint8>> s_genericOutFlags;

//Lookup type arguments for a synthetic generic class. Returns empty vector
//if not a generic instantiation.
std::vector<SnField*> GetGenericTypeArgs(SnClassDecl* pClass)
{
	if (pClass && pClass->IsGenericInstantiation())
		return pClass->GenericTypeArgs();
	auto it = s_genericTypeArgs.find(pClass);
	if (it != s_genericTypeArgs.end())
		return it->second;
	return {};
}

//Returns true if name is a recognized built-in generic class.
//Phase 8e-3: "List" (arity 1). Phase 8e-4: "Dict" (arity 2).
//Phase 13: "Func" (variadic, at least the return type).
static bool IsBuiltinGenericClassName(const std::string& name)
{
	return name == "List" || name == "Dict" || name == "Func";
}

//Round-12: built-in methods dispatch by name with no real SnFunction, so a
//name = value argument can never bind to a parameter — reject it here or
//codegen's fallback would silently stage the value as ConstZero.
bool HasNamedArgument(SnInvokeExpr& invoke)
{
	for (auto& p : invoke.Params())
		if (p.Kind() == NK_NamedArgExpr)
			return true;
	return false;
}

size_t ArgCountOf(SnInvokeExpr& invoke)
{
	size_t n = 0;
	for (auto& p : invoke.Params()) ++n;
	return n;
}

//Round-14: mirror of HasNamedArgument — built-in by-name dispatch also
//cannot write back out arguments (intrinsics return through pResult only).
bool HasOutArgument(SnInvokeExpr& invoke)
{
	for (auto& p : invoke.Params())
		if (p.Kind() == NK_OutArgExpr)
			return true;
	return false;
}

//Module import visibility (spec §6.2 rule 5): join dotted path segments.
std::string JoinDots(const std::vector<std::string>& segs)
{
	std::string joined;
	for (size_t i = 0; i < segs.size(); ++i)
	{
		if (i)
			joined += '.';
		joined += segs[i];
	}
	return joined;
}

//Collect the identifier names of a member chain's OUTER side, leftmost
//first: for `a.b.c(...)` on the member whose Inner is the invoke this is
//{a, b} (the module path). Empty when any link is not a plain identifier
//(a receiver value, a call result) — such a chain is outside the module
//fallback's contract.
std::vector<std::string> OuterIdentifierChain(
	const SnMemberExpr& snMember)
{
	std::vector<std::string> chain;  //collected inner-to-outer, reversed below
	const SyntaxNode* pLink = snMember.Outer();
	while (pLink && pLink->Kind() == NK_MemberExpr)
	{
		const auto& rLink = static_cast<const SnMemberExpr&>(*pLink);
		const SyntaxNode* pLinkInner = rLink.Inner();
		if (!pLinkInner || pLinkInner->Kind() != NK_IdentifierExpr)
			return {};
		chain.push_back(
			static_cast<const SnIdentifierExpr*>(pLinkInner)->Name());
		pLink = rLink.Outer();
	}
	if (!pLink || pLink->Kind() != NK_IdentifierExpr)
		return {};
	chain.push_back(static_cast<const SnIdentifierExpr*>(pLink)->Name());
	std::reverse(chain.begin(), chain.end());
	return chain;
}

//Phase 11: printable language name of a stdlib param kind (RTK_*). Only
//the kinds StdLibEntry::paramKinds may carry are covered — extending the
//table with a new kind means extending this switch too.
const char* StdLibKindName(uint8_t rtk)
{
	switch (rtk)
	{
	case RTK_Int32:  return "int";
	case RTK_Float:  return "float";
	case RTK_String: return "string";
	default:         return "<unknown>";
	}
}

//Returns true if class decl is a synthetic generic instantiation
//(e.g., List<int>). Used to dispatch member calls in Access(SnMemberExpr&).
bool IsGenericClassDecl(SnClassDecl* pClass)
{
	return pClass && pClass->IsGenericInstantiation();
}

//Mints (or fetches) a synthetic SnClassDecl for the given generic
//instantiation. Phase 8e-3: List<T> (arity 1). Phase 8e-4: Dict<K,V> (arity 2).
//Phase 13: Func<R, P...> (variadic, >= 1). outFlags are normalized here
//so callers that infer instantiations (List element inference) can pass
//an empty vector while declaration-path keys stay distinct.
SnClassDecl* GetGenericClassDecl(const std::string& baseName,
	const std::vector<SnField*>& typeArgs, const std::vector<uint8>& outFlags,
	const ISourceLocation* pLoc)
{
	std::vector<uint8> normOutFlags(outFlags);
	normOutFlags.resize(typeArgs.size(), 0);
	GenericInstKey key{baseName, typeArgs, normOutFlags};
	auto it = s_genericInstances.find(key);
	if (it != s_genericInstances.end())
		return it->second;

	//Built-in generic + arity check. List/Dict have fixed arity; Func is
	//variadic: first argument is the return type, the rest are params.
	bool isFunc = baseName == "Func";
	size_t expectedArity = (baseName == "Dict") ? 2 : 1;
	size_t minArity = isFunc ? 1 : expectedArity;
	size_t maxArity = isFunc ? static_cast<size_t>(-1) : expectedArity;
	if (!IsBuiltinGenericClassName(baseName)
		|| typeArgs.size() < minArity || typeArgs.size() > maxArity)
		return nullptr;

	//Build display name e.g. "List<int>", "Dict<string, int>". Array-typed
	//arguments ARE interned tokens; their Name() is empty by design, so
	//render them via ToString ("Int32[]") — without this the split
	//instantiations would be indistinguishable in diagnostics.
	std::string instName = baseName + "<";
	for (size_t i = 0; i < typeArgs.size(); ++i) {
		if (i) instName += ", ";
		if (typeArgs[i]->Kind() == NK_ArrayTypeToken)
			instName += typeArgs[i]->ToString();
		else
			instName += typeArgs[i]->Name();
	}
	instName += ">";

	auto* pName = new std::string(instName);
	auto* pMembers = new PtrList<SnField>();
	ScriptLocation fallback;
	auto* pClass = new SnClassDecl(pName, nullptr, pMembers,
		DeclLocation(pLoc, fallback));
	pClass->SetBuiltinClass();
	pClass->SetGenericInstantiation();
	pClass->SetGenericTypeArgs(typeArgs);
	pClass->SetBaseName(baseName);
	s_genericInstances[key] = pClass;
	//Side table for member-call return-type lookup (List<int>.Get() → int).
	s_genericTypeArgs[pClass] = typeArgs;
	s_genericOutFlags[pClass] = normOutFlags;
	return pClass;
}

//Phase 13: true when the field is a synthetic Func<...> instantiation.
bool IsFuncTypeDecl(SnField *pType)
{
	return pType && pType->Kind() == NK_ClassDecl
		&& static_cast<SnClassDecl*>(pType)->IsFuncType();
}

//Phase 13: exact-signature comparison between a function declaration and
//a Func<...> instantiation (no variance). Return slot: a nullptr return
//type matches a void type argument. Parameter slots: declared type field
//pointer identity plus out-flag agreement — the same discipline that
//keeps Func<void,int> and Func<void,out int> distinct in GenericInstKey.
//0.7.3 B: array-ness needs no separate comparison — an array-typed
//return/param carries the interned token, and pointer identity against
//the type argument (itself a token for Func<int[]>) rules the pairing.
static bool FuncRefMatchesDecl(const SnFunction &func, SnClassDecl *pFuncDecl)
{
	const auto &typeArgs = GetGenericTypeArgs(pFuncDecl);
	const auto &outFlags = s_genericOutFlags[pFuncDecl];
	auto *pRet = func.ReturnType();
	if (pRet)
	{
		if (!pRet->IsResolved() || pRet->Field() != typeArgs[0])
			return false;
	}
	else if (typeArgs[0]->Kind() != NK_Void)
	{
		return false;
	}
	const auto &params = func.Params();
	if (params.size() + 1 != typeArgs.size())
		return false;
	size_t i = 0;
	for (auto &param : params)
	{
		if (param.EvalDataType() != typeArgs[i + 1])
			return false;
		if (param.ContainFlags(NF_Out) != (outFlags[i + 1] != 0))
			return false;
		++i;
	}
	return true;
}

bool BindFuncRefToExpected(BuildEnvironment &env, SnIdentifierExpr &idExpr,
	SnField *pExpected)
{
	auto *pFunc = static_cast<SnFunction*>(idExpr.Field());
	assert(pFunc && pFunc->Kind() == NK_Function);
	if (!IsFuncTypeDecl(pExpected))
	{
		env.Log(CLL_Error, idExpr.Location(),
			"function reference \"%s\" requires an expected function type.",
			idExpr.Name().c_str());
		return false;
	}
	//Methods are not free-function references (receiver-bound references
	//arrive in Step 2); enum methods additionally carry an int receiver,
	//which cannot live in a heap-index slot.
	auto *pOwner = pFunc->Parent();
	if (pOwner && (pOwner->Kind() == NK_ClassDecl
		|| pOwner->Kind() == NK_InterfaceDecl
		|| pOwner->Kind() == NK_EnumDecl))
	{
		env.Log(CLL_Error, idExpr.Location(),
			"cannot reference the method \"%s\" without a receiver.",
			idExpr.Name().c_str());
		return false;
	}
	//Imported stubs carry v1.12 type descriptors for their formals, but
	//Func signatures are outside the descriptor grammar — an imported
	//callee cannot be matched against an expected Func type yet.
	if (pFunc->ContainFlags(NF_Imported))
	{
		env.Log(CLL_Error, idExpr.Location(),
			"cannot reference the imported function \"%s\": parameter "
			"signatures are not serialized.",
			idExpr.Name().c_str());
		return false;
	}
	//Default parameters are filled only on the direct-call path.
	for (auto &param : pFunc->Params())
	{
		if (param.Value())
		{
			env.Log(CLL_Error, idExpr.Location(),
				"functions with default parameters cannot be referenced: "
				"\"%s\".", idExpr.Name().c_str());
			return false;
		}
	}
	auto *pFuncDecl = static_cast<SnClassDecl*>(pExpected);
	if (!FuncRefMatchesDecl(*pFunc, pFuncDecl))
	{
		env.Log(CLL_Error, idExpr.Location(),
			"function \"%s\" does not match the signature of \"%s\".",
			idExpr.Name().c_str(), pFuncDecl->Name().c_str());
		return false;
	}
	//Bound state is structural: EvalDataType becomes the Func declaration
	//(codegen detects Field()->Kind() == NK_Function in a value position
	//and emits OP_MakeFunc). No dedicated node flag exists — the 24 flag
	//bits are fully allocated.
	idExpr.EvalDataType(pFuncDecl);
	return true;
}

//Phase 13 Step 2: collect every class declaration in the tree under
//(and including) `node` — input of the native-override scan below.
static void CollectClassDecls(SyntaxNode &node,
	std::vector<SnClassDecl*> &out)
{
	for (auto &child : node.Children())
	{
		auto &synChild = static_cast<SyntaxNode&>(child);
		if (synChild.Kind() == NK_ClassDecl)
			out.push_back(static_cast<SnClassDecl*>(&synChild));
		CollectClassDecls(synChild, out);
	}
}

//Phase 13 Step 2: a by-name binding (OP_MakeVFunc) resolves its target at
//runtime, so a NATIVE override anywhere in the method's dispatch domain
//would route the delegate into CallNative, which reads arguments straight
//from callParamBase with no callee frame for the receiver shift. The
//resolved method itself carries NF_Native only when the STATIC receiver
//type declares it — for a virtual base declaration the scan must walk the
//deriving classes, and for an interface declaration the implementers (the
//implements clause is not modeled by SuperClass()).
static bool HasNativeMethodOverride(SnFunction &method)
{
	auto *pOwner = method.Parent();
	if (!pOwner || (pOwner->Kind() != NK_ClassDecl
		&& pOwner->Kind() != NK_InterfaceDecl))
		return false;
	//Is `pClass` inside the dispatch domain of `pOwner`? Class owner:
	//derive from it directly. Interface owner: the class or any ancestor
	//names it in its implements clause.
	auto dispatchesUnder = [](SnClassDecl *pClass, SyntaxNode *pOwner) {
		for (SnClassDecl *cur = pClass; cur; cur = cur->SuperClass())
		{
			if (cur == pOwner)
				return true;
			if (pOwner->Kind() != NK_InterfaceDecl)
				continue;
			for (auto *pIface : cur->ImplementsList())
			{
				if (pIface == pOwner)
					return true;
			}
		}
		return false;
	};
	SyntaxNode *pRoot = pOwner;
	while (pRoot->Parent())
		pRoot = pRoot->Parent();
	std::vector<SnClassDecl*> classes;
	CollectClassDecls(*pRoot, classes);
	for (auto *pClass : classes)
	{
		if (pClass == pOwner)
			continue;
		if (!dispatchesUnder(pClass, pOwner))
			continue;
		for (auto &member : pClass->Members())
		{
			if (member.Kind() == NK_Function
				&& member.Name() == method.Name()
				&& member.ContainFlags(NF_Native))
				return true;
		}
	}
	return false;
}

bool BindMemberFuncRefToExpected(BuildEnvironment &env,
	SnMemberExpr &snMember, SnField *pExpected)
{
	auto &inner = static_cast<SnIdentifierExpr&>(*snMember.Inner());
	auto *pMethod = static_cast<SnFunction*>(inner.Field());
	assert(pMethod && pMethod->Kind() == NK_Function);
	auto *pOwner = pMethod->Parent();
	//Handle form mirrors the direct-call codegen decision exactly:
	//virtual methods and interface declarations dispatch by name at
	//runtime (StatementResolver's implicit virtual propagation means an
	//override of a parent virtual method carries NF_Virtual too).
	bool bDispatchesByName = pMethod->ContainFlags(NF_Virtual)
		|| (pOwner && pOwner->Kind() == NK_InterfaceDecl);
	if (!IsFuncTypeDecl(pExpected))
	{
		env.Log(CLL_Error, snMember.Location(),
			"bound method reference \"%s\" requires an expected function "
			"type.", inner.Name().c_str());
		return false;
	}
	//Enum receivers are int values — slot[1] of a handle (a heap index)
	//cannot carry the receiver.
	if (pOwner && pOwner->Kind() == NK_EnumDecl)
	{
		env.Log(CLL_Error, snMember.Location(),
			"cannot reference the enum method \"%s\": enum receivers are "
			"int values, not heap objects.", inner.Name().c_str());
		return false;
	}
	if (pMethod->ContainFlags(NF_Native)
		|| (bDispatchesByName && HasNativeMethodOverride(*pMethod)))
	{
		env.Log(CLL_Error, snMember.Location(),
			"cannot reference the native method \"%s\": native calls have "
			"no callee frame for the receiver.", inner.Name().c_str());
		return false;
	}
	//The out mask is compiled from the Func type, but a by-name handle
	//resolves the target at runtime — an overriding method's layout may
	//disagree. Same rationale as the existing virtual-direct-call reject.
	if (bDispatchesByName)
	{
		const auto &outFlags =
			s_genericOutFlags[static_cast<SnClassDecl*>(pExpected)];
		for (size_t k = 1; k < outFlags.size(); ++k)
		{
			if (outFlags[k] != 0)
			{
				env.Log(CLL_Error, snMember.Location(),
					"out parameters are not supported on virtual method "
					"references: dispatch resolves the target at runtime.");
				return false;
			}
		}
	}
	//Default parameters are filled only on the direct-call path (mirrors
	//the free-function reject above).
	for (auto &param : pMethod->Params())
	{
		if (param.Value())
		{
			env.Log(CLL_Error, snMember.Location(),
				"methods with default parameters cannot be referenced: "
				"\"%s\".", inner.Name().c_str());
			return false;
		}
	}
	auto *pFuncDecl = static_cast<SnClassDecl*>(pExpected);
	if (!FuncRefMatchesDecl(*pMethod, pFuncDecl))
	{
		env.Log(CLL_Error, snMember.Location(),
			"method \"%s\" does not match the signature of \"%s\".",
			inner.Name().c_str(), pFuncDecl->Name().c_str());
		return false;
	}
	//Bound state is structural (same as the bare-name form): the member
	//carries the Func declaration while Field() stays the SnFunction;
	//codegen emits receiver + OP_MakeBoundFunc/OP_MakeVFunc. The inner
	//identifier carries the Func type too — the end-of-build sweep (loose
	//predicate) would otherwise flag every bound member reference through
	//it.
	snMember.EvalDataType(pFuncDecl);
	inner.EvalDataType(pFuncDecl);
	return true;
}

//Phase 13: loose pending predicate — true while a bare function
//reference carries a non-Func EvalDataType (its function's return
//type). Review round-1 F1 split this into two predicates: a Func-typed
//EvalDataType does NOT prove a binding (it may be the function's Func
//RETURN type leaking through ResolveFieldExprAs), so all bind sites use
//the strict IsUnboundFuncRef below. This loose form survives only for
//the end-of-build sweep (ModuleBuilder::SweepPendingFuncRefs): a ref a
//Func-accepting consumer already handled via MakeFunc (e.g. io.print of
//a Func-returning function's bare name) must not be re-flagged there.
bool IsPendingFuncRef(SyntaxNode &expr)
{
	if (expr.Kind() != NK_IdentifierExpr)
		return false;
	auto &idExpr = static_cast<SnIdentifierExpr&>(expr);
	return idExpr.Field() && idExpr.Field()->Kind() == NK_Function
		&& !IsFuncTypeDecl(idExpr.EvalDataType());
}

//Phase 13 (review round-1 F1): strict bind-site predicate. A bare name
//is bound only when its OWN signature satisfies the Func type it
//carries. Without this, `Func<int,int> f = pick;` (where pick RETURNS
//Func<int,int> but takes no parameters) was misread as an already-bound
//reference and silently compiled into a wrong-signature handle.
//Binding is idempotent when the expected type matches, and a genuine
//mismatch gets BindFuncRefToExpected's named diagnostic.
bool IsUnboundFuncRef(SyntaxNode &expr)
{
	if (expr.Kind() != NK_IdentifierExpr)
		return false;
	auto &idExpr = static_cast<SnIdentifierExpr&>(expr);
	if (!idExpr.Field() || idExpr.Field()->Kind() != NK_Function)
		return false;
	auto *pType = idExpr.EvalDataType();
	if (!IsFuncTypeDecl(pType))
		return true;
	return !FuncRefMatchesDecl(
		*static_cast<SnFunction*>(idExpr.Field()),
		static_cast<SnClassDecl*>(pType));
}

//Phase 13 Step 2: strict bind-site predicate for receiver-bound method
//references — `receiver.name` in a value position whose inner name
//resolved to a method of a class/interface/enum. Unbound while the
//member's own signature does not satisfy the Func type it carries (the
//same round-1 F1 discipline as IsUnboundFuncRef: a Func RETURN type
//leaking through the member tail is not a binding). Struct methods and
//module functions never match — they stay on their existing channels.
bool IsUnboundMemberFuncRef(SyntaxNode &expr)
{
	if (expr.Kind() != NK_MemberExpr)
		return false;
	auto &snMember = static_cast<SnMemberExpr&>(expr);
	auto *pInner = snMember.Inner();
	if (!pInner || pInner->Kind() != NK_IdentifierExpr)
		return false;
	auto *pMethod = static_cast<SnIdentifierExpr*>(pInner)->Field();
	if (!pMethod || pMethod->Kind() != NK_Function)
		return false;
	auto *pOwner = pMethod->Parent();
	if (!pOwner || (pOwner->Kind() != NK_ClassDecl
		&& pOwner->Kind() != NK_InterfaceDecl
		&& pOwner->Kind() != NK_EnumDecl))
		return false;
	auto *pType = snMember.EvalDataType();
	if (IsFuncTypeDecl(pType)
		&& FuncRefMatchesDecl(*static_cast<SnFunction*>(pMethod),
			static_cast<SnClassDecl*>(pType)))
		return false;
	return true;
}

void ExprResolveAccessor::Access(SnLiteralExpr &sn)
{
	assert(sn.IsResolved());
}

void ExprResolveAccessor::Access(SnNameExpr &nameExpr)
{
	if (nameExpr.IsResolved())
	{
		return;
	}

	auto pFieldExpr = nameExpr.Expr();
	assert(pFieldExpr);
	pFieldExpr->Accept(*m_pVisitor);

	//Builtin class names: ByteStream, FileStream, Object (Phase 8e-1).
	//Phase 9d: Exception hierarchy (Exception, NullPointerException,
	//DivByZeroException, IndexOutOfBoundsException, AssertionException).
	//When used as a type name (e.g. "ByteStream s = ..."), the name
	//doesn't exist in the AST namespace. Synthesize a singleton SnClassDecl.
	if (!pFieldExpr->IsResolved())
	{
		const auto& name = pFieldExpr->ToString();
		if (IsBuiltinClassName(name))
		{
			ResolveFieldExprAs(*pFieldExpr,
				GetBuiltinClassDecl(name, pFieldExpr->Location()));
		}
	}

	if (!pFieldExpr->IsResolved())
		return;

	ResolveFieldExprAs(nameExpr, pFieldExpr->Field());
}

//True when `expr` is a plain field lvalue (identifier, or member access
//whose inner name is an identifier) — the shapes whose Field() binding
//IS the variable/field itself. Pure SHAPE test (array redesign B):
//array-ness is the IsArrayValued() property, not part of this check.
bool IsPlainLvalueShape(const SnExpression& expr) {
	if (expr.Kind() == NK_IdentifierExpr)
		return true;
	if (expr.Kind() == NK_MemberExpr) {
		auto* pInner = static_cast<const SnMemberExpr&>(expr).Inner();
		return pInner && pInner->Kind() == NK_IdentifierExpr;
	}
	return false;
}

//0.7.3 B: the stamp became a TOKEN BINDER. The array-valued property
//is now derived (IsArrayValued reads the interned SnArrayTypeToken in
//EvalDataType). Every shape except a fresh allocation carries the
//token from ResolveFieldExprAs already: identifiers and plain members
//bind the declaration's token, user-method invokes bind the return
//type's token, and container element flows (List<T[]>.get, li[0],
//Func<R[],...> invokes) read the instantiation's type-args slots —
//which carry tokens natively because generic type arguments resolve
//through the same Access(SnArrayTypeExpr&) intern channel. Minting
//over any of those would wrap a token in a token (the double-wrap bug
//the gc-arm tests catch). Still called at every NF_Resolved success
//site — a new value shape that is array-valued without passing a
//declaration channel needs an arm here, or it silently loses its
//array-ness.
void ExprResolveAccessor::BindArrayTypeToken(SnExpression& expr) {
	if (!expr.IsResolved())
		return;
	if (expr.Kind() != NK_NewArrayExpr)
		return;
	//`new T[n]`: the resolve tail stored the element field in
	//EvalDataType — mint the token over it. A type alias may already
	//have spliced an array type in (its Field() IS a token); never
	//wrap a token in another token.
	auto* pElem = expr.EvalDataType();
	if (pElem && pElem->Kind() != NK_ArrayTypeToken)
		expr.EvalDataType(m_Env.InternArrayTypeToken(pElem));
}

//Depth of the array-type chain (int[] = 1, int[][] = 2, …). Depth >= 2
//is a jagged declaration form: the VM has no multi-dimensional layout
//(pre-token, the element masquerade additionally degraded such
//declarations twice silently), so it is rejected at the declaration
//site (array redesign B, spec §5.5).
int ArrayTypeDepth(const SnFieldExpr* pType)
{
	int depth = 0;
	for (const auto* pCur = pType; pCur
		&& pCur->Kind() == NK_ArrayTypeExpr; ++depth)
		pCur = static_cast<const SnArrayTypeExpr*>(pCur)->ElementType();
	return depth;
}

void ExprResolveAccessor::Access(SnArrayTypeExpr &arrTypeExpr)
{
	if (arrTypeExpr.IsResolved())
		return;

	auto *pElemType = arrTypeExpr.ElementType();
	assert(pElemType);
	pElemType->Accept(*m_pVisitor);
	if (!pElemType->IsResolved())
		return;

	//0.7.3 B: intern the array type token and bind the expression to
	//it — Field() and EvalDataType() become the token together
	//(ResolveFieldExprAs writes both channels). Declaration sites and
	//value sites then share one interned token per element type, and
	//the element masquerade is gone. This is the single intern site:
	//field/param/return types arrive through ResolveDataTypes, and
	//local/for/foreach type expressions through ExprResolver::Resolve.
	ResolveFieldExprAs(arrTypeExpr,
		m_Env.InternArrayTypeToken(pElemType->Field()));
}

//Phase 8e-3: resolve a built-in generic type expression like `List<int>`.
//The base NameExpr must match a known built-in generic (currently only
//"List"). Type arguments are resolved in the caller's scope. We then
//mint (or fetch) a synthetic SnClassDecl unique to (base, typeArgs) so
//that CalcTypeDistance's pointer-identity check distinguishes
//List<int> from List<string>.
//
//Note: we deliberately do NOT call pBase->Accept() — that would invoke
//Access(SnNameExpr&) which tries to resolve "List" as a regular name
//and fails. Built-in generic names exist only in Type position with
//type arguments; bare "List" is not a valid type.
void ExprResolveAccessor::Access(SnGenericTypeExpr &genType)
{
	if (genType.IsResolved())
		return;

	auto *pBase = genType.Base();
	assert(pBase);
	std::string baseName = pBase->ToString();
	if (!IsBuiltinGenericClassName(baseName))
	{
		m_Env.Log(CLL_Error, genType.Location(),
			"\"%s\" is not a built-in generic type.", baseName.c_str());
		return;
	}

	//Resolve each type argument (e.g., int, Point).
	std::vector<SnField*> typeArgs;
	std::vector<uint8> outFlags;
	for (auto *pTA : genType.TypeArgs())
	{
		if (!pTA) continue;
		pTA->Accept(*m_pVisitor);
		if (!pTA->IsResolved())
		{
			m_Env.Log(CLL_Error, pTA->Location(),
				"Cannot resolve type argument %s.",
				pTA->ToString().c_str());
			return;
		}
		auto *pField = pTA->Field();
		if (!pField)
		{
			m_Env.Log(CLL_Error, pTA->Location(),
				"Type argument %s has no resolved field.",
				pTA->ToString().c_str());
			return;
		}
		//Phase 13: void and out are Func-only type-argument features.
		//void may only occupy Func's first (return) type slot; out may
		//only mark Func parameter slots (any position after the first).
		bool isVoidArg = pField->Kind() == NK_Void;
		bool isOutArg = pTA->ContainFlags(NF_Out);
		if (isVoidArg && !(baseName == "Func" && typeArgs.empty()))
		{
			m_Env.Log(CLL_Error, pTA->Location(),
				"void is only allowed as the return slot of Func<...>.");
			return;
		}
		if (isOutArg && !(baseName == "Func" && !typeArgs.empty()))
		{
			m_Env.Log(CLL_Error, pTA->Location(),
				"out is only allowed on Func<...> parameters.");
			return;
		}
		//Array redesign B: a jagged type argument has no VM layout — the
		//same gate as declaration sites (spec §5.5), enforced here because
		//generic type-arg position is a distinct declaration form.
		if (ArrayTypeDepth(pTA) >= 2)
		{
			m_Env.Log(CLL_Error, pTA->Location(),
				"jagged arrays (T[][]) are not supported.");
			return;
		}
		//0.7.3 B: the resolved field above IS the type identity — an
		//array-typed argument carries the interned SnArrayTypeToken
		//(same intern channel as declarations), so the key, the display
		//name and every consumer read array-ness off the token itself.
		typeArgs.push_back(pField);
		outFlags.push_back(isOutArg ? 1 : 0);
	}

	//Phase 13 Step 2: Dict keyed by a Func type — DictKeysEqual is
	//identity for Func records (no interning), so two references to the
	//same function would store as two entries. Reject at the single
	//instantiation point; both declaration types and new-expression
	//types flow through here.
	if (baseName == "Dict" && !typeArgs.empty()
		&& IsFuncTypeDecl(typeArgs[0]))
	{
		m_Env.Log(CLL_Error, genType.Location(),
			"function types cannot be used as Dict keys.");
		return;
	}

	auto *pSynClass = GetGenericClassDecl(baseName, typeArgs, outFlags,
		pBase->Location());
	if (!pSynClass)
	{
		m_Env.Log(CLL_Error, genType.Location(),
			"Generic instantiation %s<...> is not supported in this phase.",
			baseName.c_str());
		return;
	}

	ResolveFieldExprAs(genType, pSynClass);
}

void ExprResolveAccessor::Access(SnIdentifierExpr &idExpr)
{
	if (idExpr.IsResolved())
	{
		if (!idExpr.PostResolveCheck(m_Env))
			idExpr.RemoveFlags(NF_Resolved);
		return;
	}

	SnField *pField;
	//Module import visibility (D1/D7): the value-position bare pool spans
	//the current TU's directory too — foreign root/namespace function
	//candidates are skipped (ownerless symbols stay visible). curModule is
	//computed once and shared by the filter and the visibility hint below.
	const uint32_t curModule = m_Env.Registry().OwnerOfContext(*m_pContext);
	std::function<bool(SnField &)> bareFuncFilter =
		[this, curModule](SnField &field) -> bool
	{
		//No NK_Function pre-check: FindFieldInAncestor only invokes the
		//filter on function candidates in bare-pool scopes.
		return IsBareVisible(static_cast<SnFunction &>(field), curModule);
	};
	pField = FindFieldInAncestor(idExpr.Name(), *m_pContext,
		*m_pAccessor, Flags(), bareFuncFilter);

	if (!pField && ContainFlags(ERF_IgnoreUsings))
	{
		assert(idExpr.Usings());
		pField = FindFieldInUsings(idExpr.Name(), *idExpr.Usings(),
			*m_pAccessor);
	}

	if (!pField)
	{
		//Builtin class names: ByteStream, FileStream, Object (Phase 8e-1).
		//Phase 9d: Exception hierarchy.
		//Synthesize a singleton SnClassDecl when the name is not found.
		const auto& name = idExpr.Name();
		if (IsBuiltinClassName(name))
		{
			ResolveFieldExprAs(idExpr, GetBuiltinClassDecl(name, idExpr.Location()));
			return;
		}

		m_Env.Log(CLL_Error, "Cannot resolve the field: %s.",
			idExpr.Name().c_str());
		//Module import visibility (F20): the name may only exist as a
		//function outside this TU's bare pool — name the owning module.
		MaybeLogVisibilityHint(idExpr.Name(), idExpr.Location(), curModule);
		//spec §6.2 last line: a module path sharing the name turns a bare
		//mystery into a named fix (import + qualification).
		MaybeLogModuleHint(idExpr.Name());
		return;
	}

	ResolveFieldExprAs(idExpr, pField);
	BindArrayTypeToken(idExpr);
}

void ExprResolveAccessor::Access(SnInvokeExpr &snInvoke)
{
	assert(!snInvoke.IsResolved());

	if (!ResolveExpressionList(snInvoke.Params()))
		return;

	//Phase 9c: caller-side syntax validation — independent of candidates.
	//Reports specific errors for structural issues that no overload can
	//satisfy (e.g. positional arg after named, duplicate named names).
	if (!ValidateInvokeSyntax(snInvoke))
		return;

	//Phase 13: delegate call — the callee name resolves to a Func-typed
	//value (local / param / class field) rather than a function. Name
	//lookup order puts the Func value first (Python-style shadowing of a
	//same-named function); every path inside consumes the invoke.
	if (auto *pDelegateField = FindDelegateTarget(snInvoke))
	{
		BindDelegateInvoke(snInvoke, pDelegateField);
		return;
	}

	SnFunction *pCallee;
	std::vector<FormalBinding> bindings;
	bool bNameMatchedImported = false;
	bool bVisibilityHintLogged = false;
	auto res = FindFuncByInvoke(pCallee, snInvoke, bindings,
		bNameMatchedImported, bVisibilityHintLogged);

	//Phase 9e: out arguments on virtual (by-name dispatched) methods are
	//rejected before anything binds — see OutArgOnDispatchedCalleeRejected.
	if (pCallee
		&& OutArgOnDispatchedCalleeRejected(snInvoke, *pCallee, bindings))
		return;

	//Phase 12 (D9): a bare (receiver-less) invoke that binds to a METHOD
	//is a frame-shift bug — codegen's bare-invoke path emits OP_CallFunc
	//with slotBase 0 while the callee's frame expects `this` at slot 0,
	//so the first argument is read as the receiver (enum methods return
	//silent garbage; class methods fail with a field-access error — the
	//class side pre-dates Phase 12). The grammar only produces method
	//calls as the Inner of a MemberExpr (`c.f()`, `this.f()`); an invoke
	//in any other position (statement, nested argument, outer of a member
	//access) that binds to a method is bare. Free functions (parent
	//namespace) are unaffected.
	auto *pInvokeParent = snInvoke.Parent();
	bool bIsMethodCallShape = pInvokeParent
		&& pInvokeParent->Kind() == NK_MemberExpr
		&& static_cast<SnMemberExpr*>(pInvokeParent)->Inner() == &snInvoke;
	if (pCallee && !bIsMethodCallShape)
	{
		auto *pCalleeParent = pCallee->Parent();
		if (pCalleeParent && (pCalleeParent->Kind() == NK_ClassDecl
			|| pCalleeParent->Kind() == NK_InterfaceDecl
			|| pCalleeParent->Kind() == NK_EnumDecl))
		{
			m_Env.Log(CLL_Error, snInvoke.Location(),
				"method \"%s\" must be called through a receiver "
				"(e.g. this.%s(...)).",
				pCallee->Name().c_str(), pCallee->Name().c_str());
			return;
		}
	}

	if (res == FFR_ExactMatch || res == FFR_ApproximateMatch)
	{
		//M3b: the success tail is shared with the module-qualified call
		//path. A false return means a function-reference argument failed
		//to bind (diagnostic already logged) — leave the invoke unresolved.
		(void)ResolveInvokeWithFunc(snInvoke, *pCallee, res, bindings);
		return;
	}

	//Module import visibility (F20/M4): the "not visible here" hint is
	//the complete diagnosis — appending the generic failure text would
	//only blur it.
	if (!bVisibilityHintLogged)
		LogInvokeFailure(snInvoke, res, pCallee, bNameMatchedImported);
}

//Phase 9c: validate caller-side argument syntax (candidate-independent).
//Reports specific errors for:
//  - positional arg following a named arg
//  - duplicate name in named args (e.g. foo(a=1, a=2))
//Returns true if well-formed; false after logging.
bool ExprResolveAccessor::ValidateInvokeSyntax(const SnInvokeExpr &invoke)
{
	bool bSeenNamed = false;
	std::set<std::string> seenNames;
	for (auto &actual : invoke.Params())
	{
		if (actual.Kind() != NK_NamedArgExpr)
		{
			if (bSeenNamed)
			{
				m_Env.Log(CLL_Error, actual.Location(),
					"positional argument cannot follow a named argument in "
					"call to \"%s\".",
					invoke.CalleeName().c_str());
				return false;
			}
			continue;
		}
		auto &named = static_cast<const SnNamedArgExpr&>(actual);
		bSeenNamed = true;
		auto [it, inserted] = seenNames.emplace(named.Name());
		if (!inserted)
		{
			m_Env.Log(CLL_Error, named.Location(),
				"duplicate named argument \"%s\" in call to \"%s\".",
				named.Name().c_str(), invoke.CalleeName().c_str());
			return false;
		}
	}
	return true;
}

//Phase 9c: SnNamedArgExpr resolver. The name is consumed by TryBindInvoke
//when matching formals; here we only need to resolve the inner expression
//and propagate its EvalDataType / resolved flag so the parent invoke can
//type-check the binding.
void ExprResolveAccessor::Access(SnNamedArgExpr &sn)
{
	assert(!sn.IsResolved());
	auto *pInner = sn.Inner();
	assert(pInner);
	pInner->Accept(*m_pVisitor);
	if (!pInner->IsResolved())
		return;
	sn.EvalDataType(pInner->EvalDataType());
	sn.AddFlags(NF_Resolved);
}

//Phase 9e: SnOutArgExpr resolver. Grammar restricts the inner to a plain
//identifier; here we additionally require it to bind to a caller-frame
//slot — a local variable or a formal parameter of the calling function
//(both register as NK_FormalParam-kind fields). Class fields (implicit
//this.x), enum members and globals are rejected: the writeback copies
//the callee's out slot to a frame offset only.
void ExprResolveAccessor::Access(SnOutArgExpr &sn)
{
	assert(!sn.IsResolved());
	auto *pInner = sn.Inner();
	assert(pInner);
	if (pInner->Kind() != NK_IdentifierExpr)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"out argument must be a plain local variable.");
		return;
	}
	pInner->Accept(*m_pVisitor);
	if (!pInner->IsResolved())
		return;
	auto *pField = static_cast<SnIdentifierExpr*>(pInner)->Field();
	if (!pField || pField->Kind() != NK_FormalParam)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"out argument \"%s\" must be a local variable or parameter "
			"of the calling function.",
			pInner->ToString().c_str());
		return;
	}
	sn.EvalDataType(pInner->EvalDataType());
	sn.AddFlags(NF_Resolved);
}






void ExprResolveAccessor::Access(SnCastExpr &sn)
{
}

//Phase 8e-1.5: resolve `expr as T` runtime-checked cast.
//Valid kinds: TCK_Same (no-op), TCK_Box (primitive→Object), TCK_Unbox (Object→primitive),
//TCK_Downcast (ancestor→subclass). Other kinds → compile error.
void ExprResolveAccessor::Access(SnAsExpr &sn)
{
	assert(!sn.IsResolved());

	//Resolve operand first (its EvalDataType is needed for cast computation).
	sn.Operand()->Accept(*m_pVisitor);
	if (!sn.Operand()->IsResolved())
		return;

	//Resolve target type name (its Field() will be the target SnField*).
	sn.TargetType()->Accept(*m_pVisitor);
	if (!sn.TargetType()->IsResolved())
		return;

	auto *pSrcType = sn.Operand()->EvalDataType();
	auto *pTgtType = sn.TargetType()->Field();
	if (!pSrcType || !pTgtType)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"Cannot resolve types for `as` expression.");
		return;
	}

	//Array guard, `as` flavor (0.7.2 rule, kept verbatim): `as` has no
	//array-typed target spelling (the target is a name expression), and
	//string targets are assignment-only coercions — so no legal form
	//exists for an array-valued operand. Post-0.7.3 B the cast table
	//would reject every spelling anyway (token×scalar = None); the
	//named branch keeps the diagnostic specific.
	if (sn.Operand()->IsArrayValued())
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"Invalid cast \"%s as %s\": the cast operand is an array.",
			sn.Operand()->ToString().c_str(),
			sn.TargetType()->ToString().c_str());
		return;
	}

	TypeCastInfo castInfo(pSrcType, pTgtType);
	auto kind = castInfo.Kind();

	//Phase 13: `f as string` renders the handle ("func <name>"). Class→
	//string is normally an assignment-only coercion (TCK_Auto is rejected
	//for `as`); function handles get an explicit branch so all four
	//conversion paths agree (codegen emits OP_Func_to_str).
	if (pSrcType->Kind() == NK_ClassDecl && pTgtType->Kind() == NK_String
		&& static_cast<SnClassDecl*>(pSrcType)->IsFuncType())
	{
		sn.SetResolved(pTgtType, TCK_Auto);
		sn.EvalDataType(pTgtType);
		return;
	}

	if (kind == TCK_None)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"Invalid cast: `%s as %s` is not allowed.",
			pSrcType->ToString().c_str(),
			pTgtType->ToString().c_str());
		return;
	}

	//TCK_Auto (e.g. int→float) is not allowed via `as` — use primitive cast syntax.
	//TCK_Dynamic similarly. Only TCK_Same/Box/Unbox/Downcast are valid.
	if (kind != TCK_Same && kind != TCK_Box && kind != TCK_Unbox && kind != TCK_Downcast)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"`as` cannot perform implicit conversion `%s` → `%s`.",
			pSrcType->ToString().c_str(),
			pTgtType->ToString().c_str());
		return;
	}

	sn.SetResolved(pTgtType, kind);
	//EvalDataType must be the target type itself (SnInt32 for `as int`,
	//SnClassDecl for `as Foo`). Calling pTgtType->EvalDataType() would
	//yield SnType::Instance() (the type-of-type) since type-name fields
	//like SnInt32 are SnBuiltinDataType whose EvalDataType() is SnType,
	//and downstream cast checks would fail with TCK_None.
	sn.EvalDataType(pTgtType);
	//`null as T` is still the null literal semantically: propagate the
	//flag so downstream gates (the null-identity skip in FixupExprType,
	//the boxing guard family) see through the cast wrapper. ContainFlags
	//is a flat bit test — wrapper nodes do not inherit child flags.
	if (sn.Operand()->ContainFlags(NF_NullLiteral))
		sn.AddFlags(NF_NullLiteral);
}

void ExprResolveAccessor::Access(SnBinaryExpr &sn)
{
	assert(!sn.IsResolved());

	sn.Left()->Accept(*m_pVisitor);
	if (!sn.Left()->IsResolved())
		return;

	if (sn.Right())
	{
		sn.Right()->Accept(*m_pVisitor);
		if (!sn.Right()->IsResolved())
			return;
	}

	auto op = sn.Op();
	const bool isCompare = op == SnBinaryExpr::OP_Less
		|| op == SnBinaryExpr::OP_LessEqual
		|| op == SnBinaryExpr::OP_Greater
		|| op == SnBinaryExpr::OP_GreaterEqual
		|| op == SnBinaryExpr::OP_Equal
		|| op == SnBinaryExpr::OP_NotEqual;
	if (isCompare || op == SnBinaryExpr::OP_LogicalAnd ||
		op == SnBinaryExpr::OP_LogicalOr ||
		op == SnBinaryExpr::OP_LogicalNot)
	{
		if (isCompare)
		{
			//Phase 11 Q4: relational operands get type checks here — the
			//old shortcut set Int32 blindly and codegen picked the opcode
			//variant from the left operand alone.
			auto* L = sn.Left()->EvalDataType();
			auto* R = sn.Right() ? sn.Right()->EvalDataType() : nullptr;
			NodeKind lk = L ? L->Kind() : NK_Int32;
			NodeKind rk = R ? R->Kind() : NK_Int32;
			bool lNull = sn.Left()->ContainFlags(NF_NullLiteral);
			bool rNull = sn.Right()
				&& sn.Right()->ContainFlags(NF_NullLiteral);

			//Mixed string/non-string has no semantics ("a" < 5), in either
			//operand order. Null literals are exempt: KT_Null is Int32-
			//typed, and `s == null` / `c == null` are the established null
			//checks — the null side keeps its raw sentinel bits and takes
			//the identity/sentinel comparison path.
			if ((lk == NK_String) != (rk == NK_String) && !lNull && !rNull)
			{
				m_Env.Log(CLL_Error, sn.Location(),
					"cannot compare string with a non-string operand "
					"(only null is allowed as the other side).");
				return;
			}

			//Phase 13: function handles support content equality only
			//(==/!=), and only against another function handle or null.
			//Without this gate codegen's dispatch would silently fall to
			//the integer variant comparing raw heap indices.
			bool lFunc = IsFuncTypeDecl(L);
			bool rFunc = IsFuncTypeDecl(R);
			if (lFunc || rFunc)
			{
				bool bEq = op == SnBinaryExpr::OP_Equal
					|| op == SnBinaryExpr::OP_NotEqual;
				if (!bEq)
				{
					m_Env.Log(CLL_Error, sn.Location(),
						"function values cannot be ordered; only == and != "
						"are supported.");
					return;
				}
				if (!((lFunc && rFunc) || (lFunc && rNull)
					|| (rFunc && lNull)))
				{
					m_Env.Log(CLL_Error, sn.Location(),
						"a function value can only be compared with a "
						"function value or null.");
					return;
				}
			}

			//0.7.3 B D8: array values support identity comparison only
			//(==/!=), and only against another array or null. Without
			//this gate relational mixes (`a < b`) and scalar mixes
			//(`a == 5`) compile and silently compare the raw heap
			//handle against the operand. Mirrors the function-handle
			//gate above; null keeps the sentinel comparison path.
			bool lArr = lk == NK_ArrayTypeToken;
			bool rArr = rk == NK_ArrayTypeToken;
			if (lArr || rArr)
			{
				bool bEq = op == SnBinaryExpr::OP_Equal
					|| op == SnBinaryExpr::OP_NotEqual;
				if (!bEq || !((lArr && rArr) || (lArr && rNull)
					|| (rArr && lNull)))
				{
					m_Env.Log(CLL_Error, sn.Location(),
						"array values support identity comparison only "
						"(==/!=), and only against another array or "
						"null.");
					return;
				}
			}

			//Symmetric int/float promotion (Phase 8e-8 mechanism) extended
			//to comparisons. Prerequisite the arithmetic branch does not
			//have: BOTH operands numeric and neither a null literal —
			//class/enum/null pairs stay on their existing identity or
			//sentinel paths, and wrapping them (as the arithmetic branch
			//would) would break e.g. class identity equality.
			bool lNum = lk == NK_Int32 || lk == NK_Float;
			bool rNum = rk == NK_Int32 || rk == NK_Float;
			if (lNum && rNum && !lNull && !rNull && lk != rk)
			{
				SnField* T_promote =
					(lk == NK_Float || rk == NK_Float)
					? SnBuiltinDataType::InstanceOf(NK_Float)
					: SnBuiltinDataType::InstanceOf(NK_Int32);
				auto it = sn.Children().begin();
				auto& leftExpr = static_cast<SnExpression&>(*it);
				TypeCastInfo leftCI(leftExpr.EvalDataType(), T_promote);
				FixupExprType(it, leftCI);
				++it;
				auto& rightExpr = static_cast<SnExpression&>(*it);
				TypeCastInfo rightCI(rightExpr.EvalDataType(), T_promote);
				FixupExprType(it, rightCI);
			}
		}
		//Short-circuit hardening (2026-08-31): logical operands feed
		//OP_JumpIfNot, which reads one int32 — the same policy as
		//statement conditions (CheckIntCondition in
		//StatementResolver.hpp; widen both together). Without this
		//gate a float/string operand is read as raw bits, giving
		//garbage truthiness.
		if (!isCompare)
		{
			auto bop = sn.Op();
			const char* szOp = bop == SnBinaryExpr::OP_LogicalAnd
				? "&&" : bop == SnBinaryExpr::OP_LogicalOr
				? "||" : "!";
			const char* szShape = sn.Right()
				? "int operands" : "an int operand";
			auto* pLT = sn.Left()->EvalDataType();
			if (pLT && pLT->Kind() != NK_Int32)
			{
				m_Env.Log(CLL_Error, sn.Left()->Location(),
					"operator '%s' requires %s, got \"%s\".",
					szOp, szShape, pLT->ToString().c_str());
				return;
			}
			if (sn.Right())
			{
				auto* pRT = sn.Right()->EvalDataType();
				if (pRT && pRT->Kind() != NK_Int32)
				{
					m_Env.Log(CLL_Error, sn.Right()->Location(),
						"operator '%s' requires %s, got \"%s\".",
						szOp, szShape, pRT->ToString().c_str());
					return;
				}
			}
		}
		auto* intType = SnBuiltinDataType::InstanceOf(NK_Int32);
		sn.EvalDataType(intType);
	}
	else
	{
		//Phase 8e-8: symmetric arithmetic promotion.
		//Both operands are promoted to the wider type (int<float). For string
		//only OP_Add is valid (concat); other ops on string are rejected here.
		//Each operand is wrapped in SnCastExpr if its type differs from T_result
		//so that codegen sees uniform operand types matching bin.EvalDataType().
		//Phase 11 Q4: null is only meaningful through the comparison identity
		//path above; arithmetic/concat with null is a compile error. (Before
		//the null-sentinel fix it silently produced "0" concatenations; after
		//it, an unwrapped raw 0.)
		if (sn.Left()->ContainFlags(NF_NullLiteral)
			|| (sn.Right() && sn.Right()->ContainFlags(NF_NullLiteral)))
		{
			m_Env.Log(CLL_Error, sn.Location(),
				"null is not a valid arithmetic operand.");
			return;
		}
		auto* L = sn.Left()->EvalDataType();
		auto* R = sn.Right() ? sn.Right()->EvalDataType() : nullptr;
		NodeKind lk = L ? L->Kind() : NK_Int32;
		NodeKind rk = R ? R->Kind() : NK_Int32;

		SnField* T_result = nullptr;
		if (lk == NK_String || rk == NK_String)
		{
			if (op != SnBinaryExpr::OP_Add)
			{
				m_Env.Log(CLL_Error, sn.Location(),
					"operator not supported on string.");
				return;
			}
			//Phase 8e-9a: allow mixed (e.g. int + string). The non-string
			//operand is wrapped in SnCastExpr below; VmBackend.cpp:951
			//emits OP_Int32_to_str / OP_Float_to_str for the conversion.
			//Then OP_Concat_str concatenates the two string indices.
			T_result = SnBuiltinDataType::InstanceOf(NK_String);
		}
		else if (lk == NK_Float || rk == NK_Float)
		{
			T_result = SnBuiltinDataType::InstanceOf(NK_Float);
		}
		else
		{
			T_result = SnBuiltinDataType::InstanceOf(NK_Int32);
		}
		sn.EvalDataType(T_result);

		//Wrap each operand (in-place via FixupExprType) if its type differs
		//from T_result. After wrap, sn.Children()[0]/[1] hold the (possibly
		//cast) expressions; sn.Left()/Right() are stale but unused by codegen.
		auto it = sn.Children().begin();
		auto& leftExpr = static_cast<SnExpression&>(*it);
		TypeCastInfo leftCI(leftExpr.EvalDataType(), T_result);
		FixupExprType(it, leftCI);
		if (sn.Right())
		{
			++it;
			auto& rightExpr = static_cast<SnExpression&>(*it);
			TypeCastInfo rightCI(rightExpr.EvalDataType(), T_result);
			FixupExprType(it, rightCI);
		}
	}
	sn.AddFlags(NF_Resolved);
}

void ExprResolveAccessor::Access(SnNewExpr &sn)
{
	assert(!sn.IsResolved());

	auto pClassName = sn.ClassName();
	assert(pClassName);
	pClassName->Accept(*m_pVisitor);

	//Builtin class names: ByteStream, FileStream, Object.
	//Phase 9d: Exception hierarchy.
	//These don't exist in the AST namespace, so the name won't resolve
	//through the normal path. Use the singleton SnClassDecl.
	if (!pClassName->IsResolved())
	{
		const auto& name = pClassName->ToString();
		if (IsBuiltinClassName(name))
		{
			ResolveFieldExprAs(*pClassName,
				GetBuiltinClassDecl(name, pClassName->Location()));
		}
	}

	if (!pClassName->IsResolved())
		return;

	auto pClassField = pClassName->Field();
	if (!pClassField || pClassField->Kind() != NK_ClassDecl)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"\"%s\" is not a class type.", pClassName->ToString().c_str());
		return;
	}

	auto pClassDecl = static_cast<SnClassDecl*>(pClassField);
	//Phase 13: Func types are structural — values come only from function
	//or method references, so there is no by-name construction.
	if (IsGenericClassDecl(pClassDecl) && pClassDecl->BaseName() == "Func")
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"Func types cannot be constructed by name; bind a function "
			"or method reference");
		return;
	}
	sn.ClassDecl(pClassDecl);
	sn.EvalDataType(pClassDecl);
	sn.AddFlags(NF_Resolved);

	//Round-14: constructor calls emit their args positionally without
	//FormalBindings (VmBackend's NewExpr handler), so — like super(...) —
	//named arguments cannot bind, defaults declared on ctor params are not
	//applied at the call site, and the arity must match exactly. Pre-fix a
	//mismatched call compiled clean: missing args read uninitialized
	//callParam slots (a defaulted param arrived as garbage), extras were
	//silently dropped, named args hit codegen's unhandled-kind internal
	//error. Imported class stubs are skipped — their ctor lives only in the
	//merged CompiledClass, not in AST members.
	if (!pClassDecl->IsImported())
	{
		size_t argCount = 0;
		for (auto &arg : sn.Args())
		{
			if (&arg == sn.ClassName()) continue;  //Args() view includes it
			if (arg.Kind() == NK_NamedArgExpr)
			{
				m_Env.Log(CLL_Error, arg.Location(),
					"named arguments are not supported in constructor calls");
				continue;
			}
			if (arg.Kind() == NK_OutArgExpr)
			{
				m_Env.Log(CLL_Error, arg.Location(),
					"out arguments are not supported in constructor calls.");
				continue;
			}
			++argCount;
		}
		//Expected ctor arity. User classes look up the ctor SnFunction; the
		//rest are intrinsic ctor stubs registered by
		//VmBackend::RegisterBuiltinClasses (arity excludes `this`).
		size_t ctorArity = 0;
		bool hasCtor = false;
		if (IsGenericClassDecl(pClassDecl))
		{
			hasCtor = true;  //List/Dict ctor stubs take only `this`
		}
		else if (pClassDecl->IsBuiltinClass())
		{
			//FileStream(this, path, mode) → 2; Exception family
			//(this, message) → 1; Object/ByteStream → 0.
			hasCtor = true;
			const auto& clsName = pClassDecl->Name();
			if (clsName == "FileStream") ctorArity = 2;
			else if (IsBuiltinExceptionClassName(clsName)) ctorArity = 1;
		}
		else
		{
			for (auto& member : pClassDecl->Members())
			{
				if (member.Kind() == NK_Function
					&& member.Name() == pClassDecl->Name())
				{
					hasCtor = true;
					ctorArity = static_cast<SnFunction&>(member)
						.Params().size();
					break;
				}
			}
		}
		if (!hasCtor)
		{
			if (argCount != 0)
				m_Env.Log(CLL_Error, sn.Location(),
					"class \"%s\" has no constructor; new %s() cannot take "
					"arguments",
					pClassDecl->Name().c_str(), pClassDecl->Name().c_str());
		}
		else if (argCount != ctorArity)
		{
			m_Env.Log(CLL_Error, sn.Location(),
				"constructor of \"%s\" expects %zu argument(s), got %zu",
				pClassDecl->Name().c_str(), ctorArity, argCount);
		}
	}

	if (!ResolveExpressionList(sn.Args()))
		return;
}

void ExprResolveAccessor::Access(SnNewArrayExpr &sn)
{
	assert(!sn.IsResolved());

	//Resolve element type name.
	auto pElemType = sn.ElementType();
	assert(pElemType);
	if (pElemType->IsArrayType())
	{
		//Resolve nested element type for array-of-arrays (future extension).
		m_Env.Log(CLL_Error, sn.Location(),
			"Multi-dimensional arrays are not supported.");
		return;
	}
	pElemType->Accept(*m_pVisitor);
	if (!pElemType->IsResolved())
		return;

	auto pElemField = pElemType->Field();
	if (!pElemField)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"\"%s\" is not a valid array element type.",
			pElemType->ToString().c_str());
		return;
	}

	//Resolve size expression with direct Accept (SnExpressionList wrapping is broken).
	sn.Size()->Accept(*m_pVisitor);
	if (!sn.Size()->IsResolved())
		return;

	//EvalDataType briefly stores the element type, then BindArrayTypeToken
	//overwrites it with the interned array token (the array-valued
	//property). Codegen's array registration reads the AST element type
	//expression, not this slot.
	sn.EvalDataType(pElemField);
	sn.AddFlags(NF_Resolved);
	BindArrayTypeToken(sn);
}

//Phase 8e-6: Collection initializer resolver.
//Two paths:
//- Explicit form `new Type{...}`: ExplicitType() carries the type
//  expression. Resolve it like a normal type, set EvalDataType to its
//  resolved Field.
//- Bare form `[...]`: ExplicitType() is null. The parent AssignStmt
//  resolver must have populated InferredTarget() from the LHS variable's
//  type. Use that directly as EvalDataType.
//Entry values are resolved last via direct Accept (children inherit
//no expected type for now — they resolve via their normal paths).
void ExprResolveAccessor::Access(SnInitListExpr &sn)
{
	assert(!sn.IsResolved());

	SnField *pTargetField = nullptr;
	bool bIsArray = false;

	if (auto *pExplicit = sn.ExplicitType())
	{
		//Resolve the explicit type expression (NameExpr/GenericTypeExpr).
		pExplicit->Accept(*m_pVisitor);
		if (!pExplicit->IsResolved())
			return;
		pTargetField = pExplicit->Field();
		bIsArray = pExplicit->IsArrayType();
	}
	else if (auto *pInferred = sn.InferredTarget())
	{
		//Bare form: parent populated the LHS variable. bIsArray keeps the
		//declaration-side signal (IsArrayType()); the type slot carries
		//the interned token for array variables, peeled to the element
		//below.
		bIsArray = pInferred->IsArrayType();
		pTargetField = pInferred->EvalDataType();
		//0.7.3 B token path: an
		//array-typed LHS carries the interned token; the init-list's
		//per-entry element gate consumes the ELEMENT (the node itself
		//keeps the element contract — codegen's RegisterArrayType reads
		//it directly).
		if (pTargetField
			&& pTargetField->Kind() == NK_ArrayTypeToken)
			pTargetField = static_cast<SnArrayTypeToken*>(
				pTargetField)->ElemTypeOf();
	}

	if (!pTargetField)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"Collection initializer requires an explicit type or an LHS "
			"context to infer the target type.");
		return;
	}

	sn.EvalDataType(pTargetField);
	sn.TargetIsArray(bIsArray);
	sn.AddFlags(NF_Resolved);

	//Class init lists lower to `new C()` + per-field stores; the implicit
	//ctor call must not require arguments (spec: class init requires a
	//no-arg constructor, explicit or implicit). Without this check the
	//codegen emits the ctor call anyway and the VM copies garbage/reads
	//past the frame for the missing params.
	if (!bIsArray && pTargetField->Kind() == NK_ClassDecl) {
		auto* pClassDecl = static_cast<SnClassDecl*>(pTargetField);
		if (!pClassDecl->IsBuiltinClass()) {
			//Class initializers are identifier-keyed only: the codegen
			//per-field store dispatches on the key name, and declaration
			//order is meaningless with inherited fields (layout is
			//root-ancestor-first). Value-only entries used to be silently
			//skipped, leaving every field at its ctor default — reject
			//them instead (struct targets keep the ordinal form).
			for (auto& entry : sn.Entries()) {
				if (entry.keyKind != InitEntry::KeyKind::Identifier) {
					m_Env.Log(CLL_Error,
						entry.pValue ? entry.pValue->Location()
							: sn.Location(),
						"class initializer \"new %s{...}\" requires "
						"field:value entries; the positional form is "
						"only valid for struct/array targets",
						pClassDecl->Name().c_str());
					break;
				}
			}
			for (auto& field : pClassDecl->Members()) {
				if (field.Kind() == NK_Function
					&& field.Name() == pClassDecl->Name()) {
					size_t arity = static_cast<SnFunction&>(field)
						.Params().size();
					if (arity > 0) {
						m_Env.Log(CLL_Error, sn.Location(),
							"class initializer \"new %s{...}\" requires a "
							"no-arg constructor; \"%s\" takes %zu "
							"argument(s)",
							pClassDecl->Name().c_str(),
							pClassDecl->Name().c_str(), arity);
					}
					break;
				}
			}
		}
	}

	//Resolve each entry value. Keys (for {...} form) are not expressions
	//and need no resolution.
	for (auto &entry : sn.Entries())
	{
		if (entry.pValue)
			entry.pValue->Accept(*m_pVisitor);
	}

	//Phase 13: init-list entries bind pending function references to the
	//element type — the array element type, or T of a generic container
	//(`List<Func<int,int>> l = [bar];`). Class targets carry no element
	//type and are skipped.
	SnField *pElemType = nullptr;
	if (bIsArray)
	{
		pElemType = pTargetField;
	}
	else if (pTargetField->Kind() == NK_ClassDecl)
	{
		auto elemArgs = GetGenericTypeArgs(
			static_cast<SnClassDecl*>(pTargetField));
		if (!elemArgs.empty())
			pElemType = elemArgs[0];
	}
	if (pElemType)
	{
		for (auto &entry : sn.Entries())
		{
			if (!entry.pValue)
				continue;
			if (IsUnboundFuncRef(*entry.pValue))
			{
				BindFuncRefToExpected(m_Env,
					*static_cast<SnIdentifierExpr*>(entry.pValue),
					pElemType);
			}
			else if (IsUnboundMemberFuncRef(*entry.pValue))
			{
				BindMemberFuncRefToExpected(m_Env,
					*static_cast<SnMemberExpr*>(entry.pValue),
					pElemType);
			}
		}
	}

	//Array-form element-type gate: every entry is an element store, so
	//it takes the same conversion checks as `arr[i] = v` — the cast
	//table adjudicates (0.7.3 B): a mismatched entry is rejected by
	//FixupExprType (named array diagnostic for cross-element array
	//values), and the wrap makes the codegen's OP_StoreElement emit the
	//box/coercion. Container forms (List/Dict) already box through
	//their own per-method plans and are not array-form.
	if (bIsArray && pElemType)
	{
		const auto &entries = sn.Entries();
		for (size_t nIdx = 0; nIdx < entries.size(); ++nIdx)
		{
			auto *pValue = entries[nIdx].pValue;
			if (!pValue || !pValue->IsResolved()
				|| !pValue->EvalDataType())
				continue;
			TypeCastInfo castInfo(pValue->EvalDataType(), pElemType);
			auto iExpr = sn.Children().find(pValue);
			if (iExpr == sn.Children().end())
				continue;
			if (FixupExprType(iExpr, castInfo))
				sn.SetEntryValue(nIdx,
					&static_cast<SnCastExpr &>(*iExpr));
		}
	}
}

void ExprResolveAccessor::Access(SnSubscriptExpr &sn)
{
	assert(!sn.IsResolved());

	//Resolve array expression.
	auto& arrayExpr = *sn.Array();
	arrayExpr.Accept(*m_pVisitor);
	if (!arrayExpr.IsResolved())
		return;

	//Resolve index expression.
	auto& indexExpr = *sn.Index();
	indexExpr.Accept(*m_pVisitor);
	if (!indexExpr.IsResolved())
		return;

	//Look up arr.length-style access is handled by MemberExpr.
	//For now, the result type of subscript is the element type.
	auto* arrayType = arrayExpr.EvalDataType();
	//0.7.3 B D3: a string base has no subscript semantics (NLang has
	//no char type — the substring methods are the char-access surface).
	//Before this arm the subscript silently resolved to the string
	//itself and codegen read the index as an array handle, failing
	//only at runtime ("null array access").
	if (arrayType && arrayType->Kind() == NK_String)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"string does not support subscript access.");
		return;
	}
	//List<T>/Dict<K,V> subscript (li[i] / d[k]): sugar over get().
	//The base resolves to a synthetic generic-instantiation class; the
	//element type is T (List) or V (Dict). Without this peel the
	//subscript keeps the container type and every consumer (assignment,
	//member chains, nested subscripts) mis-types it.
	//
	//0.7.3 B: an array-valued base carries the interned token in
	//EvalDataType (NK_ArrayTypeToken, never a ClassDecl), so the Kind()
	//check alone distinguishes `List<int>[] a` from `List<int> li` —
	//the masquerade-era IsPlainLvalueShape guard here was removed with
	//the side channel. Array bases keep the plain element-type
	//propagation below, peeling the token.
	if (arrayType && arrayType->Kind() == NK_ClassDecl)
	{
		auto* pClass = static_cast<SnClassDecl*>(arrayType);
		if (pClass->IsGenericInstantiation())
		{
			const auto& baseName = pClass->BaseName();
			const auto& typeArgs = pClass->GenericTypeArgs();
			SnField* elem = nullptr;
			if (baseName == "List" && !typeArgs.empty())
				elem = typeArgs[0];
			else if (baseName == "Dict" && typeArgs.size() > 1)
				elem = typeArgs[1];
			if (elem)
			{
				sn.EvalDataType(elem);
				sn.AddFlags(NF_Resolved);
				BindArrayTypeToken(sn);
				return;
			}
		}
	}
	if (arrayType)
	{
		//0.7.3 B token path: an
		//array-valued base carries the interned array token — the
		//subscript's own type is its ELEMENT.
		if (arrayType->Kind() == NK_ArrayTypeToken)
			arrayType = static_cast<SnArrayTypeToken*>(
				arrayType)->ElemTypeOf();
		sn.EvalDataType(arrayType);
	}
	sn.AddFlags(NF_Resolved);
	BindArrayTypeToken(sn);
}

void ExprResolveAccessor::Access(SnThisExpr &sn)
{
	assert(!sn.IsResolved());

	auto pContext = m_pContext;
	while (pContext)
	{
		if (pContext->Kind() == NK_Function)
		{
			auto pParent = pContext->Parent();
			if (pParent && pParent->Kind() == NK_ClassDecl)
			{
				auto pClassDecl = static_cast<SnClassDecl*>(pParent);
				sn.EvalDataType(pClassDecl);
				sn.AddFlags(NF_Resolved);
				return;
			}
			//Phase 12: enum methods. `this` is the enum value — the decl
			//masquerades as Int32 at runtime (SnEnumDecl::EvalDataType),
			//so arithmetic and switch on `this` work unchanged.
			if (pParent && pParent->Kind() == NK_EnumDecl)
			{
				sn.EvalDataType(static_cast<SnEnumDecl*>(pParent));
				sn.AddFlags(NF_Resolved);
				return;
			}
		}
		pContext = pContext->Parent();
	}
	m_Env.Log(CLL_Error, sn.Location(),
		"'this' can only be used inside a class or enum method.");
}

void ExprResolveAccessor::Access(SnClassDecl &sn)
{
	//Resolve super class reference.
	if (sn.SuperName())
	{
		sn.SuperName()->Accept(*m_pVisitor);
		if (sn.SuperName()->IsResolved())
		{
			auto pSuperField = sn.SuperName()->Field();
			if (pSuperField && pSuperField->Kind() == NK_ClassDecl)
				sn.SuperClass(static_cast<SnClassDecl*>(pSuperField));
			else
				m_Env.Log(CLL_Error, sn.SuperName()->Location(),
					"\"%s\" is not a class type.", sn.SuperName()->ToString().c_str());
		}
	}
	//Resolve "implements I1, I2" names into SnInterfaceDecl* pointers.
	for (auto *pName : sn.ImplementsNames())
	{
		pName->Accept(*m_pVisitor);
		if (pName->IsResolved())
		{
			auto pField = pName->Field();
			if (pField && pField->Kind() == NK_InterfaceDecl)
				sn.AddImplements(static_cast<SnInterfaceDecl*>(pField));
			else
				m_Env.Log(CLL_Error, pName->Location(),
					"\"%s\" is not an interface type.",
					pName->ToString().c_str());
		}
	}
	sn.AddFlags(NF_Resolved);
}

void ExprResolveAccessor::Access(SnInterfaceDecl &sn)
{
	//Interface method signatures have no bodies; type resolution mirrors
	//class methods (handled by StatementResolver walking the members).
	sn.AddFlags(NF_Resolved);
}

void ExprResolveAccessor::Access(SnClassField &sn)
{
	//Class field type resolution is handled by StatementResolver.
}


bool ExprResolveAccessor::ResolveExpressionList(SnExpressionList &exprs)
{
	bool bOK = true;
	for (auto &expr : exprs)
	{
		//Skip already-resolved expressions: literals come pre-resolved
		//from parse time, and Access(SnMemberExpr) pre-resolves method
		//invoke args in the caller scope before the callee lookup runs
		//in the receiver-class scope.
		if (!expr.IsResolved())
			expr.Accept(*m_pVisitor);
		if (!expr.IsResolved() && bOK)
			bOK = false;
	}
	return bOK;
}

//Module import visibility (F20): the first same-name function on the bare
//pool's surface that isVisible rejects — the target the "not visible here"
//hint names. The scan mirrors the filtered pool exactly: the GLOBAL ROOT
//and every NAMESPACE scope, recursively; class/interface/enum members are
//not bare-pool material, so their scopes are not scanned. With several
//external modules declaring the same name, which one the hint names is

FindFuncResult ExprResolveAccessor::FindFuncByInvoke(SnFunction *&pFuncFound,
	SnInvokeExpr &invoke, std::vector<FormalBinding> &outBindings,
	bool &rbNameMatchedImported, bool &rbVisibilityHintLogged)
{
	//Contract: the out-params are always initialized. The NotFound path
	//returns early without touching pFuncFound — an uninitialized caller
	//local then holds stack garbage, and `if (pCallee)` in
	//Access(SnInvokeExpr) dereferences a dangling pointer (ncc crash;
	//observed when imported stubs shifted stack layout). Clearing here
	//covers every path.
	pFuncFound = nullptr;
	rbNameMatchedImported = false;
	rbVisibilityHintLogged = false;
	const bool bSearchInAncestor = !ContainFlags(ERF_SearchInParentOnly);
	bool bImportedMatch = false;
	auto &sFuncName = invoke.CalleeName();

	//D1/D7: the bare pool only spans the current TU's directory; the
	//candidate filter below is fed with the context owner computed once.
	const uint32_t curModule = m_Env.Registry().OwnerOfContext(*m_pContext);

	//Collect the same-name accessible candidates along the scope chain;
	//the bind/distance core itself is shared with the module-qualified
	//call path via MatchInvokeAgainst below.
	std::vector<SnFunction*> candidates;

	//Search a single scope's NameDict for matching functions. Bare-pool
	//scopes (root / namespaces) additionally drop foreign owned functions
	//(D1/D7) — skipped entirely, so they never set bImportedMatch either.
	auto searchScope = [&](SnFunctionParentField& parent, bool bBarePool) {
		auto range = parent.Members().NameDict().equal_range(sFuncName);
		for (auto iField = range.first; iField != range.second; ++iField) {
			SnField *pField = iField->second;
			if (pField->Kind() != NK_Function)
				continue;

			auto pFunc = static_cast<SnFunction *>(pField);
			if (!pFunc->AllowAccess(*m_pAccessor))
				continue;
			if (bBarePool && !IsBareVisible(*pFunc, curModule))
				continue;

			if (pFunc->ContainFlags(NF_Imported))
				bImportedMatch = true;
			candidates.push_back(pFunc);
		}
	};

	SyntaxNode *pParent = m_pContext;
	while (pParent)
	{
		if (CanBeFuncParentEx(pParent->Kind()))
		{
			auto pParentType = static_cast<SnFunctionParentField*>(pParent);
			//Root and namespaces are the bare pool (IsBarePoolScope); class
			//and interface scopes keep full visibility (D1/D7).
			searchScope(*pParentType, IsBarePoolScope(*pParent));

			//For class contexts, also search the inheritance chain
			//when the method is not found in the current class's Members().
			//A superclass is a class scope, never a bare-pool scope.
			if (pParent->Kind() == NK_ClassDecl && candidates.empty())
			{
				auto *pSuper = static_cast<SnClassDecl*>(pParent)->SuperClass();
				while (pSuper && candidates.empty())
				{
					searchScope(*pSuper, false);
					pSuper = pSuper->SuperClass();
				}
			}
			if (!bSearchInAncestor)
				break;
		}
		else if (pParent->Kind() == NK_EnumDecl)
		{
			/*
			Phase 12: enum methods. SnEnumDecl is not a
			SnFunctionParentField — its members and methods are separate
			kind-filtered child lists — so searchScope cannot be reused.
			Enums have no inheritance chain. Like the class branch above,
			a member-call context (ERF_SearchInParentOnly) stops here:
			method-call syntax does not fall through to namespace scope.
			*/
			auto &rEnumDecl = static_cast<SnEnumDecl&>(*pParent);
			auto range = rEnumDecl.Methods().NameDict().equal_range(sFuncName);
			for (auto iField = range.first; iField != range.second; ++iField)
			{
				auto *pFunc = static_cast<SnFunction *>(iField->second);
				if (!pFunc->AllowAccess(*m_pAccessor))
					continue;
				if (pFunc->ContainFlags(NF_Imported))
					bImportedMatch = true;
				candidates.push_back(pFunc);
			}
			if (!bSearchInAncestor)
				break;
		}
		pParent = pParent->Parent();
	}

	//F20: nothing on the (narrowed) bare pool carries the name. When the
	//name does exist elsewhere on the pool's surface, the visibility hint
	//is the real diagnosis — rbVisibilityHintLogged tells the caller to
	//skip its generic text (M4). The verdict stays FuncNameNotFound: the
	//name IS unknown to this pool, no type mismatch happened.
	if (candidates.empty())
	{
		if (MaybeLogVisibilityHint(sFuncName, invoke.Location(), curModule))
		{
			rbVisibilityHintLogged = true;
			return FFR_FuncNameNotFound;
		}
		return FFR_FuncNameNotFound;
	}

	//One matching core for both call paths; the ambiguity log lives there.
	//rbNameMatchedImported stays reserved for the no-viable-bind verdict
	//(the ambiguity report is complete on its own) — the distinction is
	//observable through the imported-argument diagnostic.
	bool bAmbiguous = false;
	auto res = MatchInvokeAgainst(invoke, candidates, pFuncFound,
		outBindings, bAmbiguous);
	if (res == FFR_Incompatible && !bAmbiguous)
	{
		//At least one candidate matched by name but none could bind.
		//Surface whether an imported stub was among them — its parameter
		//types are synthesized, so the Incompatible verdict may simply
		//mean the compiler could not see the real signature.
		pFuncFound = nullptr;
		rbNameMatchedImported = bImportedMatch;
	}
	return res;
}

//M3b: the TryBindInvoke / type-distance core of FindFuncByInvoke — the
//single matching implementation shared by the bare path (candidates
//collected along the scope chain there) and the module-qualified path
//(candidates from the module table). Silent on not-found / incompatible
//— the caller reports those with its own context; the ambiguity error
//is logged HERE (candidate-set independent) and reported through
//rbAmbiguous so callers can tell it apart from a no-viable-bind
//Incompatible. pFunc is nulled on every failure path.
FindFuncResult ExprResolveAccessor::MatchInvokeAgainst(SnInvokeExpr &invoke,
	const std::vector<SnFunction*> &candidates, SnFunction *&pFunc,
	std::vector<FormalBinding> &outBindings, bool &rbAmbiguous)
{
	pFunc = nullptr;
	rbAmbiguous = false;
	if (candidates.empty())
		return FFR_FuncNameNotFound;

	int nBestDistance = -1;
	SnFunction *pBest = nullptr;
	std::vector<FormalBinding> bestBindings;

	//Bind, then keep the strictest viable distance; a tie at the
	//smallest viable distance is ambiguous.
	auto consider = [&](SnFunction *pCandidate) {
		std::vector<FormalBinding> tryBind;
		if (!TryBindInvoke(invoke, *pCandidate, tryBind))
			return;
		int n = ComputeBindingDistance(tryBind);
		if (n < 0)
			return;
		if (nBestDistance < 0 || n < nBestDistance)
		{
			nBestDistance = n;
			pBest = pCandidate;
			bestBindings = std::move(tryBind);
			rbAmbiguous = false;
		}
		else if (n == nBestDistance)
		{
			rbAmbiguous = true;
		}
	};

	for (auto *pCandidate : candidates)
		consider(pCandidate);

	if (nBestDistance < 0)
		return FFR_Incompatible;
	if (rbAmbiguous)
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"ambiguous call to function \"%s\": multiple overloads match "
			"with equal distance.",
			invoke.CalleeName().c_str());
		return FFR_Incompatible;
	}
	pFunc = pBest;
	outBindings = std::move(bestBindings);
	return (nBestDistance == 0) ? FFR_ExactMatch : FFR_ApproximateMatch;
}

//M3b: the SUCCESS tail of Access(SnInvokeExpr), shared with the
//module-qualified call path so both bind a callee exactly alike.
bool ExprResolveAccessor::ResolveInvokeWithFunc(SnInvokeExpr &invoke,
	SnFunction &func, FindFuncResult match,
	std::vector<FormalBinding> &bindings)
{
	//Phase 13: argument-position function references bind against the
	//formal Func types of the chosen overload (ComputeBindingDistance
	//already required an exact signature match for candidacy). Step 2
	//adds the receiver-bound member form (c.foo).
	if (match == FFR_ExactMatch || match == FFR_ApproximateMatch)
	{
		for (auto &b : bindings)
		{
			if (b.kind == FormalBinding::B_Default || !b.pCallerExpr)
				continue;
			if (IsUnboundFuncRef(*b.pCallerExpr))
			{
				if (!BindFuncRefToExpected(m_Env,
					*static_cast<SnIdentifierExpr*>(b.pCallerExpr),
					b.pFormal->EvalDataType()))
					return false;
			}
			else if (IsUnboundMemberFuncRef(*b.pCallerExpr)
				&& !BindMemberFuncRefToExpected(m_Env,
					*static_cast<SnMemberExpr*>(b.pCallerExpr),
					b.pFormal->EvalDataType()))
				return false;
		}
	}

	if (match == FFR_ApproximateMatch)
		FixupParamTypesWithBindings(invoke, bindings);
	invoke.SetBindings(std::move(bindings));
	//A void return resolves to a null EvalDataType here — the established
	//void-invoke convention.
	ResolveFieldExprAs(invoke, &func);
	BindArrayTypeToken(invoke);
	return true;
}

//Phase 9e: out arguments on virtual methods are rejected — the
//writeback mask is baked into the call instruction against the
//static callee's parameter layout; a runtime override resolved by
//name-based dispatch could disagree with it. Interface methods are
//always dispatched by name (never NF_Virtual-flagged), so they are
//covered via the parent decl kind. Shared by the bare and the
//module-qualified call paths.
bool ExprResolveAccessor::OutArgOnDispatchedCalleeRejected(
	const SnInvokeExpr &invoke, const SnFunction &callee,
	const std::vector<FormalBinding> &bindings) const
{
	bool bDispatchedByName = callee.ContainFlags(NF_Virtual)
		|| (callee.Parent()
			&& callee.Parent()->Kind() == NK_InterfaceDecl);
	if (!bDispatchedByName)
		return false;
	for (auto &b : bindings)
	{
		if (b.bIsOut)
		{
			m_Env.Log(CLL_Error, invoke.Location(),
				"out arguments are not supported on virtual method "
				"\"%s\".",
				callee.Name().c_str());
			return true;
		}
	}
	return false;
}

//The failure branch of Access(SnInvokeExpr) — not-found, imported-stub
//and generic incompatibility diagnostics. Shared with the module-qualified
//call path so both surfaces report identically (spec §7 / M4).
void ExprResolveAccessor::LogInvokeFailure(SnInvokeExpr &invoke,
	FindFuncResult res, SnFunction *pCallee, bool bNameMatchedImported)
{
	assert((res == FFR_Incompatible || res == FFR_FuncNameNotFound)
		&& "failure logging takes failure results only");

	if (res == FFR_Incompatible)
	{
		//Phase 13 (Step 2, cross-module): an imported stub synthesizes its
		//parameter types from the return kind, so a Func argument can
		//never match — name the real reason before any generic message.
		//pCallee is null by contract on this path; the flag comes from the
		//name-matched candidate scan.
		if (bNameMatchedImported)
		{
			for (auto &arg : invoke.Params())
			{
				SnExpression *pValue = (arg.Kind() == NK_NamedArgExpr)
					? static_cast<SnNamedArgExpr&>(arg).Inner() : &arg;
				if (IsUnboundFuncRef(*pValue)
					|| IsUnboundMemberFuncRef(*pValue)
					|| IsFuncTypeDecl(pValue->EvalDataType()))
				{
					m_Env.Log(CLL_Error, invoke.Location(),
						"cannot pass a function reference to the imported "
						"function \"%s\": parameter signatures are not "
						"serialized.", invoke.CalleeName().c_str());
					return;
				}
			}
		}
		//Phase 13: a still-pending function reference among the arguments
		//had no matching Func-typed formal — sweep it with the named
		//diagnostic (the generic incompatibility text would not say why).
		for (auto &arg : invoke.Params())
		{
			SnExpression *pValue = (arg.Kind() == NK_NamedArgExpr)
				? static_cast<SnNamedArgExpr&>(arg).Inner() : &arg;
			if (IsUnboundFuncRef(*pValue))
			{
				m_Env.Log(CLL_Error, pValue->Location(),
					"function reference \"%s\" requires an expected "
					"function type.",
					pValue->ToString().c_str());
				return;
			}
			if (IsUnboundMemberFuncRef(*pValue))
			{
				m_Env.Log(CLL_Error, pValue->Location(),
					"bound method reference \"%s\" requires an expected "
					"function type.",
					pValue->ToString().c_str());
				return;
			}
		}
		m_Env.Log(CLL_Error, invoke.Location(),
			"The function invoke \"%s\" is not compatible with the "
			"declaration.", invoke.ToString().c_str());
		if (pCallee) {
			m_Env.Log(CLL_More, pCallee->Location(),
				"See also the declaration of \"%s\".",
				pCallee->ToString().c_str());
		}
		return;
	}

	assert(res == FFR_FuncNameNotFound);
	m_Env.Log(CLL_Error, invoke.Location(),
		"The function \"%s\" does not exist or is not accessible.",
		invoke.CalleeName().c_str());
}

SnField *ExprResolveAccessor::FindDelegateTarget(SnInvokeExpr &invoke)
{
	auto *pField = FindFieldInAncestor(invoke.CalleeName(), *m_pContext,
		*m_pAccessor, Flags());
	if (!pField || pField->Kind() == NK_Function)
		return nullptr;
	return IsFuncTypeDecl(pField->EvalDataType()) ? pField : nullptr;
}

void ExprResolveAccessor::BindDelegateInvoke(SnInvokeExpr &invoke,
	SnField *pDelegateField)
{
	auto *pFuncDecl = static_cast<SnClassDecl*>(
		pDelegateField->EvalDataType());
	const auto typeArgs = GetGenericTypeArgs(pFuncDecl);
	const auto &outFlags = s_genericOutFlags[pFuncDecl];
	//The Func signature has no parameter names — by-name dispatch is
	//impossible.
	if (HasNamedArgument(invoke))
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"named arguments are not supported in delegate calls.");
		return;
	}
	size_t paramCount = typeArgs.size() - 1;
	if (ArgCountOf(invoke) != paramCount)
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"delegate call expects %zu argument(s), got %zu.",
			paramCount, ArgCountOf(invoke));
		return;
	}
	bool bOK = true;
	size_t i = 0;
	for (auto &arg : invoke.Params())
	{
		SnField *pFormal = typeArgs[i + 1];
		bool bWantOut = outFlags[i + 1] != 0;
		bool bIsOut = arg.Kind() == NK_OutArgExpr;
		SnExpression *pValue = bIsOut
			? static_cast<SnOutArgExpr&>(arg).Inner() : &arg;
		if (bIsOut != bWantOut)
		{
			m_Env.Log(CLL_Error, arg.Location(),
				"argument %zu of the delegate call %s the out marker.",
				i + 1, bWantOut ? "requires" : "does not accept");
			bOK = false;
		}
		//Pending bare function references bind against the Func's own
		//parameter slot type; Step 2 adds the receiver-bound member form.
		if (IsUnboundFuncRef(*pValue))
		{
			if (!BindFuncRefToExpected(m_Env,
				static_cast<SnIdentifierExpr&>(*pValue), pFormal))
				bOK = false;
		}
		else if (IsUnboundMemberFuncRef(*pValue))
		{
			if (!BindMemberFuncRefToExpected(m_Env,
				static_cast<SnMemberExpr&>(*pValue), pFormal))
				bOK = false;
		}
		else if (!bIsOut)
		{
			//0.7.3 B review fix: distance alone is a wrong admission test
			//here — unlike the overload path, a delegate call has NO fixup
			//pass, so any accepted distance that implies a conversion
			//(int→float, array→string through the D5 arm) passes raw bits
			//and the callee reads garbage. Distance 0 covers identical
			//types, interned tokens and the enum/int32 masquerade; the
			//only sound non-zero distances are reference upcasts
			//(subclass→base, class→interface), where the handle passes
			//through unchanged.
			auto *pArgType = pValue->EvalDataType();
			const bool bRefUpcast = pArgType
				&& (pArgType->Kind() == NK_ClassDecl
					|| pArgType->Kind() == NK_InterfaceDecl)
				&& (pFormal->Kind() == NK_ClassDecl
					|| pFormal->Kind() == NK_InterfaceDecl);
			const int nDist = pArgType
				? CalcTypeDistance(*pArgType, *pFormal) : -1;
			if (!pArgType || !(nDist == 0 || (bRefUpcast && nDist > 0)))
			{
				m_Env.Log(CLL_Error, arg.Location(),
					"argument %zu of the delegate call is incompatible "
					"with \"%s\".", i + 1, pFormal->Name().c_str());
				bOK = false;
			}
		}
		++i;
	}
	if (!bOK)
		return;
	//Field() carries the delegate value — codegen detects the delegate
	//shape structurally (a resolved invoke whose Field() is not an
	//SnFunction), so no dedicated flag exists. EvalDataType follows the
	//Func's return slot — nullptr for void, the established void-invoke
	//convention. Set directly (like the generic-method path) rather than
	//through ResolveFieldExprAs, whose PostResolveCheck expects a type
	//field here.
	invoke.m_pField = pDelegateField;
	if (typeArgs[0]->Kind() != NK_Void)
		invoke.EvalDataType(typeArgs[0]);
	invoke.AddFlags(NF_Resolved);
	BindArrayTypeToken(invoke);
}

//Phase 9c: try to bind an invoke's actual arguments to a candidate
//callee's formal parameters. Handles positional args, named args, and
//default param expressions. Returns true if every formal is bound
//(either by caller or by default); false if any required formal is left
//unbound or a caller-side error occurs (positional after named, etc.).
//Does NOT log — caller reports a generic "not compatible" error when
//no candidate matches.
bool ExprResolveAccessor::TryBindInvoke(const SnInvokeExpr &invoke,
	const SnFunction &callee, std::vector<FormalBinding> &outBindings)
{
	auto &formals = const_cast<SnFunction&>(callee).Params();
	outBindings.clear();
	outBindings.reserve(formals.size());
	for (auto &f : formals)
	{
		FormalBinding b;
		b.kind = FormalBinding::B_Default;  //sentinel: "unbound so far"
		b.pCallerExpr = nullptr;
		b.pFormal = &f;
		outBindings.push_back(b);
	}

	bool bSeenNamed = false;
	size_t iNextFormal = 0;
	auto &actuals = const_cast<SnInvokeExpr&>(invoke).Params();

	//Pass 1+2: walk actuals left-to-right; route each to positional or named.
	for (auto &actual : actuals)
	{
		SnExpression *pExpr;
		std::string sName;
		bool bIsNamed = false;
		bool bIsOut = false;
		if (actual.Kind() == NK_NamedArgExpr)
		{
			auto &named = static_cast<const SnNamedArgExpr&>(actual);
			sName = named.Name();
			pExpr = named.Inner();
			bIsNamed = true;
			bSeenNamed = true;
		}
		else if (actual.Kind() == NK_OutArgExpr)
		{
			//Phase 9e: out argument. Unwrap to the inner identifier; the
			//binding must land on an NF_Out formal (checked below). Named
			//+out (`foo(b = out y)`) has no grammar form.
			auto &outArg = static_cast<const SnOutArgExpr&>(actual);
			pExpr = outArg.Inner();
			if (!pExpr->IsResolved())
				return false;  //Access(SnOutArgExpr) already logged why
			bIsOut = true;
		}
		else
		{
			pExpr = &const_cast<SnExpression&>(actual);
			if (bSeenNamed)
			{
				//"positional after named" is a caller-side error — reject
				//this candidate (caller will get a generic incompatible
				//error from FindFuncByInvoke). Phase 9c Step 4 will report
				//a specific message once grammar accepts named args.
				return false;
			}
		}

		if (!bIsNamed)
		{
			if (iNextFormal >= formals.size())
				return false;  //too many positional args
			size_t idx = iNextFormal++;
			if (outBindings[idx].pCallerExpr != nullptr)
				return false;  //should never happen (positional goes in order)
			//Phase 9e: the `out` marker must agree with the formal — an
			//out formal requires `out ident` at the call site, and `out`
			//is invalid for a normal formal. Both directions reject this
			//candidate (generic incompatibility from FindFuncByInvoke).
			if (outBindings[idx].pFormal->ContainFlags(NF_Out) != bIsOut)
				return false;
			outBindings[idx].kind = FormalBinding::B_Positional;
			outBindings[idx].pCallerExpr = pExpr;
			outBindings[idx].bIsOut = bIsOut;
		}
		else
		{
			//Find formal by name.
			size_t idx = formals.size();
			size_t i = 0;
			for (auto &f : formals)
			{
				if (f.Name() == sName)
				{
					idx = i;
					break;
				}
				++i;
			}
			if (idx == formals.size())
				return false;  //no formal with this name
			if (outBindings[idx].pCallerExpr != nullptr)
				return false;  //duplicate binding (positional+named or named+named)
			outBindings[idx].kind = FormalBinding::B_Named;
			outBindings[idx].pCallerExpr = pExpr;
		}
	}

	//Pass 3: every formal must be either caller-bound or have a default.
	for (auto &b : outBindings)
	{
		if (b.pCallerExpr == nullptr)
		{
			//Unbound — must have default expression.
			if (!b.pFormal->Value())
				return false;  //required formal left unsatisfied
			b.kind = FormalBinding::B_Default;
		}
	}

	return true;
}

//Phase 9c: sum of CalcTypeDistance over the bound (positional / named)
//entries. B_Default contributes 0. Returns -1 if any bound entry has
//incompatible types.
int ExprResolveAccessor::ComputeBindingDistance(
	const std::vector<FormalBinding> &bindings) const
{
	int nDistance = 0;
	for (auto &b : bindings)
	{
		if (b.kind == FormalBinding::B_Default)
			continue;
		assert(b.pCallerExpr && b.pFormal);
		//Phase 13: a pending function reference binds only to a Func
		//formal whose signature matches exactly (distance 0). This also
		//closes the legacy hole where the bare name's RETURN type let it
		//bind approximately to non-Func formals. Step 2 adds the
		//receiver-bound member form (same exact-match-only rule).
		if (IsUnboundFuncRef(*b.pCallerExpr))
		{
			auto *pTgt = b.pFormal->EvalDataType();
			auto *pRefFunc = static_cast<SnIdentifierExpr*>(
				b.pCallerExpr)->Field();
			if (!IsFuncTypeDecl(pTgt) || !pRefFunc
				|| !FuncRefMatchesDecl(
					*static_cast<SnFunction*>(pRefFunc),
					static_cast<SnClassDecl*>(pTgt)))
				return -1;
			continue;
		}
		if (IsUnboundMemberFuncRef(*b.pCallerExpr))
		{
			auto *pTgt = b.pFormal->EvalDataType();
			auto *pMethod = static_cast<SnIdentifierExpr*>(
				static_cast<SnMemberExpr*>(
					b.pCallerExpr)->Inner())->Field();
			if (!IsFuncTypeDecl(pTgt) || !pMethod
				|| !FuncRefMatchesDecl(
					*static_cast<SnFunction*>(pMethod),
					static_cast<SnClassDecl*>(pTgt)))
				return -1;
			continue;
		}
		auto *pSrc = b.pCallerExpr->EvalDataType();
		auto *pTgt = b.pFormal->EvalDataType();
		if (!pSrc || !pTgt)
			return -1;
		//0.7.3 B: array-ness is adjudicated by CalcTypeDistance's kind
		//matching alone — an interned token against a scalar formal (and
		//the symmetric hole) verdicts -1 there. One exemption follows:
		//null against an array formal.
		//0.7.3 B: the null literal is Int32-typed, so against an
		//array-token formal CalcTypeDistance reads -1. Null binds to
		//any array type at distance 0 — the same bridge the assignment
		//flavor grants (`int[] a = null`; the null sentinel is not an
		//array value).
		if (b.pCallerExpr->ContainFlags(NF_NullLiteral)
			&& pTgt->Kind() == NK_ArrayTypeToken)
			continue;
		int n = CalcTypeDistance(*pSrc, *pTgt);
		if (n < 0)
		{
			//0.7.3 B: a cross-element array conversion (an int[] source
			//against a string[] formal) verdicts -1 here — give it the
			//named array diagnostic; the caller's generic "not
			//compatible" alone hides the array reason.
			if (b.pCallerExpr->IsArrayValued())
				m_Env.Log(CLL_Error, b.pCallerExpr->Location(),
					"Invalid conversion \"%s\": an array value only "
					"converts to the same array type.",
					b.pCallerExpr->ToString().c_str());
			return -1;
		}
		//Phase 9e: out bindings require the exact same type — the callee
		//writes its slot straight back into the caller's variable; any
		//implicit cast (int→float etc.) would be discarded by writeback.
		if (b.bIsOut && n != 0)
			return -1;
		nDistance += n;
	}
	return nDistance;
}

//Phase 9c: apply implicit cast wrappers (SnCastExpr) to caller-side
//expressions in bindings where needed (TCK_Auto / TCK_Box). B_Default
//entries are skipped — their type was validated against the formal at
//declaration time (StatementResolver Step 2).
void ExprResolveAccessor::FixupParamTypesWithBindings(SnInvokeExpr &invoke,
	std::vector<FormalBinding> &bindings)
{
	auto &children = invoke.Children();
	for (auto &b : bindings)
	{
		if (b.kind == FormalBinding::B_Default)
			continue;
		//Phase 9e: out bindings already required exact type match in
		//ComputeBindingDistance — wrapping a cast would break the
		//variable-slot identity the writeback relies on.
		if (b.bIsOut)
			continue;
		assert(b.pCallerExpr && b.pFormal);
		auto *pSrc = b.pCallerExpr->EvalDataType();
		auto *pTgt = b.pFormal->EvalDataType();
		if (!pSrc || !pTgt)
			continue;
		TypeCastInfo castInfo(pSrc, pTgt);
		if (castInfo.Kind() == TCK_Same)
			continue;

		//Locate the caller expr's NodeIterator inside invoke.Children().
		//For positional bindings this finds the caller expr directly.
		//For named bindings, the wrapper SnNamedArgExpr is in Children();
		//its inner expr is replaced below by walking the wrapper's list.
		auto iFound = children.find(b.pCallerExpr);
		if (iFound == children.end())
		{
			//Named-arg path: pCallerExpr is inside a SnNamedArgExpr wrapper.
			//Find the wrapper, then fix up its inner expression.
			bool bReplaced = false;
			for (auto it = children.begin(); it != children.end(); ++it)
			{
				if ((*it).Kind() == NK_NamedArgExpr)
				{
					auto &named = static_cast<SnNamedArgExpr&>(*it);
					if (named.Inner() == b.pCallerExpr)
					{
						auto &innerChildren =
							const_cast<SnNamedArgExpr&>(named).Children();
						auto iInner = innerChildren.find(b.pCallerExpr);
						if (iInner == innerChildren.end())
							continue;
						FixupExprType(iInner, castInfo);
						b.pCallerExpr =
							static_cast<SnExpression*>(&*iInner);
						bReplaced = true;
						break;
					}
				}
			}
			if (!bReplaced)
				continue;
		}
		else
		{
			//FixupExprType may replace iFound with a new SnCastExpr node.
			FixupExprType(iFound, castInfo);
			//Refresh the binding's caller pointer to the (possibly new) node.
			b.pCallerExpr = static_cast<SnExpression*>(&*iFound);
		}
	}
}

int ExprResolveAccessor::CalcTypeDistance(const SnField &source,
	const SnField &target) const
{
	if (&source == &target)
		return 0;
	auto srcKind = source.Kind();
	auto tgtKind = target.Kind();
	if (srcKind == NK_EnumDecl) srcKind = NK_Int32;
	if (tgtKind == NK_EnumDecl) tgtKind = NK_Int32;
	if (srcKind == NK_StructDecl && tgtKind == NK_StructDecl)
		return (&source == &target) ? 0 : -1;
	if (srcKind == NK_StructDecl || tgtKind == NK_StructDecl)
		return -1;
	if (srcKind == NK_ClassDecl && tgtKind == NK_ClassDecl)
	{
		if (&source == &target)
			return 0;
		auto *pSrc = static_cast<const SnClassDecl*>(&source);
		auto *pParent = pSrc->SuperClass();
		int depth = 1;
		while (pParent)
		{
			if (pParent == &target)
				return depth;
			pParent = pParent->SuperClass();
			++depth;
		}
		return -1;
	}
	//Class to interface: walk source class's inheritance chain and check
	//each ancestor's implements list. Distance is 1 + inheritance depth
	//(encourage upcast to direct implementor over a deeper ancestor's
	//implementation, but still accept any depth).
	if (srcKind == NK_ClassDecl && tgtKind == NK_InterfaceDecl)
	{
		auto *pSrc = static_cast<const SnClassDecl*>(&source);
		auto *pCur = pSrc;
		int depth = 0;
		while (pCur)
		{
			for (auto *pIface : pCur->ImplementsList())
				if (pIface == &target)
					return depth + 1;
			pCur = pCur->SuperClass();
			++depth;
		}
		return -1;
	}
	//Interface to interface: identity only (no inheritance between interfaces).
	if (srcKind == NK_InterfaceDecl && tgtKind == NK_InterfaceDecl)
		return (&source == &target) ? 0 : -1;
	//Interface to class is never valid — interface refs cannot be downcast
	//implicitly (no dynamic cast in this phase).
	if (srcKind == NK_InterfaceDecl || tgtKind == NK_InterfaceDecl)
		return -1;
	if (srcKind == NK_ClassDecl || tgtKind == NK_ClassDecl)
		return -1;
	//0.7.3 B D5: an array argument coerces to a string formal through the
	//cast table's TCK_Auto (runtime toString). Grant candidacy a finite
	//distance — the same magnitude as the cheapest scalar-to-string
	//widening. Everything else array-token-shaped stays -1: same-token
	//pairs already returned 0 at the pointer check above, cross-token
	//and token-vs-scalar pairs have no conversion.
	if (srcKind == NK_ArrayTypeToken && tgtKind == NK_String)
		return 1;
	if (IsPrimitiveType(srcKind) && IsPrimitiveType(tgtKind))
		return std::abs(srcKind - tgtKind);
	return -1;
}

bool ExprResolveAccessor::FixupExprType(NodeIterator &iSrcExpr,
	TypeCastInfo &castInfo)
{
	if (castInfo.Kind() == TCK_Same)
		return false;

	assert(static_cast<SyntaxNode &>(*iSrcExpr).IsExpression());
	auto &srcExpr = static_cast<SnExpression &>(*iSrcExpr);

	//0.7.3 B: a TCK_None verdict between two array tokens (different
	//element types) keeps the NAMED array diagnostic — the branch must
	//sit before the generic reject below, or covariant/enum-array
	//conversions surface as the generic "Incompatible type". Same-type
	//array flow returned TCK_Same above; an array source against a
	//scalar target verdicts None without a token target and takes the
	//generic message; array→string coerces via TCK_Auto (runtime
	//toString dispatch, array-aware — `"${arr}"` yields "[1, 2]").
	if (castInfo.Kind() == TCK_None
		&& srcExpr.IsArrayValued()
		&& castInfo.Target()
		&& castInfo.Target()->Kind() == NK_ArrayTypeToken)
	{
		m_Env.Log(CLL_Error, srcExpr.Location(),
			"Invalid conversion \"%s\": an array value only converts "
			"to the same array type.",
			srcExpr.ToString().c_str());
		return false;
	}

	if (castInfo.Kind() != TCK_Auto && castInfo.Kind() != TCK_Box)
	{
		m_Env.Log(CLL_Error, srcExpr.Location(),
			"Incompatible type \"%s\".", srcExpr.ToString().c_str());
		return false;
	}

	//The int→class/interface/array bridge (TCK_Auto) exists for the null
	//literal only: any other int/enum value would end up as a garbage
	//handle in the slot (an array target doubly so — a raw int would
	//reintroduce the masquerade). ContainFlags is a flat bit test, so a
	//null literal wrapped in `as` propagates the flag explicitly in
	//Access(SnAsExpr).
	if (castInfo.Kind() == TCK_Auto
		&& castInfo.Target()
		&& (castInfo.Target()->Kind() == NK_ClassDecl
			|| castInfo.Target()->Kind() == NK_InterfaceDecl
			|| castInfo.Target()->Kind() == NK_ArrayTypeToken)
		&& !srcExpr.ContainFlags(NF_NullLiteral))
	{
		m_Env.Log(CLL_Error, srcExpr.Location(),
			"Incompatible value \"%s\": only the null literal converts "
			"from int to a class, interface or array type.",
			srcExpr.ToString().c_str());
		return false;
	}

	//Null literal (KT_Null is Int32-typed) must reach the slot as the raw
	//sentinel 0. Wrapping it destroys the null identity downstream:
	//Int32→String emits OP_Int32_to_str ("0"), TCK_Box to Object allocates
	//a boxed 0. Class/interface targets already treat TCK_Auto as a
	//runtime no-op, so skipping the wrap uniformly is safe there too.
	//Null operands in binary arithmetic/concat are rejected outright by
	//the guard in Access(SnBinaryExpr) (Phase 11 Step 3b) — the skip here
	//cannot leak a raw null into an arithmetic wrap anymore.
	if (srcExpr.ContainFlags(NF_NullLiteral)
		&& (castInfo.Kind() == TCK_Box
			|| (castInfo.Target()
				&& castInfo.Target()->Kind() == NK_String)))
		return false;

	auto pSrcParent = srcExpr.Parent();
	assert(pSrcParent);

	auto iInsertPos = RemoveChildFrom(iSrcExpr, *pSrcParent);
	assert(srcExpr.Location());
	auto pCastExpr =
		new SnCastExpr(&srcExpr, castInfo, *srcExpr.Location());
	//Phase 8e-8: propagate target type to the cast expr's EvalDataType so
	//consumers (e.g. binary codegen dispatching on operand type) see the
	//post-cast type without re-walking the cast. The "as T" resolver path
	//sets this explicitly at line 709; FixupExprType must do the same.
	pCastExpr->EvalDataType(castInfo.Target());
	iSrcExpr = InsertChildInto(iInsertPos, pCastExpr, *pSrcParent);
	return true;
}

bool ExprResolver::ResolveDataTypes(SnField &sn, SnField &outerType)
{
	if (sn.IsDataField())
	{
		if (sn.IsResolved())
			return true;

		if (sn.Kind() != NK_Function)
		{
			auto &dataField = static_cast<SnDataField &>(sn);
			if (!ResolveDataType(*dataField.Type(), outerType))
				return false;
			//Array redesign B: jagged declarations (T[][]) have no VM
			//layout and used to degrade silently — reject here.
			if (ArrayTypeDepth(dataField.Type()) >= 2)
				m_Accessor.m_Env.Log(CLL_Error, dataField.Location(),
					"jagged arrays (T[][]) are not supported");
			return true;
		}

		auto pReturnType = static_cast<SnFunction &>(sn).ReturnType();
		if (pReturnType)
		{
			if (!ResolveDataType(*pReturnType, outerType))
				return false;
			//Array redesign B: jagged return types are rejected like the
			//other declaration forms (no VM layout).
			if (ArrayTypeDepth(
				static_cast<SnFunction &>(sn).ReturnType()) >= 2)
				m_Accessor.m_Env.Log(CLL_Error, sn.Location(),
					"jagged arrays (T[][]) are not supported");
		}
	}

	if (!ResolveChildFields(sn))
		return false;
	sn.AddFlags(NF_Resolved);
	return true;
}

bool ExprResolver::ResolveDataType(SnFieldExpr &typeExpr, SnField &outerType)
{
	if (!Resolve(typeExpr, outerType, outerType, ERF_None))
	{
		typeExpr.AddFlags(NF_Invalid);
		return false;
	}
	return true;
}

bool ExprResolver::ResolveChildFields(SnField & sn)
{
	bool bOK = true;
	for (auto &child : sn.Children())
	{
		if (child.IsField())
		{
			auto &childField = static_cast<SnField &>(child);
			if (!ResolveDataTypes(childField, sn) && bOK)
				bOK = false;
		}
	}
	return bOK;
}

} //namespace nlang
