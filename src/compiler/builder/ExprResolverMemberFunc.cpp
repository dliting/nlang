/*---
    ExprResolverMemberFunc.cpp — func<...> 句柄方法解析面（toString）。
    泛型四类声明化（builtin-decls 阶段 2）后，合成铸型只剩 func：无
    运行时类、无成员表，方法按名派发于此。文件自
    ExprResolverMemberContainer.cpp 收缩更名（2026-10-28 阶段 2，
    List/Dict/Channel/Future 转常规成员解析）。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnArrayTypeToken.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include <nlang/runtime/BuiltinGenericNames.h>

namespace nlang
{

//Phase 13: function handles expose toString only (zero arguments).
bool ExprResolveAccessor::TryResolveFuncHandleMethod(
	SnMemberExpr &snMember, SnFieldExpr *pInnerExpr,
	SyntaxNode *pSavedContext)
{
	if (!(m_pContext && m_pContext->Kind() == NK_ClassDecl
		&& IsGenericClassDecl(static_cast<SnClassDecl*>(m_pContext))
		&& static_cast<SnClassDecl*>(m_pContext)->BaseName()
			== kBuiltinFuncTypeName
		&& pInnerExpr->Kind() == NK_InvokeExpr))
		return false;
	auto &invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
	if (invoke.CalleeName() != "toString")
		return false;
	//By-name dispatch cannot bind name = value args; out args cannot
	//write back.
	if (RejectNamedOrOutArguments(invoke))
	{
		m_pContext = pSavedContext;
		return true;
	}
	if (ArgCountOf(invoke) != 0)
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"Built-in method 'toString' called with the wrong number of "
			"arguments.");
		m_pContext = pSavedContext;
		return true;
	}
	m_pContext = pSavedContext;
	RemoveFlags(ERF_SearchInParentOnly);
	ResolveExpressionList(invoke.Params());
	pInnerExpr->AddFlags(NF_Resolved);
	auto *pStr = SnBuiltinDataType::InstanceOf(NK_String);
	//m_pField directly (not via ResolveFieldExprAs which would overwrite
	//EvalDataType with SnType) so chained access survives IsDataExpr()
	//(e.g. f.toString().length()).
	snMember.m_pField = pStr;
	snMember.EvalDataType(pStr);
	snMember.AddFlags(NF_Resolved);
	BindArrayTypeToken(snMember);
	m_pContext = pSavedContext;
	return true;
}

} //namespace nlang
