/*---
    StatementResolverDeclDefaults.cpp — 形参默认值契约族：前向引用、
    构造器 this 引用、常量适配与类型兼容。
    从 StatementResolverDecls.cpp 抽取（2026-11-02 尺寸守卫，零行为变化）。
---*/
#include "StatementResolver.h"
#include "ExprResolverCastFit.hpp"
#include "ModuleRegistry.h"

namespace nlang
{

//Phase 9c round 5: walk an expression subtree looking for any
//SnIdentifierExpr whose resolved Field() equals pTarget. Used by
//CheckDefaultForwardRefs to detect that a default expression
//references a later formal parameter (which would compile-resolve but
//crash codegen's FindLocal in caller context). C++/C# likewise forbid
//forward references in default expressions.
static bool ReferencesFormal(SnExpression& expr, SnField* pTarget)
{
	if (expr.Kind() == NK_IdentifierExpr) {
		auto& id = static_cast<SnIdentifierExpr&>(expr);
		if (id.Field() == pTarget)
			return true;
	}
	for (auto& child : expr.Children()) {
		//Children of an SnExpression are always SyntaxNode subclasses
		//(SnExpression or SnFieldExpr in our case). IsExpression is on
		//SyntaxNode, not Node — cast then check.
		auto& synChild = static_cast<SyntaxNode&>(child);
		if (!synChild.IsExpression())
			continue;
		auto& childExpr = static_cast<SnExpression&>(synChild);
		if (ReferencesFormal(childExpr, pTarget))
			return true;
	}
	return false;
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

//P3 review, refined by D-5 (0.13.13a): walk an expression subtree looking
//for a field-expression (identifier, member access, or call) whose
//binding names an instance member — the bare spelling is the implicit
//this.field binding, and a qualified spelling resolves to the same member
//node (NK_ClassField/NK_StructField/NK_Function-under-a-type distinguishes
//these from formals/locals/free functions; unqualified names cannot reach
//another type's members). The pre-D-5 predicate
//stopped at ANY field binding and falsely rejected static spellings.
//pOffenderField optionally receives the first instance-member binding so
//the diagnostic can name it. Same traversal contract as ReferencesFormal
//above; lambda bodies walk through the same children (D-5: an instance
//dependency penetrates the lambda body).
static bool FindInstanceMember(SnExpression& expr, SnField*& pOffenderField)
{
	//The field-expr family (identifier, member access, invoke) all expose
	//the resolved binding through SnFieldExpr::Field() — a call node
	//carries its callee there (SnInvokeExpr::Callee() reads the same
	//slot), while the member path deliberately leaves the inner name
	//unbound for method spellings (ExprResolverMember skips the function
	//binding so chained access survives IsDataExpr()). The delegate-invoke
	//shape binds Field() to a func-typed value declaration, which
	//IsInstanceMember correctly answers false for.
	auto kind = expr.Kind();
	if (kind == NK_IdentifierExpr || kind == NK_MemberExpr
		|| kind == NK_InvokeExpr) {
		auto* pField = static_cast<SnFieldExpr&>(expr).Field();
		if (pField && IsInstanceMember(*pField)) {
			if (!pOffenderField)
				pOffenderField = pField;
			return true;
		}
	}
	for (auto& child : expr.Children()) {
		//Children of an SnExpression are always SyntaxNode subclasses
		//(SnExpression or SnFieldExpr in our case). IsExpression is on
		//SyntaxNode, not Node — cast then check.
		auto& synChild = static_cast<SyntaxNode&>(child);
		if (!synChild.IsExpression())
			continue;
		if (FindInstanceMember(static_cast<SnExpression&>(synChild),
			pOffenderField))
			return true;
	}
	return false;
}

//The explicit-`this` half of the D-5 surface: an SnThisExpr anywhere in
//the subtree (used only when no member binding explains the dependency).
static bool ReferencesExplicitThis(SnExpression& expr)
{
	if (expr.Kind() == NK_ThisExpr)
		return true;
	for (auto& child : expr.Children()) {
		auto& synChild = static_cast<SyntaxNode&>(child);
		if (!synChild.IsExpression())
			continue;
		if (ReferencesExplicitThis(static_cast<SnExpression&>(synChild)))
			return true;
	}
	return false;
}

//The D-5 predicate, renamed/refined from ReferencesThis: the subtree
//depends on the enclosing instance only when it holds an instance-member
//binding or an explicit `this`; static members answer false. Consumed by
//the ctor-default guard below; the lambda pass-through reuses this shape
//once the expected-type channel (0.13.13a D-1) resolves lambda defaults.
static bool ReferencesInstance(SnExpression& expr)
{
	SnField* pOffender = nullptr;
	return FindInstanceMember(expr, pOffender)
		|| ReferencesExplicitThis(expr);
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
	if (!ReferencesInstance(*param.Value()))
		return;
	//Classify after the gate so the wording matches the spelling: an
	//instance member access (wording two) must win over a `this` that is
	//merely its receiver (e.g. `= this.m_seed`).
	SnField *pMember = nullptr;
	if (FindInstanceMember(*param.Value(), pMember))
	{
		NLANG_ASSERT(pMember);
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

//v0.13.9 phase 3 split (source-size guard): the default-value contract
//for one formal — resolution, the forward-ref check, the ctor
//instance-dependency check and type compat (the walkers above are this
//contract's arms).
void StatementResolveAccessor::ResolveFormalDefault(SnFunction &sn,
	std::vector<SnFormalParam *> &formals, size_t i)
{
	auto *param = formals[i];
	//0.13.1a Task 5: a type-parameter-typed formal rejects a
	//default value outright — the remaining checks presume a
	//concrete formal type and cannot say anything useful here.
	if (sn.IsGenericDeclaration() && RejectTypeParamDefault(sn, *param))
		return;
	if (!param->Value()->IsResolved())
		m_ExprResolver.Resolve(*param->Value(), sn, sn, ERF_None);

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
