/*--- ExprResolverGenericInstantiation.cpp — 声明泛型的实例化机器：实例注册表实现、
    成员签名深拷贝（占位符替换）与实例化咽喉点 GetDeclaredGenericClassDecl。
    从 ExprResolverGenerics.cpp 抽取（source-size 守卫驱动的拆分，零行为变化）。
---*/
#include "ExprResolver.h"
#include "GenericInstantiationDepthClaim.hpp"
#include "GenericMemberClone.hpp"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnData.h"
#include "SnArrayTypeToken.h"
#include "TemplateSignatureLeaves.h"
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

//==================== declared-generic machinery ====================
//
//`class Name<T, ...>` declarations (signature surfaces only) resolve
//their member signatures against interned placeholder types, and every
//`Name<Arg...>` use creates a synthetic instantiation whose members are
//signature deep-copies with placeholders substituted. Everything here
//is a compile-time artifact; the VM side reads the registry through
//GenericInstanceRegistry.h (implementation in its own TU). The
//registry instance is owned by ModuleRegistry — one per BUILD
//(process-global maps would be keyed by per-build pointers and unit
//indices; see that header).

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
		//The clone-to-instantiation pair (0.13.1a): the backend's static-
		//call arm reads the instantiation node for the frame descriptor
		//cell; recorded at the same fill-time point so later member-list
		//mutation on either side cannot drift the mapping.
		m_Env.Registry().DeclaredGenerics()
			.AdoptCloneInstantiation(pCopy, &inst);
	}
	return true;
}

//The clone-to-record-owner mapping (an instantiation member's compiled
//record lives on the template row) is GenericInstanceRegistry::
//RecordOwnerOfMember — the registry owns the instantiation topology.
//The resolver never rebinds: every type read wants the substituted
//clone surface, and the backend's FunctionSlotFor falls back to the
//record owner at its single lookup throat.

//The creation's first phase: a detached class shell carrying the erasure
//identity — the instantiation shares the template's unqualified name, the
//same way every List<T> shares the backing "List" class (the VM keys
//the CompiledClass, the intrinsic channel and the ctor classification
//off the unqualified name, so no consumer needs a display-name special case;
//distinct instantiations stay distinct compile-time types through node
//identity). The shell starts memberless; FillInstanceMembers completes
//it after the cache publishes it.
static SnClassDecl *CreateInstanceShell(const SnClassDecl &tmpl,
	const std::vector<SnField*> &typeArgs)
{
	AssertNoRefTypeArgs(typeArgs);
	auto* pInstance = new SnClassDecl(new std::string(tmpl.Name()),
		nullptr, new PtrList<SnField>(), *tmpl.Location());
	pInstance->SuperClass(tmpl.SuperClass());
	pInstance->SetGenericInstantiation();
	pInstance->SetBaseName(tmpl.Name());
	pInstance->SetGenericTypeArgs(typeArgs);
	return pInstance;
}

//Invariant: clone SIGNATURE-COMPLETE templates only. The creation can
//fire from the declaration pass (a class field of declared-generic
//type) before the template's own signatures resolved — cloning then
//copies unresolved type expressions and the cache would serve a
//broken instance to every later phase. Completing the template HERE,
//at the single chokepoint every consumer reaches the creation through,
//makes instantiation order-independent (the same machinery the
//ordinary pre-pass runs; per-member flags make the later pass a
//no-op). ResolveDataTypes is idempotent and flag-guarded. The
//implicit-root link is completed at the same chokepoint: the merged
//root walks user code ahead of the prelude, so the creation can fire
//before the template's own class pass applies the implicit Object
//chain — a cached instance would keep the null super forever and
//every inheritance-distance check (an equals(Object) argument
//binding) would reject it.
void ExprResolveAccessor::CompleteTemplateForInstantiation(SnClassDecl& tmpl)
{
	ExprResolver templateCompleter(m_Env);
	templateCompleter.ResolveDataTypes(tmpl, tmpl);
	EnsureImplicitObjectSuperclass(tmpl, m_Env.Registry());
}

