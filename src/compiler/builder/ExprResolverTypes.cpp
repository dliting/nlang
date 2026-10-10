/*--- ExprResolverTypes.cpp — 内建/泛型类型机器与类型位节点解析 ---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnArrayTypeToken.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include "ModuleRegistry.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

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
//Per-argument reject gate of the generic type-argument loop below:
//void and out are func-only features (void only in the first / return
//slot, out only on parameter slots), and a jagged argument has no VM
//layout (the same gate as declaration sites, spec §5.5). True =
//rejected with the named diagnostic.
static bool RejectInvalidTypeArg(BuildEnvironment &env, SnFieldExpr *pTA,
	const std::string &baseName, bool bFirstArg)
{
	//Phase 13: void and out are func-only type-argument features.
	//void may only occupy func's first (return) type slot; out may
	//only mark func parameter slots (any position after the first).
	bool isVoidArg = pTA->Field()->Kind() == NK_Void;
	bool isOutArg = pTA->ContainFlags(NF_Out);
	if (isVoidArg && !(baseName == kBuiltinFuncTypeName && bFirstArg))
	{
		env.Log(CLL_Error, pTA->Location(),
			"void is only allowed as the return slot of func<...>.");
		return true;
	}
	if (isOutArg && !(baseName == kBuiltinFuncTypeName && !bFirstArg))
	{
		env.Log(CLL_Error, pTA->Location(),
			"out is only allowed on func<...> parameters.");
		return true;
	}
	//Array redesign B: a jagged type argument has no VM layout — the
	//same gate as declaration sites (spec §5.5), enforced here because
	//generic type-arg position is a distinct declaration form.
	if (ArrayTypeDepth(pTA) >= 2)
	{
		env.Log(CLL_Error, pTA->Location(),
			"jagged arrays (T[][]) are not supported.");
		return true;
	}
	return false;
}

//Type-argument collection of Access(SnGenericTypeExpr&): resolves each
//argument in the caller's scope and gathers the canonical fields plus
//the parallel out flags. False = a diagnostic is logged (unresolvable
//argument, argument without a field, or the reject gate above).
bool ExprResolveAccessor::TryResolveGenericTypeArgs(
	SnGenericTypeExpr &genType, const std::string &baseName,
	std::vector<SnField*> &typeArgs, std::vector<uint8> &outFlags)
{
	for (auto *pTA : genType.TypeArgs())
	{
		if (!pTA) continue;
		pTA->Accept(*m_pVisitor);
		if (!pTA->IsResolved())
		{
			m_Env.Log(CLL_Error, pTA->Location(),
				"Cannot resolve type argument %s.",
				pTA->ToString().c_str());
			return false;
		}
		auto *pField = pTA->Field();
		if (!pField)
		{
			m_Env.Log(CLL_Error, pTA->Location(),
				"Type argument %s has no resolved field.",
				pTA->ToString().c_str());
			return false;
		}
		if (RejectInvalidTypeArg(m_Env, pTA, baseName, typeArgs.empty()))
			return false;
		//0.7.3 B: the resolved field above IS the type identity — an
		//array-typed argument carries the interned SnArrayTypeToken
		//(same intern channel as declarations), so the key, the display
		//name and every consumer read array-ness off the token itself.
		typeArgs.push_back(pField);
		outFlags.push_back(pTA->ContainFlags(NF_Out) ? 1 : 0);
	}
	return true;
}

void ExprResolveAccessor::Access(SnGenericTypeExpr &genType)
{
	if (genType.IsResolved())
		return;

	auto *pBase = genType.Base();
	assert(pBase);
	std::string baseName = pBase->ToString();
	if (!IsBuiltinGenericTypeName(baseName))
	{
		m_Env.Log(CLL_Error, genType.Location(),
			"\"%s\" is not a built-in generic type.", baseName.c_str());
		return;
	}

	std::vector<SnField*> typeArgs;
	std::vector<uint8> outFlags;
	if (!TryResolveGenericTypeArgs(genType, baseName, typeArgs, outFlags))
		return;

	//Phase 13 Step 2: Dict keyed by a func type — DictKeysEqual is
	//identity for func records (no interning), so two references to the
	//same function would store as two entries. Reject at the single
	//instantiation point; both declaration types and new-expression
	//types flow through here.
	if (baseName == kBuiltinDictTypeName && !typeArgs.empty()
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

//Phase 4b: a qualified type reference "ns.Type" (or "a.b.Type") in a type
//position. Mirrors the module-qualified CALL resolution: the namespace must
//be imported, then the type declaration is looked up inside that compiled-in
//unit and bound. An external .ncu exposes no source-level types in v1.
void ExprResolveAccessor::Access(SnQualifiedTypeExpr &qtype)
{
	if (qtype.IsResolved())
		return;

	const auto &segs = qtype.Segments();
	if (qtype.IsMalformed() || segs.size() < 2)
	{
		//A single-segment type uses SnNameExpr; a one-segment qualified
		//node, or a chain that is not identifiers (`a.b() v;`), is refused.
		m_Env.Log(CLL_Error, qtype.Location(),
			"Malformed qualified type reference.");
		return;
	}

	const std::string nsPath = qtype.NamespacePath();
	auto &reg = m_Env.Registry();
	const uint32_t curModule = reg.OwnerOfContext(qtype);
	//The import gate protects references THIS TU's source wrote. A
	//reference synthesized from an imported module's signature (its
	//nearest owner is an EXTERNAL registry entry — source nodes are
	//never owned by one) was not authored by the consumer: it names a
	//type its own module legitimately owns, so it binds directly
	//through the registry, no consumer-side import required.
	if (!reg.IsExternal(curModule)
		&& !reg.IsModuleImported(curModule, nsPath))
	{
		RejectUnimportedQualifiedType(qtype, nsPath);
		return;
	}

	SnField *pType = reg.FindModuleType(nsPath, qtype.TypeName());
	if (pType == nullptr)
	{
		m_Env.Log(CLL_Error, qtype.Location(),
			"Type '%s' is not a member of package '%s'.",
			qtype.TypeName().c_str(), nsPath.c_str());
		return;
	}

	ResolveFieldExprAs(qtype, pType);
}

//Phase 4b: the package named by a qualified type is not imported into
//the current TU — name the fix (mirrors RejectUnimportedModuleCall).
void ExprResolveAccessor::RejectUnimportedQualifiedType(
	SnQualifiedTypeExpr &qtype, const std::string &nsPath)
{
	//Single-segment: "Package" for a library package, "Module" for an
	//external .ncu; a dotted (nested) path is always a module.
	const char *pKind = "Module";
	if (nsPath.find('.') == std::string::npos
		&& m_Env.IsLibraryPackage(nsPath))
		pKind = "Package";
	m_Env.Log(CLL_Error, qtype.Location(),
		"%s '%s' is not imported. Add 'import %s;' at the top of this "
		"file before using type '%s'.",
		pKind, nsPath.c_str(), nsPath.c_str(),
		qtype.TypeName().c_str());
}

//The not-found fallback of Access(SnIdentifierExpr&): a built-in class
//name (ByteStream, FileStream, Object, the Exception family) resolves
//to its singleton decl; anything else gets the generic not-resolved
//error plus the module visibility / module-path hints. True = resolved
//as a built-in; false = the diagnostics are logged.
bool ExprResolveAccessor::TryResolveBuiltinNameFallback(
	SnIdentifierExpr &idExpr, uint32_t curModule)
{
	//Builtin class names: ByteStream, FileStream, Object (Phase 8e-1).
	//Phase 9d: Exception hierarchy.
	//Synthesize a singleton SnClassDecl when the name is not found.
	const auto& name = idExpr.Name();
	if (IsBuiltinClassName(name))
	{
		ResolveFieldExprAs(idExpr, GetBuiltinClassDecl(name, idExpr.Location()));
		return true;
	}

	m_Env.Log(CLL_Error, "Cannot resolve the field: %s.",
		idExpr.Name().c_str());
	//Module import visibility (F20): the name may only exist as a
	//function outside this TU's bare pool — name the owning module.
	MaybeLogVisibilityHint(idExpr.Name(), idExpr.Location(), curModule);
	//spec §6.2 last line: a module path sharing the name turns a bare
	//mystery into a named fix (import + qualification).
	MaybeLogModuleHint(idExpr.Name());
	return false;
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
		TryResolveBuiltinNameFallback(idExpr, curModule);
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
