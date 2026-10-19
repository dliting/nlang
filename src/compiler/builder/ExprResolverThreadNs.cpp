/*---
    ExprResolverThreadNs.cpp — K1 并发语言面（解析侧）：
    ① 内建 thread 命名空间：签名表/单例工厂与调用拦截。
    形态与模块限定调用（TryResolveModuleQualified）同构：链形 [thread][member]()，
    先过 m12 遮蔽探针（用户局部/字段/类型名为 thread 者胜），再对签名表匹配。
    发射走既有自由调用轨：外层标识符绑定命名空间单例（EmitMemberHeaderDispatch
    的命名空间臂要求），内层 invoke 绑定合成 SnFunction，OP_CallFunc 的内建臂派发。
    ② Mutex/Channel 同步原语方法（TryResolveSyncPrimitiveMethod）：按名分类梯
    ＋流方法族尾态（调用方作用域实参解析、void 不留类型），发射走
    OP_CallMethod-by-name 既有轨。
---*/
#include "ExprResolver.h"
#include "ThreadNamespace.h"
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

//--- 签名表与单例工厂 ------------------------------------------------------

static const ThreadNsFuncEntry s_ThreadNsFuncs[] = {
	{"spawn", INTR_Thread_Spawn, TNP_FuncVoid,  TNP_ThreadObj},
	{"join",  INTR_Thread_Join,  TNP_ThreadObj, TNP_Void},
	{"yield", INTR_Thread_Yield, TNP_Void,      TNP_Void},
	{"id",    INTR_Thread_Id,    TNP_Void,      TNP_Int32},
	{"sleep", INTR_Thread_Sleep, TNP_Int32,     TNP_Void},
};

const ThreadNsFuncEntry* ThreadNsFuncTable()
{
	return s_ThreadNsFuncs;
}

size_t ThreadNsFuncCount()
{
	return std::size(s_ThreadNsFuncs);
}

const ThreadNsFuncEntry* FindThreadNsFunc(const std::string& memberName)
{
	for (const auto& e : s_ThreadNsFuncs)
		if (memberName == e.memberName)
			return &e;
	return nullptr;
}

//The namespace singleton. Members stay empty forever: the surface is a
//flat function table, and only the node kind matters to the emission
//contract (EmitMemberHeaderDispatch's namespace arm).
SnNamespace* GetThreadNamespaceDecl()
{
	static SnNamespace* s_pNs = nullptr;
	if (!s_pNs)
	{
		ScriptLocation fallback;
		s_pNs = new SnNamespace(new std::string(kThreadNamespaceName),
			new std::list<SnField*>(), fallback);
	}
	return s_pNs;
}

//Type EXPRESSION minting for one table kind. The expression starts
//unbound: Field() binding is an ExprResolveAccessor-only operation
//(BindThreadNsFormalTypes does it lazily on the resolver side).
static SnIdentifierExpr* NewThreadNsTypeRef(const char* pDisplayName)
{
	ScriptLocation fallback;
	return new SnIdentifierExpr(new std::string(pDisplayName), fallback);
}

//The synthetic callee of table entry `tableIndex`. Formals carry the
//declared kind; the function's return type flows through the return
//expression's later-bound Field() (SnFunction::EvalDataType reads it).
//Singletons: process-lifetime built-ins, minted once per entry.
SnFunction* GetThreadNsCallee(size_t tableIndex)
{
	static SnFunction* s_Callees[std::size(s_ThreadNsFuncs)] = {};
	assert(tableIndex < std::size(s_ThreadNsFuncs));
	SnFunction*& rpRef = s_Callees[tableIndex];
	if (rpRef)
		return rpRef;
	const ThreadNsFuncEntry& entry = s_ThreadNsFuncs[tableIndex];
	ScriptLocation fallback;
	UniquePtrList<SnFormalParam> params;
	if (entry.paramKind != TNP_Void)
	{
		const char* pParamName =
			(entry.paramKind == TNP_FuncVoid) ? "f"
			: (entry.paramKind == TNP_ThreadObj) ? "t" : "ms";
		params.Add(new SnFormalParam(NF_NONE,
			NewThreadNsTypeRef(
				(entry.paramKind == TNP_FuncVoid) ? "func"
				: (entry.paramKind == TNP_ThreadObj) ? "Thread" : "int"),
			new std::string(pParamName), nullptr, fallback));
	}
	SnFieldExpr* pReturnType = (entry.returnKind == TNP_Void) ? nullptr
		: NewThreadNsTypeRef(
			(entry.returnKind == TNP_ThreadObj) ? "Thread" : "int");
	rpRef = new SnFunction(FA_Public, NF_NONE, pReturnType,
		new std::string(entry.memberName), std::move(params), fallback);
	return rpRef;
}

