/*---
    ExprResolverFuncRefGeneric.cpp — 泛型静态引用的unify与clone绑定（0.13.10抽取）。
---*/
#include "ExprResolver.h"
#include "SnMisc.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "ModuleRegistry.h"
#include <vector>

namespace nlang
{

//0.13.10 Task 5: signature-slot unification of one declared type node
//against the expected func instantiation's slot. A declared slot that
//IS a type-parameter placeholder binds its ordinal to the expected
//field (a conflicting rebinding fails); any other slot demands field-
//pointer identity — the FuncRefMatchesDecl discipline. Composite shapes
//(T[] elements, nested instantiations) keep the exact-match failure:
//the func-value channel stays conservative, the recursive substitution
//shapes stay with the invoke channel's InferTypeArgsForCall.
//bDeclaredRef/bExpectedRef carry the & marks (FuncRefMatchesDecl's
//return-slot-vs-refFlags rule, generalized).
static bool UnifyFuncRefSlot(GenericInstanceRegistry &generics,
	SnField *pDeclared, bool bDeclaredRef, SnField *pExpected,
	bool bExpectedRef, std::vector<SnField*> &bindings)
{
	if (bDeclaredRef != bExpectedRef)
		return false;
	if (pDeclared && generics.IsPlaceholder(pDeclared))
	{
		const size_t ordinal = generics.PlaceholderIndex(pDeclared);
		NLANG_ASSERT(ordinal < bindings.size());
		if (bindings[ordinal] && bindings[ordinal] != pExpected)
			return false;
		bindings[ordinal] = pExpected;
		return true;
	}
	return pDeclared == pExpected;
}

//0.13.10 Task 5: unify a GENERIC declaration's signature against the
//expected func instantiation — the value-binding twin of the invoke
//channel's inference. The return slot walks typeArgs[0], parameter i
//walks typeArgs[i + 1]; every type parameter must bind exactly once
//(unbound ordinals and arity mismatches fail). On failure the binder
//keeps its existing "does not match the signature" diagnostic — the
//spec adds no new generic wording here. True = outArgs filled, indexed
//by type-parameter ordinal. (Declared in ExprResolver.h:
//ExprResolverBinding.cpp's FuncRefBindingDistance shares it.)
bool UnifyGenericFuncRefAgainst(const SnFunction &func,
	SnClassDecl *pFuncDecl, GenericInstanceRegistry &generics,
	std::vector<SnField*> &outArgs)
{
	if (!func.IsGenericDeclaration())
		return false;
	const auto &typeArgs = GetGenericTypeArgs(pFuncDecl);
	const auto &refFlags = GetGenericRefFlags(pFuncDecl);
	outArgs.assign(func.TypeParamNames().size(), nullptr);
	auto *pRet = func.ReturnType();
	if (pRet)
	{
		if (!pRet->IsResolved()
			|| !UnifyFuncRefSlot(generics, pRet->Field(),
				pRet->ContainFlags(NF_Ref) != 0, typeArgs[0],
				refFlags[0] != 0, outArgs))
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
		if (!UnifyFuncRefSlot(generics, param.EvalDataType(),
				param.ContainFlags(NF_Ref) != 0, typeArgs[i + 1],
				refFlags[i + 1] != 0, outArgs))
			return false;
		++i;
	}
	for (SnField *pArg : outArgs)
	{
		if (!pArg)
			return false;
	}
	return true;
}

//0.13.10 Task 5: unify a generic CLASS's type parameters from the
//expected func instantiation's slots — the inferred static-reference
//arm (Tag.tag against func<string>). The class template's placeholders
//in the method's signature bind from the func slots (return slot, then
//each parameter); every class parameter must bind. True = outBindings
//filled, indexed by the CLASS's type-parameter ordinal. On failure with
//rbUnderdetermined set, the slots unified cleanly but the signature
//mentions none of the class's parameters — the expected type gives no
//binding (the CS0411 precedent; the explicit spelling names it).
//(Declared in ExprResolver.h: ExprResolverBinding.cpp's
//FuncRefBindingDistance shares it.)
bool UnifyClassStaticRefAgainst(const SnFunction &method,
	SnClassDecl &tmpl, SnClassDecl *pFuncDecl,
	GenericInstanceRegistry &generics, std::vector<SnField*> &outBindings,
	bool &rbUnderdetermined)
{
	rbUnderdetermined = false;
	const auto &typeArgs = GetGenericTypeArgs(pFuncDecl);
	const auto &refFlags = GetGenericRefFlags(pFuncDecl);
	outBindings.assign(tmpl.TypeParamNames().size(), nullptr);
	auto *pRet = method.ReturnType();
	if (pRet)
	{
		if (!pRet->IsResolved()
			|| !UnifyFuncRefSlot(generics, pRet->Field(),
				pRet->ContainFlags(NF_Ref) != 0, typeArgs[0],
				refFlags[0] != 0, outBindings))
			return false;
	}
	else if (typeArgs[0]->Kind() != NK_Void || refFlags[0] != 0)
	{
		return false;
	}
	const auto &params = method.Params();
	if (params.size() + 1 != typeArgs.size())
		return false;
	size_t i = 0;
	for (auto &param : params)
	{
		if (!UnifyFuncRefSlot(generics, param.EvalDataType(),
				param.ContainFlags(NF_Ref) != 0, typeArgs[i + 1],
				refFlags[i + 1] != 0, outBindings))
			return false;
		++i;
	}
	for (SnField *pArg : outBindings)
	{
		if (!pArg)
		{
			rbUnderdetermined = true;
			return false;
		}
	}
	return true;
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
			//0.9.10 D2.4: only an instance member with the same parameter
			//list can occupy the dispatch slot — a same-name static or
			//differently-shaped overload must not veto the binding.
			if (member.Kind() == NK_Function
				&& member.Name() == method.Name()
				&& !member.ContainFlags(NF_Static)
				&& member.ContainFlags(NF_Native)
				&& static_cast<SnFunction&>(member).ConflictedWith(method))
				return true;
		}
	}
	return false;
}

