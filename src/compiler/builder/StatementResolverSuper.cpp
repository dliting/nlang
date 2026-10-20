/*---
    StatementResolverSuper.cpp — super(...) 调用解析：实参检查与父类
    构造器重载选择（0.9.11 T6：绑定距离最近者胜出；内置父类保持
    固有 arity 判定）。
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

//0.9.11 T6 (D10): the parent ctor a super(...) call binds to. User
//parents run the overload match over the parent's same-name non-static
//members (a same-name static lives in the static namespace, 0.9.10
//D2.4); builtin parents keep the intrinsic Exception-ctor arity check
//and answer null — their ctor is a table entry, not an AST member. A
//parent without ctors is a legal no-op for a bare super(); anything
//else logs and answers null.
SnFunction *StatementResolveAccessor::SelectSuperCtor(SnSuperCallStmt &sn,
	SnClassDecl *pParent)
{
	std::vector<SnExpression*> actuals;
	for (auto *pArg : sn.Args())
		actuals.push_back(pArg);

	if (pParent->IsBuiltinClass()) {
		//Builtin Exception family ctor is (this, message) → 1 user arg;
		//other built-ins are not subclassable.
		if (actuals.size() != 1) {
			m_Env.Log(CLL_Error, sn.Location(),
				"super(...) expects %zu argument(s), got %zu",
				static_cast<size_t>(1), actuals.size());
		}
		return nullptr;  //table-path ctor (codegen reads CtorSlotFor)
	}

	std::vector<SnFunction*> candidates;
	for (auto& field : pParent->Members()) {
		if (field.Kind() == NK_Function
			&& field.Name() == pParent->Name()
			&& !field.ContainFlags(NF_Static)) {
			candidates.push_back(static_cast<SnFunction*>(&field));
		}
	}
	if (candidates.empty()) {
		if (!actuals.empty()) {
			m_Env.Log(CLL_Error, sn.Location(),
				"parent class \"%s\" has no constructor; super(...) "
				"cannot take arguments",
				pParent->Name().c_str());
		}
		return nullptr;  //bare super(); with no parent ctor is a legal no-op
	}

	bool bAmbiguous = false;
	auto *pCtor = m_ExprResolver.MatchCtorAgainst(actuals, candidates,
		bAmbiguous);
	if (pCtor)
		return pCtor;
	if (bAmbiguous) {
		m_Env.Log(CLL_Error, sn.Location(),
			"ambiguous constructor call to class \"%s\": multiple "
			"constructors match the argument list",
			pParent->Name().c_str());
	} else {
		m_Env.Log(CLL_Error, sn.Location(),
			"no constructor of class \"%s\" matches the argument list",
			pParent->Name().c_str());
	}
	return nullptr;
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
	sn.CtorFunction(SelectSuperCtor(sn, pParent));
}

} //namespace nlang
