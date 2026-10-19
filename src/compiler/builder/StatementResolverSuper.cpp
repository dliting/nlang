/*---
    StatementResolverSuper.cpp — super(...) 调用解析：实参检查与父类
    构造器 arity 区间判定（尾随默认形参可省略，required..total）。
    从 StatementResolverSwitchTry.cpp 抽取（2026-10-17 P3 缺陷批，
    super 调用点开始应用形参默认值）。
---*/
#include "StatementResolver.h"

namespace nlang
{

//Resolve the call arguments; named and out arguments are rejected —
//super(...) forwards positionally, so a named argument cannot bind and
//an out argument could never write back.
void StatementResolveAccessor::ResolveSuperCallArgs(SnSuperCallStmt &sn)
{
	for (auto* pArg : sn.Args()) {
		if (pArg->Kind() == NK_NamedArgExpr) {
			m_Env.Log(CLL_Error, pArg->Location(),
				"named arguments are not supported in super(...)");
			continue;
		}
		if (pArg->Kind() == NK_OutArgExpr) {
			m_Env.Log(CLL_Error, pArg->Location(),
				"out arguments are not supported in super(...)");
			continue;
		}
		pArg->Accept(*m_pVisitor);
	}
}

//Arity range of the parent constructor; false when the parent has none.
//Built-in Exception family ctor is (this, message) → 1 user arg; other
//built-ins are not subclassable. requiredArity counts formals without a
//default expression, totalArity all of them.
bool StatementResolveAccessor::FindParentCtorShape(SnClassDecl *pParent,
	size_t &requiredArity, size_t &totalArity)
{
	if (pParent->IsBuiltinClass()) {
		requiredArity = totalArity = 1;
		return true;
	}
	for (auto& field : pParent->Members()) {
		//0.9.10 D2.4: a same-name static is not the constructor — the
		//arity range must come from the instance ctor, regardless of
		//declaration order.
		if (field.Kind() == NK_Function
			&& field.Name() == pParent->Name()
			&& !field.ContainFlags(NF_Static)) {
			auto& parentCtor = static_cast<SnFunction&>(field);
			totalArity = parentCtor.Params().size();
			requiredArity = totalArity;
			for (auto& param : parentCtor.Params())
				if (param.Value())
					--requiredArity;
			return true;
		}
	}
	return false;
}

//super(args); verdict against the parent ctor's arity range. A parent
//without a ctor is a legal no-op for a bare super(); anything outside
//the range is an error — the exact-count form stays verbatim for ctors
//without defaults, the range form names the accepted span.
void StatementResolveAccessor::CheckSuperCtorArity(SnSuperCallStmt &sn,
	SnClassDecl *pParent)
{
	size_t requiredArity = 0;
	size_t totalArity = 0;
	if (!FindParentCtorShape(pParent, requiredArity, totalArity)) {
		if (!sn.Args().empty()) {
			m_Env.Log(CLL_Error, sn.Location(),
				"parent class \"%s\" has no constructor; super(...) "
				"cannot take arguments",
				pParent->Name().c_str());
		}
		return;  //bare super(); with no parent ctor is a legal no-op
	}
	if (sn.Args().size() >= requiredArity
		&& sn.Args().size() <= totalArity)
		return;
	if (requiredArity == totalArity) {
		m_Env.Log(CLL_Error, sn.Location(),
			"super(...) expects %zu argument(s), got %zu",
			requiredArity, sn.Args().size());
	} else {
		m_Env.Log(CLL_Error, sn.Location(),
			"super(...) expects between %zu and %zu argument(s), "
			"got %zu",
			requiredArity, totalArity, sn.Args().size());
	}
}

//Phase 9d-2: super(args); — forwards ctor args to the direct parent
//class constructor. Valid only inside a user constructor (a method of
//a class whose name equals the function name); omitted trailing formals
//fall to their defaults.
void StatementResolveAccessor::Access(SnSuperCallStmt &sn)
{
	if (sn.IsResolved())
		return;
	assert(m_pVisitor);
	sn.AddFlags(NF_Resolved);

	//1. Must be inside a constructor of a user class. 0.9.11 D2: this
	//also fires for super(...) inside a struct constructor — structs
	//have no inheritance, and the text names the class-only rule.
	if (!m_pCurrClass || !m_pCurrType
		|| m_pCurrType->Kind() != NK_Function
		|| m_pCurrType->Name() != m_pCurrClass->Name()) {
		m_Env.Log(CLL_Error, sn.Location(),
			"'super' is only valid inside a class constructor");
		return;
	}
	//2. Direct parent must exist (Object and extension-less classes
	//have no SuperClass).
	auto pParent = m_pCurrClass->SuperClass();
	if (!pParent) {
		m_Env.Log(CLL_Error, sn.Location(),
			"class \"%s\" has no parent class for super(...)",
			m_pCurrClass->Name().c_str());
		return;
	}
	ResolveSuperCallArgs(sn);
	CheckSuperCtorArity(sn, pParent);
}

} //namespace nlang
