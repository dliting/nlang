/*---
    LambdaDisplaySynthesis.cpp — capture promotion pre-pass, synthesis
    half (lambda/closure plan Task 6, spec §7.3). Consumes the
    discovery state the analysis half collected (shared through
    CapturePass in LambdaCaptureAnalysis.h), phase by phase in walk
    order:

      3. Synthesis. One "$disp_N" class per display scope holding one
         field per captured variable (first-reference order), plus
         "$this" when a lambda anchored there — or anywhere in its
         display subtree — captures the receiver (a display inside a
         lambda body loads the receiver through the frame boundary, so
         every ancestor on the path needs the field too), plus "$outer"
         when a parent display exists — the parent is the nearest
         enclosing display inside the same frame, or across the frame
         boundary the enclosing lambda's own anchor (the instance local 0
         carries at runtime). One "$lambda_M" method per lambda, formals
         rebuilt from the resolved parameter descriptors, body MOVED in
         (block bodies directly; expression bodies wrapped in a
         synthesized return). Classes go to the root namespace's member
         list end; every user code node generates before any display.

    Type expressions for synthesized nodes must be final at birth —
    resolution never re-runs — hence SynthBornResolvedTypeExpr below.
---*/
#include "LambdaCaptureAnalysis.h"
#include "ModuleRegistry.h"
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnStatements.h>
#include <stdexcept>

