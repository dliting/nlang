/*---
    ExprResolverFuncRef.cpp — 函数/方法引用绑定与未绑定谓词
    从 ExprResolver.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnArrayTypeToken.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include "ModuleRegistry.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

//Phase 13: exact-signature comparison between a function declaration and
//a Func<...> instantiation (no variance). Return slot: a nullptr return
//type matches a void type argument. Parameter slots: declared type field
//pointer identity plus out-flag agreement — the same discipline that
//keeps Func<void,int> and Func<void,out int> distinct in GenericInstKey.
//0.7.3 B: array-ness needs no separate comparison — an array-typed
//return/param carries the interned token, and pointer identity against
//the type argument (itself a token for Func<int[]>) rules the pairing.
bool FuncRefMatchesDecl(const SnFunction &func, SnClassDecl *pFuncDecl)
{
	const auto &typeArgs = GetGenericTypeArgs(pFuncDecl);
	const auto &outFlags = GetGenericOutFlags(pFuncDecl);
	auto *pRet = func.ReturnType();
	if (pRet)
	{
		if (!pRet->IsResolved() || pRet->Field() != typeArgs[0])
			return false;
	}
	else if (typeArgs[0]->Kind() != NK_Void)
	{
		return false;
	}
	const auto &params = func.Params();
	if (params.size() + 1 != typeArgs.size())
		return false;
	size_t i = 0;
	for (auto &param : params)
	{
		if (param.EvalDataType() != typeArgs[i + 1])
			return false;
		if (param.ContainFlags(NF_Out) != (outFlags[i + 1] != 0))
			return false;
		++i;
	}
	return true;
}

//Rejection gates of BindFuncRefToExpected: the expected type must be a
//Func instantiation, the callee a free function (not a method), a local
//declaration (imported stubs serialize no parameter signatures) and one
//without default parameters. True = rejected with the named diagnostic.
static bool RejectFuncRefTarget(BuildEnvironment &env,
	SnIdentifierExpr &idExpr, SnFunction *pFunc, SnField *pExpected)
{
	if (!IsFuncTypeDecl(pExpected))
	{
		env.Log(CLL_Error, idExpr.Location(),
			"function reference \"%s\" requires an expected function type.",
			idExpr.Name().c_str());
		return true;
	}
	//Methods are not free-function references (receiver-bound references
	//arrive in Step 2); enum methods additionally carry an int receiver,
	//which cannot live in a heap-index slot.
	auto *pOwner = pFunc->Parent();
	if (pOwner && (pOwner->Kind() == NK_ClassDecl
		|| pOwner->Kind() == NK_InterfaceDecl
		|| pOwner->Kind() == NK_EnumDecl))
	{
		env.Log(CLL_Error, idExpr.Location(),
			"cannot reference the method \"%s\" without a receiver.",
			idExpr.Name().c_str());
		return true;
	}
	//Imported stubs carry v1.12 type descriptors for their formals, but
	//Func signatures are outside the descriptor grammar — an imported
	//callee cannot be matched against an expected Func type yet.
	if (pFunc->ContainFlags(NF_Imported))
	{
		env.Log(CLL_Error, idExpr.Location(),
			"cannot reference the imported function \"%s\": parameter "
			"signatures are not serialized.",
			idExpr.Name().c_str());
		return true;
	}
	//Default parameters are filled only on the direct-call path.
	for (auto &param : pFunc->Params())
	{
		if (param.Value())
		{
			env.Log(CLL_Error, idExpr.Location(),
				"functions with default parameters cannot be referenced: "
				"\"%s\".", idExpr.Name().c_str());
			return true;
		}
	}
	return false;
}

bool BindFuncRefToExpected(BuildEnvironment &env, SnIdentifierExpr &idExpr,
	SnField *pExpected)
{
	auto *pFunc = static_cast<SnFunction*>(idExpr.Field());
	assert(pFunc && pFunc->Kind() == NK_Function);
	if (RejectFuncRefTarget(env, idExpr, pFunc, pExpected))
		return false;
	auto *pFuncDecl = static_cast<SnClassDecl*>(pExpected);
	if (!FuncRefMatchesDecl(*pFunc, pFuncDecl))
	{
		env.Log(CLL_Error, idExpr.Location(),
			"function \"%s\" does not match the signature of \"%s\".",
			idExpr.Name().c_str(), pFuncDecl->Name().c_str());
		return false;
	}
	//Bound state is structural: EvalDataType becomes the Func declaration
	//(codegen detects Field()->Kind() == NK_Function in a value position
	//and emits OP_MakeFunc). No dedicated node flag exists — the 24 flag
	//bits are fully allocated.
	idExpr.EvalDataType(pFuncDecl);
	return true;
}

//Phase 13 Step 2: collect every class declaration in the tree under
//(and including) `node` — input of the native-override scan below.
static void CollectClassDecls(SyntaxNode &node,
	std::vector<SnClassDecl*> &out)
{
	for (auto &child : node.Children())
	{
		auto &synChild = static_cast<SyntaxNode&>(child);
		if (synChild.Kind() == NK_ClassDecl)
			out.push_back(static_cast<SnClassDecl*>(&synChild));
		CollectClassDecls(synChild, out);
	}
}

//Phase 13 Step 2: a by-name binding (OP_MakeVFunc) resolves its target at
//runtime, so a NATIVE override anywhere in the method's dispatch domain
//would route the delegate into CallNative, which reads arguments straight
//from callParamBase with no callee frame for the receiver shift. The
//resolved method itself carries NF_Native only when the STATIC receiver
//type declares it — for a virtual base declaration the scan must walk the
//deriving classes, and for an interface declaration the implementers (the
//implements clause is not modeled by SuperClass()).
static bool HasNativeMethodOverride(SnFunction &method)
{
	auto *pOwner = method.Parent();
	if (!pOwner || (pOwner->Kind() != NK_ClassDecl
		&& pOwner->Kind() != NK_InterfaceDecl))
		return false;
	//Is `pClass` inside the dispatch domain of `pOwner`? Class owner:
	//derive from it directly. Interface owner: the class or any ancestor
	//names it in its implements clause.
	auto dispatchesUnder = [](SnClassDecl *pClass, SyntaxNode *pOwner) {
		for (SnClassDecl *cur = pClass; cur; cur = cur->SuperClass())
		{
			if (cur == pOwner)
				return true;
			if (pOwner->Kind() != NK_InterfaceDecl)
				continue;
			for (auto *pIface : cur->ImplementsList())
			{
				if (pIface == pOwner)
					return true;
			}
		}
		return false;
	};
	SyntaxNode *pRoot = pOwner;
	while (pRoot->Parent())
		pRoot = pRoot->Parent();
	std::vector<SnClassDecl*> classes;
	CollectClassDecls(*pRoot, classes);
	for (auto *pClass : classes)
	{
		if (pClass == pOwner)
			continue;
		if (!dispatchesUnder(pClass, pOwner))
			continue;
		for (auto &member : pClass->Members())
		{
			if (member.Kind() == NK_Function
				&& member.Name() == method.Name()
				&& member.ContainFlags(NF_Native))
				return true;
		}
	}
	return false;
}

//Form gates of BindMemberFuncRefToExpected: the expected type must be a
//Func instantiation, and an enum receiver is rejected outright — enum
//values are ints, and slot[1] of a handle (a heap index) cannot carry
//the receiver. True = rejected with the named diagnostic.
static bool RejectMemberFuncRefForm(BuildEnvironment &env,
	SnMemberExpr &snMember, SnIdentifierExpr &inner, SnField *pExpected,
	SnFunction *pMethod)
{
	if (!IsFuncTypeDecl(pExpected))
	{
		env.Log(CLL_Error, snMember.Location(),
			"bound method reference \"%s\" requires an expected function "
			"type.", inner.Name().c_str());
		return true;
	}
	auto *pOwner = pMethod->Parent();
	if (pOwner && pOwner->Kind() == NK_EnumDecl)
	{
		env.Log(CLL_Error, snMember.Location(),
			"cannot reference the enum method \"%s\": enum receivers are "
			"int values, not heap objects.", inner.Name().c_str());
		return true;
	}
	return false;
}

//Signature gates of BindMemberFuncRefToExpected: native methods (and,
//for by-name bindings, any native override in the dispatch domain) have
//no callee frame for the receiver, by-name dispatch cannot bake an out
//mask, and default parameters are filled only on the direct-call path.
//True = rejected with the named diagnostic.
static bool RejectMemberFuncRefSignature(BuildEnvironment &env,
	SnMemberExpr &snMember, SnIdentifierExpr &inner, SnFunction *pMethod,
	SnField *pExpected, bool bDispatchesByName)
{
	if (pMethod->ContainFlags(NF_Native)
		|| (bDispatchesByName && HasNativeMethodOverride(*pMethod)))
	{
		env.Log(CLL_Error, snMember.Location(),
			"cannot reference the native method \"%s\": native calls have "
			"no callee frame for the receiver.", inner.Name().c_str());
		return true;
	}
	//The out mask is compiled from the Func type, but a by-name handle
	//resolves the target at runtime — an overriding method's layout may
	//disagree. Same rationale as the existing virtual-direct-call reject.
	if (bDispatchesByName)
	{
		const auto &outFlags =
			GetGenericOutFlags(static_cast<SnClassDecl*>(pExpected));
		for (size_t k = 1; k < outFlags.size(); ++k)
		{
			if (outFlags[k] != 0)
			{
				env.Log(CLL_Error, snMember.Location(),
					"out parameters are not supported on virtual method "
					"references: dispatch resolves the target at runtime.");
				return true;
			}
		}
	}
	//Default parameters are filled only on the direct-call path (mirrors
	//the free-function reject above).
	for (auto &param : pMethod->Params())
	{
		if (param.Value())
		{
			env.Log(CLL_Error, snMember.Location(),
				"methods with default parameters cannot be referenced: "
				"\"%s\".", inner.Name().c_str());
			return true;
		}
	}
	return false;
}

bool BindMemberFuncRefToExpected(BuildEnvironment &env,
	SnMemberExpr &snMember, SnField *pExpected)
{
	auto &inner = static_cast<SnIdentifierExpr&>(*snMember.Inner());
	auto *pMethod = static_cast<SnFunction*>(inner.Field());
	assert(pMethod && pMethod->Kind() == NK_Function);
	auto *pOwner = pMethod->Parent();
	//Handle form mirrors the direct-call codegen decision exactly:
	//virtual methods and interface declarations dispatch by name at
	//runtime (StatementResolver's implicit virtual propagation means an
	//override of a parent virtual method carries NF_Virtual too).
	bool bDispatchesByName = pMethod->ContainFlags(NF_Virtual)
		|| (pOwner && pOwner->Kind() == NK_InterfaceDecl);
	if (RejectMemberFuncRefForm(env, snMember, inner, pExpected, pMethod))
		return false;
	if (RejectMemberFuncRefSignature(env, snMember, inner, pMethod,
			pExpected, bDispatchesByName))
		return false;
	auto *pFuncDecl = static_cast<SnClassDecl*>(pExpected);
	if (!FuncRefMatchesDecl(*pMethod, pFuncDecl))
	{
		env.Log(CLL_Error, snMember.Location(),
			"method \"%s\" does not match the signature of \"%s\".",
			inner.Name().c_str(), pFuncDecl->Name().c_str());
		return false;
	}
	//Bound state is structural (same as the bare-name form): the member
	//carries the Func declaration while Field() stays the SnFunction;
	//codegen emits receiver + OP_MakeBoundFunc/OP_MakeVFunc. The inner
	//identifier carries the Func type too — the end-of-build sweep (loose
	//predicate) would otherwise flag every bound member reference through
	//it.
	snMember.EvalDataType(pFuncDecl);
	inner.EvalDataType(pFuncDecl);
	return true;
}

//Phase 13: loose pending predicate — true while a bare function
//reference carries a non-Func EvalDataType (its function's return
//type). Review round-1 F1 split this into two predicates: a Func-typed
//EvalDataType does NOT prove a binding (it may be the function's Func
//RETURN type leaking through ResolveFieldExprAs), so all bind sites use
//the strict IsUnboundFuncRef below. This loose form survives only for
//the end-of-build sweep (ModuleBuilder::SweepPendingFuncRefs): a ref a
//Func-accepting consumer already handled via MakeFunc (e.g. io.print of
//a Func-returning function's bare name) must not be re-flagged there.
bool IsPendingFuncRef(SyntaxNode &expr)
{
	if (expr.Kind() != NK_IdentifierExpr)
		return false;
	auto &idExpr = static_cast<SnIdentifierExpr&>(expr);
	return idExpr.Field() && idExpr.Field()->Kind() == NK_Function
		&& !IsFuncTypeDecl(idExpr.EvalDataType());
}

//Phase 13 (review round-1 F1): strict bind-site predicate. A bare name
//is bound only when its OWN signature satisfies the Func type it
//carries. Without this, `Func<int,int> f = pick;` (where pick RETURNS
//Func<int,int> but takes no parameters) was misread as an already-bound
//reference and silently compiled into a wrong-signature handle.
//Binding is idempotent when the expected type matches, and a genuine
//mismatch gets BindFuncRefToExpected's named diagnostic.
bool IsUnboundFuncRef(SyntaxNode &expr)
{
	if (expr.Kind() != NK_IdentifierExpr)
		return false;
	auto &idExpr = static_cast<SnIdentifierExpr&>(expr);
	if (!idExpr.Field() || idExpr.Field()->Kind() != NK_Function)
		return false;
	auto *pType = idExpr.EvalDataType();
	if (!IsFuncTypeDecl(pType))
		return true;
	return !FuncRefMatchesDecl(
		*static_cast<SnFunction*>(idExpr.Field()),
		static_cast<SnClassDecl*>(pType));
}

//Phase 13 Step 2: strict bind-site predicate for receiver-bound method
//references — `receiver.name` in a value position whose inner name
//resolved to a method of a class/interface/enum. Unbound while the
//member's own signature does not satisfy the Func type it carries (the
//same round-1 F1 discipline as IsUnboundFuncRef: a Func RETURN type
//leaking through the member tail is not a binding). Struct methods and
//module functions never match — they stay on their existing channels.
bool IsUnboundMemberFuncRef(SyntaxNode &expr)
{
	if (expr.Kind() != NK_MemberExpr)
		return false;
	auto &snMember = static_cast<SnMemberExpr&>(expr);
	auto *pInner = snMember.Inner();
	if (!pInner || pInner->Kind() != NK_IdentifierExpr)
		return false;
	auto *pMethod = static_cast<SnIdentifierExpr*>(pInner)->Field();
	if (!pMethod || pMethod->Kind() != NK_Function)
		return false;
	auto *pOwner = pMethod->Parent();
	if (!pOwner || (pOwner->Kind() != NK_ClassDecl
		&& pOwner->Kind() != NK_InterfaceDecl
		&& pOwner->Kind() != NK_EnumDecl))
		return false;
	auto *pType = snMember.EvalDataType();
	if (IsFuncTypeDecl(pType)
		&& FuncRefMatchesDecl(*static_cast<SnFunction*>(pMethod),
			static_cast<SnClassDecl*>(pType)))
		return false;
	return true;
}

} //namespace nlang
