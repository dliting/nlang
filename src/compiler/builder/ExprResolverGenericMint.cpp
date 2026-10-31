/*--- ExprResolverGenericMint.cpp — 声明泛型的铸造机器：实例注册表实现、
    成员签名深拷贝（占位符替换）与铸造咽喉点 GetDeclaredGenericClassDecl。
    从 ExprResolverGenerics.cpp 抽取（source-size 守卫驱动的拆分，零行为变化）。
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
#include <nlang/runtime/RnTypes.h>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

namespace
{
//Scoped increment of the concurrent-mint depth around the mint window:
//nested instantiations fire synchronously inside BOTH phases — template
//completion resolves the template's own member types (a member type
//like A<A<T>> resolves through Access back into
//GetDeclaredGenericClassDecl), and member cloning substitutes them into
//the instance — each nested mint through a freshly constructed
//resolver, so the chain counter lives on the registry, shared across
//resolver instances, and the guard reads it at the mint entry.
class MintDepthClaim
{
public:
	explicit MintDepthClaim(GenericInstanceRegistry& registry)
		: m_rRegistry(registry)
		{ m_rRegistry.EnterMintDepth(); }
	~MintDepthClaim() { m_rRegistry.LeaveMintDepth(); }
	MintDepthClaim(const MintDepthClaim&) = delete;
	MintDepthClaim& operator=(const MintDepthClaim&) = delete;
private:
	GenericInstanceRegistry& m_rRegistry;
};
}

//==================== declared-generic machinery ====================
//
//`class Name<T, ...>` declarations (signature surfaces only) resolve
//their member signatures against interned placeholder types, and every
//`Name<Arg...>` use mints a synthetic instantiation whose members are
//signature deep-copies with placeholders substituted. Everything here
//is a compile-time artifact; the VM side reads the registry through
//GenericInstanceRegistry.h (implementation in its own TU). The
//registry instance is owned by ModuleRegistry — one per BUILD
//(process-global maps would be keyed by per-build pointers and unit
//indices; see that header).

//One template member's signature deep-copy: the copy is the metadata
//surface only — return type and formal parameters with their constant
//defaults (bodies and native bindings stay with the template row).
SnField* CloneGenericMember(SnField& src,
	const std::vector<SnField*>& typeArgs,
	const std::function<SnFieldExpr*(SnFieldExpr&)>& substitute,
	const std::function<SnExpression*(SnExpression*)>& cloneDefault)
{
	if (src.Kind() == NK_Function)
	{
		auto& fn = static_cast<SnFunction&>(src);
		SnFieldExpr* pRetCopy = fn.ReturnType()
			? substitute(*fn.ReturnType()) : nullptr;
		auto* pParams = new PtrList<SnFormalParam>();
		for (auto& param : fn.Params())
		{
			auto* pTypeCopy = param.Type()
				? substitute(*param.Type()) : nullptr;
			pParams->push_back(new SnFormalParam(param.Flags(), pTypeCopy,
				new std::string(param.Name()),
				cloneDefault(param.Value()), *param.Location()));
		}
		return new SnFunction(fn.AccessType(), fn.Flags(), pRetCopy,
			new std::string(fn.Name()), pParams, *fn.Location());
	}
	if (src.Kind() == NK_ClassField)
	{
		auto& field = static_cast<SnClassField&>(src);
		auto* pTypeCopy = field.Type()
			? substitute(*field.Type()) : nullptr;
		return new SnClassField(pTypeCopy, new std::string(field.Name()),
			field.AccessType(), *field.Location());
	}
	return nullptr;
}

//GetDeclaredGenericClassDecl's member-fill loop: each member copy has
//its type expressions substituted before joining the shell. Copies
//pushed into Members() are owned by the instance from the push; a
//member shape the instantiation cannot carry logs its diagnostic and
//stops the fill — earlier copies stay owned (the build already
//carries the error).
bool ExprResolveAccessor::FillInstanceMembers(SnClassDecl& tmpl,
	const std::vector<SnField*>& typeArgs, SnClassDecl& inst)
{
	for (auto& member : tmpl.Members())
	{
		//v0.12.3 dictionary model: every template member clones — the
		//copies are the resolver's substituted SIGNATURE surface (the
		//one channel type-checking ever reads members through). The
		//compiled records stay with the template row; the backend's
		//slot throats resolve record reads back to it
		//(AdoptMemberClone below).
		auto* pCopy = CloneGenericMember(member, typeArgs,
			[this, &typeArgs](SnFieldExpr& srcType)
			{ return SubstituteTypeExpr(srcType, typeArgs); },
			[this](SnExpression* pSrcDefault)
			{ return CloneConstantDefaultExpr(pSrcDefault); });
		if (!pCopy)
		{
			m_Env.Log(CLL_Error, member.Location(),
				"The class \"%s\" is a generic declaration: the member "
				"\"%s\" has a shape instantiations cannot carry.",
				tmpl.Name().c_str(), member.Name().c_str());
			return false;
		}
		inst.Members().push_back(pCopy);
		//The clone-to-record-owner pair is recorded HERE, where the copy
		//and its source are in hand: the backend's slot throats resolve
		//every record read through this map (AdoptMemberClone), and a
		//fill-time recording keeps the mapping immune to later member-
		//list mutation on either side.
		m_Env.Registry().DeclaredGenerics()
			.AdoptMemberClone(pCopy, &member);
	}
	return true;
}

//The clone-to-record-owner mapping (an instantiation member's compiled
//record lives on the template row) is GenericInstanceRegistry::
//RecordOwnerOfMember — the registry owns the instantiation topology.
//The resolver never rebinds: every type read wants the substituted
//clone surface, and the backend's FunctionSlotFor falls back to the
//record owner at its single lookup throat.

//The mint's first phase: a detached class shell carrying the erasure
//identity — the instantiation shares the template's bare name, the
//same way every List<T> shares the backing "List" class (the VM keys
//the CompiledClass, the intrinsic channel and the ctor classification
//off the bare name, so no consumer needs a display-name special case;
//distinct instantiations stay distinct compile-time types through node
//identity). The shell starts memberless; FillInstanceMembers completes
//it after the cache publishes it.
static SnClassDecl *MintInstanceShell(const SnClassDecl &tmpl,
	const std::vector<SnField*> &typeArgs)
{
	auto* pInstance = new SnClassDecl(new std::string(tmpl.Name()),
		nullptr, new PtrList<SnField>(), *tmpl.Location());
	pInstance->SuperClass(tmpl.SuperClass());
	pInstance->SetGenericInstantiation();
	pInstance->SetBaseName(tmpl.Name());
	pInstance->SetGenericTypeArgs(typeArgs);
	return pInstance;
}

//Invariant: clone SIGNATURE-COMPLETE templates only. The mint can
//fire from the declaration pass (a class field of declared-generic
//type) before the template's own signatures resolved — cloning then
//copies unresolved type expressions and the cache would serve a
//broken instance to every later phase. Completing the template HERE,
//at the single chokepoint every consumer reaches the mint through,
//makes instantiation order-independent (the same machinery the
//ordinary pre-pass runs; per-member flags make the later pass a
//no-op). ResolveDataTypes is idempotent and flag-guarded. The
//implicit-root link is completed at the same chokepoint: the merged
//root walks user code ahead of the prelude, so the mint can fire
//before the template's own class pass applies the implicit Object
//chain — a cached instance would keep the null super forever and
//every inheritance-distance check (an equals(Object) argument
//binding) would reject it.
void ExprResolveAccessor::CompleteTemplateForMint(SnClassDecl& tmpl)
{
	ExprResolver templateCompleter(m_Env);
	templateCompleter.ResolveDataTypes(tmpl, tmpl);
	EnsureImplicitObjectSuperclass(tmpl, m_Env.Registry());
}

//The mint entry's depth guard: true when this entry must refuse —
//the concurrent-instantiation chain ran past the cap. Checked AFTER
//the instance cache: a re-reference to an instantiation — adopted or
//in flight — terminates a chain through the cache (two-phase
//construction publishes the shell before the recursive window), but a
//member type whose argument EMBEDS the instance itself (A<A<T>>
//inside A<T>) grows a genuinely fresh key per link and would recurse
//until the stack overflowed. The diagnostic is deduped per template
//per build: every later trip of the same family is the same defect.
bool ExprResolveAccessor::MintDepthLimitReached(const SnClassDecl& tmpl)
{
	auto& generics = m_Env.Registry().DeclaredGenerics();
	if (generics.GenericMintDepth() < kMaxGenericInstantiationDepth)
		return false;
	if (generics.MarkDepthLimitReported(&tmpl))
		m_Env.Log(CLL_Error, tmpl.Location(),
			"The generic instantiation depth exceeds the limit of %d "
			"nested instantiations.", kMaxGenericInstantiationDepth);
	return true;
}

//The mint's second phase — the fill window: complete the template,
//refresh the shell's snapshot of its superclass, and clone the member
//signatures in. Re-entrant lookups during the window see the shell
//with partial members: every reader in the window binds the node as a
//type and none reads its members (the same incomplete-during-
//construction contract Clang holds for class instantiation; Task-5
//flip-list entry).
void ExprResolveAccessor::FillInstanceWindow(SnClassDecl& tmpl,
	const std::vector<SnField*>& typeArgs, SnClassDecl& inst)
{
	CompleteTemplateForMint(tmpl);
	//The shell snapshotted tmpl.SuperClass() before completion: the
	//mint can fire ahead of the template's own class pass, when the
	//implicit Object chain is not linked yet, and a published instance
	//keeps whatever it snapshotted forever. Refresh from the completed
	//template so inheritance-distance checks (an equals(Object)
	//argument binding) see the chain.
	inst.SuperClass(tmpl.SuperClass());
	//A member the instantiation cannot carry already logged its
	//diagnostic; the partially filled shell is still served so later
	//phases maximize diagnostics instead of failing on a null type.
	FillInstanceMembers(tmpl, typeArgs, inst);
}

SnClassDecl* ExprResolveAccessor::GetDeclaredGenericClassDecl(
	SnClassDecl& tmpl, const std::vector<SnField*>& typeArgs)
{
	auto& generics = m_Env.Registry().DeclaredGenerics();
	std::vector<const SnField*> keyArgs(typeArgs.begin(), typeArgs.end());
	if (auto* pCached = generics.FindInstance(&tmpl, keyArgs))
		return pCached;

	//Depth guard (rationale on MintDepthLimitReached).
	if (MintDepthLimitReached(tmpl))
		return nullptr;
	//The claim spans EVERYTHING below the guard, not just the member
	//cloning: completing the template below resolves its member types,
	//and a self-referential member type re-enters the mint from there —
	//the recursion point the guard exists to cut.
	MintDepthClaim depthClaim(generics);

	//Two-phase construction (Clang's InstantiatingSpecializations
	//technique): the shell — name, erasure markers, type arguments — is
	//published in the cache BEFORE the recursive window. A fixed-key
	//self-reference (Node<T> inside Node<T>) re-enters the mint for the
	//same key while this frame is still filling its shell; the cache
	//hit binds the member to the very instance being minted and the
	//recursion terminates at depth one. Publishing after the window —
	//the previous shape — left every re-entry minting a fresh instance:
	//the stack overflow, the fixed-key rejection and the exponential
	//diagnostic cascade were all that one defect.
	auto* pInstance = MintInstanceShell(tmpl, typeArgs);
	generics.AdoptInstance(pInstance, &tmpl, keyArgs);
	FillInstanceWindow(tmpl, typeArgs, *pInstance);
	return pInstance;
}

SnFieldExpr* ExprResolveAccessor::SubstituteTypeExpr(
	SnFieldExpr& srcType, const std::vector<SnField*>& typeArgs)
{
	//Name leaf: transfer the source binding; a placeholder leaf binds to
	//the actual argument instead (unresolved leaves stay unresolved — the
	//template member's own diagnostic already fired).
	if (srcType.Kind() == NK_NameExpr)
	{
		auto& name = static_cast<SnNameExpr&>(srcType);
		auto* pInner = dynamic_cast<SnIdentifierExpr*>(name.Expr());
		if (!pInner)
			return nullptr;
		auto* pCopy = new SnNameExpr(
			new SnIdentifierExpr(new std::string(pInner->Name()),
				*srcType.Location()), *srcType.Location());
		if (name.IsResolved())
		{
			auto& generics = m_Env.Registry().DeclaredGenerics();
			auto* pBinding = generics.IsPlaceholder(name.Field())
				? typeArgs[generics.PlaceholderIndex(name.Field())]
				: name.Field();
			ResolveFieldExprAs(*pCopy, pBinding);
		}
		return pCopy;
	}
	//Array: substitute the element, then re-intern the token over it —
	//ResolveFieldExprAs exactly like Access(SnArrayTypeExpr), because a
	//formal's type is read via Type()->Field() (the token binding).
	if (srcType.Kind() == NK_ArrayTypeExpr)
	{
		auto& arr = static_cast<SnArrayTypeExpr&>(srcType);
		auto* pElemCopy = SubstituteTypeExpr(*arr.ElementType(), typeArgs);
		if (!pElemCopy)
			return nullptr;
		auto* pCopy = new SnArrayTypeExpr(pElemCopy, *srcType.Location(),
			arr.Rank());
		if (arr.IsResolved())
		{
			ResolveFieldExprAs(*pCopy, m_Env.InternArrayTypeToken(
				pElemCopy->Field(), arr.Rank()));
		}
		return pCopy;
	}
	//Nested instantiation (`Future<T>`, `List<T>`): substitute base and
	//arguments, then resolve — the cache mints the ACTUAL instantiation.
	if (srcType.Kind() == NK_GenericTypeExpr)
		return SubstituteGenericTypeExpr(
			static_cast<SnGenericTypeExpr&>(srcType), typeArgs);
	return nullptr;
}

SnFieldExpr* ExprResolveAccessor::SubstituteGenericTypeExpr(
	SnGenericTypeExpr& gen, const std::vector<SnField*>& typeArgs)
{
	auto* pBaseCopy = SubstituteTypeExpr(*gen.Base(), typeArgs);
	if (!pBaseCopy)
		return nullptr;
	auto* pArgs = new std::vector<SnFieldExpr*>;
	for (auto* pArg : gen.TypeArgs())
	{
		auto* pArgCopy = SubstituteTypeExpr(*pArg, typeArgs);
		if (!pArgCopy)
			return nullptr;
		pArgs->push_back(pArgCopy);
	}
	auto* pCopy = new SnGenericTypeExpr(pBaseCopy, pArgs,
		*gen.Location());
	if (gen.IsResolved())
		Access(*pCopy);
	return pCopy;
}

ExprResolveAccessor::TypeParamBindResult
ExprResolveAccessor::BindTypeParamPlaceholderIfAny(SnIdentifierExpr& idExpr)
{
	const std::string& name = idExpr.Name();
	for (SyntaxNode* p = idExpr.Parent(); p; p = p->Parent())
	{
		const auto kind = p->Kind();
		if (kind == NK_ClassDecl)
		{
			auto& cls = static_cast<SnClassDecl&>(*p);
			if (!cls.IsGenericDeclaration())
				return TypeParamBindResult::NotApplicable;
			const auto& params = cls.TypeParamNames();
			const auto paramIt = std::find(params.begin(), params.end(),
				name);
			if (paramIt == params.end())
				return TypeParamBindResult::NotApplicable;
			//Every T-typed member is one value slot (scalars inline in
			//the low half, heap kinds as handles), so the single shared
			//template layout holds for every instantiation — T fields
			//bind to the placeholder like any other T reference.
			auto* pPlaceholder = m_Env.Registry().DeclaredGenerics()
				.PlaceholderFor(cls, name,
					static_cast<size_t>(paramIt - params.begin()));
			ResolveFieldExprAs(idExpr, pPlaceholder);
			return TypeParamBindResult::Bound;
		}
	}
	return TypeParamBindResult::NotApplicable;
}

} //namespace nlang
