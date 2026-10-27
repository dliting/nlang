/*---
    StatementResolverDeclEnum.cpp — 枚举声明解析：成员值赋值、枚举方法的声明侧拒绝与访问。
    从 StatementResolverDecls.cpp 抽取（2026-10-13 源尺寸守卫拆分，零行为变化）。
---*/
#include "StatementResolver.h"
#include "ExprResolverCastFit.hpp"

namespace nlang
{

//D4/D5 declaration-side rejections for enum methods: toString is
//reserved for the built-in conversion, bodyless methods are silently
//skipped everywhere else, and defaults/out params would need call
//paths enum calls don't have (Callee() stays null).
void StatementResolveAccessor::RejectIllegalEnumMethod(SnFunction &method)
{
	//D5: toString on enums is the built-in OP_Enum_to_str
	//dispatch (Phase 8e-9b); a user method of that name would be
	//silently shadowed by it, so reject at declaration.
	if (method.Name() == "toString")
		m_Env.Log(CLL_Error, method.Location(),
			"enum method cannot be named \"toString\"; the name is "
			"reserved for the built-in conversion.");
	//D4: bodyless methods are rejected here — Access(SnFunction)
	//silently returns on !Body(), and RegisterFunctions skips
	//them too, so a declaration-side check is the only site that
	//reports.
	if (method.ContainFlags(NF_Abstract) || !method.Body())
		m_Env.Log(CLL_Error, method.Location(),
			"enum method \"%s\" must have a body.",
			method.Name().c_str());
	for (auto &param : method.Params())
	{
		//D4: default values need the callee-bound call path
		//(default fill + walker default-depth); enum method calls
		//keep Callee() null, so a default would silently read the
		//adjacent frame slot as garbage (plan 12b r3 M1).
		if (param.Value())
			m_Env.Log(CLL_Error, param.Location(),
				"enum method \"%s\" cannot have default parameter "
				"values.", method.Name().c_str());
		//D4: out parameters need the writeback path of
		//OP_CallMethodDirect's bound form; same Callee-null
		//limitation as above.
		if (param.ContainFlags(NF_Out))
			m_Env.Log(CLL_Error, param.Location(),
				"enum method \"%s\" cannot have out parameters.",
				method.Name().c_str());
	}
}

void StatementResolveAccessor::Access(SnEnumDecl &sn)
{
	assert(m_pVisitor);
	//Unconditional: the flag is NOT a reliable "values assigned" signal
	//here — ResolveDataTypes flags enum decls early without assigning
	//values (Step 1 finding), and the pre-pass already ran, so this is
	//an idempotent re-assignment.
	AssignEnumMemberValues(sn);
	/*
	Method bodies resolve AFTER the decl is flagged NF_Resolved —
	deliberately diverging from the class path (bodies first, flag
	last). The D8 duplicate-label defense gate in switch statements
	reads member values only from a resolved enum decl; resolving
	method bodies first would make `case Color.A, Color.A:` inside a
	method skip value extraction (plan 12b r4 MINOR-1).
	*/
	for (auto &method : sn.Methods())
	{
		//0.10.14: the declaration-side rejections target user-written
		//declarations — the stub's bodyless method stubs passed the
		//same contract at the producer. The methods are still walked
		//so their synthesized formals resolve (same as class stubs).
		if (!sn.IsImported())
			RejectIllegalEnumMethod(method);
		method.Accept(*m_pVisitor);
	}
}

//Explicit-value arm of AssignEnumMemberValues: resolve the value
//expression in the enum decl's own scope (the generic expression
//visitor asserts a current type context, and both callers run outside
//any function; since the 0.7.5 sign retirement `A = -1` is an OP_Neg
//tree that needs a real resolve — same shape as formal defaults), fold
//it via the shared constant extractor, and apply the non-negative
//invariant. Returns true when the member carried a value expression
//(assigned or rejected) — false leaves the caller nothing to do.
bool StatementResolveAccessor::TryAssignExplicitEnumValue(
	SnEnumMember &member, SnEnumDecl &sn, int32_t &nextValue)
{
	if (!member.ValueExpr()->IsResolved())
		m_ExprResolver.Resolve(*member.ValueExpr(), sn, sn, ERF_None);
	auto* pValueType = member.ValueExpr()->IsResolved()
		? member.ValueExpr()->EvalDataType() : nullptr;
	if (!pValueType || pValueType->Kind() != NK_Int32)
		return true;  //diagnosed by its own resolve; not implicitly bumped
	//0.7.5 sign retirement: `A = -1` parses as OP_Neg over an int
	//literal. The shared constant fold covers both the plain and the
	//negated shape (negated ulong folds to long, so the NK_Int32 gate
	//still rejects it).
	ConstLiteralValue v;
	if (!TryGetConstantLiteral(member.ValueExpr(), v) || v.kind != NK_Int32)
		return true;  //non-constant value: not implicitly bumped
	//The runtime enum name table is indexed by the member value
	//(RegisterEnums), so a negative value has no slot — reject at
	//declaration instead of letting the backend's table fill run away.
	if (v.i < 0)
	{
		m_Env.Log(CLL_Error, member.Location(),
			"enum member \"%s\" has negative value %lld; enum member "
			"values must be non-negative.",
			member.Name().c_str(), (long long)v.i);
		return true;
	}
	nextValue = static_cast<int32_t>(v.i);
	member.SetValue(nextValue);
	nextValue++;
	return true;
}

//Assign sequential/explicit values to members and flag the decl
//resolved. Shared by the document-order Access and the pre-pass
//(PreAssignEnumMemberValues), which must NOT resolve method bodies —
//that would double-resolve them once Access runs.
void StatementResolveAccessor::AssignEnumMemberValues(SnEnumDecl &sn)
{
	assert(m_pVisitor);
	//0.10.14: an imported enum stub already carries the producer's
	//member values (wire table indices — sparse explicit-value gaps
	//included); re-assigning would collapse them to sequential 0..N.
	//The declaration contract ran at the producer, so only the
	//resolved flag is missing (the switch D8 defense gate reads
	//member values from resolved enum decls).
	if (sn.IsImported())
	{
		sn.AddFlags(NF_Resolved);
		return;
	}
	int32_t nextValue = 0;
	for (auto &member : sn.Members())
	{
		if (member.ValueExpr())
			TryAssignExplicitEnumValue(member, sn, nextValue);
		else
		{
			member.SetValue(nextValue);
			nextValue++;
		}
	}
	sn.AddFlags(NF_Resolved);
}

void StatementResolveAccessor::Access(SnEnumMember &sn)
{
}

} //namespace nlang
