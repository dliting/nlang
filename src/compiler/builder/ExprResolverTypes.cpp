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
#include "ModuleBuilderChecks.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

//ArrayTypeDepth / RejectInvalidTypeArg 位于 ExprResolverTypeArgs.cpp
//（2026-10-27 尺寸守卫触发的拆分）。

void ExprResolveAccessor::Access(SnArrayTypeExpr &arrTypeExpr)
{
	if (arrTypeExpr.IsResolved())
		return;

	auto *pElemType = arrTypeExpr.ElementType();
	assert(pElemType);
	pElemType->Accept(*m_pVisitor);
	if (!pElemType->IsResolved())
		return;
	//v0.12.2: the element is a type position too — a face-class elem
	//(String[]) is rejected before any token is interned over it.
	if (RejectPreludeFaceUse(m_Env, pElemType->Field(),
		pElemType->Location(), "used as a type"))
		return;

	//v0.12.3 dictionary model: T[] in a type position is a legal HANDLE
	//position — the token interns over the placeholder and the array
	//registration classifies the element as the type-parameter label
	//(one slot whatever the instantiation binds). Only array CREATION
	//over T stays rejected (the sized-declarator gate in
	//StatementResolverDecls: the element kind is unknowable until the
	//instantiation, the same family as new T[n]).

	//0.7.3 B: intern the array type token and bind the expression to
	//it — Field() and EvalDataType() become the token together
	//(ResolveFieldExprAs writes both channels). Declaration sites and
	//value sites then share one interned token per element type, and
	//the element masquerade is gone. Field/param/return types arrive
	//through ResolveDataTypes, and local/for/foreach type expressions
	//through ExprResolver::Resolve; sized declarators intern on the
	//declaration channel (RegisterLocalDeclarators).
	ResolveFieldExprAs(arrTypeExpr,
		m_Env.InternArrayTypeToken(pElemType->Field(), arrTypeExpr.Rank()));
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
	//Single-point import-gate policy (ModuleRegistry::ImportGateAllows):
	//stub-synthesized references (external owner) bypass the consumer
	//gate; source references must have the path in this TU's gate.
	if (!reg.ImportGateAllows(curModule, nsPath))
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
		ResolveFieldExprAs(idExpr, GetBuiltinClassDecl(
			m_Env.Registry(), name, idExpr.Location()));
		return true;
	}

	//Declared-generic machinery: a type parameter name inside its own
	//template binds to the interned placeholder.
	switch (BindTypeParamPlaceholderIfAny(idExpr))
	{
	case TypeParamBindResult::Bound:
		return true;
	case TypeParamBindResult::NotApplicable:
		break;
	}

	m_Env.Log(CLL_Error, "Cannot resolve the field: %s.",
		idExpr.Name().c_str());
	//D10: a stamped stdlib class/function lives behind an import; a
	//package hit is the specific answer, so the generic function/
	//module-path guesses stay quiet.
	if (!MaybeLogPackageHint(idExpr.Name()))
	{
		//Module import visibility (F20): the name may only exist as a
		//function outside this TU's bare pool — name the owning module.
		MaybeLogVisibilityHint(idExpr.Name(), idExpr.Location(), curModule);
		//spec §6.2 last line: a module path sharing the name turns a bare
		//mystery into a named fix (import + qualification).
		MaybeLogModuleHint(idExpr.Name());
	}
	return false;
}

//0.9.10: a bare identifier resolving to a class field carries the
//implicit this.field meaning; a static method has no receiver, so the
//binding is rejected here (the explicit `this` form is gated in
//Access(SnThisExpr&)). True = rejected with its diagnostic.
namespace
{

//0.9.10 D3.4: the only legal reference to an interface constant is the
//qualified member form `I.NAME`, where the receiver names the interface
//that owns the field. Bare names would make the constant look like a
//function-local value and are rejected before the implicit-this gate
//can misreport the reference.
bool IsQualifiedInterfaceConstantAccess(const SnIdentifierExpr &idExpr,
	const SnField &field)
{
	auto *pOwner = field.Parent();
	auto *pParent = idExpr.Parent();
	if (!pOwner || !pParent || pParent->Kind() != NK_MemberExpr)
		return false;
	auto &member = static_cast<SnMemberExpr &>(*pParent);
	if (member.Inner() != &idExpr)
		return false;
	auto *pOuter = member.Outer();
	if (!pOuter || (pOuter->Kind() != NK_IdentifierExpr
		&& pOuter->Kind() != NK_MemberExpr))
		return false;
	return static_cast<SnFieldExpr *>(pOuter)->Field() == pOwner;
}

} //namespace