//The creation entry's depth guard: true when this entry must refuse —
//the concurrent-instantiation chain ran past the cap. Checked AFTER
//the instance cache: a re-reference to an instantiation — adopted or
//in flight — terminates a chain through the cache (two-phase
//construction publishes the shell before the recursive window), but a
//member type whose argument EMBEDS the instance itself (A<A<T>>
//inside A<T>) grows a genuinely fresh key per link and would recurse
//until the stack overflowed. The diagnostic is deduped per template
//per build: every later trip of the same family is the same defect.
bool ExprResolveAccessor::InstantiationDepthLimitReached(const SyntaxNode& tmpl)
{
	auto& generics = m_Env.Registry().DeclaredGenerics();
	if (generics.GenericInstantiationDepth() < kMaxGenericInstantiationDepth)
		return false;
	if (generics.MarkDepthLimitReported(&tmpl))
		m_Env.Log(CLL_Error, tmpl.Location(),
			"The generic instantiation depth exceeds the limit of %d "
			"nested instantiations.", kMaxGenericInstantiationDepth);
	return true;
}

//The creation's second phase — the fill window: complete the template,
//refresh the shell's snapshot of its superclass, and clone the member
//signatures in. Re-entrant lookups during the window see the shell
//with partial members: every reader in the window binds the node as a
//type and none reads its members (the same incomplete-during-
//construction contract Clang holds for class instantiation; Task-5
//flip-list entry).
void ExprResolveAccessor::FillInstanceWindow(SnClassDecl& tmpl,
	const std::vector<SnField*>& typeArgs, SnClassDecl& inst)
{
	CompleteTemplateForInstantiation(tmpl);
	//The shell snapshotted tmpl.SuperClass() before completion: the
	//create can fire ahead of the template's own class pass, when the
	//implicit Object chain is not linked yet, and a published instance
	//keeps whatever it snapshotted forever. Refresh from the completed
	//template so inheritance-distance checks (an equals(Object)
	//argument binding) see the chain.
	inst.SuperClass(tmpl.SuperClass());
	//The template's implements edges were resolved against its own
	//placeholders; the instantiation re-creates them against its concrete
	//arguments (Box<X> implements Wrapper<X> makes Box<string> a
	//Wrapper<string>).
	FillInstanceImplementsEdges(tmpl, typeArgs, inst);
	//A member the instantiation cannot carry already logged its
	//diagnostic; the partially filled shell is still served so later
	//phases maximize diagnostics instead of failing on a null type.
	FillInstanceMembers(tmpl, typeArgs, inst);
}

//One implements-edge type argument, substituted against the creation's
//argument vector (contract on the declaration in ExprResolver.h).
SnField* ExprResolveAccessor::SubstitutedEdgeTypeArg(SnField* pArg,
	const std::vector<SnField*>& typeArgs)
{
	auto& generics = m_Env.Registry().DeclaredGenerics();
	if (generics.IsPlaceholder(pArg))
		return typeArgs[generics.PlaceholderIndex(pArg)];
	//A nested instantiation whose arguments embed placeholders
	//(`Wrapper<Box<X>>` as an edge argument): re-create it substituted —
	//the same registry channels the use-site resolution reaches, so
	//both paths land on one canonical node.
	if (pArg->Kind() == NK_InterfaceDecl)
	{
		auto* pNested = static_cast<SnInterfaceDecl*>(pArg);
		auto* pNestedTmpl = generics.InstanceOfInterface(pNested);
		if (!pNestedTmpl)
			return pArg;
		std::vector<SnField*> nestedArgs;
		nestedArgs.reserve(pNested->GenericTypeArgs().size());
		for (auto* pNestedArg : pNested->GenericTypeArgs())
			nestedArgs.push_back(
				SubstitutedEdgeTypeArg(pNestedArg, typeArgs));
		if (auto* pCreated = GetDeclaredGenericInterfaceDecl(
				*pNestedTmpl, nestedArgs))
			return pCreated;
		return pArg;
	}
	if (pArg->Kind() == NK_ClassDecl)
	{
		auto* pNested = static_cast<SnClassDecl*>(pArg);
		auto* pNestedTmpl = generics.InstanceOf(pNested);
		if (!pNestedTmpl)
			return pArg;
		std::vector<SnField*> nestedArgs;
		nestedArgs.reserve(pNested->GenericTypeArgs().size());
		for (auto* pNestedArg : pNested->GenericTypeArgs())
			nestedArgs.push_back(
				SubstitutedEdgeTypeArg(pNestedArg, typeArgs));
		if (auto* pCreated = GetDeclaredGenericClassDecl(
				*pNestedTmpl, nestedArgs))
			return pCreated;
		return pArg;
	}
	return pArg;
}

