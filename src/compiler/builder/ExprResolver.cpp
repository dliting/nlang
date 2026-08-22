#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include <nlang/vm/StdLib.h>
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
static bool IsBuiltinExceptionClassName(const std::string& name)
{
    return name == "Exception" || name == "NullPointerException"
        || name == "DivByZeroException" || name == "IndexOutOfBoundsException"
        || name == "AssertionException" || name == "IOException";
}

//Phase 10 audit H2: single predicate for every name GetBuiltinClassDecl
//can synthesize. Call sites used to duplicate this filter three times —
//a future builtin added to one copy but not the chain below would fall
//into the old `s_pObjectClass` fallback and silently corrupt the Object
//singleton. Everything routes through this one function now.
static bool IsBuiltinClassName(const std::string& name)
{
    return name == "ByteStream" || name == "FileStream" || name == "Object"
        || IsBuiltinExceptionClassName(name);
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
		ScriptLocation loc;
		if (pLoc)
			loc = *static_cast<const ScriptLocation*>(pLoc);
		s_pExceptionClass = new SnClassDecl(pName, nullptr, pMembers, loc);
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
		ScriptLocation loc;
		if (pLoc)
			loc = *static_cast<const ScriptLocation*>(pLoc);
		rpRef = new SnClassDecl(pName, nullptr, pMembers, loc);
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
	bool operator<(const GenericInstKey& rhs) const {
		if (baseName != rhs.baseName) return baseName < rhs.baseName;
		if (typeArgs.size() != rhs.typeArgs.size())
			return typeArgs.size() < rhs.typeArgs.size();
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

//Lookup type arguments for a synthetic generic class. Returns empty vector
//if not a generic instantiation.
static std::vector<SnField*> GetGenericTypeArgs(SnClassDecl* pClass)
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
static bool IsBuiltinGenericClassName(const std::string& name)
{
	return name == "List" || name == "Dict";
}

//Round-12: built-in methods dispatch by name with no real SnFunction, so a
//name = value argument can never bind to a parameter — reject it here or
//codegen's fallback would silently stage the value as ConstZero.
static bool HasNamedArgument(SnInvokeExpr& invoke)
{
	for (auto& p : invoke.Params())
		if (p.Kind() == NK_NamedArgExpr)
			return true;
	return false;
}

static size_t ArgCountOf(SnInvokeExpr& invoke)
{
	size_t n = 0;
	for (auto& p : invoke.Params()) ++n;
	return n;
}

//Round-14: mirror of HasNamedArgument — built-in by-name dispatch also
//cannot write back out arguments (intrinsics return through pResult only).
static bool HasOutArgument(SnInvokeExpr& invoke)
{
	for (auto& p : invoke.Params())
		if (p.Kind() == NK_OutArgExpr)
			return true;
	return false;
}

//Phase 11: printable language name of a stdlib param kind (RTK_*). Only
//the kinds StdLibEntry::paramKinds may carry are covered — extending the
//table with a new kind means extending this switch too.
static const char* StdLibKindName(uint8_t rtk)
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
static bool IsGenericClassDecl(SnClassDecl* pClass)
{
	return pClass && pClass->IsGenericInstantiation();
}

//Mints (or fetches) a synthetic SnClassDecl for the given generic
//instantiation. Phase 8e-3: List<T> (arity 1). Phase 8e-4: Dict<K,V> (arity 2).
static SnClassDecl* GetGenericClassDecl(const std::string& baseName,
	const std::vector<SnField*>& typeArgs, const ISourceLocation* pLoc)
{
	GenericInstKey key{baseName, typeArgs};
	auto it = s_genericInstances.find(key);
	if (it != s_genericInstances.end())
		return it->second;

	//Built-in generic + arity check.
	size_t expectedArity = (baseName == "Dict") ? 2 : 1;
	if (!IsBuiltinGenericClassName(baseName) || typeArgs.size() != expectedArity)
		return nullptr;

	//Build display name e.g. "List<int>", "Dict<string, int>".
	std::string instName = baseName + "<";
	for (size_t i = 0; i < typeArgs.size(); ++i) {
		if (i) instName += ", ";
		instName += typeArgs[i]->Name();
	}
	instName += ">";

	auto* pName = new std::string(instName);
	auto* pMembers = new PtrList<SnField>();
	ScriptLocation loc;
	if (pLoc)
		loc = *static_cast<const ScriptLocation*>(pLoc);
	auto* pClass = new SnClassDecl(pName, nullptr, pMembers, loc);
	pClass->SetBuiltinClass();
	pClass->SetGenericInstantiation();
	pClass->SetGenericTypeArgs(typeArgs);
	pClass->SetBaseName(baseName);
	s_genericInstances[key] = pClass;
	//Side table for member-call return-type lookup (List<int>.Get() → int).
	s_genericTypeArgs[pClass] = typeArgs;
	return pClass;
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

//True when `baseExpr` denotes an ARRAY-typed lvalue (local, param, or
//member field). EvalDataType() alone cannot tell: for array types it
//returns the ELEMENT type (e.g. `List<int>[] a` → the List<int>
//instantiation), so Kind()-based container/array dispatch must consult
//the IsArrayType flag on the resolved field first (EvalDataType
//dispatch-order trap — same family as toString/assign/member/length).
bool IsArrayTypedBase(SnExpression& baseExpr) {
	SnIdentifierExpr* pId = nullptr;
	if (baseExpr.Kind() == NK_IdentifierExpr)
		pId = static_cast<SnIdentifierExpr*>(&baseExpr);
	else if (baseExpr.Kind() == NK_MemberExpr) {
		auto* pInner = static_cast<SnMemberExpr&>(baseExpr).Inner();
		if (pInner && pInner->Kind() == NK_IdentifierExpr)
			pId = static_cast<SnIdentifierExpr*>(pInner);
	}
	return pId && pId->Field() && pId->Field()->IsArrayType();
}

//True when the expression VALUE is an array, covering the shapes that can
//flow into a call argument. IsArrayTypedBase handles the lvalue shapes
//(identifier / member); the value shapes below share the same masquerade:
//EvalDataType() of an array-valued expression returns the ELEMENT kind,
//so a Kind()-based type check alone would let the array handle through
//(Step 0 review round 2: `math.sqrt(new int[3])` and `math.sqrt(mk())`
//with `int[] mk()` both slipped past the lvalue-only guard).
bool IsArrayValuedExpr(SnExpression& expr) {
	switch (expr.Kind()) {
	case NK_IdentifierExpr:
	case NK_MemberExpr:
		return IsArrayTypedBase(expr);
	case NK_NewArrayExpr:
		return true;  //`new T[n]` is always an array value
	case NK_InvokeExpr:
	{
		//Call returning T[]: the invoke resolves AS the callee
		//(ResolveFieldExprAs), so the callee's return TYPE EXPR carries
		//the IsArrayType flag (same level as `sn.Type()->IsArrayType()`
		//in local declarations — NOT Field()->IsArrayType(), which is
		//the local-var level and would read the element type's flag).
		auto& invoke = static_cast<SnInvokeExpr&>(expr);
		auto* pCallee = invoke.Callee();
		auto* pReturnType = pCallee ? pCallee->ReturnType() : nullptr;
		return pReturnType && pReturnType->IsArrayType();
	}
	default:
		return false;
	}
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

	//Propagate the element type's field to the array type expression.
	ResolveFieldExprAs(arrTypeExpr, pElemType->Field());
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
		typeArgs.push_back(pField);
	}

	auto *pSynClass = GetGenericClassDecl(baseName, typeArgs,
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
	pField = FindFieldInAncestor(idExpr.Name(), *m_pContext, *m_pAccessor,
		Flags());

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
		return;
	}

	ResolveFieldExprAs(idExpr, pField);
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

	SnFunction *pCallee;
	std::vector<FormalBinding> bindings;
	auto res = FindFuncByInvoke(pCallee, snInvoke, bindings);

	//Phase 9e: out arguments on virtual methods are rejected — the
	//writeback mask is baked into the call instruction against the
	//static callee's parameter layout; a runtime override resolved by
	//name-based dispatch could disagree with it. Interface methods are
	//always dispatched by name (never NF_Virtual-flagged), so they are
	//covered via the parent decl kind.
	if (pCallee)
	{
		bool bDispatchedByName = pCallee->ContainFlags(NF_Virtual)
			|| (pCallee->Parent()
				&& pCallee->Parent()->Kind() == NK_InterfaceDecl);
		if (bDispatchedByName)
		{
			for (auto &b : bindings)
			{
				if (b.bIsOut)
				{
					m_Env.Log(CLL_Error, snInvoke.Location(),
						"out arguments are not supported on virtual method "
						"\"%s\".",
						pCallee->Name().c_str());
					return;
				}
			}
		}
	}

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

	switch (res)
	{
	case FFR_ApproximateMatch:
		assert(pCallee);
		FixupParamTypesWithBindings(snInvoke, bindings);
		snInvoke.SetBindings(std::move(bindings));
		break;
	case FFR_ExactMatch:
		assert(pCallee);
		snInvoke.SetBindings(std::move(bindings));
		break;
	case FFR_Incompatible:
		m_Env.Log(CLL_Error, snInvoke.Location(),
			"The function invoke \"%s\" is not compatible with the "
			"declaration.", snInvoke.ToString().c_str());
		if (pCallee) {
			m_Env.Log(CLL_More, pCallee->Location(),
				"See also the declaration of \"%s\".",
				pCallee->ToString().c_str());
		}
		return;
	default:
		assert(res == FFR_FuncNameNotFound);
		m_Env.Log(CLL_Error, snInvoke.Location(),
			"The function \"%s\" does not exist or is not accessible.",
			snInvoke.CalleeName().c_str());
		return;
	}

	ResolveFieldExprAs(snInvoke, pCallee);
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

//Phase 11: namespace-qualified stdlib call (math.sqrt(x), io.print(s)).
//Resolves against the built-in table in StdLib.h. Every branch consumes
//the expression — resolved or diagnosed — because namespace names are
//reserved and never resolve as fields (no fallback path exists).
void ExprResolveAccessor::TryResolveStdLibCall(SnMemberExpr &snMember,
	SnIdentifierExpr &outerId, SnInvokeExpr &invoke)
{
	const std::string ns(outerId.Name());
	const auto& fnName = invoke.CalleeName();

	//Table-driven by-name dispatch: named/out arguments can never bind
	//(same guards as the built-in string methods in Access(SnMemberExpr&)).
	if (HasNamedArgument(invoke))
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"Named arguments are not supported by standard library functions.");
		return;
	}
	if (HasOutArgument(invoke))
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"out arguments are not supported by standard library functions.");
		return;
	}

	const StdLibEntry* pEntry = FindStdLibFunction(ns, fnName);
	if (!pEntry)
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"Unknown standard library function \"%s.%s\".",
			ns.c_str(), fnName.c_str());
		return;
	}

	const size_t argCount = ArgCountOf(invoke);
	if (argCount < pEntry->minArgs || argCount > pEntry->maxArgs)
	{
		if (pEntry->minArgs == pEntry->maxArgs)
			m_Env.Log(CLL_Error, invoke.Location(),
				"\"%s.%s\" expects %d argument(s).",
				ns.c_str(), fnName.c_str(), (int)pEntry->minArgs);
		else
			m_Env.Log(CLL_Error, invoke.Location(),
				"\"%s.%s\" expects %d to %d argument(s).",
				ns.c_str(), fnName.c_str(),
				(int)pEntry->minArgs, (int)pEntry->maxArgs);
		return;
	}

	//Args resolve in the caller's scope. The intercept runs before
	//Access(SnMemberExpr&) sets ERF_SearchInParentOnly / swaps m_pContext,
	//so no context restore is needed (unlike the string-methods branch).
	ResolveExpressionList(invoke.Params());

	//Per-param type policy: exact RTK kind match, or int->float widening
	//(wrapped in a cast expr in place — FixupParamTypes recipe over
	//invoke.Children()). Everything else is a compile error naming the
	//function, so the user sees which call is wrong.
	auto& children = invoke.Children();
	size_t paramIdx = 0;
	for (auto it = children.begin(); it != children.end(); ++it, ++paramIdx)
	{
		auto& arg = static_cast<SnExpression&>(*it);
		//Array-valued args must be rejected BEFORE the kind match:
		//EvalDataType of an array-valued expression returns the ELEMENT
		//kind (EvalDataType dispatch-order trap), so `int[]` would
		//masquerade as int and the intrinsic would reinterpret the array
		//handle — garbage values today, an out-of-bounds pool read once
		//io.print widens the accepted kinds (Step 2). Covers identifier,
		//member, new-array and array-returning-call shapes.
		if (IsArrayValuedExpr(arg))
		{
			m_Env.Log(CLL_Error, arg.Location(),
				"Argument %d of \"%s.%s\" is an array; \"%s\" expected.",
				(int)paramIdx + 1, ns.c_str(), fnName.c_str(),
				StdLibKindName(pEntry->paramKinds[paramIdx]));
			continue;
		}
		auto* pArgType = arg.EvalDataType();
		if (!pArgType)
		{
			//A resolved arg with no type is a void call (the assignment
			//statement guards the same shape): passing it would silently
			//stage a stale pResult in the claim slot.
			if (arg.IsResolved())
				m_Env.Log(CLL_Error, arg.Location(),
					"Argument %d of \"%s.%s\" has no value: a void function "
					"result cannot be used as an argument.",
					(int)paramIdx + 1, ns.c_str(), fnName.c_str());
			continue;  //unresolved arg was diagnosed above
		}
		const NodeKind argKind = pArgType->Kind();
		const uint8_t want = pEntry->paramKinds[paramIdx];
		//io.print (coerceToString): every param accepts string|int|float —
		//codegen branches on the arg's own static kind and converts at the
		//call site. No widening wrap here; class/struct/enum must call
		//.toString() explicitly.
		if (pEntry->coerceToString)
		{
			//null literal is Int32-typed (KT_Null); accepting it would
			//print "0". Reject it explicitly.
			if (arg.ContainFlags(NF_NullLiteral))
			{
				m_Env.Log(CLL_Error, arg.Location(),
					"Argument %d of \"%s.%s\" cannot be null.",
					(int)paramIdx + 1, ns.c_str(), fnName.c_str());
			}
			else if (argKind != NK_String && argKind != NK_Int32
				&& argKind != NK_Float)
			{
				m_Env.Log(CLL_Error, arg.Location(),
					"Argument %d of \"%s.%s\" has type \"%s\"; string, int "
					"or float expected (class and enum values: call "
					".toString() first).",
					(int)paramIdx + 1, ns.c_str(), fnName.c_str(),
					pArgType->ToString().c_str());
			}
			continue;
		}
		bool ok = (argKind == NK_Int32 && want == RTK_Int32)
			|| (argKind == NK_Float && want == RTK_Float)
			|| (argKind == NK_String && want == RTK_String);
		const bool widen = (argKind == NK_Int32 && want == RTK_Float);
		if (!ok && !widen)
		{
			m_Env.Log(CLL_Error, arg.Location(),
				"Argument %d of \"%s.%s\" has type \"%s\"; \"%s\" expected.",
				(int)paramIdx + 1, ns.c_str(), fnName.c_str(),
				pArgType->ToString().c_str(), StdLibKindName(want));
			continue;
		}
		if (widen)
		{
			//Sole automatic promotion (same policy as user-function calls).
			auto* pFloatType = SnBuiltinDataType::InstanceOf(NK_Float);
			TypeCastInfo castInfo(pArgType, pFloatType);
			FixupExprType(it, castInfo);
		}
	}

	//Wrap up. invoke.Callee() deliberately stays null — same as the built-in
	//string methods — so the walker reserves argCount+1 slots; the
	//namespace-shaped emission only uses argCount (over-reserve is safe).
	invoke.AddFlags(NF_Resolved);
	SnField* pResultField = nullptr;
	switch ((StdLibReturnType)pEntry->returnType)
	{
	case SLRT_Float:
		pResultField = SnBuiltinDataType::InstanceOf(NK_Float);
		break;
	case SLRT_Int32:
		pResultField = SnBuiltinDataType::InstanceOf(NK_Int32);
		break;
	case SLRT_String:
		pResultField = SnBuiltinDataType::InstanceOf(NK_String);
		break;
	case SLRT_ListString:
		//fs.listFiles / s.split (Step 3+): List<string> generic instance.
	{
		std::vector<SnField*> listArgs{
			SnBuiltinDataType::InstanceOf(NK_String) };
		pResultField = GetGenericClassDecl("List", listArgs,
			invoke.Location());
		break;
	}
	case SLRT_Void:
		break;  //void: no result type; void assignment rejected downstream
	}
	if (pResultField)
	{
		snMember.EvalDataType(pResultField);
		//Set m_pField directly (not via ResolveFieldExprAs) so chained
		//access (fs.join(a, b).length()) survives IsDataExpr().
		snMember.m_pField = pResultField;
	}
	snMember.AddFlags(NF_Resolved);
}

