/*---
    StatementResolverDeclDefaults.cpp — 形参默认值契约族：前向引用、
    构造器 this 引用、常量适配与类型兼容。
    从 StatementResolverDecls.cpp 抽取（2026-11-02 尺寸守卫，零行为变化）。
---*/
#include "StatementResolver.h"
#include "ExprResolverCastFit.hpp"

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

//P3 review: walk an expression subtree looking for a `this` reference —
//an explicit SnThisExpr, or a bare identifier that resolved to a class
//or struct field (the implicit this.field binding; NK_ClassField/
//NK_StructField distinguishes it from formals/locals, and bare names
//cannot reach another type's fields). Same traversal contract as
//ReferencesFormal above. 0.9.11 D2: struct constructor defaults join
//the rejection surface.
static bool ReferencesThis(SnExpression& expr)
{
	if (expr.Kind() == NK_ThisExpr)
		return true;
	if (expr.Kind() == NK_IdentifierExpr) {
		auto& id = static_cast<SnIdentifierExpr&>(expr);
		auto* pField = id.Field();
		if (pField && (pField->Kind() == NK_ClassField
			|| pField->Kind() == NK_StructField))
			return true;
	}
	for (auto& child : expr.Children()) {
		auto& synChild = static_cast<SyntaxNode&>(child);
		if (!synChild.IsExpression())
			continue;
		if (ReferencesThis(static_cast<SnExpression&>(synChild)))
			return true;
	}
	return false;
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

//P3 review: reject a ctor default that references `this` (explicitly, or
//implicitly through a bare field). Defaults are emitted and evaluated at
//the CALL SITE: a construction call stages them before the object exists
//(thisSlot is UINT16_MAX there — NK_ThisExpr would fall back to the
//caller's local 0), and a super() call binds the derived object whose
//fields are not initialized yet. Either way the default reads the wrong
//object or crashes, so the declaration is the rejection point. Method
//and free-function defaults keep their existing behavior.
void StatementResolveAccessor::CheckCtorDefaultThisRefs(SnFunction &sn,
	SnFormalParam &param)
{
	if (!IsConstructorFunction(sn))
		return;
	if (!ReferencesThis(*param.Value()))
		return;
	m_Env.Log(CLL_Error,
		param.Value()->Location(),
		"default value for parameter \"%s\" of constructor \"%s\" "
		"cannot reference 'this' (explicitly or through a bare "
		"field); constructor defaults evaluate at the call site, "
		"where no valid 'this' exists.",
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

} //namespace nlang
