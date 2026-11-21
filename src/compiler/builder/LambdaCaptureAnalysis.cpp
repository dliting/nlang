/*---
    LambdaCaptureAnalysis.cpp — capture promotion pre-pass, analysis
    half (lambda/closure plan Task 6, spec §7.3). One discovery /
    classification walk over the resolved tree:

      1. Discovery/classification walk. Every resolved lambda records its
         enclosing paragraph chain; every identifier inside a lambda body
         whose Field() names storage outside that body is a capture. The
         declaring paragraph of each captured variable (or, for lambda
         parameters, the capturing lambda's own block-body paragraph)
         becomes a display scope.
      2. Anchor assignment. A lambda anchors at the nearest display scope
         within its own emission frame — the chain bounded by the nest
         boundary (paragraphs inside the innermost enclosing lambda's
         body); a lambda whose bounded range has no display scope promotes
         its immediate paragraph. A zero-capture lambda therefore still
         binds a (field-less) display instance — every lambda's emission
         shape is uniform.

    The synthesis half (one "$disp_N" class per display scope, one
    "$lambda_M" method per lambda, bodies moved in) lives in
    LambdaDisplaySynthesis.cpp; the pass internals are shared through
    the header.

    Captured variables keep their descriptors (name lookups still
    resolve to them); the pass only stamps NF_Captured and the
    display-field pointer. The backend's identifier classification
    redirects every reference through the display instance, so there is
    exactly one storage location per captured variable.

    Storage for a captured lambda parameter lives in the owner lambda's
    synthetic frame; nesting through an owner's EXPRESSION body has no
    paragraph to host a display, so that shape is refused loudly until
    generic displays land (plan Task 10).
---*/
#include "LambdaCaptureAnalysis.h"
#include "ModuleRegistry.h"
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnStatements.h>
#include <algorithm>
#include <stdexcept>

