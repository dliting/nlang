/*--- ExprResolverGenericMintIface.cpp — generic INTERFACE declarations'
    mint: the shell, the member clone fill and the cache throat
    GetDeclaredGenericInterfaceDecl. Split from
    ExprResolverGenericMint.cpp (0.13.1c, source-size guard driven);
    the machinery mirrors the class mint contract for contract.
---*/
#include "ExprResolver.h"
#include "GenericMintDepthClaim.hpp"
#include "GenericMemberClone.hpp"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "TemplateSignatureLeaves.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "ModuleRegistry.h"
#include <nlang/compiler/GenericInstanceRegistry.h>
#include <vector>

namespace nlang
{

//The interface mint's shell: the same erasure identity the class shell
//carries — the instantiation shares the template's bare name and stays
//a distinct compile-time type through node identity. Memberless until
//FillInterfaceInstanceMembers completes it after the cache publishes
//it.
static SnInterfaceDecl *MintInterfaceInstanceShell(
	const SnInterfaceDecl &tmpl, const std::vector<SnField*> &typeArgs)
{
	auto* pInstance = new SnInterfaceDecl(new std::string(tmpl.Name()),
		new PtrList<SnField>(), *tmpl.Location());
	pInstance->SetGenericInstantiation();
	pInstance->SetBaseName(tmpl.Name());
	pInstance->SetGenericTypeArgs(typeArgs);
	return pInstance;
}

bool ExprResolveAccessor::FillInterfaceInstanceMembers(SnInterfaceDecl& tmpl,
	const std::vector<SnField*>& typeArgs, SnInterfaceDecl& inst)
{
	for (auto& member : tmpl.Members())
	{
		//The same clone machinery the class fill uses: methods clone as
		//signature surfaces, constants clone their TYPE only — the
		//folded initializer stays with the template row (compliance
		//reads signatures, never values).
		auto* pCopy = CloneGenericMember(member, typeArgs,
			[this, &typeArgs](SnFieldExpr& srcType)
			{ return SubstituteTypeExpr(srcType, typeArgs); },
			[this](SnExpression* pSrcDefault)
			{ return CloneConstantDefaultExpr(pSrcDefault); });
		if (!pCopy)
		{
			m_Env.Log(CLL_Error, member.Location(),
				"The interface \"%s\" is a generic declaration: the "
				"member \"%s\" has a shape instantiations cannot "
				"carry.",
				tmpl.Name().c_str(), member.Name().c_str());
			return false;
		}
		inst.Members().push_back(pCopy);
		//0.13.1c Task 8: the clone pairs the backend's dispatch reads.
		//RecordOwnerOfMember is the clone test behind the static-call
		//arm AND the FunctionSlotFor throat (a static interface member
		//carries a template-row record); the instantiation pair feeds the
		//static arm's frame descriptor. Function members only — the
		//constant clones keep the Task 7 shape (no records, no dispatch).
		if (member.Kind() == NK_Function)
		{
			m_Env.Registry().DeclaredGenerics()
				.AdoptMemberClone(pCopy, &member);
			m_Env.Registry().DeclaredGenerics()
				.AdoptInterfaceCloneInstantiation(pCopy, &inst);
		}
	}
	return true;
}

SnInterfaceDecl* ExprResolveAccessor::GetDeclaredGenericInterfaceDecl(
	SnInterfaceDecl& tmpl, const std::vector<SnField*>& typeArgs)
{
	auto& generics = m_Env.Registry().DeclaredGenerics();
	std::vector<const SnField*> keyArgs(typeArgs.begin(), typeArgs.end());
	if (auto* pCached = generics.FindInterfaceInstance(&tmpl, keyArgs))
		return pCached;

	//Depth guard (rationale on MintDepthLimitReached).
	if (MintDepthLimitReached(tmpl))
		return nullptr;
	//The claim spans everything below the guard, the class mint's
	//contract: the template completion below resolves member types,
	//and a self-referential member type re-enters the mint from there.
	MintDepthClaim depthClaim(generics);

	//Two-phase construction: the shell is published in the interface
	//cache BEFORE the recursive window (the class mint's fixed-key
	//self-reference contract, verbatim).
	auto* pInstance = MintInterfaceInstanceShell(tmpl, typeArgs);
	generics.AdoptInterfaceInstance(pInstance, &tmpl, keyArgs);

	//Template completion at the same chokepoint invariant (clone
	//SIGNATURE-COMPLETE templates only): ResolveDataTypes is
	//idempotent and flag-guarded. No implicit-Object super to link —
	//interfaces carry no superclass chain.
	ExprResolver templateCompleter(m_Env);
	templateCompleter.ResolveDataTypes(tmpl, tmpl);

	FillInterfaceInstanceMembers(tmpl, typeArgs, *pInstance);
	return pInstance;
}

} //namespace nlang
