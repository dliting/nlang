/*---
    StatementResolverRef.cpp — 引用绑定家族（v0.13.9 phase 3）：声明符形态、
    存储门、绑定判定、&返回契约与foreach &源检查。
    按语句族分文件（source-size guard，沿用DeclDefaults先例）。
    Task 5: the shared judgment (binding targets, promotion marking,
    storage gates, position family) moved to the free functions in
    RefBindJudgment.cpp — the &-argument call site consumes the same
    judgment; these members are thin delegates.
---*/
#include "StatementResolver.h"
#include "RefBindJudgment.h"
#include <nlang/compiler/CastInfo.h>
#include <nlang/runtime/BuiltinGenericNames.h>

namespace nlang
{

//v0.13.9 phase 3: the &-declarator shape contract (see
//StatementResolver.h). The statement reaches here with its type
//resolved and NF_Ref set, so the declarator continuations the grammar
//still admits (bare, ctor, sized) name their own diagnostics instead
//of silently degrading into plain value declarations.
bool StatementResolveAccessor::RejectRefDeclaratorShapes(
	SnLocalDeclStmt &sn)
{
	bool bRejected = false;
	for (auto &decl : sn.Decls())
	{
		if (decl.pCtorArgs)
		{
			m_Env.Log(CLL_Error, sn.Location(),
				"a reference cannot use constructor initialization; "
				"bind an initializer with '=' instead.");
			bRejected = true;
		}
		else if (decl.pArraySize)
		{
			m_Env.Log(CLL_Error, sn.Location(),
				"a reference cannot use an array declarator; bind an "
				"initializer with '=' instead.");
			bRejected = true;
		}
		else if (!decl.pInitExpr)
		{
			m_Env.Log(CLL_Error, sn.Location(),
				"reference \"%s\" must bind an initializer; a "
				"reference has no storage of its own.",
				decl.name.c_str());
			bRejected = true;
		}
	}
	return bRejected;
}

//v0.13.9 phase 3: the storage gates shared by the identifier and member
//arms of the &-binding judgment AND by the &-return judgment. Delegate
//(RefBindJudgment.cpp) — the &-argument call site consumes the same
//gates. True = rejected.
bool StatementResolveAccessor::RejectRefTargetStorage(SnField *pField,
	const ISourceLocation *pLoc)
{
	return ::nlang::RejectRefTargetStorage(m_Env, pField, pLoc);
}

//v0.13.9 phase 3: the binding-only gate — a binding aliases STORAGE,
//not a binding slot, so the target must not itself be a reference.
//Delegate (RefBindJudgment.cpp). True = rejected.
bool StatementResolveAccessor::RejectRefTargetIsReference(SnField *pField,
	const ISourceLocation *pLoc)
{
	return ::nlang::RejectRefTargetIsReference(m_Env, pField, pLoc);
}

//v0.13.9 phase 3: the position family of the &-binding judgment.
//Delegate (RefBindJudgment.cpp). True = rejected.
bool StatementResolveAccessor::RejectRefBindPosition(SnExpression &expr,
	const ISourceLocation *pLoc)
{
	return ::nlang::RejectRefBindPosition(m_Env, expr, pLoc);
}

//v0.13.9 phase 3 Task 4: promotion marking for an accepted &-binding.
//Delegate (RefBindJudgment.cpp).
void StatementResolveAccessor::MarkPromotedBindTarget(SnExpression &expr)
{
	::nlang::MarkPromotedBindTarget(expr);
}

//v0.13.9 phase 3: the &-binding judgment for one resolved initializer.
//Delegate (RefBindJudgment.cpp) — Task 5 shares it with the
//&-argument call site.
bool StatementResolveAccessor::RejectRefBindTarget(SnExpression &expr,
	SnField *pTypeField, const ISourceLocation *pLoc)
{
	return ::nlang::RejectRefBindTarget(m_Env, expr, pTypeField, pLoc);
}

//v0.13.9 phase 3: the &-return target contract (see StatementResolver.h).
bool StatementResolveAccessor::RejectRefReturnTarget(SnFunction &outerFunc,
	SnExpression &expr, const ISourceLocation *pLoc)
{
	auto *pReturnType = outerFunc.ReturnType();
	if (!pReturnType)
		return false;
	//Identity return types: & is a no-op mark (spec 3.1).
	if (IsRefNoOpType(pReturnType->Field()))
		return false;
	if (expr.Kind() == NK_IdentifierExpr)
	{
		auto *pField = static_cast<SnIdentifierExpr &>(expr).Field();
		if (pField)
		{
			//Static/const slots take no aliasing at all (the shared
			//storage gates; the same boundary as the &-binding site).
			if (::nlang::RejectRefTargetStorage(m_Env, pField, pLoc))
				return true;
			//A local's storage ends with the call — the record
			//survives (GC) but writes land in an orphan. Locals are
			//SnLocalVar descriptors under the NK_FormalParam kind
			//while formal params are real AST nodes; a checked cast
			//tells them apart (params may legally be returned).
			if (dynamic_cast<SnLocalVar *>(pField))
			{
				m_Env.Log(CLL_Error, pLoc,
					"cannot return a reference to local \"%s\"; its "
					"storage ends with the call.",
					pField->Name().c_str());
				return true;
			}
		}
	}
	//Exact-type rule: a converted return builds new storage and breaks
	//the alias — the same boundary as the &-binding site.
	auto *pDeclField = pReturnType->Field();
	auto *pInitType = expr.EvalDataType();
	if (pDeclField && pInitType && pInitType != pDeclField)
	{
		m_Env.Log(CLL_Error, pLoc,
			"cannot return %s from %s&; a reference return requires "
			"the exact type.",
			pInitType->Name().c_str(), pDeclField->Name().c_str());
		return true;
	}
	return false;
}

//v0.13.9 phase 3: a & loop variable aliases the iteration element, but
//Dict keys and string code points are materialized per-iteration
//copies — write-through would land in the copy and be silently lost
//(spec 3.5). Logs only; the caller proceeds.
void StatementResolveAccessor::RejectRefForeachSource(SnForeachStmt &sn)
{
	auto *pSrcType = sn.Iterable()->EvalDataType();
	if (pSrcType && pSrcType->Kind() == NK_ClassDecl)
	{
		auto *pGen = static_cast<SnClassDecl *>(pSrcType);
		if (pGen->IsGenericInstantiation()
			&& pGen->BaseName() == kBuiltinDictTypeName)
		{
			m_Env.Log(CLL_Error, sn.VarType()->Location(),
				"cannot iterate Dict keys by reference; the key list "
				"is a copy. Iterate by value instead.");
			return;
		}
	}
	if (pSrcType && pSrcType->Kind() == NK_String)
	{
		m_Env.Log(CLL_Error, sn.VarType()->Location(),
			"cannot iterate a string by reference; string content is "
			"immutable. Iterate by value instead.");
	}
}

} //namespace nlang
