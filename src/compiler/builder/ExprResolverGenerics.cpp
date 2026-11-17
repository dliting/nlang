/*--- ExprResolverGenerics.cpp — 合成内建泛型注册表与成员调用的元素实参
    包装（声明泛型的实例化机器在 ExprResolverGenericInstantiation.cpp）。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnData.h"
#include "SnArrayTypeToken.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include "ModuleRegistry.h"
#include <nlang/compiler/GenericInstanceRegistry.h>
#include <nlang/compiler/TypeArgInference.h>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace nlang
{


//==================== synthetic builtin registry ====================
//
//Definitions for SyntheticBuiltinRegistry (owned by ModuleRegistry —
//one per build; see the header). The creating free functions below it
//keep the name/arity logic; this block is storage and ownership only,
//the same split GenericInstanceRegistry keeps with
//GetDeclaredGenericClassDecl.

SyntheticBuiltinRegistry::~SyntheticBuiltinRegistry()
{
	//The registry owns every node it created. The synthetic decls are
	//detached from any tree, so nothing else frees them.
	for (auto* pDecl : m_created)
		delete pDecl;
}

void SyntheticBuiltinRegistry::Own(SnClassDecl* pDecl)
{
	m_created.push_back(pDecl);
}

SnClassDecl* SyntheticBuiltinRegistry::FindGeneric(
	const GenericInstKey& key) const
{
	auto it = m_genericInstances.find(key);
	return it != m_genericInstances.end() ? it->second : nullptr;
}

void SyntheticBuiltinRegistry::AdoptGeneric(const GenericInstKey& key,
	SnClassDecl* pDecl)
{
	m_genericInstances[key] = pDecl;
	Own(pDecl);
}

SnClassDecl* SyntheticBuiltinRegistry::FindBuiltinClass(
	const std::string& name) const
{
	auto it = m_builtinClasses.find(name);
	return it != m_builtinClasses.end() ? it->second : nullptr;
}

void SyntheticBuiltinRegistry::AdoptBuiltinClass(const std::string& name,
	SnClassDecl* pDecl)
{
	m_builtinClasses[name] = pDecl;
	Own(pDecl);
}

SnClassDecl* SyntheticBuiltinRegistry::FindFaceClass(
	const std::string& typeName) const
{
	auto it = m_faceClasses.find(typeName);
	return it != m_faceClasses.end() ? it->second : nullptr;
}

void SyntheticBuiltinRegistry::AdoptFaceClass(const std::string& typeName,
	SnClassDecl* pDecl)
{
	m_faceClasses[typeName] = pDecl;
	Own(pDecl);
}

//Returns true if class decl is a synthetic generic instantiation
//(e.g., List<int>). Used to dispatch member calls in Access(SnMemberExpr&).
bool IsGenericClassDecl(SnClassDecl* pClass)
{
	return pClass && pClass->IsGenericInstantiation();
}

//Creates (or fetches) a synthetic SnClassDecl for the given generic
//instantiation. Phase 8e-3: List<T> (arity 1). Phase 8e-4: Dict<K,V> (arity 2).
//Phase 13: func<R, P...> (variadic, >= 1). outFlags and refFlags are
//normalized here so callers that infer instantiations (func handle
//coercion) can pass empty vectors while declaration-path keys stay
//distinct.
//The cache is per-build (ModuleRegistry's SyntheticBuiltinRegistry):
//the key is built from interned type-arg pointers, which only keep
//identity within one build's syntax tree.
//Display name e.g. "List<int>"; array-typed arguments are interned
//tokens with empty Name(), so render them via ToString ("Int32[]").
static std::string RenderGenericInstName(const std::string& baseName,
	const std::vector<SnField*>& typeArgs)
{
	std::string instName = baseName + "<";
	for (size_t i = 0; i < typeArgs.size(); ++i) {
		if (i) instName += ", ";
		instName += typeArgs[i]->Kind() == NK_ArrayTypeToken
			? typeArgs[i]->ToString() : typeArgs[i]->Name();
	}
	instName += ">";
	return instName;
}

SnClassDecl* GetGenericClassDecl(ModuleRegistry& reg,
	const std::string& baseName,
	const std::vector<SnField*>& typeArgs, const std::vector<uint8>& outFlags,
	const std::vector<uint8>& refFlags, const ISourceLocation* pLoc)
{
	std::vector<uint8> normOutFlags(outFlags);
	normOutFlags.resize(typeArgs.size(), 0);
	//v0.13.9 phase 3: the & marks share the out flags' normalization —
	//short caller vectors pad to per-argument zeros so the key stays
	//position-stable.
	std::vector<uint8> normRefFlags(refFlags);
	normRefFlags.resize(typeArgs.size(), 0);
	SyntheticBuiltinRegistry::GenericInstKey key{baseName, typeArgs,
		normOutFlags, normRefFlags};
	auto& synthetics = reg.SyntheticBuiltins();
	if (SnClassDecl* pCached = synthetics.FindGeneric(key))
		return pCached;

	//Phase-2 D4: func is the only synthetic creation left. The four generic
	//prelude names and every user template resolve through
	//GetDeclaredGenericClassDecl; a miss here is a caller bug, so the
	//admission rejects everything but func (variadic: first argument is
	//the return type, the rest are params).
	if (baseName != kBuiltinFuncTypeName || typeArgs.empty())
		return nullptr;

	const std::string instName = RenderGenericInstName(baseName, typeArgs);

	auto* pName = new std::string(instName);
	auto* pMembers = new PtrList<SnField>();
	ScriptLocation fallback;
	auto* pClass = new SnClassDecl(pName, nullptr, pMembers,
		DeclLocation(pLoc, fallback));
	pClass->SetBuiltinClass();
	pClass->SetGenericInstantiation();
	//v0.13.9 phase 3: the func arms own the & type-arg spelling; a &
	//container argument is a grammar bypass (assert-grade).
	if (baseName != kBuiltinFuncTypeName)
		AssertNoRefTypeArgs(typeArgs);
	pClass->SetGenericTypeArgs(typeArgs);
	pClass->SetGenericOutFlags(normOutFlags);
	pClass->SetGenericRefFlags(normRefFlags);
	pClass->SetBaseName(baseName);
	synthetics.AdoptGeneric(key, pClass);
	return pClass;
}

//D7 helper: find a DECLARED generic template by unqualified name in the scope
//chain and instantiate it (the three infer-site creators that used to
//call GetGenericClassDecl directly). Null = the name does not address
//a generic declaration in scope — callers keep their own fallback or
//diagnostic; silent on purpose (probe semantics).
SnClassDecl *ExprResolveAccessor::FindDeclaredGenericInstance(
	const std::string &baseName, const std::vector<SnField*> &typeArgs)
{
	if (!m_pContext)
		return nullptr;
	auto *pProbed = FindTypeNameInAncestor(baseName, *m_pContext,
		*m_pAccessor, false);
	if (!pProbed || pProbed->Kind() != NK_ClassDecl
		|| !static_cast<SnClassDecl*>(pProbed)->IsGenericDeclaration())
		return nullptr;
	auto &tmpl = *static_cast<SnClassDecl*>(pProbed);
	if (typeArgs.size() != tmpl.TypeParamNames().size())
		return nullptr;
	return GetDeclaredGenericClassDecl(tmpl, typeArgs);
}

//Phase-2 builtin-decls: element-typed actuals of a member invoke on a
//declared-generic receiver (List.add(T), Dict.set(K, V), Channel.send(T)
//— and the same shape for user templates). The template's signature
//names the element slots: a formal whose resolved type leaf is a
//type-parameter placeholder, in every same-name overload (a position
//that splits between element and index across overloads must not
//pre-convert for one candidate and break the other's binding). The
//wrap admits Same/Box/Auto casts (GetCastInfo; an incompatible cast
//rejects inside FixupExprType) and rewrites the actual in place — an
//Object element formal boxes primitives, and a widened element kind
//stages an int probe at the element's width. The retired synthetic
//arm encoded the same positions as a per-name table.
//One overload's placeholder-formal positions: formal position → its
//placeholder leaf. Formals typed by anything else stay out of the map.
static void PlaceholderFormalsOf(SnFunction &fn,
	const GenericInstanceRegistry &generics,
	std::map<size_t, const SnField *> &own)
{
	size_t iParam = 0;
	for (auto &param : fn.Params())
	{
		auto *pType = param.Type();
		if (pType && pType->Kind() == NK_NameExpr)
		{
			auto *pLeaf = static_cast<SnNameExpr *>(pType)->Field();
			if (pLeaf && generics.IsPlaceholder(pLeaf))
				own[iParam] = pLeaf;
		}
		++iParam;
	}
}

//Same-name template members and their shared element-formal mask: a
//formal position stays in the mask only when EVERY overload names the
//same placeholder there (a mixed position is not element-typed across
//the overload set and wraps nothing). Non-const member refs: ParamList
//indexing needs them (the binding core's const_cast idiom).
static bool ElemFormalMaskOf(SnClassDecl &tmpl, const std::string &callee,
	const GenericInstanceRegistry &generics,
	std::vector<SnFunction *> &members,
	std::map<size_t, const SnField *> &elemFormals)
{
	bool bSeeded = false;
	for (auto &member : tmpl.Members())
	{
		if (member.Kind() != NK_Function || member.Name() != callee)
			continue;
		auto &fn = static_cast<SnFunction &>(member);
		members.push_back(&fn);
		std::map<size_t, const SnField *> own;
		PlaceholderFormalsOf(fn, generics, own);
		if (!bSeeded)
		{
			elemFormals = own;
			bSeeded = true;
		}
		else
		{
			for (auto it = elemFormals.begin(); it != elemFormals.end(); )
			{
				auto ownIt = own.find(it->first);
				if (ownIt == own.end() || ownIt->second != it->second)
					it = elemFormals.erase(it);
				else
					++it;
			}
		}
	}
	return !members.empty() && !elemFormals.empty();
}

//A named actual's formal position: the first same-name overload formal
//that carries it (overloads share the mask, so the first hit suffices;
//SIZE_MAX when no overload names it).
static size_t NamedFormalIndex(const std::vector<SnFunction *> &members,
	const std::string &name)
{
	for (auto *pFn : members)
	{
		size_t i = 0;
		for (auto &param : pFn->Params())
		{
			if (param.Name() == name)
				return i;
			++i;
		}
	}
	return SIZE_MAX;
}

//The wrap candidates: one per actual sitting in an element-formal
//position (arg expression → the type argument its placeholder binds
//to). Pending function references stay out — their placeholder typing
//rejects at GetCastInfo, and the ordinary invoke binding scores them
//against the substituted formal (the same path a user class's
//func-typed formal takes).
static void CollectElemWrapTargets(SnInvokeExpr &invoke,
	const std::vector<SnFunction *> &members,
	const std::map<size_t, const SnField *> &elemFormals,
	const GenericInstanceRegistry &generics,
	const std::vector<SnField *> &typeArgs,
	std::vector<std::pair<SnExpression *, SnField *>> &wrapTargets)
{
	size_t iNextFormal = 0;
	for (auto &arg : invoke.Params())
	{
		SnExpression *pValue = &arg;
		size_t formalIdx;
		if (arg.Kind() == NK_NamedArgExpr)
		{
			auto &named = static_cast<SnNamedArgExpr &>(arg);
			pValue = named.Inner();
			formalIdx = NamedFormalIndex(members, named.Name());
		}
		else
		{
			//Positional (out markers included): consumes the next
			//formal slot. Out formals are caller variables, never
			//element storage — the mask match below skips them.
			formalIdx = iNextFormal++;
		}
		auto itElem = elemFormals.find(formalIdx);
		if (itElem == elemFormals.end())
			continue;
		if (IsUnboundFuncRef(*pValue) || IsUnboundMemberFuncRef(*pValue))
			continue;
		if (!pValue->IsResolved() || !pValue->EvalDataType())
			continue;
		const size_t argIdx = generics.PlaceholderIndex(itElem->second);
		if (argIdx >= typeArgs.size() || !typeArgs[argIdx])
			continue;
		wrapTargets.push_back({pValue, typeArgs[argIdx]});
	}
}

void ExprResolveAccessor::WrapGenericElemArgs(SnFieldExpr *pInnerExpr)
{
	if (pInnerExpr->Kind() != NK_InvokeExpr)
		return;
	auto &invoke = static_cast<SnInvokeExpr &>(*pInnerExpr);
	if (!m_pContext || m_pContext->Kind() != NK_ClassDecl)
		return;
	auto &cls = static_cast<SnClassDecl &>(*m_pContext);
	if (cls.IsGenericInstantiation())
	{
		auto &generics = m_Env.Registry().DeclaredGenerics();
		SnClassDecl *pTmpl = generics.InstanceOf(&cls);
		if (!pTmpl)
			return;

		std::vector<SnFunction *> members;
		std::map<size_t, const SnField *> elemFormals;
		if (!ElemFormalMaskOf(*pTmpl, invoke.CalleeName(), generics,
				members, elemFormals))
			return;

		//Deferred-wrap discipline (the retired arm's lesson): FixupExprType
		//frees the arg's list cell (Params() aliases Children()), so the
		//targets collect first and the wraps apply after the scan.
		std::vector<std::pair<SnExpression *, SnField *>> wrapTargets;
		CollectElemWrapTargets(invoke, members, elemFormals, generics,
			cls.GenericTypeArgs(), wrapTargets);
		for (auto &target : wrapTargets)
		{
			auto castInfo = GetCastInfo(
				target.first->EvalDataType(), target.second);
			auto iArg = invoke.Children().find(target.first);
			FixupExprType(iArg, castInfo);
		}
		return;
	}
	//0.13.1a Task 2: a template-row receiver (Maker.pass(7)) — the
	//ordinary binding would score the actuals against the raw placeholder
	//formals and reject; inference creates the instantiation first.
	if (cls.IsGenericDeclaration())
		TryInferTemplateRowStaticCall(invoke, cls);
}

//The D5 diagnosis for a template-row static whose type arguments no
//actual supplies (the zero-argument shape): the generic invoke-
//incompatible text would be misleading when no argument exists to be
//incompatible, so the message names the callee and points at the
//explicit spelling.
static void LogCannotInferTypeArgs(BuildEnvironment &env,
	SnInvokeExpr &invoke, SnClassDecl &tmpl)
{
	std::string paramList;
	for (const auto &name : tmpl.TypeParamNames())
	{
		if (!paramList.empty())
			paramList += ", ";
		paramList += name;
	}
	env.Log(CLL_Error, invoke.Location(),
		"cannot infer type argument for \"%s\" in \"%s.%s(...)\"; spell "
		"the instantiation: %s<%s>(...).",
		paramList.c_str(), tmpl.Name().c_str(),
		invoke.CalleeName().c_str(), tmpl.Name().c_str(),
		paramList.c_str());
}

namespace
{
//Same-name static candidates of the template row: the arm's call shape
//(a type-name receiver admits statics only).
std::vector<SnFunction *> TemplateRowStaticCandidates(SnClassDecl &tmpl,
	const std::string &calleeName)
{
	std::vector<SnFunction *> candidates;
	for (auto &member : tmpl.Members())
	{
		if (member.Kind() != NK_Function || member.Name() != calleeName)
			continue;
		auto &fn = static_cast<SnFunction &>(member);
		if (!fn.ContainFlags(NF_Static))
			continue;
		candidates.push_back(&fn);
	}
	return candidates;
}

//True when an inferred argument is THIS template's own placeholder —
//the in-class identity shape. The ordinary binding scores it directly;
//pinning an instantiation here would kill the caller-forwarding
//semantics generic_static_intra pins.
bool BindsOwnTemplatePlaceholder(GenericInstanceRegistry &generics,
	SnClassDecl &tmpl, const std::vector<SnField *> &args)
{
	const auto &params = tmpl.TypeParamNames();
	for (size_t o = 0; o < params.size(); ++o)
	{
		if (generics.IsPlaceholder(args[o])
			&& args[o] == generics.PlaceholderFor(tmpl, params[o], o))
			return true;
	}
	return false;
}
} //namespace

//0.13.1a Task 2: a type-name-qualified static call into a generic
//TEMPLATE row (Maker.pass(7)). Infers the type arguments from the
//resolved actuals, creates the instantiation, and leaves m_pContext on it
//— the queued invoke resolution binds the substituted clone member, so
//formals AND the return type carry the inferred arguments into binding
//and codegen. Silent when the call is not the arm's shape: named
//arguments, no static candidate, per-candidate shape mismatches (the
//ordinary path's per-argument notes are the precise report there), and
//candidates that would pin an instantiation over an own-placeholder
//actual (the in-class identity shape — Task 1's caller forwarding owns
//it). Two candidates that infer DIFFERENT arguments are not ranked
//(no specificity ordering) — the arm declines and the ordinary
//diagnostic reports.
void ExprResolveAccessor::TryInferTemplateRowStaticCall(
	SnInvokeExpr &invoke, SnClassDecl &tmpl)
{
	auto &generics = m_Env.Registry().DeclaredGenerics();
	if (HasNamedArgument(invoke))
		return;
	std::vector<SnExpression *> actuals;
	for (auto &arg : invoke.Params())
		actuals.push_back(&arg);

	std::vector<SnFunction *> candidates =
		TemplateRowStaticCandidates(tmpl, invoke.CalleeName());
	if (candidates.empty())
		return;

	std::vector<SnField *> args;
	bool bUnbound = false;
	for (auto *pCand : candidates)
	{
		auto outcome = InferTypeArgsForCall(generics, *pCand, actuals);
		if (outcome.result == TypeArgInferenceResult::Mismatched)
			continue;
		if (outcome.result == TypeArgInferenceResult::Unbound)
		{
			bUnbound = true;
			continue;
		}
		std::vector<SnField *> candArgs = std::move(outcome.bindings);
		if (BindsOwnTemplatePlaceholder(generics, tmpl, candArgs))
			return;
		if (!args.empty() && args != candArgs)
			return;
		args = std::move(candArgs);
	}
	if (args.empty())
	{
		if (bUnbound)
			LogCannotInferTypeArgs(m_Env, invoke, tmpl);
		return;
	}
	auto *pInst = GetDeclaredGenericClassDecl(tmpl, args);
	if (pInst)
		m_pContext = pInst;
}

} //namespace nlang