namespace nlang
{

//Builds an ALREADY-RESOLVED type expression from a resolved type row.
//An array row (either spelling) becomes a rank-annotated SnArrayTypeExpr
//over the born element; anything else becomes a named reference with the
//field bound and NF_Resolved set. Ordinary synthesis cannot do this: its
//output relies on later resolution passes to bind m_pField, and none run
//after resolution.
SnExpression *SynthBornResolvedTypeExpr(SnField *pRow,
	const ISourceLocation &loc)
{
	NLANG_ASSERT(pRow);
	if (auto *pToken = dynamic_cast<SnArrayTypeToken *>(pRow))
	{
		//Annotated array spelling: the token carries element and rank.
		auto *pElemExpr = dynamic_cast<SnFieldExpr *>(
			SynthBornResolvedTypeExpr(pToken->ElemTypeOf(), loc));
		NLANG_ASSERT(pElemExpr);
		auto *pArr = new SnArrayTypeExpr(pElemExpr, loc, pToken->Rank());
		pArr->EvalDataType(pRow);
		pArr->m_pField = pRow;  //friend channel
		pArr->AddFlags(NF_Resolved);
		return pArr;
	}
	if (pRow->Kind() == NK_ClassDecl
		&& static_cast<SnClassDecl *>(pRow)->IsArrayType())
	{
		//Array<E> generic spelling: rank-1 annotation over the element.
		SnField *pElemRow = ArrayInstantiationElem(pRow);
		NLANG_ASSERT(pElemRow);
		auto *pElemExpr = dynamic_cast<SnFieldExpr *>(
			SynthBornResolvedTypeExpr(pElemRow, loc));
		NLANG_ASSERT(pElemExpr);
		auto *pArr = new SnArrayTypeExpr(pElemExpr, loc, 1);
		pArr->EvalDataType(pRow);
		pArr->m_pField = pRow;
		pArr->AddFlags(NF_Resolved);
		return pArr;
	}
	if (IsBuiltinType(pRow->Kind()))
	{
		auto *pLeaf = new SnIdentifierExpr(pRow->Kind(), loc);
		pLeaf->EvalDataType(pRow);
		pLeaf->m_pField = pRow;
		pLeaf->AddFlags(NF_Resolved);
		return pLeaf;
	}
	auto *pNamed = new SnIdentifierExpr(new std::string(pRow->Name()), loc);
	pNamed->EvalDataType(pRow);
	pNamed->m_pField = pRow;
	pNamed->AddFlags(NF_Resolved);
	return pNamed;
}

void CapturePass::BuildDisplaysAndMethods(SnNamespace &root,
	ModuleRegistry &registry)
{
	CreateDisplayClasses(root, registry);
	ResolveDisplayParents();
	MarkReceiverChains();
	SynthDisplayFields();
	SynthLambdaMethods();
}

//Phase 1: one "$disp_N" class per display scope (walk order), stamped on
//the paragraph (the backend prologue's discovery pointer), appended to
//the root namespace's member list end, and owner-tagged for per-unit
//registration. Fields and methods reference parents via pointers, so
//the classes come first.
void CapturePass::CreateDisplayClasses(SnNamespace &root,
	ModuleRegistry &registry)
{
	for (auto *pPara : m_displayParas)
	{
		DisplayScope scope;
		scope.pPara = pPara;
		scope.pOwnerType = m_declOwnerOfPara[pPara];
		m_displays[pPara] = scope;
	}
	int dispCounter = 0;
	for (auto *pPara : m_displayParas)
	{
		auto *pClass = new SnClassDecl(
			new std::string("$disp_" + std::to_string(dispCounter++)),
			nullptr, new PtrList<SnField>(), *pPara->Location());
		pClass->AddFlags(NF_Generated);
		m_displays[pPara].pClass = pClass;
		m_classOfPara[pPara] = pClass;
		//Stamp the paragraph: the backend's frame prologue discovers the
		//marked paragraphs of its body subtree through this pointer.
		pPara->SetDisplayClass(pClass);
		root.Members().push_back(pClass);
		//Owner tag: per-unit registration gates on OwnerOfContext, and a
		//synthesized class carries no parse-time tag — without one the
		//NO_OWNER fallback would ghost-register every display into every
		//unit image. The paragraph's ancestor chain names the owning unit;
		//its $lambda_N methods inherit the owner through the tagged class.
		registry.TagOwner(*pClass, registry.OwnerOfContext(*pPara));
	}
}

//Phase 2: parent resolution. The parent is the nearest enclosing display
//inside the same frame (chain positions at/above the nest boundary), or
//across the frame boundary the enclosing lambda's own anchor — the
//display instance local 0 carries inside the synthetic frame.
void CapturePass::ResolveDisplayParents()
{
	for (auto *pPara : m_displayParas)
	{
		DisplayScope &scope = m_displays[pPara];
		const auto &chain = m_paraChainOf[pPara];
		const size_t boundary = m_paraBoundaryOf[pPara];
		SnParagraph *pParent = nullptr;
		for (size_t i = chain.size(); i > boundary; --i)
		{
			if (DisplayParaOf(chain[i - 1]))
			{
				pParent = chain[i - 1];
				break;
			}
		}
		if (!pParent)
		{
			//Frame boundary: the enclosing lambda's anchor. A display
			//outside any lambda body has no parent (frame root).
			SnLambdaExpr *pEnclosing = m_paraEnclosingLambdaOf[pPara];
			if (pEnclosing)
			{
				for (auto &site : m_sites)
					if (site.pLambda == pEnclosing)
					{
						pParent = site.pAnchor;
						break;
					}
			}
		}
		//Anchors never sit inside their lambda's own body (the anchor
		//scan is bounded by the visit boundary), so a display can never
		//be its own parent — the $outer chain would not terminate.
		NLANG_ASSERT(pParent != pPara);
		scope.pParentPara = pParent;
		scope.pParentClass = pParent ? m_classOfPara[pParent] : nullptr;
	}
}

//Phase 3: "$this" marking. Every lambda anchored at a display that
//captures the receiver marks that display AND its whole parent chain —
//a display inside a lambda body loads the receiver through the frame
//boundary (local 0 = the anchor instance, whose "$this" the prologue
//reads), so every ancestor on the path needs the field too.
void CapturePass::MarkReceiverChains()
{
	for (auto &site : m_sites)
	{
		if (!site.bReceiverCapture)
			continue;
		SnParagraph *pPara = site.pAnchor;
		while (pPara)
		{
			m_displays[pPara].bNeedsThis = true;
			pPara = m_displays[pPara].pParentPara;
		}
	}
}

//One fixed display field ("$this"/"$outer") over an already-resolved
//type row.
void CapturePass::SynthNamedField(SnClassDecl &cls, SnField &typeRow,
	const ISourceLocation &loc, const char *name)
{
	auto *pTypeExpr = dynamic_cast<SnFieldExpr *>(
		SynthBornResolvedTypeExpr(&typeRow, loc));
	NLANG_ASSERT(pTypeExpr);
	cls.Members().push_back(new SnClassField(pTypeExpr,
		new std::string(name), FA_Private, loc));
}

//One captured variable's display field: the type born from the resolved
//row, the flavor mirrored onto the field flags, and the capture stamp
//plus field pointer published back onto the descriptor.
void CapturePass::SynthCapturedField(SnClassDecl &cls, CapturedVar &var)
{
	auto *pTypeExpr = dynamic_cast<SnFieldExpr *>(
		SynthBornResolvedTypeExpr(var.pDecl->EvalDataType(),
			*var.pDecl->Location()));
	NLANG_ASSERT(pTypeExpr);
	var.pField = new SnClassField(pTypeExpr,
		new std::string(var.pDecl->Name()),
		FA_Private, *var.pDecl->Location());
	//Flavor mirrors onto the field: the GC handle-mask pass and
	//the ref/promoted emission arms classify through the field's
	//flags (the owning local descriptor is out of reach at read
	//sites that resolved straight to the display).
	switch (var.flavor)
	{
	case CaptureFlavor::Ref:
		var.pField->AddFlags(NF_Ref);
		break;
	case CaptureFlavor::Promoted:
		var.pField->AddFlags(NF_Promoted);
		break;
	case CaptureFlavor::Value:
		break;
	}
	cls.Members().push_back(var.pField);
	var.pDecl->AddFlags(NF_Captured);
	if (auto *pLocal = dynamic_cast<SnLocalVar *>(var.pDecl))
		pLocal->SetDisplayField(var.pField);
	else
		static_cast<SnFormalParam *>(var.pDecl)
			->SetDisplayField(var.pField);
}

//Phase 4: fields, per display in walk order — captured variables first,
//then "$this", then "$outer" (the prologue emits in the same order).
void CapturePass::SynthDisplayFields()
{
	for (auto *pPara : m_displayParas)
	{
		DisplayScope &scope = m_displays[pPara];
		auto *pClass = scope.pClass;
		for (auto *pVar : m_capturedByPara[pPara])
			SynthCapturedField(*pClass, *pVar);
		if (scope.bNeedsThis)
		{
			//The receiver's type is the class/struct hosting the frame's
			//method. A receiver capture in a free function is impossible
			//(`this` does not resolve there), so a null owner here means
			//an invariant break, not a user error.
			if (!scope.pOwnerType)
				throw std::runtime_error(
					"LambdaCaptureAnalysis: receiver capture outside a "
					"class method");
			SynthNamedField(*pClass, *scope.pOwnerType,
				*pPara->Location(), "$this");
		}
		if (scope.pParentClass)
			SynthNamedField(*pClass, *scope.pParentClass,
				*pPara->Location(), "$outer");
	}
}

//One lambda's rebuilt formal list: a formal per resolved parameter
//descriptor (same name, type, and flags), the capture marks mirrored
//onto the rebuild so the synthetic frame prologue copies them.
void CapturePass::RebuildLambdaFormals(SnLambdaExpr &lambda,
	const ISourceLocation &loc, UniquePtrList<SnFormalParam> &upParams)
{
	for (auto &upLocal : lambda.ParamLocals())
	{
		SnLocalVar *pLocal = upLocal.get();
		auto *pParamExpr = dynamic_cast<SnFieldExpr *>(
			SynthBornResolvedTypeExpr(pLocal->EvalDataType(), loc));
		NLANG_ASSERT(pParamExpr);
		if (pLocal->ContainFlags(NF_Ref))
			pParamExpr->AddFlags(NF_Ref);
		auto *pFormal = new SnFormalParam(NF_NONE, pParamExpr,
			new std::string(pLocal->Name()), nullptr, loc);
		//The DESCRIPTOR flags are what the backend reads (allocation
		//param kind, signature type descs, the promoted/ref prologues
		//resolve the formal by name) — the born expr's flag alone is
		//invisible to them.
		if (pLocal->ContainFlags(NF_Ref))
			pFormal->AddFlags(NF_Ref);
		if (pLocal->ContainFlags(NF_Promoted))
			pFormal->AddFlags(NF_Promoted);
		//Mirror the parameter's capture onto the rebuilt formal: the
		//synthetic frame prologue copies captured formals by walking
		//the function node's params.
		auto itCaptured = m_capturedOf.find(pLocal);
		if (itCaptured != m_capturedOf.end())
		{
			pFormal->AddFlags(NF_Captured);
			pFormal->SetDisplayField(itCaptured->second.pField);
		}
		upParams.Add(pFormal);
	}
}

//One lambda's synthetic method: formals rebuilt, the body MOVED in
//(block bodies directly; expression bodies wrapped in a synthesized
//return), the display class and method published back onto the lambda.
void CapturePass::SynthLambdaMethod(LambdaSite &site, DisplayScope &scope,
	int &lambdaCounter)
{
	auto &lambda = *site.pLambda;
	const ISourceLocation &loc = *lambda.Location();

	auto *pFuncRow = lambda.EvalDataType();
	auto *pFuncDecl = dynamic_cast<SnClassDecl *>(pFuncRow);
	NLANG_ASSERT(pFuncDecl && pFuncDecl->IsFuncType());
	const auto &typeArgs = pFuncDecl->GenericTypeArgs();
	NLANG_ASSERT(typeArgs.size() == lambda.ParamLocals().size() + 1);
	SnField *pRetRow = typeArgs[0];
	const bool bVoidReturn = pRetRow->Kind() == NK_Void;
	auto *pRetExpr = bVoidReturn
		? nullptr : dynamic_cast<SnFieldExpr *>(
			SynthBornResolvedTypeExpr(pRetRow, loc));

	UniquePtrList<SnFormalParam> upParams;
	RebuildLambdaFormals(lambda, loc, upParams);

	NodeBits methodFlags = NF_Generated
		| (bVoidReturn ? NF_ExplicitVoid : NF_NONE);
	auto *pMethod = new SnFunction(FA_Private, methodFlags, pRetExpr,
		new std::string("$lambda_" + std::to_string(lambdaCounter++)),
		std::move(upParams), loc);

	//Body move — the analysis is fully complete before any move.
	if (lambda.BlockBody())
	{
		pMethod->Body(lambda.DetachBlockBody());
	}
	else
	{
		SnExpression *pExpr = lambda.DetachExprBody();
		NLANG_ASSERT(pExpr);
		UniquePtrList<SnStatement> upStmts;
		upStmts.Add(new SnReturnStmt(pExpr, *pExpr->Location()));
		pMethod->Body(new SnParagraph(std::move(upStmts),
			*pExpr->Location()));
	}

	lambda.DisplayClass(scope.pClass);
	lambda.SyntheticMethod(pMethod);
	scope.pClass->Members().push_back(pMethod);
}

//Phase 5: one "$lambda_M" method per lambda, walk order.
void CapturePass::SynthLambdaMethods()
{
	int lambdaCounter = 0;
	for (auto &site : m_sites)
		SynthLambdaMethod(site, m_displays[site.pAnchor], lambdaCounter);
}

} //namespace nlang
