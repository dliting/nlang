/*---
    StatementResolverRef.cpp — 引用绑定家族（v0.13.9 phase 3）：声明符形态、
    存储门、绑定判定、&返回契约与foreach &源检查。
    按语句族分文件（source-size guard，沿用DeclDefaults先例）。
---*/
#include "StatementResolver.h"
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
//arms of the &-binding judgment AND by the &-return judgment — the
//out-argument three-check template (const/static) widened with the
//constant-position family. Locals and formals carry NF_Ref on their
//DESCRIPTOR (propagated from the type node); the binding-to-a-reference
//gate is a BINDING-only rule (RejectRefTargetIsReference) because
//returning a reference formal is the canonical legal shape. True =
//rejected.
bool StatementResolveAccessor::RejectRefTargetStorage(SnField *pField,
	const ISourceLocation *pLoc)
{
	if (!pField)
		return false;	//resolution already reported the shape
	if (pField->Kind() == NK_EnumMember)
	{
		m_Env.Log(CLL_Error, pLoc,
			"cannot bind a reference to enum member \"%s\".",
			pField->Name().c_str());
		return true;
	}
	if (pField->ContainFlags(NF_Const)
		|| IsInterfaceConstantField(*pField))
	{
		m_Env.Log(CLL_Error, pLoc,
			"cannot bind a reference to const storage \"%s\"; a "
			"reference may write through its target.",
			pField->Name().c_str());
		return true;
	}
	if (pField->ContainFlags(NF_Static))
	{
		m_Env.Log(CLL_Error, pLoc,
			"cannot bind a reference to static storage \"%s\"; static "
			"storage promotion is not supported yet.",
			pField->Name().c_str());
		return true;
	}
	return false;
}

//v0.13.9 phase 3: the binding-only gate — a binding aliases STORAGE,
//not a binding slot, so the target must not itself be a reference
//(locals and formals carry NF_Ref on their descriptor). A reference
//formal MAY be returned (the canonical &-param plumbing), which is why
//this gate is not part of the shared storage check.
bool StatementResolveAccessor::RejectRefTargetIsReference(SnField *pField,
	const ISourceLocation *pLoc)
{
	if (!pField)
		return false;
	auto *pTargetType = pField->EvalDataType();
	if (pField->ContainFlags(NF_Ref)
		|| (pTargetType && pTargetType->ContainFlags(NF_Ref)))
	{
		m_Env.Log(CLL_Error, pLoc,
			"cannot bind a reference to reference \"%s\"; a binding "
			"aliases storage, not a binding slot.",
			pField->Name().c_str());
		return true;
	}
	return false;
}

//v0.13.9 phase 3: the position family of the &-binding judgment (see
//StatementResolver.h) — the out-argument position template widened to
//the bindable-position list. Member/identifier targets run the shared
//storage gates; subscript binds elements and rejects slices. True =
//rejected.
bool StatementResolveAccessor::RejectRefBindPosition(SnExpression &expr,
	const ISourceLocation *pLoc)
{
	if (expr.Kind() == NK_MemberExpr)
	{
		auto &member = static_cast<SnMemberExpr &>(expr);
		if (member.Inner() && member.Inner()->Kind() == NK_InvokeExpr)
		{
			m_Env.Log(CLL_Error, pLoc,
				"cannot bind a reference to a method call result.");
			return true;
		}
		if (RejectRefTargetStorage(
				member.Inner() ? member.Inner()->Field() : nullptr,
				pLoc))
			return true;
	}
	else if (expr.Kind() == NK_SubscriptExpr)
	{
		//Element positions bind; a slice is a view over a range, not
		//one element's storage.
		for (auto *pIndex
				: static_cast<SnSubscriptExpr &>(expr).Indices())
		{
			if (pIndex->Kind() == NK_SliceItemExpr)
			{
				m_Env.Log(CLL_Error, pLoc,
					"a reference cannot bind a slice; slices are "
					"views, not element storage.");
				return true;
			}
		}
	}
	else if (expr.Kind() == NK_IdentifierExpr)
	{
		auto *pTargetField
			= static_cast<SnIdentifierExpr &>(expr).Field();
		if (RejectRefTargetStorage(pTargetField, pLoc))
			return true;
		//Only a named binding target can itself be a reference (fields
		//cannot carry &: the grammar rejects the declaration spelling).
		if (RejectRefTargetIsReference(pTargetField, pLoc))
			return true;
	}
	return false;
}

//v0.13.9 phase 3: the &-binding judgment for one resolved initializer
//(see StatementResolver.h).
bool StatementResolveAccessor::RejectRefBindTarget(SnExpression &expr,
	SnField *pTypeField, const ISourceLocation *pLoc)
{
	//Identity types: & is a no-op mark — the statement degrades to a
	//plain value declaration with no binding contract (spec 3.1).
	if (IsRefNoOpType(pTypeField))
		return false;
	if (RejectRefBindPosition(expr, pLoc))
		return true;
	//Everything else (constructor/free-function invoke results,
	//literals, binary results, explicit `as`) is an accepted rvalue:
	//the binding aliases a fresh temporary and writes to it are lost
	//(spec 3.9 rvalue-binding contract, documented).
	//A compiler-inserted SnCastExpr is different: it wraps the resolved
	//initializer AFTER resolution (the assign-side cast fixup), so its
	//EvalDataType already reports the conversion TARGET and the exact
	//compare below would pass. A non-Same cast means a conversion
	//happened — conversions build new storage and break alias pointing,
	//so the reject names the cast's SOURCE type (the byte in
	//`int& r = b;`). Explicit `as` keeps the rvalue contract.
	if (expr.Kind() == NK_CastExpr
		&& static_cast<SnCastExpr &>(expr).CastKind() != TCK_Same)
	{
		auto &cast = static_cast<SnCastExpr &>(expr);
		auto *pSourceType = cast.Source() && cast.Source()->IsResolved()
			? cast.Source()->EvalDataType() : nullptr;
		if (pSourceType && pTypeField)
		{
			m_Env.Log(CLL_Error, pLoc,
				"cannot bind %s to %s&; a reference binding requires "
				"the exact type.",
				pSourceType->Name().c_str(),
				pTypeField->Name().c_str());
			return true;
		}
	}
	//Exact-type rule for slot-copy bindings: a conversion would build
	//new storage and break alias pointing, so even widening is a
	//reject — enums and their underlying integers included.
	auto *pInitType = expr.EvalDataType();
	if (pInitType && pTypeField && pInitType != pTypeField)
	{
		m_Env.Log(CLL_Error, pLoc,
			"cannot bind %s to %s&; a reference binding requires "
			"the exact type.",
			pInitType->Name().c_str(), pTypeField->Name().c_str());
		return true;
	}
	return false;
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
			if (RejectRefTargetStorage(pField, pLoc))
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