void ExprResolveAccessor::Access(SnMemberExpr &snMember)
{
	assert(!snMember.IsResolved());

	auto pOuterExpr = snMember.Outer();
	assert(pOuterExpr);

	//Phase 11: namespace-qualified stdlib call (math.sqrt(x)). Intercept
	//before the outer identifier resolves — namespace names are reserved
	//and never resolve as fields, so the normal path below would only log
	//"Cannot resolve the field" without naming the actual mistake.
	if (pOuterExpr->Kind() == NK_IdentifierExpr)
	{
		auto& outerId = static_cast<SnIdentifierExpr&>(*pOuterExpr);
		auto* pInnerExpr = snMember.Inner();
		if (IsStdLibNamespaceName(outerId.Name())
			&& pInnerExpr && pInnerExpr->Kind() == NK_InvokeExpr)
		{
			TryResolveStdLibCall(snMember, outerId,
				static_cast<SnInvokeExpr&>(*pInnerExpr));
			return;
		}
	}

	pOuterExpr->Accept(*m_pVisitor);
	if (!pOuterExpr->IsResolved())
		return;

	auto pSavedContext = m_pContext;

	if (snMember.Outer()->IsDataExpr())
		m_pContext = snMember.Outer()->EvalDataType();
	else
	{
		auto &outerFieldExpr = static_cast<SnFieldExpr &>(*snMember.Outer());
		auto* pOuterField = static_cast<SnField *>(outerFieldExpr.Field());
		m_pContext = pOuterField;
		if (pOuterField && !pOuterField->IsTypeField())
			m_pContext = snMember.Outer()->EvalDataType();
		//Phase 12: enum member receiver (`Color.Blue.rank()`). The member
		//masquerades as Int32 (SnEnumMember::EvalDataType), which would
		//land the context on the int builtin — re-anchor to the owning
		//enum decl so the method search starts at the enum scope.
		if (pOuterField && pOuterField->Kind() == NK_EnumMember)
			m_pContext = pOuterField->Parent();
	}

	//Phase 12 review MAJOR-1: array-valued receivers masquerade as their
	//ELEMENT type in EvalDataType (trap-12 family, instance #7). Without
	//this gate the element type's method table binds — string[] receivers
	//enter the string-builtin block, enum/class receivers bind user
	//methods — and codegen passes the array's heap index as the receiver
	//(silent wrong value; verified: enum[].rank() returned heapIdx+10).
	//toString is exempt: the non-class toString dispatch below handles
	//array receivers explicitly via the IsArrayType() field check.
	{
		auto* pInnerForGate = snMember.Inner();
		if (pInnerForGate && pInnerForGate->Kind() == NK_InvokeExpr
			&& IsArrayValuedExpr(*pOuterExpr))
		{
			auto& invoke = static_cast<SnInvokeExpr&>(*pInnerForGate);
			if (invoke.CalleeName() != "toString")
			{
				m_Env.Log(CLL_Error, invoke.Location(),
					"methods cannot be called on an array; index an element "
					"first (e.g. a[i].%s(...)).",
					invoke.CalleeName().c_str());
				m_pContext = pSavedContext;
				return;
			}
		}
	}

	SCOPED_FLAG_RESETER(*this);
	AddFlags(ERF_SearchInParentOnly);
	auto pInnerExpr = snMember.Inner();
	assert(pInnerExpr);

	//Builtin string methods: s.length(), s.GetHashCode(), s.Equals(other).
	if (m_pContext && m_pContext->Kind() == NK_String
		&& pInnerExpr->Kind() == NK_InvokeExpr)
	{
		auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
		const auto& name = invoke.CalleeName();
		if (name == "length" && invoke.Params().begin() == invoke.Params().end())
		{
			pInnerExpr->AddFlags(NF_Resolved);
			snMember.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
			snMember.AddFlags(NF_Resolved);
			m_pContext = pSavedContext;
			return;
		}
		//Phase 8e-1: string.GetHashCode() and string.Equals(string) — value semantics.
		//Both intrinsified in VmBackend; resolver just needs to accept them.
		if (name == "getHashCode" && invoke.Params().begin() == invoke.Params().end())
		{
			pInnerExpr->AddFlags(NF_Resolved);
			snMember.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
			snMember.AddFlags(NF_Resolved);
			m_pContext = pSavedContext;
			return;
		}
		if (name == "equals")
		{
			//Round-12: by-name dispatch cannot bind named arguments, and the
			//intrinsic reads exactly {this, other} — reject both shapes.
			if (HasNamedArgument(invoke))
			{
				m_Env.Log(CLL_Error, invoke.Location(),
					"Named arguments are not supported by built-in methods.");
				m_pContext = pSavedContext;
				return;
			}
			//Round-14: intrinsics return through pResult only — an out
			//argument could never write back.
			if (HasOutArgument(invoke))
			{
				m_Env.Log(CLL_Error, invoke.Location(),
					"out arguments are not supported by built-in methods.");
				m_pContext = pSavedContext;
				return;
			}
			if (ArgCountOf(invoke) != 1)
			{
				m_Env.Log(CLL_Error, invoke.Location(),
					"string.equals requires exactly 1 argument.");
				m_pContext = pSavedContext;
				return;
			}
			//Round-11: args must resolve in the CALLER's scope — same recipe
			//as the user-class equals path below. Without this the argument
			//stayed unresolved and codegen's silent fallbacks (ConstZero /
			//skipped call) made t.equals(t) compare against stale memory.
			m_pContext = pSavedContext;
			RemoveFlags(ERF_SearchInParentOnly);
			ResolveExpressionList(invoke.Params());
			pInnerExpr->AddFlags(NF_Resolved);
			snMember.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
			snMember.AddFlags(NF_Resolved);
			m_pContext = pSavedContext;
			return;
		}
		//Phase 11 Step 3: table-driven built-in string methods (12 new;
		//equals/getHashCode above keep their 8e-1 ids). The method surface
		//is frozen as the future string class's methods (user decision #6).
		if (const StringMethodEntry* pMethod = FindStringMethod(name))
		{
			if (HasNamedArgument(invoke))
			{
				m_Env.Log(CLL_Error, invoke.Location(),
					"Named arguments are not supported by built-in methods.");
				m_pContext = pSavedContext;
				return;
			}
			if (HasOutArgument(invoke))
			{
				m_Env.Log(CLL_Error, invoke.Location(),
					"out arguments are not supported by built-in methods.");
				m_pContext = pSavedContext;
				return;
			}
			const size_t argCount = ArgCountOf(invoke);
			if (argCount < pMethod->minArgs || argCount > pMethod->maxArgs)
			{
				if (pMethod->minArgs == pMethod->maxArgs)
					m_Env.Log(CLL_Error, invoke.Location(),
						"string.%s expects %d argument(s).",
						pMethod->name, (int)pMethod->minArgs);
				else
					m_Env.Log(CLL_Error, invoke.Location(),
						"string.%s expects %d to %d argument(s).",
						pMethod->name, (int)pMethod->minArgs,
						(int)pMethod->maxArgs);
				m_pContext = pSavedContext;
				return;
			}
			//Args resolve in the caller's scope (equals recipe: restore the
			//context and drop the parent-only search first).
			m_pContext = pSavedContext;
			RemoveFlags(ERF_SearchInParentOnly);
			ResolveExpressionList(invoke.Params());
			//Per-arg policy: exact kind match vs paramKinds, no widening
			//(substring offsets are int; a float offset is a compile error).
			//Same guards as the namespace-call path: arrays masquerade as
			//their element kind, void calls have no value.
			auto& children = invoke.Children();
			size_t paramIdx = 0;
			for (auto it = children.begin(); it != children.end();
				++it, ++paramIdx)
			{
				auto& arg = static_cast<SnExpression&>(*it);
				if (IsArrayValuedExpr(arg))
				{
					m_Env.Log(CLL_Error, arg.Location(),
						"Argument %d of string.%s is an array; \"%s\" "
						"expected.",
						(int)paramIdx + 1, pMethod->name,
						StdLibKindName(pMethod->paramKinds[paramIdx]));
					continue;
				}
				auto* pArgType = arg.EvalDataType();
				if (!pArgType)
				{
					if (arg.IsResolved())
						m_Env.Log(CLL_Error, arg.Location(),
							"Argument %d of string.%s has no value: a void "
							"function result cannot be used as an argument.",
							(int)paramIdx + 1, pMethod->name);
					continue;
				}
				const uint8_t want = pMethod->paramKinds[paramIdx];
				const bool ok =
					(pArgType->Kind() == NK_Int32 && want == RTK_Int32)
					|| (pArgType->Kind() == NK_String && want == RTK_String);
				if (!ok)
				{
					m_Env.Log(CLL_Error, arg.Location(),
						"Argument %d of string.%s has type \"%s\"; \"%s\" "
						"expected.",
						(int)paramIdx + 1, pMethod->name,
						pArgType->ToString().c_str(), StdLibKindName(want));
					continue;
				}
			}
			pInnerExpr->AddFlags(NF_Resolved);
			SnField* pResultField = nullptr;
			switch ((StdLibReturnType)pMethod->returnType)
			{
			case SLRT_Int32:
				pResultField = SnBuiltinDataType::InstanceOf(NK_Int32);
				break;
			case SLRT_Float:
				pResultField = SnBuiltinDataType::InstanceOf(NK_Float);
				break;
			case SLRT_String:
				pResultField = SnBuiltinDataType::InstanceOf(NK_String);
				break;
			case SLRT_ListString:
			{
				std::vector<SnField*> listArgs{
					SnBuiltinDataType::InstanceOf(NK_String) };
				pResultField = GetGenericClassDecl("List", listArgs,
					invoke.Location());
				break;
			}
			case SLRT_Void:
				break;
			}
			if (pResultField)
			{
				snMember.EvalDataType(pResultField);
				//m_pField directly (not via ResolveFieldExprAs) so chained
				//access (s.substring(1).toUpper()) survives IsDataExpr() —
				//same rationale as the stdlib path.
				snMember.m_pField = pResultField;
			}
			snMember.AddFlags(NF_Resolved);
			m_pContext = pSavedContext;
			return;
		}
		//Phase 8e-9b: string.toString() — identity. Resolver folds the call
		//to a no-op (callee=null, EvalDataType=String). Codegen emits nothing
		//and the inner string idx flows through unchanged.
		if (name == "toString" && invoke.Params().begin() == invoke.Params().end())
		{
			pInnerExpr->AddFlags(NF_Resolved);
			snMember.EvalDataType(SnBuiltinDataType::InstanceOf(NK_String));
			snMember.AddFlags(NF_Resolved);
			//Mark the invoke as folded so codegen skips it. Use NF_Resolved flag
			//on the inner expression (already set above) and leave callee as-is;
			//VmBackend detects string receiver + toString name and emits nothing.
			m_pContext = pSavedContext;
			return;
		}
	}

		//Builtin array.length property.
		if (pOuterExpr->Kind() == NK_IdentifierExpr
			&& pInnerExpr->Kind() == NK_IdentifierExpr)
		{
			auto* pOuterField = static_cast<SnIdentifierExpr*>(pOuterExpr)->Field();
			auto& innerId = static_cast<SnIdentifierExpr&>(*pInnerExpr);
			if (pOuterField && pOuterField->IsArrayType()
				&& innerId.Name() == "length")
			{
				pInnerExpr->AddFlags(NF_Resolved);
				snMember.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
				snMember.AddFlags(NF_Resolved);
				m_pContext = pSavedContext;
				return;
			}
		}

		//Builtin stream methods: ByteStream/FileStream member calls.
		//These are resolved by name since the synthesized SnClassDecl has
		//no real method members. The return type is determined by method name.
		if (m_pContext && m_pContext->Kind() == NK_ClassDecl
			&& static_cast<SnClassDecl*>(m_pContext)->IsBuiltinClass()
			&& pInnerExpr->Kind() == NK_InvokeExpr)
		{
			auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
			const auto& name = invoke.CalleeName();
			bool isStreamMethod = false;
			NodeKind retKind = NK_Int32;  //default, overridden below
			if (name == "readInt" || name == "length" || name == "position")
				isStreamMethod = true;  // retKind = NK_Int32
			else if (name == "readFloat")
				{ isStreamMethod = true; retKind = NK_Float; }
			else if (name == "readString")
				{ isStreamMethod = true; retKind = NK_String; }
			else if (name == "writeInt" || name == "writeFloat"
				|| name == "writeString" || name == "reset" || name == "close"
				|| name == "writeStruct" || name == "writeObject")
				isStreamMethod = true;  // void return — no EvalDataType
			else if (name == "readStruct")
			{
				//ReadStruct("TypeName") returns a struct value of the named type.
				//The type-name argument MUST be a string literal so we can resolve
				//it at compile time. (Variables rejected — no generics in NLang.)
				isStreamMethod = true;
				auto& params = invoke.Params();
				auto it = params.begin();
				if (it == params.end() || (*it).Kind() != NK_LiteralExpr
					|| !(*it).EvalDataType()
					|| (*it).EvalDataType()->Kind() != NK_String)
				{
					m_Env.Log(CLL_Error, invoke.Location(),
						"ReadStruct requires a string literal argument.");
					m_pContext = pSavedContext;
					return;
				}
				auto& lit = static_cast<SnLiteralExpr&>(*it);
				const std::string* pTypeName = lit.Value().Data().m_String;
				const std::string typeName = pTypeName ? *pTypeName : std::string();
				//Look up typeName as a struct in the caller's namespace chain.
				//NOT m_pContext — that is the synthesized builtin stream class,
				//whose Parent() is null, so the walk would never reach the
				//user's translation-unit scope where structs are declared.
				SnField* found = nullptr;
				auto* ctx = pSavedContext;
				while (ctx && !found)
				{
					found = ctx->FindField(typeName);
					ctx = ctx->Parent();
				}
				if (!found || found->Kind() != NK_StructDecl)
				{
					m_Env.Log(CLL_Error, invoke.Location(),
						"ReadStruct type not found: %s.", typeName.c_str());
					m_pContext = pSavedContext;
					return;
				}
				snMember.EvalDataType(found);
			}
			else if (name == "readObject")
			{
				//ReadObject("TypeName") returns a class object of the named type.
				//Mirrors ReadStruct but resolves typeName as a class (NK_ClassDecl).
				//The declared type may be a base class of the stream's actual type;
				//polymorphic deserialization is enforced in VmExecutor via
				//IsSubclassOf (Phase 8d).
				isStreamMethod = true;
				auto& params = invoke.Params();
				auto it = params.begin();
				if (it == params.end() || (*it).Kind() != NK_LiteralExpr
					|| !(*it).EvalDataType()
					|| (*it).EvalDataType()->Kind() != NK_String)
				{
					m_Env.Log(CLL_Error, invoke.Location(),
						"ReadObject requires a string literal argument.");
					m_pContext = pSavedContext;
					return;
				}
				auto& lit = static_cast<SnLiteralExpr&>(*it);
				const std::string* pTypeName = lit.Value().Data().m_String;
				const std::string typeName = pTypeName ? *pTypeName : std::string();
				//Look up typeName as a class in the caller's namespace chain.
				SnField* found = nullptr;
				auto* ctx = pSavedContext;
				while (ctx && !found)
				{
					found = ctx->FindField(typeName);
					ctx = ctx->Parent();
				}
				if (!found || found->Kind() != NK_ClassDecl)
				{
					m_Env.Log(CLL_Error, invoke.Location(),
						"ReadObject type not found: %s.", typeName.c_str());
					m_pContext = pSavedContext;
					return;
				}
				snMember.EvalDataType(found);
			}
			if (isStreamMethod)
			{
				//Round-12: by-name dispatch cannot bind name = value args.
				if (HasNamedArgument(invoke))
				{
					m_Env.Log(CLL_Error, invoke.Location(),
						"Named arguments are not supported by built-in methods.");
					m_pContext = pSavedContext;
					return;
				}
				//Round-14: out args cannot write back through by-name dispatch.
				if (HasOutArgument(invoke))
				{
					m_Env.Log(CLL_Error, invoke.Location(),
						"out arguments are not supported by built-in methods.");
					m_pContext = pSavedContext;
					return;
				}
				//Resolve the args so each param's Field()/EvalDataType() is
				//populated (e.g. struct-typed IdentifierExpr needs Field() set
				//so VmBackend can emit the correct load opcode). Without this,
				//the early return below skips arg resolution entirely and the
				//backend falls through to const_zero for unresolved idents.
				//
				//Args are evaluated in the CALLER's scope, not the synthesized
				//builtin-class scope (which is empty). Restore m_pContext AND
				//clear ERF_SearchInParentOnly (set above for the member lookup)
				//so a normal scope walk finds the caller's locals.
				m_pContext = pSavedContext;
				RemoveFlags(ERF_SearchInParentOnly);
				ResolveExpressionList(invoke.Params());
				pInnerExpr->AddFlags(NF_Resolved);
				//For void-returning methods, leave EvalDataType unset.
				if (name != "writeInt" && name != "writeFloat"
					&& name != "writeString" && name != "reset" && name != "close"
					&& name != "writeStruct" && name != "writeObject")
				{
					//ReadStruct/ReadObject already set EvalDataType above; others use retKind.
					if (name != "readStruct" && name != "readObject")
						snMember.EvalDataType(SnBuiltinDataType::InstanceOf(retKind));
				}
				snMember.AddFlags(NF_Resolved);
				m_pContext = pSavedContext;
				return;
			}
		}

	//Phase 8e-1: implicit Object protocol methods on user classes.
	//Every user class inherits Equals(Object)→int and GetHashCode()→int from
	//the synthesized Object base class. The methods have no AST representation
	//(they're intrinsic stubs in VmBackend), so the normal InvokeExpr resolution
	//path fails. Treat them as builtin virtuals here, parallel to stream methods.
	//Args must be resolved in the CALLER's scope, not the empty Object scope —
	//hence the early return before pInnerExpr->Accept below.
	if (m_pContext && m_pContext->Kind() == NK_ClassDecl
		&& pInnerExpr->Kind() == NK_InvokeExpr)
	{
		auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
		const auto& name = invoke.CalleeName();
		auto* pClassDecl = static_cast<SnClassDecl*>(m_pContext);
		bool isUserClass = !pClassDecl->IsBuiltinClass();
		bool isObjectClass = pClassDecl->IsBuiltinClass()
			&& pClassDecl->Name() == "Object";
		if (name == "equals" || name == "getHashCode")
		{
			if (isUserClass)
			{
				//Round-12: same validation as the string branch — by-name
				//dispatch cannot bind named args, and the intrinsics read
				//exactly {this[, other]}.
				if (HasNamedArgument(invoke))
				{
					m_Env.Log(CLL_Error, invoke.Location(),
						"Named arguments are not supported by built-in methods.");
					m_pContext = pSavedContext;
					return;
				}
				//Round-14: out args cannot write back through by-name dispatch.
				if (HasOutArgument(invoke))
				{
					m_Env.Log(CLL_Error, invoke.Location(),
						"out arguments are not supported by built-in methods.");
					m_pContext = pSavedContext;
					return;
				}
				if ((name == "equals" && ArgCountOf(invoke) != 1)
					|| (name == "getHashCode" && ArgCountOf(invoke) != 0))
				{
					m_Env.Log(CLL_Error, invoke.Location(),
						"Built-in method '%s' called with the wrong number of arguments.",
						name.c_str());
					m_pContext = pSavedContext;
					return;
				}
				m_pContext = pSavedContext;
				RemoveFlags(ERF_SearchInParentOnly);
				ResolveExpressionList(invoke.Params());
			pInnerExpr->AddFlags(NF_Resolved);
			snMember.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
			snMember.AddFlags(NF_Resolved);
				m_pContext = pSavedContext;
				return;
			}
		}
		//Phase 8e-9b: string toString() — user class inherits Object.toString().
		//User-defined override is resolved via the normal class-method path
		//(CalleeName resolves to a real SnFunction); this branch only catches
		//the no-override case to fall through to Object intrinsic dispatch.
		if (name == "toString"
			&& invoke.Params().begin() == invoke.Params().end()
			&& (isUserClass || isObjectClass))
		{
			m_pContext = pSavedContext;
			RemoveFlags(ERF_SearchInParentOnly);
			ResolveExpressionList(invoke.Params());
			pInnerExpr->AddFlags(NF_Resolved);
			snMember.EvalDataType(SnBuiltinDataType::InstanceOf(NK_String));
			snMember.AddFlags(NF_Resolved);
			m_pContext = pSavedContext;
			return;
		}
	}

	//Phase 8e-9b: non-class receiver toString() — enum, int, float.
	//These types have no method table; the resolver accepts the call by setting
	//EvalDataType=String + NF_Resolved. Codegen dispatches based on the
	//outer expression's EvalDataType (enum→OP_Enum_to_str, int→OP_Int32_to_str,
	//float→OP_Float_to_str). No m_pField hack needed — the type information
	//flows through the existing outer->EvalDataType() channel, same as struct/
	//class/interface field access in codegen.
	//
	//Detection: m_pContext (the receiver's type context) is NK_EnumDecl,
	//NK_Int32, or NK_Float. For enum literal access (Color.Green.toString()),
	//m_pContext is NK_Int32 (SnEnumMember::EvalDataType returns NK_Int32), so
	//we also check the outer's Field() chain for NK_EnumMember.
	if (pInnerExpr->Kind() == NK_InvokeExpr)
	{
		auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
		if (invoke.CalleeName() == "toString"
			&& invoke.Params().begin() == invoke.Params().end())
		{
			bool isNonClassToString = false;
			//Phase 9b-pre: array receiver — Array is a VM primitive, not a
			//class. MUST be checked BEFORE the int/float path because
			//EvalDataType for `int[] arr` returns the element type (NK_Int32);
			//array-ness is stored separately on the SnField via IsArrayType().
			//Detection matches ExprResolver's array.length path.
			{
				auto outerKind = snMember.Outer()->Kind();
				if (outerKind == NK_IdentifierExpr
					|| outerKind == NK_MemberExpr)
				{
					auto& outerFieldExpr = static_cast<SnFieldExpr&>(
						*snMember.Outer());
					auto* outerField = outerFieldExpr.Field();
					if (outerField && outerField->IsArrayType())
						isNonClassToString = true;
				}
			}
			//Enum via m_pContext (typed enum variable: Color c; c.toString())
			if (!isNonClassToString && m_pContext
				&& m_pContext->Kind() == NK_EnumDecl)
				isNonClassToString = true;
			//Enum via outer Field() chain (Color.Green.toString())
			if (!isNonClassToString)
			{
				auto outerKind = snMember.Outer()->Kind();
				if (outerKind == NK_MemberExpr || outerKind == NK_IdentifierExpr)
				{
					auto& outerFieldExpr = static_cast<SnFieldExpr&>(
						*snMember.Outer());
					auto* outerField = outerFieldExpr.Field();
					if (outerField && outerField->Kind() == NK_EnumMember)
						isNonClassToString = true;
				}
			}
			//Int/float via m_pContext (int x; x.toString(), 42.toString())
			if (!isNonClassToString && m_pContext
				&& (m_pContext->Kind() == NK_Int32
					|| m_pContext->Kind() == NK_Float))
			{
				isNonClassToString = true;
			}
			if (isNonClassToString)
			{
				m_pContext = pSavedContext;
				RemoveFlags(ERF_SearchInParentOnly);
				ResolveExpressionList(invoke.Params());
				pInnerExpr->AddFlags(NF_Resolved);
				snMember.EvalDataType(SnBuiltinDataType::InstanceOf(NK_String));
				snMember.AddFlags(NF_Resolved);
				m_pContext = pSavedContext;
				return;
			}
		}
	}

	//Phase 8e-3 / 8e-4: built-in generic List<T> / Dict<K,V> methods.
	//Synthetic generic SnClassDecl carries no real method members; dispatch
	//by name here. Return types:
	// - List Add/Set/RemoveAt/Clear, Dict Set/Clear: void (no EvalDataType)
	// - List Length/IndexOf/Contains, Dict ContainsKey/Remove/Count: int
	// - List Get: T (typeArgs[0]); Dict Get: V (typeArgs[1])
	//All elements at runtime are heap idxs (boxed primitives or class refs);
	//VmBackend emits OP_Box/OP_Unbox around primitive-typed call sites.
	if (m_pContext && m_pContext->Kind() == NK_ClassDecl
		&& IsGenericClassDecl(static_cast<SnClassDecl*>(m_pContext))
		&& pInnerExpr->Kind() == NK_InvokeExpr)
	{
		auto* pGenClass = static_cast<SnClassDecl*>(m_pContext);
		auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
		const auto& name = invoke.CalleeName();
		const auto& baseName = pGenClass->BaseName();
		bool isGenericMethod = false;
		if (baseName == "List") {
			isGenericMethod = (name == "add" || name == "get" || name == "set"
				|| name == "length" || name == "removeAt" || name == "indexOf"
				|| name == "contains" || name == "clear"
					|| name == "toString");
		} else if (baseName == "Dict") {
			isGenericMethod = (name == "set" || name == "get"
				|| name == "containsKey" || name == "remove"
				|| name == "clear" || name == "count"
				|| name == "keys" || name == "toString");
		}
		if (isGenericMethod)
		{
			//Round-12: by-name dispatch cannot bind name = value args.
			if (HasNamedArgument(invoke))
			{
				m_Env.Log(CLL_Error, invoke.Location(),
					"Named arguments are not supported by built-in methods.");
				m_pContext = pSavedContext;
				return;
			}
			//Round-14: out args cannot write back through by-name dispatch.
			if (HasOutArgument(invoke))
			{
				m_Env.Log(CLL_Error, invoke.Location(),
					"out arguments are not supported by built-in methods.");
				m_pContext = pSavedContext;
				return;
			}
			//Round-13: validate the argument count against the VM intrinsic
			//stubs (VmBackend's addMethod tables). A mismatch previously
			//slipped to codegen — extra args were silently ignored, missing
			//args read uninitialized callParam slots. Must match the
			//isGenericMethod name sets above.
			static const std::map<std::string, size_t> kListMethodArities = {
				{"add", 1}, {"get", 1}, {"set", 2}, {"length", 0},
				{"removeAt", 1}, {"indexOf", 1}, {"contains", 1},
				{"clear", 0}, {"toString", 0},
			};
			static const std::map<std::string, size_t> kDictMethodArities = {
				{"set", 2}, {"get", 1}, {"containsKey", 1}, {"remove", 1},
				{"clear", 0}, {"count", 0}, {"keys", 0}, {"toString", 0},
			};
			const auto& arities = (baseName == "List")
				? kListMethodArities : kDictMethodArities;
			auto arityIt = arities.find(name);
			if (arityIt != arities.end() && ArgCountOf(invoke) != arityIt->second)
			{
				m_Env.Log(CLL_Error, invoke.Location(),
					"Built-in method '%s' called with the wrong number of arguments.",
					name.c_str());
				m_pContext = pSavedContext;
				return;
			}
			m_pContext = pSavedContext;
			RemoveFlags(ERF_SearchInParentOnly);
			ResolveExpressionList(invoke.Params());
			pInnerExpr->AddFlags(NF_Resolved);
			SnField* pResultField = nullptr;
			auto typeArgs = GetGenericTypeArgs(pGenClass);
			if (baseName == "List" && name == "get") {
				//Return type = T (typeArgs[0]).
				if (!typeArgs.empty() && typeArgs[0]) {
					snMember.EvalDataType(typeArgs[0]);
					pResultField = typeArgs[0];
				}
			} else if (baseName == "Dict" && name == "get") {
				//Return type = V (typeArgs[1]).
				if (typeArgs.size() > 1 && typeArgs[1]) {
					snMember.EvalDataType(typeArgs[1]);
					pResultField = typeArgs[1];
				}
			} else if (
				(baseName == "List"
					&& (name == "length" || name == "indexOf" || name == "contains"))
				|| (baseName == "Dict"
					&& (name == "containsKey" || name == "remove" || name == "count"))
			) {
				auto* pInt = SnBuiltinDataType::InstanceOf(NK_Int32);
				snMember.EvalDataType(pInt);
				pResultField = pInt;
			} else if (baseName == "Dict" && name == "keys") {
				//Phase 8e-5: Dict.Keys() returns List<K> where K = typeArgs[0].
				//Synthesize a List<K> generic instantiation so foreach lowering
				//and codegen's per-method boxing plan see the right element type.
				if (!typeArgs.empty() && typeArgs[0]) {
					std::vector<SnField*> listArgs{ typeArgs[0] };
					auto* pListClass = GetGenericClassDecl("List", listArgs,
						pInnerExpr->Location());
					if (pListClass) {
						//SnClassDecl IS-A SnField, so it can serve as EvalDataType.
						snMember.EvalDataType(pListClass);
						pResultField = pListClass;
					}
				}
			} else if (name == "toString") {
				//Phase 9b-pre: List/Dict toString() returns string.
				auto* pStr = SnBuiltinDataType::InstanceOf(NK_String);
				snMember.EvalDataType(pStr);
				pResultField = pStr;
			}
			// Set m_pField directly (not via ResolveFieldExprAs
			// which would overwrite EvalDataType with SnType).
			// Needed so IsDataExpr() doesn't crash when chained
			// (e.g. lst.Get(0).length()).
			if (pResultField)
				snMember.m_pField = pResultField;
			snMember.AddFlags(NF_Resolved);
			m_pContext = pSavedContext;
			return;
		}
	}

	//Phase 9d: built-in Exception class field access (e.message, e.backtrace).
	//The synthetic SnClassDecl has no real member fields, so resolve by name.
	//Field offsets are hard-coded in VmBackend::FindClassFieldOffset:
	//  message  → slot[1] (offset 4)
	//  backtrace → slot[2] (offset 8)
	//This also handles user subclasses of Exception — walk SuperClass()
	//chain to detect Exception ancestry.
	if (m_pContext && m_pContext->Kind() == NK_ClassDecl
		&& pInnerExpr->Kind() == NK_IdentifierExpr)
	{
		auto* pClass = static_cast<SnClassDecl*>(m_pContext);
		//Walk SuperClass chain looking for a built-in Exception class.
		bool isExceptionSubclass = false;
		for (SnClassDecl* pWalk = pClass; pWalk; ) {
			if (pWalk->IsBuiltinClass()
				&& IsBuiltinExceptionClassName(pWalk->Name())) {
				isExceptionSubclass = true;
				break;
			}
			pWalk = pWalk->SuperClass();
		}
		if (isExceptionSubclass) {
			auto& innerId = static_cast<SnIdentifierExpr&>(*pInnerExpr);
			const auto& fieldName = innerId.Name();
			SnField* pResultField = nullptr;
			if (fieldName == "message") {
				pResultField = SnBuiltinDataType::InstanceOf(NK_String);
			} else if (fieldName == "backtrace") {
				//backtrace is List<string> — synthesize the generic instantiation.
				auto* pStr = SnBuiltinDataType::InstanceOf(NK_String);
				std::vector<SnField*> listArgs{ pStr };
				pResultField = GetGenericClassDecl("List", listArgs,
					pInnerExpr->Location());
			}
			if (pResultField) {
				innerId.AddFlags(NF_Resolved);
				snMember.EvalDataType(pResultField);
				snMember.m_pField = pResultField;
				snMember.AddFlags(NF_Resolved);
				m_pContext = pSavedContext;
				return;
			}
		}
	}

	//Phase 9e (pre-existing gap exposed by out params): a method invoke's
	//ARGUMENTS must resolve in the caller's scope. m_pContext is the
	//receiver's class here (set for the callee lookup) and
	//ERF_SearchInParentOnly hides the calling function's locals — so
	//`c.f(v)` failed with "Cannot resolve the field: v". Existing tests
	//never hit this because they only pass literals. Resolve the args in
	//the caller scope first (mirroring the stream/generic early-return
	//paths above), then re-enter the class scope so Access(SnInvokeExpr)
	//finds the callee; its ResolveExpressionList skips resolved params.
	if (pInnerExpr->Kind() == NK_InvokeExpr)
	{
		auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
		auto* pClassCtx = m_pContext;
		m_pContext = pSavedContext;
		RemoveFlags(ERF_SearchInParentOnly);
		ResolveExpressionList(invoke.Params());
		m_pContext = pClassCtx;
		AddFlags(ERF_SearchInParentOnly);
	}

	pInnerExpr->Accept(*m_pVisitor);
	if (pInnerExpr->IsResolved())
		ResolveFieldExprAs(snMember, pInnerExpr->Field());

	m_pContext = pSavedContext;
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

	TypeCastInfo castInfo(pSrcType, pTgtType);
	auto kind = castInfo.Kind();
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

	//EvalDataType: store the element type for later array type registration.
	//We do NOT set Field() here since arrays are not a single field; the
	//backend registers a CompiledArrayType entry from this element type.
	sn.EvalDataType(pElemField);
	sn.AddFlags(NF_Resolved);
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
		//Bare form: parent populated the LHS variable.
		//For array variables (int[] arr), IsArrayType()==true but
		//EvalDataType() returns the element type — preserve both signals.
		bIsArray = pInferred->IsArrayType();
		pTargetField = pInferred->EvalDataType();
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
	//List<T>/Dict<K,V> subscript (li[i] / d[k]): sugar over get().
	//The base resolves to a synthetic generic-instantiation class; the
	//element type is T (List) or V (Dict). Without this peel the
	//subscript keeps the container type and every consumer (assignment,
	//member chains, nested subscripts) mis-types it.
	//
	//Array-ness is a SEPARATE flag (field->IsArrayType()) — EvalDataType
	//of `List<int>[] a` returns the element type, which IS a generic
	//instantiation, so the Kind() check below would mistake the array
	//for a container (EvalDataType-dispatch-order trap, 5th instance).
	//Array bases keep the plain element-type propagation below.
	if (!IsArrayTypedBase(arrayExpr)
		&& arrayType && arrayType->Kind() == NK_ClassDecl)
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
				return;
			}
		}
	}
	if (arrayType)
		sn.EvalDataType(arrayType);
	sn.AddFlags(NF_Resolved);
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

