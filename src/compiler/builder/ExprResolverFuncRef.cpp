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
#include <nlang/runtime/StackDump.h>

namespace nlang
{

//Phase 13: exact-signature comparison between a function declaration and
//a func<...> instantiation (no variance). Return slot: a nullptr return
//type matches a void type argument. Parameter slots: declared type field
//pointer identity — the same discipline that keeps distinct spellings
//distinct in GenericInstKey.
//v0.13.9 phase 3: the & marks join the agreement (return-slot NF_Ref
//against refFlags[0], per-parameter against refFlags[i+1]) — no silent
//binding across a & spelling. Array-ness needs no separate comparison:
//the interned token's pointer identity rules the pairing (0.7.3 B).
bool FuncRefMatchesDecl(const SnFunction &func, SnClassDecl *pFuncDecl)
{
	const auto &typeArgs = GetGenericTypeArgs(pFuncDecl);
	const auto &refFlags = GetGenericRefFlags(pFuncDecl);
	auto *pRet = func.ReturnType();
	if (pRet)
	{
		if (!pRet->IsResolved() || pRet->Field() != typeArgs[0])
			return false;
		if (pRet->ContainFlags(NF_Ref) != (refFlags[0] != 0))
			return false;
	}
	else if (typeArgs[0]->Kind() != NK_Void || refFlags[0] != 0)
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
		if (param.ContainFlags(NF_Ref) != (refFlags[i + 1] != 0))
			return false;
		++i;
	}
	return true;
}

//0.13.10 Task 5: the BARE spelling of a generic-class static inside its
//own class body (`tag` in a Tag<T> method) keeps the 0.13.1a named
//reject — the free channel has no class-instantiation context to pair
//the descriptor row with; the member spellings (Tag.tag, Tag<int>.tag)
//bind through BindMemberFuncRefToExpected instead.
static bool RejectBareGenericStaticRef(BuildEnvironment &env,
	const ISourceLocation *loc, const std::string &name,
	SyntaxNode *pOwner)
{
	if (!pOwner || pOwner->Kind() != NK_ClassDecl)
		return false;
	auto *pDecl = static_cast<SnClassDecl*>(pOwner);
	if (!pDecl->IsGenericDeclaration()
		&& !(pDecl->IsGenericInstantiation()
			&& env.Registry().DeclaredGenerics().InstanceOf(pDecl)))
		return false;
	env.Log(CLL_Error, loc,
		"a function of a generic class cannot be referenced as a "
		"value; call it directly: \"%s\".", name.c_str());
	return true;
}

//Rejection gates of BindFuncRefToExpected: the expected type must be a
//func instantiation, the callee a free function (not a method) and one
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
	//which cannot live in a heap-index slot. 0.9.14 D-new-2: a static
	//method inside its owning class is a receiverless target — the bare
	//form binds exactly like a free function.
	auto *pOwner = pFunc->Parent();
	if (!pFunc->ContainFlags(NF_Static)
		&& pOwner && (pOwner->Kind() == NK_ClassDecl
		|| pOwner->Kind() == NK_InterfaceDecl
		|| pOwner->Kind() == NK_EnumDecl))
	{
		env.Log(CLL_Error, idExpr.Location(),
			"cannot reference the method \"%s\" without a receiver.",
			idExpr.Name().c_str());
		return true;
	}
	if (RejectBareGenericStaticRef(env, idExpr.Location(),
			idExpr.Name(), pOwner))
		return true;
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
	NLANG_ASSERT(pFunc && pFunc->Kind() == NK_Function);
	if (RejectFuncRefTarget(env, idExpr, pFunc, pExpected))
		return false;
	auto *pFuncDecl = static_cast<SnClassDecl*>(pExpected);
	if (!FuncRefMatchesDecl(*pFunc, pFuncDecl))
	{
		//0.13.10 Task 5: a generic free function binds through the
		//expected type's instantiation — signature unification fills
		//the type arguments (the value twin of the invoke channel);
		//the handle pairs the template row with the composed descriptor
		//row at emission. The stamp doubles as the bind predicates'
		//binding proof.
		std::vector<SnField*> inferredArgs;
		if (!UnifyGenericFuncRefAgainst(*pFunc, pFuncDecl,
				env.Registry().DeclaredGenerics(), inferredArgs))
		{
			env.Log(CLL_Error, idExpr.Location(),
				"function \"%s\" does not match the signature of \"%s\".",
				idExpr.Name().c_str(), pFuncDecl->Name().c_str());
			return false;
		}
		idExpr.SetInferredTypeArgs(std::move(inferredArgs));
	}
	//Bound state is structural: EvalDataType becomes the func declaration
	//(codegen detects Field()->Kind() == NK_Function in a value position
	//and emits OP_MakeFunc); all 24 node flag bits are allocated.
	idExpr.EvalDataType(pFuncDecl);
	return true;
}