//--- 解析侧 ----------------------------------------------------------------

//Bind one minted type expression to its type node. The synthetic callee
//is a process-lifetime singleton, but SnBuiltinDataType::InstanceOf (and
//the func<...> instantiations built on it) resolve through TheAST() — a
//process-singleton tree whose builtin nodes are re-minted on EVERY build
//(ModuleBuilder::InitSyntaxTree clears and rebuilds it). A Field()
//captured by an earlier build therefore dangles once that build ends:
//an in-process host compiling many programs would match thread.spawn
//against freed-then-reused nodes. The guard compares against the
//CURRENT build's node and rebinds on rotation — a manual rebind, since
//ResolveFieldExprAs asserts on already-resolved expressions. Within one
//build the interned nodes are stable and the bind is idempotent.
void ExprResolveAccessor::BindThreadNsFormalTypes(
	const ThreadNsFuncEntry& entry)
{
	const size_t tableIndex = &entry - ThreadNsFuncTable();
	SnFunction& callee = *GetThreadNsCallee(tableIndex);
	auto bind = [this](SnFieldExpr& expr, SnField* pType)
	{
		if (expr.Field() == pType)
			return;   //already bound to this build's node
		if (expr.IsResolved())
		{
			//Cross-build rotation: refresh the stale capture. Same writes
			//ResolveFieldExprAs makes, minus its unresolved-only assert.
			expr.m_pField = pType;
			expr.EvalDataType(pType->EvalDataType());
			return;
		}
		ResolveFieldExprAs(expr, pType);
	};
	if (entry.paramKind != TNP_Void)
	{
		//The K1 surface carries at most one parameter per function.
		for (auto& formal : callee.Params())
		{
			bind(*formal.Type(), ThreadNsTypeNode(entry.paramKind));
			break;
		}
	}
	if (callee.ReturnType())
		bind(*callee.ReturnType(), ThreadNsTypeNode(entry.returnKind));
}

//The type NODE of one table kind — resolved here, on the resolver side
//only (the backend never reads the synthetic formals).
SnField* ExprResolveAccessor::ThreadNsTypeNode(unsigned char kind)
{
	ScriptLocation fallback;
	switch (kind)
	{
	case TNP_Int32:
		return SnBuiltinDataType::InstanceOf(NK_Int32);
	case TNP_ThreadObj:
		return GetBuiltinClassDecl("Thread", &fallback);
	case TNP_FuncVoid:
		return GetGenericClassDecl(kBuiltinFuncTypeName,
			{SnBuiltinDataType::InstanceOf(NK_Void)}, {}, &fallback);
	default:
		return nullptr;   //TNP_Void: no node to build
	}
}

