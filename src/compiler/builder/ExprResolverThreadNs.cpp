/*---
    ExprResolverThreadNs.cpp — thread 包解析侧（K3 真实化后）：
    ① 类型化 spawn 窄臂（TryResolveThreadSpawn）：thread.spawn 的
    func<T>→Future<T> 形态无法用单一静态原型表达，保留一个先于模块
    限定回退的专用臂；其余 thread.* 形态（join/yield/id/sleep、
    spawn-void、未知成员）全部落回 TryResolveModuleQualified＋import
    准入——thread 与 io 等普通标准库包同轨。
    ② intrinsic 名→id 表（FindThreadIntrinsicId）：后端给
    stdlib/thread.n 的 native 记录盖 intrinsic 章用。
    ③ Mutex/Semaphore 同步原语方法（TryResolveSyncPrimitiveMethod）：按名分类梯
    ＋流方法族尾态（调用方作用域实参解析、void 不留类型），发射走
    OP_CallMethod-by-name 既有轨。Channel<T> 方法走泛型容器方法族
    （ExprResolverMemberContainer.cpp）。
---*/
#include "ExprResolver.h"
#include "ThreadNamespace.h"
#include "ModuleRegistry.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include <nlang/runtime/AutoPointers.h>
#include <nlang/runtime/BuiltinGenericNames.h>
#include <iterator>
#include <vector>

