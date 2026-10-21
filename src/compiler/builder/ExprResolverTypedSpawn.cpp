/*---
    ExprResolverTypedSpawn.cpp — K3 类型化 thread.spawn 解析臂：
    单实参 spawn 的专用窄臂分派（TryResolveThreadSpawn）。裸函数引用
    按自身签名铸 func<T> 并走既有绑定门，实参为 func<T> 值时直接取
    其类型实参；调用点结果类型 Future<T>（T=void 保留 K1 Thread 面）。
    派发骑 stdlib/thread.n 的 spawn 声明（空 bindings＋FFR_ExactMatch
    ——声明形参与委托同形），成员类型逐调用点覆写。从
    ExprResolverThreadNs.cpp 抽出（源文件 500 行预算）。
---*/
#include "ExprResolver.h"
#include "ThreadNamespace.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include <nlang/runtime/BuiltinGenericNames.h>
#include <vector>

namespace nlang
{

//K3 typed-spawn arm (see TryResolveThreadSpawn's dispatch). The argument
//must be a zero-parameter func<T>; a bare function reference binds against
//the func<T> instantiation of its own signature (BindFuncRefToExpected
//runs the method/default-parameter gates), a func<T>-typed value
//contributes its type argument directly. The call site's result type is
//Future<T>; T=void keeps the K1 Thread face. Dispatch rides the stdlib
//spawn declaration — OP_CallFunc's built-in arm reads the invoke's bound
//callee; the member carries the per-site result decl. True = consumed
//(bound or diagnosed); false = not this arm's shape, the fixed path's
//diagnostics cover the rejection.
bool ExprResolveAccessor::TryBindTypedSpawn(SnMemberExpr &snMember,
	SnInvokeExpr &invoke, SnFunction &spawnDecl)
{
	SnExpression *pArg = nullptr;
	int argCount = 0;
	for (auto &param : invoke.Params())
	{
		++argCount;
		pArg = &param;
	}
	if (argCount != 1 || !pArg || pArg->Kind() == NK_NamedArgExpr)
		return false;

	SnField *pT = nullptr;
	const SpawnArgOutcome outcome = ClassifyTypedSpawnArg(*pArg, pT);
	if (outcome == SpawnArgOutcome::Declined)
		return false;   //fixed path names the mismatch
	if (outcome == SpawnArgOutcome::Diagnosed)
		return true;

	//Result decl: Thread for a void delegate, Future<T> otherwise.
	SnField *pRetDecl = nullptr;
	if (pT->Kind() == NK_Void)
	{
		ScriptLocation fallback;
		pRetDecl = GetBuiltinClassDecl("Thread", &fallback);
	}
	else
	{
		pRetDecl = GetGenericClassDecl(kBuiltinFutureTypeName, {pT}, {},
			snMember.Location());
	}
	if (!pRetDecl)
		return false;
	std::vector<FormalBinding> bindings;
	if (!BindQualifiedInvoke(snMember, invoke, spawnDecl, FFR_ExactMatch,
			bindings))
		return true;   //diagnosed (out-argument reject)
	snMember.m_pField = pRetDecl;
	snMember.EvalDataType(pRetDecl);
	return true;
}

//TryBindTypedSpawn's argument classification: one pass over the single
//spawn argument. Bare function reference → the target's own signature
//decides T (SpawnTFromBareRef); a resolved func<T> value contributes its
//type argument; anything else is Declined (the fixed path names it).
ExprResolveAccessor::SpawnArgOutcome
ExprResolveAccessor::ClassifyTypedSpawnArg(SnExpression &arg, SnField *&rpT)
{
	if (IsUnboundFuncRef(arg))
		return SpawnTFromBareRef(arg, rpT);
	if (arg.IsResolved() && arg.EvalDataType())
	{
		auto *pDecl = static_cast<SnClassDecl *>(arg.EvalDataType());
		if (!IsFuncTypeDecl(pDecl))
			return SpawnArgOutcome::Declined;
		const auto &typeArgs = GetGenericTypeArgs(pDecl);
		if (typeArgs.size() != 1 || !typeArgs[0])
		{
			m_Env.Log(CLL_Error, arg.Location(),
				"thread.spawn requires a zero-argument function: "
				"\"%s\" is not one.", arg.ToString().c_str());
			return SpawnArgOutcome::Diagnosed;
		}
		rpT = typeArgs[0];
		return SpawnArgOutcome::Bound;
	}
	return SpawnArgOutcome::Declined;
}

//The bare-reference arm of ClassifyTypedSpawnArg: the referenced
//function's own signature decides T, and the reference binds against
//the matching func<T> instantiation (running the usual bind gates).
ExprResolveAccessor::SpawnArgOutcome
ExprResolveAccessor::SpawnTFromBareRef(SnExpression &arg, SnField *&rpT)
{
	auto *pTarget = static_cast<SnFunction *>(
		static_cast<SnIdentifierExpr &>(arg).Field());
	if (pTarget->Params().size() != 0)
	{
		m_Env.Log(CLL_Error, arg.Location(),
			"thread.spawn requires a zero-argument function: "
			"\"%s\" takes parameters.", pTarget->Name().c_str());
		return SpawnArgOutcome::Diagnosed;
	}
	if (pTarget->ReturnType() && !pTarget->ReturnType()->IsResolved())
		return SpawnArgOutcome::Declined;   //fixed path names the mismatch
	ScriptLocation fallback;
	rpT = pTarget->ReturnType()
		? static_cast<SnField *>(pTarget->ReturnType()->Field())
		: static_cast<SnField *>(
			SnBuiltinDataType::InstanceOf(NK_Void));
	if (!rpT)
		return SpawnArgOutcome::Declined;
	std::vector<SnField *> funcArgs{rpT};
	auto *pFuncDecl = GetGenericClassDecl(kBuiltinFuncTypeName,
		funcArgs, {}, &fallback);
	if (!pFuncDecl || !BindFuncRefToExpected(m_Env,
			static_cast<SnIdentifierExpr &>(arg), pFuncDecl))
		return SpawnArgOutcome::Diagnosed;   //diagnosed by the bind gate
	return SpawnArgOutcome::Bound;
}

//The narrow arm's fixed-path mismatch tail: a bare function reference
//that failed to match the declared func<void> formal would otherwise get
//the generic pending-ref sweep ("requires an expected function type") —
//misleading here: an expected type did exist, the signature disagreed.
//Name the actual contract. True = the specific diagnostic fired
//(consumed).
bool ExprResolveAccessor::ReportSpawnFuncRefFailure(SnMemberExpr &snMember,
	SnInvokeExpr &invoke, FindFuncResult res)
{
	if (res != FFR_Incompatible)
		return false;
	for (auto& arg : invoke.Params())
	{
		if (arg.Kind() != NK_NamedArgExpr && IsUnboundFuncRef(arg))
		{
			m_Env.Log(CLL_Error, arg.Location(),
				"thread.spawn requires a zero-argument "
				"function: \"%s\" does not match func<void>.",
				arg.ToString().c_str());
			FinishConsumedMember(snMember);
			return true;
		}
	}
	return false;
}

} //namespace nlang