//Phase 13 Step 2: collect every class declaration in the tree under
//(and including) `node` — input of the native-override scan below.
void CollectClassDecls(SyntaxNode &node,
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

//Phase 13: loose pending predicate — true while a bare function
//reference carries a non-func EvalDataType (its function's return
//type). Review round-1 F1 split this into two predicates: a func-typed
//EvalDataType does NOT prove a binding (it may be the function's func
//RETURN type leaking through ResolveFieldExprAs), so all bind sites use
//the strict IsUnboundFuncRef below. This loose form survives only for
//the end-of-build sweep (ModuleBuilder::SweepPendingFuncRefs): a ref a
//func-accepting consumer already handled via MakeFunc (e.g. io.print of
//a func-returning function's unqualified name) must not be re-flagged there.
bool IsPendingFuncRef(SyntaxNode &expr)
{
	if (expr.Kind() != NK_IdentifierExpr)
		return false;
	auto &idExpr = static_cast<SnIdentifierExpr&>(expr);
	return idExpr.Field() && idExpr.Field()->Kind() == NK_Function
		&& !IsFuncTypeDecl(idExpr.EvalDataType());
}

//Phase 13 (review round-1 F1): strict bind-site predicate. An unqualified name
//is bound only when its OWN signature satisfies the func type it
//carries. Without this, `func<int,int> f = pick;` (where pick RETURNS
//func<int,int> but takes no parameters) was misread as an already-bound
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
	//0.13.10 Task 5: a generic declaration never satisfies a concrete
	//func type by direct signature comparison — the stamped inferred
	//arguments are the binding proof (the unify channel).
	if (static_cast<SnFunction*>(idExpr.Field())->IsGenericDeclaration())
		return idExpr.InferredTypeArgs().empty();
	return !FuncRefMatchesDecl(
		*static_cast<SnFunction*>(idExpr.Field()),
		static_cast<SnClassDecl*>(pType));
}

//Phase 13 Step 2: strict bind-site predicate for receiver-bound method
//references — `receiver.name` in a value position whose inner name
//resolved to a method of a class/interface/enum. Unbound while the
//member's own signature does not satisfy the func type it carries (the
//same round-1 F1 discipline as IsUnboundFuncRef: a func RETURN type
//leaking through the member tail is not a binding). Struct methods stay
//on their existing channels. Module-level free functions
//(`lib.name` — namespace owner, or no parent at all for a detached
//imported stub) join this channel since signatures serialize: the
//bind sites and the overload-distance machinery are shared, only the
//codegen arm differs (no receiver, static OP_MakeFunc).
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
	if (pOwner && !IsModuleOwnedFunction(*pMethod)
		&& pOwner->Kind() != NK_ClassDecl
		&& pOwner->Kind() != NK_InterfaceDecl
		&& pOwner->Kind() != NK_EnumDecl)
		return false;
	auto *pType = snMember.EvalDataType();
	//0.13.10 Task 5: a member-channel generic free function (lib.pick)
	//binds through the unify channel — the stamped arguments are the
	//proof. (The inferred class-static arm rebinds to the substituted
	//clone, whose signature satisfies the direct comparison.)
	if (IsFuncTypeDecl(pType)
		&& static_cast<SnFunction*>(pMethod)->IsGenericDeclaration()
		&& !snMember.InferredTypeArgs().empty())
		return false;
	//0.13.10 Task 5: a receiverless static of a generic CLASS never
	//satisfies a concrete func type structurally — even a T-less
	//signature (tag() against func<string>) leaves the instantiation
	//underdetermined (the CS0411 shape), so the binder's clone arm
	//always runs. After the arm's rebind Field() is the substituted
	//clone, whose owner is the instantiation: this guard no longer
	//fires and the direct compare below judges it.
	auto *pMOwner = pMethod->Parent();
	if (pMOwner && pMOwner->Kind() == NK_ClassDecl
		&& static_cast<SnClassDecl*>(pMOwner)->IsGenericDeclaration()
		&& IsReceiverlessFuncRefTarget(*pMethod))
		return true;
	if (IsFuncTypeDecl(pType)
		&& FuncRefMatchesDecl(*static_cast<SnFunction*>(pMethod),
			static_cast<SnClassDecl*>(pType)))
		return false;
	return true;
}

//Init-list entry slots: a pending function reference in a container
//literal ([dbl]) binds against the slot's element type here — the
//entry positions have no overload set to score them against. Moved
//from ExprResolverInitList.cpp (source-size budget; the predicates
//and binders live in this file).
static void BindEntrySlotFuncRef(BuildEnvironment &env,
    ExprResolveAccessor &accessor, SnExpression *pExpr, SnField *pSlotType)
{
    if (!pExpr || !pSlotType)
        return;
    if (IsUnboundFuncRef(*pExpr))
    {
        BindFuncRefToExpected(env,
            *static_cast<SnIdentifierExpr*>(pExpr), pSlotType);
    }
    else if (IsUnboundMemberFuncRef(*pExpr))
    {
        BindMemberFuncRefToExpected(env,
            *static_cast<SnMemberExpr*>(pExpr), pSlotType, &accessor);
    }
}

void ExprResolveAccessor::BindInitListFuncRefs(SnInitListExpr &sn,
    const InitListSlotTypes &slots)
{
    for (auto &entry : sn.Entries())
    {
        BindEntrySlotFuncRef(m_Env, *this, entry.pKey, slots.pKeyType);
        BindEntrySlotFuncRef(m_Env, *this, entry.pValue, slots.pValueType);
    }
}

} //namespace nlang