namespace nlang
{

//--- intrinsic 表 -----------------------------------------------------------

static const ThreadIntrinsicEntry s_ThreadIntrinsics[] = {
    {"spawn", INTR_Thread_Spawn},
    {"join",  INTR_Thread_Join},
    {"yield", INTR_Thread_Yield},
    {"id",    INTR_Thread_Id},
    {"sleep", INTR_Thread_Sleep},
};

uint16_t FindThreadIntrinsicId(const std::string& memberName)
{
    for (const auto& e : s_ThreadIntrinsics)
        if (memberName == e.memberName)
            return e.intrinsicId;
    return INTR_None;
}

//--- 类型化 spawn 窄臂 ------------------------------------------------------

//K3 materialization's narrow arm: chain head `thread` + member `spawn` —
//the one shape a static prototype cannot express (func<T> delegate →
//Future<T>), so it keeps a dedicated try before the module-qualified
//fallback. Runs the SAME import admission first (a typed spawn must not
//slip past the gate the package path enforces); every other thread.*
//shape declines to TryResolveModuleQualified, whose admission and
//diagnostics then apply unchanged. m12 priority: the same probes as the
//fallback — a user local/field/type named `thread` wins. True = consumed
//(bound or diagnosed); false = declined.
bool ExprResolveAccessor::TryResolveThreadSpawn(SnMemberExpr &snMember)
{
	if (!snMember.Outer() || !snMember.Inner()
		|| snMember.Inner()->Kind() != NK_InvokeExpr)
		return false;
	const std::vector<std::string> pathSegs =
		OuterIdentifierChain(snMember);
	if (pathSegs.size() != 1 || pathSegs.front() != kThreadNamespaceName)
		return false;
	if (ProbeNonFunctionField(pathSegs.front())
		|| IsBuiltinClassName(pathSegs.front()))
		return false;
	auto &invoke = static_cast<SnInvokeExpr &>(*snMember.Inner());
	if (invoke.CalleeName() != "spawn")
		return false;
	//Import admission BEFORE any binding. A closed gate declines — the
	//module-qualified fallback then names the missing import with the
	//ordinary package wording (no separate typed-spawn wording exists).
	const uint32_t owner =
		m_Env.Registry().OwnerOfContext(*m_pContext);
	if (!m_Env.Registry().ImportGateAllows(owner, kThreadNamespaceName))
		return false;
	//The stdlib thread.n declaration this import inlined; an empty scan
	//means the package is not what the index promised — decline and let
	//the fallback diagnose it.
	std::vector<SnFunction*> candidates = m_Env.Registry().ModuleFunctions(
		kThreadNamespaceName, "spawn");
	if (candidates.empty())
		return false;
	return BindThreadSpawnCall(snMember, invoke, candidates);
}

//TryResolveThreadSpawn's bind tail (split for the 50-line function
//budget): caller-scope argument resolution, the typed func<T>→Future<T>
//arm, then the fixed match against the declared func<void> prototype.
//Always consumes the member.
bool ExprResolveAccessor::BindThreadSpawnCall(SnMemberExpr &snMember,
	SnInvokeExpr &invoke, std::vector<SnFunction*> &candidates)
{
	//Arguments resolve in the caller's scope — same contract and tail
	//(diagnosed failures consume the member) as module-qualified calls.
	if (!TryResolveModuleQualifiedArgs(snMember, invoke))
		return true;

	//Typed arm first: func<T> → Future<T> (T=void keeps the Thread face).
	if (TryBindTypedSpawn(snMember, invoke, *candidates.front()))
	{
		FinishConsumedMember(snMember);
		return true;
	}
	//Fixed match against the declared func<void> prototype, with the
	//spawn-specific mismatch wording for bare function references.
	std::vector<FormalBinding> bindings;
	SnFunction *pCallee = nullptr;
	bool bAmbiguous = false;
	auto res = MatchInvokeAgainst(invoke, candidates, pCallee, bindings,
		bAmbiguous);
	if (res != FFR_ExactMatch && res != FFR_ApproximateMatch)
	{
		if (ReportSpawnFuncRefFailure(snMember, invoke, res))
			return true;
		LogInvokeFailure(invoke, res, pCallee, false, candidates);
		FinishConsumedMember(snMember);
		return true;
	}
	if (!BindQualifiedInvoke(snMember, invoke, *pCallee, res, bindings))
	{
		FinishConsumedMember(snMember);
		return true;
	}
	FinishConsumedMember(snMember);
	return true;
}

//--- 同步原语方法（Mutex/Semaphore） ---------------------------------

//Fixed call shapes of the K1 sync-primitive surface: SyncMethodShape
//lives in ThreadNamespace.h (shared with the accessor declaration).
//Channel<T> methods (send/recv/close) are NOT here — they ride the
//generic container-method family (ExprResolverMemberContainer.cpp),
//where the element type argument drives boxing and recv's return type.
static bool ClassifySyncPrimitiveMethod(const std::string& className,
	const std::string& methodName, SyncMethodShape& rShape)
{
	if (className == "Mutex")
	{
		if (methodName == "lock" || methodName == "unlock")
		{
			rShape = {0, NK_Void};
			return true;
		}
		if (methodName == "tryLock")
		{
			rShape = {0, NK_Bool};
			return true;
		}
	}
	//P2-1: the native counting semaphore — the Mutex method triple's
	//counterpart (acquire/release/tryAcquire).
	if (className == "Semaphore")
	{
		if (methodName == "acquire" || methodName == "release")
		{
			rShape = {0, NK_Void};
			return true;
		}
		if (methodName == "tryAcquire")
		{
			rShape = {0, NK_Bool};
			return true;
		}
	}
	return false;
}

//Mutex/Semaphore member calls. The synthesized SnClassDecl has no
//method members, so resolution is by name against the shape table (the
//stream family's arrangement); emission rides OP_CallMethod-by-name the
//same way.
bool ExprResolveAccessor::TryResolveSyncPrimitiveMethod(
	SnMemberExpr &snMember, SnFieldExpr *pInnerExpr,
	SyntaxNode *pSavedContext)
{
	if (!(m_pContext && m_pContext->Kind() == NK_ClassDecl
		&& static_cast<SnClassDecl*>(m_pContext)->IsBuiltinClass()
		&& pInnerExpr->Kind() == NK_InvokeExpr))
		return false;
	auto* pClass = static_cast<SnClassDecl*>(m_pContext);
	const std::string className = pClass->Name();
	if (className != "Mutex" && className != "Semaphore")
		return false;
	auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
	SyncMethodShape shape;
	if (!ClassifySyncPrimitiveMethod(className, invoke.CalleeName(),
			shape))
		return false;   //not ours (a user typo) — generic failure
	return FinishSyncPrimitiveCall(snMember, invoke, pInnerExpr, shape,
		pSavedContext);
}

//TryResolveSyncPrimitiveMethod's post-classification tail: named/out
//veto, caller-scope argument resolution, arity check, and the member's
//type wiring. Always consumes the member.
bool ExprResolveAccessor::FinishSyncPrimitiveCall(SnMemberExpr &snMember,
	SnInvokeExpr &invoke, SnFieldExpr *pInnerExpr,
	const SyncMethodShape &shape, SyntaxNode *pSavedContext)
{
	//Named/out arguments cannot ride by-name dispatch (Round-12/14 rule).
	if (RejectNamedOrOutArguments(invoke))
	{
		m_pContext = pSavedContext;
		return true;
	}
	//Args resolve in the caller's scope (the synthesized class scope is
	//empty) — the stream tail's arrangement.
	m_pContext = pSavedContext;
	RemoveFlags(ERF_SearchInParentOnly);
	ResolveExpressionList(invoke.Params());
	//Params()is a child-node view without size() — count by iteration.
	int argCount = 0;
	for (auto& param : invoke.Params())
	{
		(void)param;
		++argCount;
	}
	if (argCount != shape.paramCount)
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"'%s' takes %d argument%s.", invoke.CalleeName().c_str(),
			shape.paramCount, shape.paramCount == 1 ? "" : "s");
		m_pContext = pSavedContext;
		return true;
	}
	pInnerExpr->AddFlags(NF_Resolved);
	if (shape.returnType != NK_Void)
	{
		auto* pRet = SnBuiltinDataType::InstanceOf(shape.returnType);
		snMember.EvalDataType(pRet);
		snMember.m_pField = pRet;
	}
	snMember.AddFlags(NF_Resolved);
	BindArrayTypeToken(snMember);
	m_pContext = pSavedContext;
	return true;
}

} //namespace nlang