//The creation's implements-edge substitution (contract on the declaration
//in ExprResolver.h).
void ExprResolveAccessor::FillInstanceImplementsEdges(SnClassDecl& tmpl,
	const std::vector<SnField*>& typeArgs, SnClassDecl& inst)
{
	auto& generics = m_Env.Registry().DeclaredGenerics();
	for (auto* pEdge : tmpl.ImplementsList())
	{
		if (!pEdge->IsGenericInstantiation())
		{
			//Bare interface: module-global node, shared directly.
			inst.AddImplements(pEdge);
			continue;
		}
		auto* pEdgeTmpl = generics.InstanceOfInterface(pEdge);
		if (!pEdgeTmpl)
		{
			//Defensive: an unresolved edge cannot be substituted —
			//share it and let the template's own contract check speak.
			inst.AddImplements(pEdge);
			continue;
		}
		std::vector<SnField*> substitutedArgs;
		substitutedArgs.reserve(pEdge->GenericTypeArgs().size());
		for (auto* pArg : pEdge->GenericTypeArgs())
			substitutedArgs.push_back(
				SubstitutedEdgeTypeArg(pArg, typeArgs));
		if (auto* pSubstituted = GetDeclaredGenericInterfaceDecl(
				*pEdgeTmpl, substitutedArgs))
			inst.AddImplements(pSubstituted);
	}
}

SnClassDecl* ExprResolveAccessor::GetDeclaredGenericClassDecl(
	SnClassDecl& tmpl, const std::vector<SnField*>& typeArgs)
{
	auto& generics = m_Env.Registry().DeclaredGenerics();
	std::vector<const SnField*> keyArgs(typeArgs.begin(), typeArgs.end());
	if (auto* pCached = generics.FindInstance(&tmpl, keyArgs))
		return pCached;

	//Depth guard (rationale on InstantiationDepthLimitReached).
	if (InstantiationDepthLimitReached(tmpl))
		return nullptr;
	//The claim spans EVERYTHING below the guard, not just the member
	//cloning: completing the template below resolves its member types,
	//and a self-referential member type re-enters the creation from there —
	//the recursion point the guard exists to cut.
	InstantiationDepthClaim depthClaim(generics);

	//Two-phase construction (Clang's InstantiatingSpecializations
	//technique): the shell — name, erasure markers, type arguments — is
	//published in the cache BEFORE the recursive window. A fixed-key
	//self-reference (Node<T> inside Node<T>) re-enters the creation for the
	//same key while this frame is still filling its shell; the cache
	//hit binds the member to the very instance being created and the
	//recursion terminates at depth one. Publishing after the window —
	//the previous shape — left every re-entry creating a fresh instance:
	//the stack overflow, the fixed-key rejection and the exponential
	//diagnostic cascade were all that one defect.
	auto* pInstance = CreateInstanceShell(tmpl, typeArgs);
	generics.AdoptInstance(pInstance, &tmpl, keyArgs);
	FillInstanceWindow(tmpl, typeArgs, *pInstance);
	return pInstance;
}

