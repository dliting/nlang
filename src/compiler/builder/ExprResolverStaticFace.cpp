/*---
    ExprResolverStaticFace.cpp — 内建泛型类型名上的静态调用面
    （Future.selectAny，首个静态面）。臂挂在成员头链最前
    （TryResolveThreadSpawn 之后、模块限定回退之前）：单段外层标识符链
    ＋内层调用，(类型名, 方法名) 命中 BuiltinNames.h 的静态面表才接管。
    m12 优先级：用户局部/字段/类型与面类型名同名即弃权，普通成员路径
    接管（其诊断落在用户自己的声明上）。绑定＝外层标识符绑到静态面
    单例（指针身份即 codegen 标记）、内层调用 NF_Resolved 无被调（容器
    方法族同形）、成员结果类型＝int（胜者下标）。
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
#include "ModuleRegistry.h"
#include <vector>

namespace nlang
{

//Static-face arm (Future.selectAny(futures)): the TYPE NAME as a call
//qualifier — no receiver instance exists, the intrinsic's arguments
//stage from slot 0 (the binding row's noReceiver ABI). True = consumed
//(bound or diagnosed); false = declined — the chain is not a face
//shape, and the ordinary paths keep it untouched.
bool ExprResolveAccessor::TryResolveBuiltinStaticFace(
	SnMemberExpr &snMember)
{
	if (!snMember.Inner() || snMember.Inner()->Kind() != NK_InvokeExpr)
		return false;
	const std::vector<std::string> pathSegs =
		OuterIdentifierChain(snMember);
	if (pathSegs.size() != 1)
		return false;
	auto &invoke = static_cast<SnInvokeExpr &>(*snMember.Inner());
	const BuiltinStaticFace *pFace = FindBuiltinStaticFace(
		pathSegs.front(), invoke.CalleeName());
	if (!pFace)
		return false;
	//m12 priority: a user local/field named like the face's type wins
	//(ProbeNonFunctionField), and so does a user TYPE declaration
	//(FindTypeNameInAncestor — the builtin generics are minted, never
	//declared, so only a user spelling can hit). The ordinary member
	//path then owns the chain.
	if (ProbeNonFunctionField(pathSegs.front()))
		return false;
	if (m_pContext && FindTypeNameInAncestor(pathSegs.front(),
		*m_pContext, *m_pAccessor, false))
		return false;
	//The by-name dispatch reject of the container method family: the
	//no-receiver intrinsic reads positional slots only.
	if (RejectNamedOrOutArguments(invoke))
	{
		FinishConsumedMember(snMember);
		return true;
	}
	if (ArgCountOf(invoke) != pFace->argCount)
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"'%s.%s' expects %zu argument(s).",
			pFace->typeName, pFace->methodName, pFace->argCount);
		FinishConsumedMember(snMember);
		return true;
	}
	return BindStaticFaceCall(snMember, invoke, pFace, pathSegs.front());
}

//TryResolveBuiltinStaticFace's bind tail (split for the 50-line
//budget): caller-scope argument resolution, the wait-set shape check,
//then the binding. Always consumes the member.
bool ExprResolveAccessor::BindStaticFaceCall(SnMemberExpr &snMember,
	SnInvokeExpr &invoke, const BuiltinStaticFace *pFace,
	const std::string &typeName)
{
	//Arguments resolve in the caller's scope (the head position —
	//m_pContext is the caller's context, no receiver switch happens).
	ResolveExpressionList(invoke.Params());
	//The wait set: one array of Future values (any Future<T> — the
	//erasure 口径). Array-ness rides the interned token (locals and
	//call results alike — EvalDataType, the foreach source shape), the
	//element is the erased generic instantiation's base name.
	SnExpression *pArg = nullptr;
	for (auto &param : invoke.Params())
	{
		pArg = &param;
		break;
	}
	auto *pArgType = pArg ? pArg->EvalDataType() : nullptr;
	bool elemOk = false;
	if (pArgType && pArgType->Kind() == NK_ArrayTypeToken)
	{
		auto *pElem = static_cast<SnArrayTypeToken *>(pArgType)
			->ElemTypeOf();
		elemOk = pElem && pElem->Kind() == NK_ClassDecl
			&& static_cast<SnClassDecl *>(pElem)->BaseName()
				== kBuiltinFutureTypeName;
	}
	if (!elemOk)
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"'%s.%s' requires an array of Future values.",
			pFace->typeName, pFace->methodName);
		FinishConsumedMember(snMember);
		return true;
	}
	//Bind: outer identifier → the face qualifier singleton (pointer
	//identity is the codegen marker; nothing ever emits it as a value);
	//inner invoke resolved with no callee (the container-method family
	//shape); member result = int (the winning index).
	ResolveFieldExprAs(static_cast<SnFieldExpr &>(*snMember.Outer()),
		BuiltinStaticFaceClassDecl(typeName));
	invoke.AddFlags(NF_Resolved);
	auto *pInt = SnBuiltinDataType::InstanceOf(NK_Int32);
	snMember.EvalDataType(pInt);
	snMember.m_pField = pInt;
	snMember.AddFlags(NF_Resolved);
	BindArrayTypeToken(snMember);
	return true;
}

} //namespace nlang