//Built-in thread namespace: `thread.<member>(...)`, resolved against
//the signature table. Same chain shape and m12 priority as the
//module-qualified fallback (a user local/field/type named `thread`
//wins; O1's reserved-name gate guarantees no module can ever sit at
//the path). True = consumed (resolved or diagnosed); false = declined.
bool ExprResolveAccessor::TryResolveThreadNamespace(SnMemberExpr &snMember)
{
	if (!snMember.Outer() || !snMember.Inner()
		|| snMember.Inner()->Kind() != NK_InvokeExpr)
		return false;
	const std::vector<std::string> pathSegs =
		OuterIdentifierChain(snMember);
	if (pathSegs.empty() || pathSegs.front() != kThreadNamespaceName)
		return false;
	if (ProbeNonFunctionField(pathSegs.front())
		|| IsBuiltinClassName(pathSegs.front()))
		return false;
	auto &invoke = static_cast<SnInvokeExpr &>(*snMember.Inner());

	const ThreadNsFuncEntry* entry = nullptr;
	if (pathSegs.size() == 1)
		entry = FindThreadNsFunc(invoke.CalleeName());
	if (!entry)
	{
		std::string dotted = kThreadNamespaceName;
		for (const auto& seg : pathSegs)
			dotted += "." + seg;
		dotted += "." + invoke.CalleeName();
		m_Env.Log(CLL_Error, snMember.Location(),
			"'%s' is not a member of the built-in thread namespace "
			"(thread.spawn, thread.join, thread.yield, thread.id, "
			"thread.sleep).", dotted.c_str());
		FinishConsumedMember(snMember);
		return true;
	}

	//Emission contract: the outer identifier's Field() must be the
	//namespace singleton (EmitMemberHeaderDispatch's namespace arm).
	ResolveFieldExprAs(static_cast<SnFieldExpr &>(*snMember.Outer()),
		GetThreadNamespaceDecl());

	//Arguments resolve in the caller's scope — same contract and tail
	//(diagnosed failures consume the member) as module-qualified calls.
	if (!TryResolveModuleQualifiedArgs(snMember, invoke))
		return true;

	return BindThreadNsCall(snMember, invoke, *entry);
}

//TryResolveThreadNamespace's match-and-bind tail: lazily rebind the
//synthetic callee's formal types to the current build, match the invoke
//against the one-entry candidate list, and wire the call site through
//the shared module-qualified close-out. Always consumes the member.
bool ExprResolveAccessor::BindThreadNsCall(SnMemberExpr &snMember,
	SnInvokeExpr &invoke, const ThreadNsFuncEntry& entry)
{
	BindThreadNsFormalTypes(entry);
	std::vector<SnFunction*> candidates{
		GetThreadNsCallee(static_cast<size_t>(&entry - ThreadNsFuncTable()))};
	SnFunction *pCallee = nullptr;
	std::vector<FormalBinding> bindings;
	bool bAmbiguous = false;
	auto res = MatchInvokeAgainst(invoke, candidates, pCallee, bindings,
		bAmbiguous);
	if (res != FFR_ExactMatch && res != FFR_ApproximateMatch)
	{
		//A function reference that failed to match a func<> formal would
		//otherwise get the generic pending-ref sweep ("requires an
		//expected function type") — misleading here: an expected type
		//did exist, the signature disagreed. Name the actual contract.
		if (res == FFR_Incompatible && entry.paramKind == TNP_FuncVoid)
		{
			for (auto& arg : invoke.Params())
			{
				if (arg.Kind() != NK_NamedArgExpr && IsUnboundFuncRef(arg))
				{
					m_Env.Log(CLL_Error, arg.Location(),
						"thread.spawn requires a zero-argument void "
						"function: \"%s\" does not match func<void>.",
						arg.ToString().c_str());
					FinishConsumedMember(snMember);
					return true;
				}
			}
		}
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

//--- 同步原语方法（Mutex/Channel） ------------------------------------------

//Fixed call shapes of the K1 sync-primitive surface: SyncMethodShape
//lives in ThreadNamespace.h (shared with the accessor declaration).
//Channel.recv arrives in Slice 9 with its element-type decision — until
//then it is simply not a member (the generic resolution failure names it).
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
	else if (className == "Channel")
	{
		//send's value boxes like a List/Dict element (runtime path);
		//arity alone is checked here.
		if (methodName == "send")
		{
			rShape = {1, NK_Void};
			return true;
		}
		if (methodName == "close")
		{
			rShape = {0, NK_Void};
			return true;
		}
	}
	return false;
}

//Mutex/Channel member calls. The synthesized SnClassDecl has no method
//members, so resolution is by name against the shape table (the stream
//family's arrangement); emission rides OP_CallMethod-by-name the same
//way. Runs BEFORE TryResolveStreamBuiltinMethod — close() is also a
//stream method name and would be captured there.
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
	if (className != "Mutex" && className != "Channel")
		return false;
	auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
	SyncMethodShape shape;
	if (!ClassifySyncPrimitiveMethod(className, invoke.CalleeName(),
			shape))
		return false;   //not ours (recv, or a user typo) — generic failure
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
	//Params() is a child-node view without size() — count by iteration.
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