//SubstituteTypeExpr's name-leaf arms' shared tail (definition moved out
//of the arms for the source-size cap — see the declaration comment).
void ExprResolveAccessor::BindSubstitutedLeaf(SnFieldExpr& copy,
	const SnFieldExpr& srcLeaf, const std::vector<SnField*>& typeArgs)
{
	if (!srcLeaf.IsResolved())
		return;
	auto& generics = m_Env.Registry().DeclaredGenerics();
	auto* pBinding = generics.IsPlaceholder(srcLeaf.Field())
		? typeArgs[generics.PlaceholderIndex(srcLeaf.Field())]
		: srcLeaf.Field();
	ResolveFieldExprAs(copy, pBinding);
}

//SubstituteTypeExpr's array arm (definition moved out of the dispatch
//for the source-size cap): substitute the element, then re-intern the
//token over it — ResolveFieldExprAs exactly like Access(SnArrayTypeExpr),
//because a formal's type is read via Type()->Field() (the token binding).
SnFieldExpr* ExprResolveAccessor::SubstituteArrayTypeExpr(
	SnArrayTypeExpr& arr, const std::vector<SnField*>& typeArgs)
{
	auto* pElemCopy = SubstituteTypeExpr(*arr.ElementType(), typeArgs);
	if (!pElemCopy)
		return nullptr;
	auto* pCopy = new SnArrayTypeExpr(pElemCopy, *arr.Location(),
		arr.Rank());
	if (arr.IsResolved())
	{
		ResolveFieldExprAs(*pCopy, m_Env.InternArrayTypeToken(
			pElemCopy->Field(), arr.Rank()));
	}
	return pCopy;
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
		BindSubstitutedLeaf(*pCopy, name, typeArgs);
		return pCopy;
	}
	//v0.13.1 D7: the PRE-BOUND builtin leaf family (SnIdentifierExpr
	//created through the NodeKind ctor — the compiler-wide idiom for
	//scalar/string types; the synthesizer's builtin arm uses it too).
	//A pre-bound leaf's Field() is a builtin datatype, never a
	//placeholder, so BindSubstitutedLeaf transfers the binding verbatim.
	//Deleting this arm clones every non-T signature of a stub template
	//as void (substitute's null becomes the clone's null return type).
	if (srcType.Kind() == NK_IdentifierExpr)
	{
		auto& id = static_cast<SnIdentifierExpr&>(srcType);
		//The leaf contract is enforced HERE, at the consumption point:
		//only the pre-bound builtin family arrives bare — a divergent
		//create fails this assertion in Debug instead of cloning a silent
		//void signature (TemplateSignatureLeaves.h).
		NLANG_ASSERT(TemplateSignatureLeavesAreCanonical(&srcType));
		auto* pCopy = new SnIdentifierExpr(
			new std::string(id.Name()), *srcType.Location());
		BindSubstitutedLeaf(*pCopy, id, typeArgs);
		return pCopy;
	}
	if (srcType.Kind() == NK_ArrayTypeExpr)
		return SubstituteArrayTypeExpr(
			static_cast<SnArrayTypeExpr&>(srcType), typeArgs);
	//Nested instantiation (`Future<T>`, `List<T>`): substitute base and
	//arguments, then resolve — the cache creates the ACTUAL instantiation.
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

