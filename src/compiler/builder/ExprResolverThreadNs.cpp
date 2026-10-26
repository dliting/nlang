/*---
    ExprResolverThreadNs.cpp — thread 包解析侧（K3 真实化后）：
    ① 类型化 spawn 窄臂（TryResolveThreadSpawn）：thread.spawn 的
    func<T>→Future<T> 形态无法用单一静态原型表达，保留一个先于模块
    限定回退的专用臂；其余 thread.* 形态（join/yield/id/sleep、
    spawn-void、未知成员）全部落回 TryResolveModuleQualified＋import
    准入——thread 与 io 等普通标准库包同轨。
    （intrinsic 名→id 表已并入 VmBackend 的统一绑定表——
    IsStampedBindingPackage/LookupIntrinsicId。Mutex/Semaphore 的
    同步原语解析特例已随声明化退役——方法走普通成员解析，
    Channel<T> 方法走泛型容器方法族。）
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

} //namespace nlang
