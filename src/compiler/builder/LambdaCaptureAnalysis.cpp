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

    The read-classification family (identifier/invoke reads down to a
    declaring paragraph) lives in LambdaCaptureClassify.cpp and the
    synthesis half (one "$disp_N" class per display scope, one
    "$lambda_M" method per lambda, bodies moved in) in
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
	case NK_ForStmt:
		WalkFor(static_cast<SnForStmt &>(node));
		return;
	case NK_ForeachStmt:
		WalkForeach(static_cast<SnForeachStmt &>(node));
		return;
	case NK_WhileStmt:
		WalkWhile(static_cast<SnWhileStmt &>(node));
		return;
	case NK_DoStmt:
		WalkDo(static_cast<SnDoStmt &>(node));
		return;
	case NK_ClassDecl:
	case NK_StructDecl:
		WalkTypeDecl(node);
		return;
	default:
		ClassifyExprRead(node);
		break;
	}
	for (auto &child : node.Children())
		Walk(child);
}

//The expression arms of Walk's dispatch: receiver and storage reads are
//classified only inside a lambda body (the site stack is empty
//elsewhere). Non-matching kinds fall through untouched; the children
//walk stays with the caller either way.
void CapturePass::ClassifyExprRead(Node &node)
{
	switch (node.Kind())
	{
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
}

void CapturePass::WalkFor(SnForStmt &stmt)
{
	//The body is a paragraph by grammar construction (WrapLoopBody) —
	//the assert-bearing read replaces the old silent skip of
	//single-statement bodies.
	SnParagraph *pBody = stmt.BodyParagraph();
	pBody->SetLoopActivationOwner(pBody);
	//The for-init declarators publish their descriptors through
	//d.pNode (CreateDeclaratorLocal); a non-decl init (an assign to
	//an outer variable) declares nothing and registers no head.
	if (auto *pDecl = dynamic_cast<SnLocalDeclStmt *>(stmt.Init())) {
		for (const auto &decl : pDecl->Decls()) {
			if (decl.pNode) {
				pBody->AddLoopHeadLocal(decl.pNode);
				m_loopHeadParaOf[decl.pNode] = pBody;
			}
		}
	}
	WalkLoopChildren(stmt, pBody);
}

void CapturePass::WalkForeach(SnForeachStmt &stmt)
{
	SnParagraph *pBody = stmt.BodyParagraph();
	pBody->SetLoopActivationOwner(pBody);
	//The foreach variable's descriptor is published on the statement
	//(RegisterForeachLoopVar): the resolver's swap/restore of the
	//paragraph map means this analysis — which runs after resolution
	//finished — cannot recover it by name lookup.
	if (auto *pLocal = stmt.VarLocal()) {
		pBody->AddLoopHeadLocal(pLocal);
		m_loopHeadParaOf[pLocal] = pBody;
	}
	WalkLoopChildren(stmt, pBody);
}

//While/do bodies own an activation boundary like every loop body (spec
//§4.1 — a loop-body scope re-activates per iteration) but declare no
//head locals, so nothing is registered: only the owner is stamped.
void CapturePass::WalkWhile(SnWhileStmt &stmt)
{
	SnParagraph *pBody = stmt.BodyParagraph();
	pBody->SetLoopActivationOwner(pBody);
	WalkLoopChildren(stmt, pBody);
}

void CapturePass::WalkDo(SnDoStmt &stmt)
{
	SnParagraph *pBody = stmt.BodyParagraph();
	pBody->SetLoopActivationOwner(pBody);
	WalkLoopChildren(stmt, pBody);
}

//Shared loop-statement walk: the body paragraph becomes the activation
//owner for every display inside it; the stack propagates that owner to
//nested paragraphs until an inner loop replaces it with its own body.
//WalkFor/WalkForeach set the owner before this runs, so the inheritance
//for inner paragraphs always starts from the body itself.
void CapturePass::WalkLoopChildren(Node &stmt, SnParagraph *pBody)
{
	for (auto &child : stmt.Children()) {
		const bool bEnterBody = pBody && &child == pBody;
		if (bEnterBody)
			m_loopBodyStack.push_back(pBody);
		Walk(child);
		if (bEnterBody)
			m_loopBodyStack.pop_back();
	}
}

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
	if (!m_loopBodyStack.empty())
		para.SetLoopActivationOwner(m_loopBodyStack.back());
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
	m_loopBodyStack.clear();
	//A formal's declaring scope is its function's body paragraph — the
	//display hosting a captured formal's storage (and running the
	//prologue copy from the formal's frame slot).
	for (auto &param : func.Params())
		m_declParaOfFormal[&param] = func.Body();
	Walk(*func.Body());
	m_paraStack.clear();
	m_lambdaStack.clear();
	m_loopBodyStack.clear();
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
	//A lambda body is a fresh emission frame: paragraphs inside it take
	//no loop-activation owner from the enclosing frame's loops. Move the
	//stack aside rather than clearing in place — WalkLoopChildren's
	//push/pop pairing around an enclosing loop body must stay balanced
	//when the walk descends through this lambda and comes back.
	auto savedLoopBodies = std::move(m_loopBodyStack);
	m_loopBodyStack.clear();
	for (auto &child : lambda.Children())
		Walk(child);
	m_loopBodyStack = std::move(savedLoopBodies);
	//Classification marked the stack entry (a copy of the local site) —
	//sync it back before dropping the entry.
	site.bReceiverCapture = m_lambdaStack.back().bReceiverCapture;
	m_lambdaStack.pop_back();
	m_sites.push_back(site);
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