//0.13.1a Task 4 (D4): the call-site value type of a generic free
//function — the template row's return type expression substituted with
//the invoke's inferred arguments, per the established convention (the
//value slot reads the return type expr's resolved Field, exactly what
//SnFunction::EvalDataType yields). The substituted copy is transient:
//the substitution binds its leaves, and the resolved field it yields is
//tree/registry-owned, so the invoke's slot outlives the copy.
//No Access() re-entry here: the copy's static type is the base
//SnFieldExpr, so an Access call static-dispatches to the defaulted
//accessor and asserts regardless of the dynamic kind (the Task 4
//Debug-only crash). Every SubstituteTypeExpr arm already owns the
//copy's resolution — a resolved source leaf resolves the copy via
//ResolveFieldExprAs/token interning/an explicit Access on the precise
//SnGenericTypeExpr static type, and an unresolved source leaf stays
//unresolved on purpose (the template row's own diagnostic already
//fired; re-resolving the name at the call site would consult the
//wrong scope).
void ExprResolveAccessor::SubstituteInvokeReturnType(
	SnInvokeExpr& invoke, SnFunction& func)
{
	if (!func.HasReturn() || !func.ReturnType())
		return;
	std::unique_ptr<SnFieldExpr> pRetCopy(
		SubstituteTypeExpr(*func.ReturnType(),
			invoke.InferredTypeArgs()));
	if (!pRetCopy)
		return;
	if (pRetCopy->IsResolved())
		invoke.EvalDataType(pRetCopy->Field());
}

ExprResolveAccessor::TypeParamBindResult
ExprResolveAccessor::BindTypeParamInDecl(
	const std::vector<std::string>& params, const std::string& name,
	SyntaxNode& tmpl, SnIdentifierExpr& idExpr)
{
	const auto paramIt = std::find(params.begin(), params.end(), name);
	if (paramIt == params.end())
		return TypeParamBindResult::NotApplicable;
	auto* pPlaceholder = m_Env.Registry().DeclaredGenerics()
		.PlaceholderFor(tmpl, name,
			static_cast<size_t>(paramIt - params.begin()));
	ResolveFieldExprAs(idExpr, pPlaceholder);
	return TypeParamBindResult::Bound;
}

ExprResolveAccessor::TypeParamBindResult
ExprResolveAccessor::BindTypeParamPlaceholderIfAny(SnIdentifierExpr& idExpr)
{
	const std::string& name = idExpr.Name();
	for (SyntaxNode* p = idExpr.Parent(); p; p = p->Parent())
	{
		const auto kind = p->Kind();
		//0.13.1a: a type parameter of a generic FREE function. Checked
		//before the class arm so the innermost declaration that names the
		//parameter wins (a nested function inside a generic class method
		//shadows the class parameters); a non-generic function level must
		//not shadow — the walk continues to the enclosing class.
		if (kind == NK_Function)
		{
			auto& fn = static_cast<SnFunction&>(*p);
			if (!fn.IsGenericDeclaration())
				continue;
			return BindTypeParamInDecl(fn.TypeParamNames(), name, fn,
				idExpr);
		}
		if (kind == NK_ClassDecl)
		{
			auto& cls = static_cast<SnClassDecl&>(*p);
			if (!cls.IsGenericDeclaration())
				return TypeParamBindResult::NotApplicable;
			//Every T-typed member is one value slot (scalars inline in
			//the low half, heap kinds as handles), so the single shared
			//template layout holds for every instantiation — T fields
			//bind to the placeholder like any other T reference.
			return BindTypeParamInDecl(cls.TypeParamNames(), name, cls,
				idExpr);
		}
		//0.13.1c: a type parameter of a generic INTERFACE declaration.
		//Interface T references are signature positions only (members,
		//formals, constant TYPES) — the placeholder binds exactly like
		//the class arm's; no storage kind is ever asked of it.
		if (kind == NK_InterfaceDecl)
		{
			auto& iface = static_cast<SnInterfaceDecl&>(*p);
			if (!iface.IsGenericDeclaration())
				return TypeParamBindResult::NotApplicable;
			return BindTypeParamInDecl(iface.TypeParamNames(), name,
				iface, idExpr);
		}
	}
	return TypeParamBindResult::NotApplicable;
}

} //namespace nlang