void ExprResolveAccessor::ResolveFieldExprAs(SnFieldExpr &expr, SnField *pField)
{
	assert(pField && !expr.IsResolved());

	expr.m_pField = pField;
	if (!expr.PostResolveCheck(m_Env))
	{
		expr.AddFlags(NF_Invalid);
		return;
	}

	expr.EvalDataType(pField->EvalDataType());
	expr.AddFlags(NF_Resolved);
	return;
}

SnField *ExprResolveAccessor::FindFieldInAncestor(const std::string &sName,
	SyntaxNode &parent, const SnField &accessor, ExprResolveFlagSet flags)
{
	SnField *pField = parent.FindField(sName);
	if (pField && pField->AllowAccess(accessor))
		return pField;
	if (flags & ERF_SearchInParentOnly)
		return pField;
	auto pParent = parent.Parent();
	if (!pParent)
		return nullptr;
	return FindFieldInAncestor(sName, *pParent, accessor, flags);
}

SnField *ExprResolveAccessor::FindFieldInUsings(std::string &sName,
	const UsingList &usings, const SnField & accessor)
{
	for (auto pUsing : usings)
	{
		if (!pUsing->IsResolved())
			continue;
		auto pNamespace = pUsing->Namespace();
		assert(pNamespace);
		auto pField = FindFieldInAncestor(sName, *pNamespace, accessor,
			ERF_SearchInParentOnly);
		if (pField)
			return pField;
	}
	return nullptr;
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

FindFuncResult ExprResolveAccessor::FindFuncByInvoke(SnFunction *&pFuncFound,
	SnInvokeExpr &invoke, std::vector<FormalBinding> &outBindings)
{
	//Contract: the out-param is always initialized. The NotFound path
	//returns early without touching it — an uninitialized caller local
	//then holds stack garbage, and `if (pCallee)` in Access(SnInvokeExpr)
	//dereferences a dangling pointer (ncc crash; observed when imported
	//stubs shifted stack layout). Clearing here covers every path.
	pFuncFound = nullptr;
	const bool bSearchInAncestor = !ContainFlags(ERF_SearchInParentOnly);
	bool bFoundByName = false;
	auto &sFuncName = invoke.CalleeName();

	//Best candidate across all scanned scopes.
	int nBestDistance = -1;
	bool bAmbiguous = false;
	SnFunction *pBest = nullptr;
	std::vector<FormalBinding> bestBindings;

	//Evaluate a single candidate. Returns true if candidate is viable
	//(non-negative distance). Updates pBest/nBestDistance/bAmbiguous.
	auto consider = [&](SnFunction *pFunc) {
		std::vector<FormalBinding> tryBind;
		if (!TryBindInvoke(invoke, *pFunc, tryBind))
			return;
		int n = ComputeBindingDistance(tryBind);
		if (n < 0)
			return;
		if (nBestDistance < 0 || n < nBestDistance)
		{
			nBestDistance = n;
			pBest = pFunc;
			bestBindings = std::move(tryBind);
			bAmbiguous = false;
		}
		else if (n == nBestDistance)
		{
			//Tie at the smallest viable distance — ambiguous.
			bAmbiguous = true;
		}
	};

	//Search a single scope's NameDict for matching functions.
	auto searchScope = [&](SnFunctionParentField& parent) {
		auto range = parent.Members().NameDict().equal_range(sFuncName);
		for (auto iField = range.first; iField != range.second; ++iField) {
			SnField *pField = iField->second;
			if (pField->Kind() != NK_Function)
				continue;

			auto pFunc = static_cast<SnFunction *>(pField);
			if (!pFunc->AllowAccess(*m_pAccessor))
				continue;

			if (!bFoundByName)
				bFoundByName = true;
			consider(pFunc);
		}
	};

	SyntaxNode *pParent = m_pContext;
	while (pParent)
	{
		if (CanBeFuncParentEx(pParent->Kind()))
		{
			auto pParentType = static_cast<SnFunctionParentField*>(pParent);
			searchScope(*pParentType);

			//For class contexts, also search the inheritance chain
			//when the method is not found in the current class's Members().
			if (pParent->Kind() == NK_ClassDecl && !bFoundByName)
			{
				auto *pSuper = static_cast<SnClassDecl*>(pParent)->SuperClass();
				while (pSuper && !bFoundByName)
				{
					searchScope(*pSuper);
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
				if (!bFoundByName)
					bFoundByName = true;
				consider(pFunc);
			}
			if (!bSearchInAncestor)
				break;
		}
		pParent = pParent->Parent();
	}

	if (!bFoundByName)
		return FFR_FuncNameNotFound;

	if (nBestDistance < 0)
	{
		//At least one candidate matched by name but none could bind.
		pFuncFound = nullptr;
		return FFR_Incompatible;
	}

	if (bAmbiguous)
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"ambiguous call to function \"%s\": multiple overloads match "
			"with equal distance.",
			sFuncName.c_str());
		pFuncFound = nullptr;
		return FFR_Incompatible;
	}

	pFuncFound = pBest;
	outBindings = std::move(bestBindings);
	return (nBestDistance == 0) ? FFR_ExactMatch : FFR_ApproximateMatch;
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
		auto *pSrc = b.pCallerExpr->EvalDataType();
		auto *pTgt = b.pFormal->EvalDataType();
		if (!pSrc || !pTgt)
			return -1;
		int n = CalcTypeDistance(*pSrc, *pTgt);
		if (n < 0)
			return -1;
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

int ExprResolveAccessor::CalcDistanceOfParams(
	const SnExpressionList &concretParams,
	const SnFunction::ParamList &formalParams) const
{
	int nDistance = 0;
	auto iFormal = formalParams.begin();
	auto iFormalEnd = formalParams.end();
	for (auto &concret : concretParams)
	{
		if (!concret.EvalDataType() || !iFormal->EvalDataType())
			return -1;
		if (iFormal == iFormalEnd)
			return -1;
		nDistance +=
			CalcTypeDistance(*concret.EvalDataType(), *iFormal->EvalDataType());
		++iFormal;
	}
	//Too few arguments — formal params remaining
	if (iFormal != iFormalEnd)
		return -1;
	return nDistance;
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
	if (IsPrimitiveType(srcKind) && IsPrimitiveType(tgtKind))
		return std::abs(srcKind - tgtKind);
	return -1;
}

void ExprResolveAccessor::FixupParamTypes(SnInvokeExpr &invoke,
	SnFunction::ParamList &formalParams)
{
	auto &concreteParams = invoke.Children();
	auto iConcreteEnd = concreteParams.end();
	auto iFormalEnd = formalParams.end();
	auto iFormal = formalParams.begin();
	for (auto iConcrete = concreteParams.begin();
		iConcrete != iConcreteEnd; ++iConcrete)
	{
		assert(iFormal != iFormalEnd);
		assert(static_cast<SyntaxNode &>(*iConcrete).IsExpression());
		auto &cParam = static_cast<SnExpression &>(*iConcrete);
		auto &fParam = *iFormal;
		TypeCastInfo castInfo(cParam.EvalDataType(), fParam.EvalDataType());
		FixupExprType(iConcrete, castInfo);
	}
}

bool ExprResolveAccessor::FixupExprType(NodeIterator &iSrcExpr,
	TypeCastInfo &castInfo)
{
	if (castInfo.Kind() == TCK_Same)
		return false;

	assert(static_cast<SyntaxNode &>(*iSrcExpr).IsExpression());
	auto &srcExpr = static_cast<SnExpression &>(*iSrcExpr);

	if (castInfo.Kind() != TCK_Auto && castInfo.Kind() != TCK_Box)
	{
		m_Env.Log(CLL_Error, srcExpr.Location(),
			"Incompatible type \"%s\".", srcExpr.ToString().c_str());
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
			return ResolveDataType(*dataField.Type(), outerType);
		}

		auto pReturnType = static_cast<SnFunction &>(sn).ReturnType();
		if (pReturnType)
		{
			if (!ResolveDataType(*pReturnType, outerType))
				return false;
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
