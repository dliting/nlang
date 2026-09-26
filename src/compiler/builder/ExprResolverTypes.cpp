/*---
    ExprResolverTypes.cpp — 内建/泛型类型机器与类型位节点解析
    从 ExprResolver.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
---*/
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
SnClassDecl* GetBuiltinClassDecl(const std::string& name,
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

//Phase 13: out-flag side-table accessor (same key discipline as
//GetGenericTypeArgs; map operator[] default-inserts on miss, matching
//the historical direct map access at the Func bind sites).
const std::vector<uint8>& GetGenericOutFlags(SnClassDecl* pClass)
{
	return s_genericOutFlags[pClass];
}

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

} //namespace nlang
