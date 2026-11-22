/*---
    LambdaCaptureClassify.cpp — capture promotion pre-pass, classification
    half (lambda/closure plan Task 6, spec §7.3): the read
    classification of one identifier/invoke callee inside a lambda body
    down to a storage declaration and its declaring paragraph (the loop-
    head and plain-local arms), plus the receiver-capture rules. Split
    from LambdaCaptureAnalysis.cpp (source-size guard, zero behavior
    change); the pass internals are shared through the header.
---*/
#include "LambdaCaptureAnalysis.h"
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnStatements.h>
#include <algorithm>
#include <stdexcept>

namespace nlang
{

void CapturePass::ClassifyIdentifier(SnIdentifierExpr &id,
	LambdaSite &innermost)
{
	//A member-access inner names the target member; its receiver is the
	//outer side (a captured object), not `this` — no receiver capture.
	auto *pParent = id.Parent();
	if (pParent && pParent->Kind() == NK_MemberExpr
		&& static_cast<SnMemberExpr *>(pParent)->Inner() == &id)
		return;
	ClassifyFieldRead(id.Field(), innermost);
}

//The storage classification shared by bare identifier reads and
//delegate-invoke callees. A delegate invoke's callee identifier is
//detached at resolution (the binding rides the invoke node), so the
//recursive self-reference `f = ... f(x - 1) ...` names its storage only
//through the invoke — without classifying there, the function value
//would never capture itself.
void CapturePass::ClassifyFieldRead(SnField *pField, LambdaSite &innermost)
{
	if (!pField)
		return;
	//Locals and lambda parameters share the NK_FormalParam kind; the
	//lightweight descriptor answers the dynamic type first.
	if (auto *pLocal = dynamic_cast<SnLocalVar *>(pField))
	{
		ClassifyLocalRead(*pLocal, innermost);
		return;
	}
	if (pField->Kind() == NK_FormalParam)
	{
		//A real formal of an enclosing function (lambda parameters are
		//SnLocalVar descriptors; the lambda answers those first).
		auto *pFormal = static_cast<SnFormalParam *>(pField);
		auto it = m_declParaOfFormal.find(pFormal);
		if (it == m_declParaOfFormal.end())
			return;  //not walked (imported stub) — defensive
		RecordCapture(pFormal, it->second, FlavorOf(*pFormal));
		return;
	}
	if (pField->Kind() == NK_ClassField)
	{
		//Instance member through implicit this: receiver capture. Static
		//members keep module slots; explicit-receiver member reads were
		//filtered above.
		auto *pClassField = static_cast<SnClassField *>(pField);
		if (pClassField->ContainFlags(NF_Static))
			return;
		innermost.bReceiverCapture = true;
		return;
	}
	//Types, functions, enum members: no capture.
}

//The SnLocalVar branch of ClassifyFieldRead: static locals keep slot
//storage in every frame, the innermost lambda's own parameter is frame-
//local, every other local resolves its declaring paragraph — an outer
//lambda's parameter storage lives in the owner's block-body paragraph
//(expression-body owners have no paragraph to host a display and are
//refused loudly until generic displays land).
void CapturePass::ClassifyLocalRead(SnLocalVar &local, LambdaSite &innermost)
{
	if (local.ContainFlags(NF_Static))
		return;
	for (auto &up : innermost.pLambda->ParamLocals())
		if (up.get() == &local)
			return;  //the innermost lambda's own parameter
	SnParagraph *pDeclPara = DeclParaOfLoopHead(local);
	if (!pDeclPara)
		pDeclPara = DeclParaOfPlainLocal(local);
	if (!pDeclPara)
		return;  //not capturable storage (defensive)
	//A declaration inside the innermost lambda's own body is frame-
	//local there — no capture.
	if (m_paraStack.size() > innermost.visitDepth)
	{
		for (size_t i = innermost.visitDepth;
			i < m_paraStack.size(); ++i)
			if (m_paraStack[i] == pDeclPara)
				return;
	}
	RecordCapture(&local, pDeclPara, FlavorOf(local));
}

//Loop-head arm of the declaring-paragraph resolution. A loop-header
//descriptor is declared in the enclosing paragraph for name lookup, but
//its storage belongs to the loop body's per-iteration display once a
//lambda captures it. One shape cannot reach that display and is refused
//loudly instead of miscompiling: a lambda lexically outside the loop
//body (the for header) has no display chain down to the body's display.
//The storage flavors ride along (the display field mirrors the flavor
//and the activation/carry emitters dispatch on it): a reference head's
//pair re-binds per foreach iteration, a promoted head's fresh box
//wraps per iteration (the outgoing iteration's closures and references
//keep the old box, frozen at its final value — Go 1.22 semantics).
//Null = not a loop head.
SnParagraph *CapturePass::DeclParaOfLoopHead(SnLocalVar &local)
{
	auto itLoop = m_loopHeadParaOf.find(&local);
	if (itLoop == m_loopHeadParaOf.end())
		return nullptr;
	SnParagraph *pLoopBody = itLoop->second;
	if (std::find(m_paraStack.begin(), m_paraStack.end(), pLoopBody)
		== m_paraStack.end())
		throw std::runtime_error(
			"LambdaCaptureAnalysis: a lambda capturing the loop-head "
			"variable '" + local.Name() + "' must be inside the loop "
			"body; move it out of the loop header");
	return pLoopBody;
}

//Plain-local arm of the declaring-paragraph resolution: the harvested
//paragraph of an ordinary declaration, or — for a parameter of an OUTER
//lambda in the nest — the owner's block body paragraph (its storage is
//the owner's synthetic frame). Null only on a defensive miss.
SnParagraph *CapturePass::DeclParaOfPlainLocal(SnLocalVar &local)
{
	auto itLocal = m_declParaOfLocal.find(&local);
	if (itLocal != m_declParaOfLocal.end())
		return itLocal->second;
	auto itOwner = m_paramOwnerOf.find(&local);
	if (itOwner == m_paramOwnerOf.end())
		return nullptr;  //not capturable storage (defensive)
	SnLambdaExpr *pOwner = itOwner->second;
	if (!pOwner->BlockBody())
		throw std::runtime_error(
			"LambdaCaptureAnalysis: a lambda nested in another "
			"lambda's expression body cannot capture the outer "
			"lambda's parameter yet; bind the lambda to a local "
			"first");
	return pOwner->BlockBody();
}

} //namespace nlang
