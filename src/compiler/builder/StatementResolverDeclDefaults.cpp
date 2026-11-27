/*---
    StatementResolverDeclDefaults.cpp — the formal-parameter default
    value contract family: the expected-type channel (D-1), forward
    references, ctor instance-dependency rejection, constant fit and
    type compat.
    Extracted from StatementResolverDecls.cpp (2026-11-02 size guard,
    zero behavior change).
---*/
#include "StatementResolver.h"
#include "ExprResolverCastFit.hpp"
#include "ModuleRegistry.h"

namespace nlang
{

//D-1 (0.13.13a): the traversal contract shared by the default-expression
//walkers — visit every node of the subtree. Children of a parse node are
//always SyntaxNode subclasses (the cast contract the pre-D-1 walkers
//documented); unlike the pre-D-1 IsExpression-only filter the walk also
//descends through non-expression children, because a lambda's block body
//holds its references inside statement children (D-1: defaults may carry
//lambdas; D-5: an instance dependency penetrates the lambda body;
//Phase 9c: a later-formal reference would compile-resolve but crash
//codegen's FindLocal in caller context).
template <typename Visit>
static void WalkDefaultRefs(Node& node, const Visit& visit)
{
	visit(node);
	//Children() yields Node& elements (NodeIteratorBase::value_type).
	for (auto& child : node.Children())
		WalkDefaultRefs(child, visit);
}

//Phase 9c round 5: walk a default subtree looking for any
//SnIdentifierExpr whose resolved Field() equals pTarget. Used by
//CheckDefaultForwardRefs to detect that a default expression
//references a later formal parameter (which would compile-resolve but
//crash codegen's FindLocal in caller context). C++/C# likewise forbid
//forward references in default expressions. Inside a lambda body the
//lambda's own parameters are skipped for free: their identifiers bind
//to the lambda's parameter descriptors (separate SnField objects), so
//the identity test against the outer formal fails (D-1).
static bool ReferencesFormal(SnExpression& expr, SnField* pTarget)
{
	bool found = false;
	WalkDefaultRefs(expr, [&found, pTarget](Node& node)
	{
		if (!found && node.Kind() == NK_IdentifierExpr
			&& static_cast<SnIdentifierExpr&>(node).Field() == pTarget)
			found = true;
	});
	return found;
}

//D-5 (0.13.13a): an instance member is a NON-STATIC field of a class or
//struct, or a NON-STATIC method declared in one — every other binding
//(static member, formal, local, free function, type name) is reachable
//without an instance. 0.9.11 D2 lineage: struct members joined the
//rejection surface alongside class members.
static bool IsInstanceMember(const SnField& field)
{
	auto kind = field.Kind();
	if (kind != NK_ClassField && kind != NK_StructField
		&& kind != NK_Function)
		return false;
	if (field.ContainFlags(NF_Static))
		return false;
	if (kind == NK_Function) {
		auto* pParent = field.Parent();
		return pParent && (pParent->Kind() == NK_ClassDecl
			|| pParent->Kind() == NK_StructDecl);
	}
	return true;
}

//Phase 9c round 5: detect forward references in default expressions.
//Default[i] may only reference formals[0..i-1]. A reference to
//formal[j] (j >= i) would compile-resolve but crash codegen (FindLocal
//throws in caller context). C++/C# also forbid this. Walk default[i]'s
//subtree for any IdentifierExpr whose Field() is formal[j] (j >= i).
void StatementResolveAccessor::CheckDefaultForwardRefs(
	std::vector<SnFormalParam*> &formals, size_t i)
{
	auto *param = formals[i];
	for (size_t j = i; j < formals.size(); ++j) {
		auto *later_formal = formals[j];
		if (ReferencesFormal(*param->Value(), later_formal)) {
			m_Env.Log(CLL_Error,
				param->Value()->Location(),
				"default value for parameter \"%s\" references "
				"later parameter \"%s\"; defaults may only "
				"reference earlier parameters.",
				param->Name().c_str(),
				later_formal->Name().c_str());
			break;  //one error per default[i]
		}
	}
}

//D-5 (0.13.13a): classify one ctor default subtree on the instance-
//dependency surface. One walk classifies the whole surface (Task 1
//review M1: the gate walked first, then the wording split re-walked to
//recover the offender). The field-expr family (identifier, member
//access, invoke) all expose the resolved binding through
//SnFieldExpr::Field() — a call node carries its callee there
//(SnInvokeExpr::Callee() reads the same slot), while the member path
//deliberately leaves the inner name unbound for method spellings
//(ExprResolverMember skips the function binding so chained access
//survives IsDataExpr()). The delegate-invoke shape binds Field() to a
//func-typed value declaration, which IsInstanceMember correctly
//answers false for. D-1 (0.13.13a): the walk reaches references
//written inside lambda bodies — expression or block — so a default
//lambda whose body captures the instance is rejected instead of
//silently capturing the wrong receiver at the call site.
static void ClassifyCtorDefaultRefs(SnExpression &value,
	SnField *&pMember, bool &bExplicitThis)
{
	WalkDefaultRefs(value, [&pMember, &bExplicitThis](Node &node)
	{
		auto kind = node.Kind();
		if (kind == NK_ThisExpr)
			bExplicitThis = true;
		else if (!pMember
			&& (kind == NK_IdentifierExpr || kind == NK_MemberExpr
				|| kind == NK_InvokeExpr))
		{
			auto *pField = static_cast<SnFieldExpr &>(node).Field();
			if (pField && IsInstanceMember(*pField))
				pMember = pField;
		}
	});
}

//P3 review + D-5 (0.13.13a): reject a ctor default that depends on the
//instance — an explicit `this`, or access bound to a non-static
//class/struct member. Defaults are emitted and evaluated at the CALL
//SITE: a construction call stages them before the object exists (thisSlot
//is UINT16_MAX there — NK_ThisExpr would fall back to the caller's local
//0), and a super() call binds the derived object whose fields are not
//initialized yet. Either way the default reads the wrong object or
//crashes, so the declaration is the rejection point. D-5 narrowed the
//surface from "any field binding" to instance bindings: static members
//resolve without an instance and are allowed (the C++/Kotlin
//ctor-default rule). The diagnostic follows the actual spelling — a
//member access names <declaring type>.<member>, a bare explicit `this`
//says 'this'; the pre-D-5 text claimed a 'this' reference even for
//spellings containing none. Method and free-function defaults keep their
//existing behavior (intentional asymmetry, spec D-5).
void StatementResolveAccessor::CheckCtorDefaultThisRefs(SnFunction &sn,
	SnFormalParam &param)
{
	if (!IsConstructorFunction(sn))
		return;
	//The wording order is a property of the visitor, not of the
	//classification walk: an instance member access (wording two) must
	//win over a `this` that is merely its receiver (e.g. `=
	//this.m_seed`).
	SnField *pMember = nullptr;
	bool bExplicitThis = false;
	ClassifyCtorDefaultRefs(*param.Value(), pMember, bExplicitThis);
	if (pMember)
	{
		//The owner of a class/struct member is always a named
		//declaration (SnClassDecl/SnStructDecl are themselves SnFields)
		//— same upcast-to-named-base as RejectBareInterfaceConstant.
		auto *pOwner = static_cast<SnField *>(pMember->Parent());
		NLANG_ASSERT(pOwner);
		m_Env.Log(CLL_Error,
			param.Value()->Location(),
			"default value for parameter \"%s\" of constructor \"%s\" "
			"cannot access instance member \"%s.%s\"; constructor "
			"defaults evaluate at the call site, where no valid 'this' "
			"exists.",
			param.Name().c_str(),
			sn.Name().c_str(),
			pOwner->Name().c_str(),
			pMember->Name().c_str());
		return;
	}
	if (!bExplicitThis)
		return;
	m_Env.Log(CLL_Error,
		param.Value()->Location(),
		"default value for parameter \"%s\" of constructor \"%s\" "
		"cannot reference 'this'; constructor defaults evaluate at "
		"the call site, where no valid 'this' exists.",
		param.Name().c_str(),
		sn.Name().c_str());
}

//0.7.5: an explicit-only default must clear the same constant-fit bar
//as an assignment — `float x = 3.14` is an error exactly where
//`float f = 3.14;` is (the double literal is not float-representable),
//while `float x = 0.5` passes and the fill-side EmitScalarSlotCast
//(VmBackend::EmitBinding) normalizes the staged slot to the formal's
//kind. Without the gate the double default's low bytes reached the
//callee as the float's bits.
void StatementResolveAccessor::CheckDefaultConstantFit(
	SnFormalParam *param, SnField *pFormalType)
{
	int ri = ScalarPrimIndexOf(pFormalType->Kind());
	ConstantFitOutcome fit;
	if (ri >= 0)
		fit = TryConstantFit(*param->Value(), kScalarPrims[ri]);
	if (fit.verdict == CF_Fits)
		return;
	if (fit.verdict == CF_OutOfRange)
		m_Env.Log(CLL_Error,
			param->Value()->Location(),
			"default value for parameter \"%s\": "
			"constant %s out of range for '%s'",
			param->Name().c_str(), fit.constantText,
			kScalarPrims[ri].name);
	else
		m_Env.Log(CLL_Error,
			param->Value()->Location(),
			"default value for parameter \"%s\" "
			"requires an explicit conversion to \"%s\".",
			param->Name().c_str(),
			pFormalType->ToString().c_str());
}

//Reporting an incompatible default at declaration gives clearer errors
//than at every call site that uses the default.
void StatementResolveAccessor::CheckDefaultTypeCompat(SnFormalParam *param)
{
	auto *pDefaultType = param->Value()->EvalDataType();
	auto *pFormalType = param->EvalDataType();
	if (!pDefaultType || !pFormalType)
		return;
	//string is a value type (0.13.4): the null literal has no string
	//value; GetCastInfo would report the int→string rendering TCK_Auto.
	if (param->Value()->ContainFlags(NF_NullLiteral)
		&& pFormalType->Kind() == NK_String)
	{
		m_Env.Log(CLL_Error, param->Value()->Location(),
			"default value for parameter \"%s\" cannot be null: string "
			"is a value type; use \"\" or string.EMPTY.",
			param->Name().c_str());
		return;
	}
	auto ci = GetCastInfo(pDefaultType, pFormalType);
	if (ci.Kind() == TCK_None) {
		m_Env.Log(CLL_Error,
			param->Value()->Location(),
			"default value for parameter \"%s\" has "
			"incompatible type \"%s\"; expected \"%s\".",
			param->Name().c_str(),
			pDefaultType->ToString().c_str(),
			pFormalType->ToString().c_str());
		return;
	}
	if (ci.Kind() == TCK_Explicit)
		CheckDefaultConstantFit(param, pFormalType);
}

//0.13.1a Task 5: a type-parameter-typed formal on a generic
//declaration rejects a default value (same line as the class-template
//T-initializer rule: without a constraint no meaningful T initial
//value exists — every call site already infers T from its actuals).
//The formal type must be the bare placeholder; a concrete type keeps
//its ordinary default contract. 0.13.1b: the message is the canonical
//text shared with the class-member path (GenericDeclContracts.cpp).
bool StatementResolveAccessor::RejectTypeParamDefault(SnFunction &fn,
	SnFormalParam &param)
{
	auto *pFormalType = param.EvalDataType();
	if (!pFormalType
		|| !m_Env.Registry().DeclaredGenerics().IsPlaceholder(pFormalType))
		return false;
	m_Env.Log(CLL_Error, param.Location(),
		"default value for parameter \"%s\": the type parameter \"%s\" "
		"has no concrete kind, so no default value exists.",
		param.Name().c_str(), pFormalType->Name().c_str());
	return true;
}

//v0.13.13a D-1 (web-framework lang gaps Task 2): a func-typed formal
//declares the expected type of its default expression. The deferred
//forms (untyped lambda, bare init list) need that expectation in place
//BEFORE resolution: the lambda arm's target gate consumes InferredTarget
//during the resolve and flags itself resolved on every error exit, so a
//late push is never seen (same pre-push discipline as the call-argument
//channel's ResolveDeferredArgs, ExprResolverInvoke.cpp). The by-name
//function-reference arms need no push — an unbound identifier resolves
//cleanly and the ModuleBuilder sweep stays silent — and bind after the
//resolve (see ResolveFormalDefault). Returns the formal's func type
//when its declared type is one, nullptr otherwise.
static SnField *PushDefaultInferredTarget(SnFormalParam &param)
{
	auto *pFormalType = param.EvalDataType();
	if (!pFormalType || !IsFuncTypeDecl(pFormalType))
		return nullptr;
	if (auto *pLambda = UntypedLambdaOf(*param.Value()))
		pLambda->InferredTarget(&param);
	else if (auto *pInitList = UntypedInitListOf(*param.Value()))
		pInitList->InferredTarget(&param);
	return pFormalType;
}

//v0.13.9 phase 3 split (source-size guard): the default-value contract
//for one formal — the expected-type channel, resolution, the forward-ref
//check, the ctor instance-dependency check and type compat (the walkers
//above are this contract's arms).
void StatementResolveAccessor::ResolveFormalDefault(SnFunction &sn,
	std::vector<SnFormalParam *> &formals, size_t i)
{
	auto *param = formals[i];
	//0.13.1a Task 5: a type-parameter-typed formal rejects a
	//default value outright — the remaining checks presume a
	//concrete formal type and cannot say anything useful here.
	if (sn.IsGenericDeclaration() && RejectTypeParamDefault(sn, *param))
		return;
	//D-1: the deferred forms take the formal as expected type first.
	auto *pExpectedFuncType = PushDefaultInferredTarget(*param);
	if (!param->Value()->IsResolved())
		m_ExprResolver.Resolve(*param->Value(), sn, sn, ERF_None);
	//D-1: a func-typed formal binds a by-name function reference (bare
	//name or qualified member tail) against its declared type after the
	//resolve — the same statement-channel helper the assignment and
	//return paths use (TryBindStatementFuncRef). The strict pending
	//predicates answer true only for spellings that still lack a
	//func-typed binding, and each binder logs its own mismatch text; a
	//successful bind lands the formal's declared type, so
	//CheckDefaultTypeCompat below sees the identity cast.
	if (pExpectedFuncType)
		TryBindStatementFuncRef(*param->Value(), pExpectedFuncType);

	//Phase 9c round 5: default[i] may only reference formals[0..i-1].
	if (param->Value()->IsResolved())
		CheckDefaultForwardRefs(formals, i);

	//P3 review: a ctor default cannot depend on the instance
	//(D-5: static members are fine) — imported stubs skip like
	//CheckDefaultTypeCompat (the producer unit already validated its
	//own defaults).
	if (param->Value()->IsResolved() && !sn.IsImported())
		CheckCtorDefaultThisRefs(sn, *param);

	//Verify default's type is compatible with the formal's
	//declared type. Literals may already be IsResolved() from
	//parse time, so the check runs regardless of the resolve
	//above. Imported stubs are skipped: the stub formal's type
	//is a placeholder (int32) since CompiledFunction doesn't
	//carry per-formal type info; only the default expression
	//preserves the original type. R5-4 already skips call-site
	//type checks for imported callees; this declaration-time
	//check would falsely reject valid cross-module defaults
	//like string/null.
	if (param->Value()->IsResolved() && !sn.IsImported())
		CheckDefaultTypeCompat(param);
}

} //namespace nlang