//Form gates of BindMemberFuncRefToExpected: the expected type must be a
//func instantiation, and an enum receiver is rejected outright — enum
//values are ints, and slot[1] of a handle (a heap index) cannot carry
//the receiver. True = rejected with the named diagnostic.
static bool RejectMemberFuncRefForm(BuildEnvironment &env,
	SnMemberExpr &snMember, SnIdentifierExpr &inner, SnField *pExpected,
	SnFunction *pMethod)
{
	if (!IsFuncTypeDecl(pExpected))
	{
		env.Log(CLL_Error, snMember.Location(),
			IsReceiverlessFuncRefTarget(*pMethod)
				? "function reference \"%s\" requires an expected function "
				  "type."
				: "bound method reference \"%s\" requires an expected "
				  "function type.",
			inner.Name().c_str());
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

//By-name dispatch cannot bake late-bound marks: the & marks are
//compiled from the func type, but the handle resolves its target at
//runtime — an override's layout may disagree (the virtual-direct-call
//rationale). The RETURN slot can carry a & mark (func<int&>), so the
//ref scan starts at 0. v0.13.9 phase 4: the out half is gone with the
//out mechanism. True = rejected.
static bool RejectByNameLateBoundMarks(BuildEnvironment &env,
	const ISourceLocation *pLoc, SnField *pExpected)
{
	auto *pFunc = static_cast<SnClassDecl *>(pExpected);
	const auto &refFlags = GetGenericRefFlags(pFunc);
	for (size_t k = 0; k < refFlags.size(); ++k)
	{
		if (refFlags[k] != 0)
		{
			env.Log(CLL_Error, pLoc,
				"reference parameters and reference returns are not "
				"supported on virtual method references: dispatch "
				"resolves the target at runtime.");
			return true;
		}
	}
	return false;
}

//Signature gates of BindMemberFuncRefToExpected: native methods (and,
//for by-name bindings, any native override in the dispatch domain) have
//no callee frame for the receiver, by-name dispatch cannot bake an out
//mask, and default parameters are filled only on the direct-call path.
//Receiverless targets skip the receiver-frame and mark arms — a native
//free function or static method is referenceable exactly like an
//unqualified-name one. True = rejected with the named diagnostic.
static bool RejectMemberFuncRefSignature(BuildEnvironment &env,
	SnMemberExpr &snMember, SnIdentifierExpr &inner, SnFunction *pMethod,
	SnField *pExpected)
{
	//Handle form mirrors the direct-call codegen decision exactly:
	//virtual methods and interface declarations dispatch by name at
	//runtime (StatementResolver's implicit virtual propagation means an
	//override of a parent virtual method carries NF_Virtual too).
	//Receiverless targets have no receiver and always take the static
	//handle — dispatch never resolves by name.
	auto *pOwner = pMethod->Parent();
	bool bDispatchesByName = !IsReceiverlessFuncRefTarget(*pMethod)
		&& (pMethod->ContainFlags(NF_Virtual)
			|| (pOwner && pOwner->Kind() == NK_InterfaceDecl));
	if (!IsReceiverlessFuncRefTarget(*pMethod))
	{
		if (pMethod->ContainFlags(NF_Native)
			|| (bDispatchesByName && HasNativeMethodOverride(*pMethod)))
		{
			env.Log(CLL_Error, snMember.Location(),
				"cannot reference the native method \"%s\": native calls "
				"have no callee frame for the receiver.",
				inner.Name().c_str());
			return true;
		}
		if (bDispatchesByName
			&& RejectByNameLateBoundMarks(env, snMember.Location(),
				pExpected))
			return true;
	}
	//Default parameters are filled only on the direct-call path (mirrors
	//the free-function reject of the unqualified-name channel).
	for (auto &param : pMethod->Params())
	{
		if (param.Value())
		{
			env.Log(CLL_Error, snMember.Location(),
				IsReceiverlessFuncRefTarget(*pMethod)
					? "functions with default parameters cannot be "
					  "referenced: \"%s\"."
					: "methods with default parameters cannot be "
					  "referenced: \"%s\".",
				inner.Name().c_str());
			return true;
		}
	}
	return false;
}

//The CS0411 shape's diagnostic: name the first unbound class
//parameter; point at the explicit spelling.
static void LogCannotInferTypeArg(BuildEnvironment &env,
	SnMemberExpr &snMember, const SnFunction &tmplMethod,
	const SnClassDecl &tmpl, const std::vector<SnField*> &classArgs)
{
	size_t ord = 0;
	while (ord < classArgs.size() && classArgs[ord])
		++ord;
	const auto &names = tmpl.TypeParamNames();
	const char *pName = ord < names.size() ? names[ord].c_str() : "T";
	env.Log(CLL_Error, snMember.Location(),
		"cannot infer type argument for %s: the signature of "
		"\"%s\" does not mention it; name the instantiation "
		"explicitly (e.g. %s<%s>.%s).",
		pName, tmplMethod.Name().c_str(), tmpl.Name().c_str(),
		pName, tmplMethod.Name().c_str());
}

//Locate the instantiation's clone recorded against the template method.
static SnFunction *FindInstantiationClone(
	GenericInstanceRegistry &generics, SnClassDecl *pInst,
	const SnFunction &tmplMethod)
{
	if (!pInst)
		return nullptr;
	for (auto &member : pInst->Members())
	{
		if (member.Kind() == NK_Function
			&& member.Name() == tmplMethod.Name()
			&& generics.RecordOwnerOfMember(&member) == &tmplMethod)
			return static_cast<SnFunction*>(&member);
	}
	return nullptr;
}

//0.13.10 Task 5: the inferred static-reference arm (declaration in
//ExprResolver.h). Unify + create + clone rebind, with the friend
//channel writing the pair.
bool ExprResolveAccessor::BindInferredGenericStaticClone(
	BuildEnvironment &env, SnMemberExpr &snMember, SnFunction &tmplMethod,
	SnClassDecl &tmpl, SnClassDecl *pFuncDecl)
{
	auto &inner = static_cast<SnIdentifierExpr&>(*snMember.Inner());
	auto &generics = env.Registry().DeclaredGenerics();
	std::vector<SnField*> classArgs;
	bool bUnderdetermined = false;
	if (!UnifyClassStaticRefAgainst(tmplMethod, tmpl, pFuncDecl,
			generics, classArgs, bUnderdetermined))
	{
		if (bUnderdetermined)
			LogCannotInferTypeArg(env, snMember, tmplMethod, tmpl,
				classArgs);
		else
			env.Log(CLL_Error, snMember.Location(),
				"function \"%s\" does not match the signature of \"%s\".",
				inner.Name().c_str(), pFuncDecl->Name().c_str());
		return false;
	}
	auto *pInst = GetDeclaredGenericClassDecl(tmpl, classArgs);
	auto *pClone = FindInstantiationClone(generics, pInst, tmplMethod);
	if (!pClone)
		return false;
	//Friend-channel rebind: the inner name and the member both read the
	//clone — the explicit-spelling shape. The binding end stamps the
	//func type; the clone's substituted signature satisfies the strict
	//predicates without a stamp.
	inner.m_pField = pClone;
	snMember.m_pField = pClone;
	return true;
}

//A receiverless generic-class static routed through the accessor's
//clone arm: unify the CLASS's parameters, rebind to the clone member.
static bool BindGenericStaticClone(
	ExprResolveAccessor *pAccessor, BuildEnvironment &env,
	SnMemberExpr &snMember, SnFunction &pMethod, SnClassDecl &tmplOwner,
	SnClassDecl *pFuncDecl)
{
	NLANG_ASSERT(pAccessor);
	return pAccessor
		&& pAccessor->BindInferredGenericStaticClone(env, snMember,
			pMethod, tmplOwner, pFuncDecl);
}

//The mismatch wording: module forms say "function"; methods say "method".
static void LogMemberSignatureMismatch(BuildEnvironment &env,
	SnMemberExpr &snMember, SnIdentifierExpr &inner, SnFunction &pMethod,
	SnClassDecl *pFuncDecl)
{
	env.Log(CLL_Error, snMember.Location(),
		IsReceiverlessFuncRefTarget(pMethod)
			? "function \"%s\" does not match the signature of \"%s\"."
			: "method \"%s\" does not match the signature of \"%s\".",
		inner.Name().c_str(), pFuncDecl->Name().c_str());
}

//0.13.10 Task 5: a module-qualified generic free function (lib.pick)
//unifies through the same channel as the unqualified form; the stamp
//drives the emission's descriptor row.
static bool UnifyFreeGenericStaticRef(BuildEnvironment &env,
	SnMemberExpr &snMember, SnFunction &pMethod, SnClassDecl *pFuncDecl)
{
	auto &inner = static_cast<SnIdentifierExpr&>(*snMember.Inner());
	auto &generics = env.Registry().DeclaredGenerics();
	std::vector<SnField*> inferredArgs;
	if (!UnifyGenericFuncRefAgainst(pMethod, pFuncDecl, generics,
			inferredArgs))
	{
		env.Log(CLL_Error, snMember.Location(),
			"function \"%s\" does not match the signature of \"%s\".",
			inner.Name().c_str(), pFuncDecl->Name().c_str());
		return false;
	}
	snMember.SetInferredTypeArgs(std::move(inferredArgs));
	return true;
}

//0.13.10 Task 5: a generic-class inferred static (Tag.tag) routes
//through the clone arm on BOTH match outcomes. Non-null = the
//receiverless static sits in a generic class template.
static SnClassDecl *GenericClassTemplateOwner(SnFunction *pMethod)
{
	auto *pOwner = pMethod->Parent();
	return (pOwner && pOwner->Kind() == NK_ClassDecl
		&& static_cast<SnClassDecl*>(pOwner)->IsGenericDeclaration()
		&& IsReceiverlessFuncRefTarget(*pMethod))
		? static_cast<SnClassDecl*>(pOwner) : nullptr;
}

bool BindMemberFuncRefToExpected(BuildEnvironment &env,
	SnMemberExpr &snMember, SnField *pExpected,
	ExprResolveAccessor *pAccessor)
{
	auto &inner = static_cast<SnIdentifierExpr&>(*snMember.Inner());
	auto *pMethod = static_cast<SnFunction*>(inner.Field());
	NLANG_ASSERT(pMethod && pMethod->Kind() == NK_Function);
	if (RejectMemberFuncRefForm(env, snMember, inner, pExpected, pMethod))
		return false;
	if (RejectMemberFuncRefSignature(env, snMember, inner, pMethod,
			pExpected))
		return false;
	auto *pFuncDecl = static_cast<SnClassDecl*>(pExpected);
	//0.13.10 Task 5: the 0.13.1a generic-record reject retired — generic
	//targets bind through the expected type's instantiation; a matched
	//generic-class static is still underdetermined (tag() against
	//func<string>). Every arm falls through to the common stamp.
	auto *pTmplOwner = GenericClassTemplateOwner(pMethod);
	if (!FuncRefMatchesDecl(*pMethod, pFuncDecl))
	{
		if (pMethod->IsGenericDeclaration())
		{
			if (!UnifyFreeGenericStaticRef(env, snMember, *pMethod,
					pFuncDecl))
				return false;
		}
		else if (pTmplOwner)
		{
			if (!BindGenericStaticClone(pAccessor, env, snMember,
					*pMethod, *pTmplOwner, pFuncDecl))
				return false;
		}
		else
		{
			LogMemberSignatureMismatch(env, snMember, inner, *pMethod,
				pFuncDecl);
			return false;
		}
	}
	else if (pTmplOwner && !BindGenericStaticClone(pAccessor, env,
			snMember, *pMethod, *pTmplOwner, pFuncDecl))
		return false;
	//Bound state is structural (same as the unqualified-name form): the
	//member carries the func declaration while Field() stays the
	//SnFunction; the inner identifier carries the func type too.
	snMember.EvalDataType(pFuncDecl);
	inner.EvalDataType(pFuncDecl);
	return true;
}

} //namespace nlang