SnField *ExprResolveAccessor::FindBareValueField(SnIdentifierExpr &idExpr,
	uint32_t curModule)
{
	//This is the BARE-POOL owner filter (IsBareVisible), not the import
	//gate — the gate policy lives in ModuleRegistry::ImportGateAllows.
	std::function<bool(SnField &)> bareFuncFilter =
		[this, curModule](SnField &field) -> bool
	{
		//No NK_Function pre-check: FindFieldInAncestor only invokes the
		//filter on function candidates in bare-pool scopes.
		return IsBareVisible(static_cast<SnFunction &>(field), curModule);
	};
	SnField *pField = FindFieldInAncestor(idExpr.Name(), *m_pContext,
		*m_pAccessor, Flags(), bareFuncFilter);
	if (!pField && ContainFlags(ERF_IgnoreUsings))
	{
		assert(idExpr.Usings());
		pField = FindFieldInUsings(idExpr.Name(), *idExpr.Usings(),
			*m_pAccessor);
	}
	return pField;
}

bool ExprResolveAccessor::RejectBareInterfaceConstant(
	SnIdentifierExpr &idExpr, SnField &field)
{
	if (!IsInterfaceConstantField(field)
		|| IsQualifiedInterfaceConstantAccess(idExpr, field))
		return false;
	auto *pOwner = static_cast<SnField *>(field.Parent());
	m_Env.Log(CLL_Error, idExpr.Location(),
		"\"%s\" is an interface constant: reference it as \"%s.%s\".",
		idExpr.Name().c_str(),
		pOwner ? pOwner->Name().c_str() : "",
		idExpr.Name().c_str());
	return true;
}

bool ExprResolveAccessor::RejectImplicitThisInStatic(SnIdentifierExpr &idExpr,
	SnField *pField)
{
	if (!pField || pField->Kind() != NK_ClassField)
		return false;
	auto *pEnclosing = EnclosingFunction();
	if (!pEnclosing || !pEnclosing->ContainFlags(NF_Static))
		return false;
	m_Env.Log(CLL_Error, idExpr.Location(),
		"this is not available in a static method.");
	return true;
}

void ExprResolveAccessor::Access(SnIdentifierExpr &idExpr)
{
	if (idExpr.IsResolved())
	{
		if (!idExpr.PostResolveCheck(m_Env))
			idExpr.RemoveFlags(NF_Resolved);
		return;
	}

	//Module import visibility (D1/D7): the value-position bare pool spans
	//the current TU's directory too — foreign root/namespace function
	//candidates are skipped (ownerless symbols stay visible). curModule is
	//computed once and shared by the lookup filter and the hint below.
	const uint32_t curModule = m_Env.Registry().OwnerOfContext(*m_pContext);
	SnField *pField = FindBareValueField(idExpr, curModule);
	if (!pField)
	{
		TryResolveBuiltinNameFallback(idExpr, curModule);
		return;
	}

	//0.9.10 D3.4: interface constants are qualified-only. This runs
	//before the implicit-this gate, which would otherwise report the
	//bare constant read as a `this` use inside a static method.
	if (RejectBareInterfaceConstant(idExpr, *pField))
		return;

	if (RejectImplicitThisInStatic(idExpr, pField))
		return;

	if (!ApplyReceiverNamespaceFilter(idExpr, pField))
		return;

	ResolveFieldExprAs(idExpr, pField);
	BindArrayTypeToken(idExpr);
}

//FindPreludeClassDecl / EnsureImplicitObjectSuperclass 位于
//ExprResolverBuiltinClasses.cpp（2026-10-27 尺寸守卫触发的拆分）。

void ExprResolveAccessor::Access(SnClassDecl &sn)
{
	//Builtin-decls source of truth: a baseless class inherits the
	//prelude-declared Object (both resolver passes call this — the
	//statement pass needs the chain live for virtual propagation).
	EnsureImplicitObjectSuperclass(sn, m_Env.Registry());
	//Resolve super class reference.
	if (sn.SuperName())
	{
		sn.SuperName()->Accept(*m_pVisitor);
		if (sn.SuperName()->IsResolved())
		{
			auto pSuperField = sn.SuperName()->Field();
			if (pSuperField && pSuperField->Kind() == NK_ClassDecl
				&& RejectPreludeFaceUse(m_Env, pSuperField,
					sn.SuperName()->Location(), "extended"))
			{
				//v0.12.2: face bases are rejected — the class stays
				//baseless (faces own no instances or dispatch shapes
				//to inherit).
			}
			else if (pSuperField && pSuperField->Kind() == NK_ClassDecl)
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