namespace nlang
{

CaptureFlavor FlavorOf(const SnField &decl)
{
	if (decl.ContainFlags(NF_Ref))
		return CaptureFlavor::Ref;
	if (decl.ContainFlags(NF_Promoted))
		return CaptureFlavor::Promoted;
	return CaptureFlavor::Value;
}

void CapturePass::Run(SnNamespace &root, ModuleRegistry &registry)
{
	Walk(root);
	AssignAnchors();
	BuildDisplaysAndMethods(root, registry);
}

void RunLambdaCaptureAnalysis(SnNamespace &root, ModuleRegistry &registry)
{
	CapturePass pass;
	pass.Run(root, registry);
}

void CapturePass::MarkDisplayPara(SnParagraph *pPara)
{
	if (!DisplayParaOf(pPara))
		m_displayParas.push_back(pPara);
}

SnParagraph *CapturePass::DisplayParaOf(SnParagraph *pPara) const
{
	return std::find(m_displayParas.begin(), m_displayParas.end(), pPara)
		!= m_displayParas.end() ? pPara : nullptr;
}

void CapturePass::Walk(Node &node)
{
	switch (node.Kind())
	{
	case NK_Paragraph:
		EnterParagraph(static_cast<SnParagraph &>(node));
		return;
	case NK_Function:
		EnterFunction(static_cast<SnFunction &>(node));
		return;
	case NK_LambdaExpr:
		EnterLambda(static_cast<SnLambdaExpr &>(node));
		return;
	case NK_ClassDecl:
	case NK_StructDecl:
		WalkTypeDecl(node);
		return;
	case NK_ThisExpr:
		//`this` inside a lambda body captures the receiver.
		if (!m_lambdaStack.empty())
			m_lambdaStack.back().bReceiverCapture = true;
		return;
	case NK_InvokeExpr:
	{
		//Delegate invoke only — a free-function/method invoke binds an
		//SnFunction, so Callee() answers null exactly on the delegate
		//shape (the backend's IsDelegateInvoke predicate). The callee
		//identifier is detached at resolution, but the arguments keep
		//their own identifier children and are classified by the
		//recursion below.
		auto &inv = static_cast<SnInvokeExpr &>(node);
		if (inv.Field() && !inv.Callee() && !m_lambdaStack.empty())
			ClassifyFieldRead(inv.Field(), m_lambdaStack.back());
		break;
	}
	case NK_IdentifierExpr:
	{
		auto &id = static_cast<SnIdentifierExpr &>(node);
		if (id.Field() && !m_lambdaStack.empty())
			ClassifyIdentifier(id, m_lambdaStack.back());
		break;
	}
	default:
		break;
	}
	for (auto &child : node.Children())
		Walk(child);
}

//Class/struct declarations push the owner stack and walk their members.
//Func/array type descriptors (also class nodes) are rows, not method
//hosts — they skip the stack but their children still walk.
void CapturePass::WalkTypeDecl(Node &node)
{
	auto *pClass = static_cast<SnClassDecl *>(&node);
	if (node.Kind() == NK_ClassDecl
		&& (pClass->IsFuncType() || pClass->IsArrayType()))
	{
		for (auto &child : node.Children())
			Walk(child);
		return;
	}
	m_ownerStack.push_back(pClass);
	for (auto &child : node.Children())
		Walk(child);
	m_ownerStack.pop_back();
}

void CapturePass::EnterParagraph(SnParagraph &para)
{
	//Record the frame context before pushing: the enclosing chain, the
	//nest boundary, the innermost enclosing lambda (the frame-boundary
	//parent for displays inside lambda bodies), and the owner type.
	m_paraChainOf[&para] = m_paraStack;
	m_paraBoundaryOf[&para] = m_lambdaStack.empty()
		? 0 : m_lambdaStack.back().visitDepth;
	m_paraEnclosingLambdaOf[&para] = m_lambdaStack.empty()
		? nullptr : m_lambdaStack.back().pLambda;
	m_declOwnerOfPara[&para] = m_ownerStack.empty()
		? nullptr : m_ownerStack.back();
	m_paraStack.push_back(&para);
	//Harvest this paragraph's local descriptors (lightweight nodes
	//outside the child lists).
	for (auto &stmt : para.Statements())
	{
		if (stmt.Kind() != NK_LocalDeclStmt)
			continue;
		auto &decl = static_cast<SnLocalDeclStmt &>(stmt);
		for (auto &local : decl.Decls())
			if (local.pNode)
				m_declParaOfLocal[local.pNode] = &para;
	}
	for (auto &child : para.Children())
		Walk(child);
	m_paraStack.pop_back();
}

void CapturePass::EnterFunction(SnFunction &func)
{
	//Imported stubs and body-less members carry no captures.
	if (func.ContainFlags(NF_Imported) || !func.Body())
		return;
	m_paraStack.clear();  //defensive: a function body never nests
	m_lambdaStack.clear();
	//A formal's declaring scope is its function's body paragraph — the
	//display hosting a captured formal's storage (and running the
	//prologue copy from the formal's frame slot).
	for (auto &param : func.Params())
		m_declParaOfFormal[&param] = func.Body();
	Walk(*func.Body());
	m_paraStack.clear();
	m_lambdaStack.clear();
}

void CapturePass::EnterLambda(SnLambdaExpr &lambda)
{
	NLANG_ASSERT(lambda.EvalDataType());  //typed before this pass runs
	LambdaSite site;
	site.pLambda = &lambda;
	site.paraChain = m_paraStack;
	site.visitDepth = m_paraStack.size();
	site.boundaryDepth = m_lambdaStack.empty()
		? 0 : m_lambdaStack.back().visitDepth;
	for (auto &up : lambda.ParamLocals())
		m_paramOwnerOf[up.get()] = &lambda;
	m_lambdaStack.push_back(site);
	for (auto &child : lambda.Children())
		Walk(child);
	//Classification marked the stack entry (a copy of the local site) —
	//sync it back before dropping the entry.
	site.bReceiverCapture = m_lambdaStack.back().bReceiverCapture;
	m_lambdaStack.pop_back();
	m_sites.push_back(site);
}

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
	SnParagraph *pDeclPara = nullptr;
	auto itLocal = m_declParaOfLocal.find(&local);
	if (itLocal != m_declParaOfLocal.end())
	{
		pDeclPara = itLocal->second;
	}
	else
	{
		//A parameter of an OUTER lambda in the nest: its storage is
		//the owner's synthetic frame, hosted by the owner's block
		//body paragraph.
		auto itOwner = m_paramOwnerOf.find(&local);
		if (itOwner == m_paramOwnerOf.end())
			return;  //not capturable storage (defensive)
		SnLambdaExpr *pOwner = itOwner->second;
		if (!pOwner->BlockBody())
			throw std::runtime_error(
				"LambdaCaptureAnalysis: a lambda nested in another "
				"lambda's expression body cannot capture the outer "
				"lambda's parameter yet; bind the lambda to a local "
				"first");
		pDeclPara = pOwner->BlockBody();
	}
	//A declaration inside the innermost lambda's own body is frame-
	//local there — no capture.
	if (pDeclPara && m_paraStack.size() > innermost.visitDepth)
	{
		for (size_t i = innermost.visitDepth;
			i < m_paraStack.size(); ++i)
			if (m_paraStack[i] == pDeclPara)
				return;
	}
	RecordCapture(&local, pDeclPara, FlavorOf(local));
}

void CapturePass::RecordCapture(SnField *pDecl, SnParagraph *pDeclPara,
	CaptureFlavor flavor)
{
	NLANG_ASSERT(pDeclPara);
	auto it = m_capturedOf.find(pDecl);
	if (it == m_capturedOf.end())
	{
		CapturedVar var;
		var.pDecl = pDecl;
		var.pDeclPara = pDeclPara;
		var.flavor = flavor;
		it = m_capturedOf.emplace(pDecl, var).first;
		m_capturedByPara[pDeclPara].push_back(&it->second);
		MarkDisplayPara(pDeclPara);
	}
}

void CapturePass::AssignAnchors()
{
	//Walk order: a lambda anchors at the nearest display scope inside
	//its emission frame — the chain range bounded below by the nest
	//boundary; a lambda whose bounded range has no display scope
	//promotes its immediate paragraph.
	for (auto &site : m_sites)
	{
		NLANG_ASSERT(!site.paraChain.empty());  //field initializers carry
			//no paragraphs; resolution only admits constant-foldable
			//initializers, so a lambda there never reaches this pass
		SnParagraph *pAnchor = nullptr;
		for (size_t i = site.paraChain.size(); i > site.boundaryDepth; --i)
		{
			if (DisplayParaOf(site.paraChain[i - 1]))
			{
				pAnchor = site.paraChain[i - 1];
				break;
			}
		}
		if (!pAnchor)
		{
			pAnchor = site.paraChain.back();  //the immediate paragraph
			MarkDisplayPara(pAnchor);
		}
		site.pAnchor = pAnchor;
	}
}

} //namespace nlang
