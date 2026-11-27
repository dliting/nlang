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
    declaring paragraph) lives in LambdaCaptureClassify.cpp; the
    synthesis half (one "$disp_N" class per display scope, one
    "$lambda_M" method per lambda) in LambdaDisplaySynthesis.cpp.

    Captured variables keep their descriptors (name lookups still
    resolve to them); the pass only stamps NF_Captured and the
    display-field pointer. The backend's identifier classification
    redirects every reference through the display instance, so there
    is exactly one storage location per captured variable. A captured
    lambda parameter's storage lives in the owner lambda's synthetic
    frame; Phase 0 below normalizes every expression body to its
    return paragraph BEFORE the walk, so nesting through an owner's
    expression body anchors like block-body nesting always did.
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

//Phase 0: every lambda's expression body is normalized to a return
//paragraph at the lambda's own site, before any capture fact exists.
//The walk, anchor assignment, and synthesis then see exactly one body
//shape — a paragraph — so a lambda nested in an EXPRESSION body
//anchors inside the owner's synthetic frame like block-body nesting
//(currying). Done here, not at synthesis time: the paragraph must
//exist when the nested lambda's chain is recorded.
static void NormalizeLambdaExprBodies(Node &node)
{
	if (node.Kind() == NK_LambdaExpr)
	{
		auto &lambda = static_cast<SnLambdaExpr &>(node);
		if (lambda.ExprBody())
		{
			SnExpression *pExpr = lambda.DetachExprBody();
			UniquePtrList<SnStatement> upStmts;
			upStmts.Add(new SnReturnStmt(pExpr, *pExpr->Location()));
			lambda.AdoptBlockBody(new SnParagraph(std::move(upStmts),
				*pExpr->Location()));
		}
	}
	for (auto &child : node.Children())
		NormalizeLambdaExprBodies(child);
}

void CapturePass::Run(SnNamespace &root, ModuleRegistry &registry)
{
	NormalizeLambdaExprBodies(root);
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
	case NK_CatchClause:
		//Harvest the catch variable's declaring paragraph (I2): the
		//clause's descriptor is not an SnLocalDeclStmt, so the paragraph
		//harvest below never sees it — without this entry the variable
		//classifies as non-capturable and a lambda inside the handler
		//moves away with an unresolvable reference. Children walk below
		//runs the handler body paragraph.
		HarvestCatchVar(static_cast<SnCatchClause &>(node));
		break;
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

//While/do bodies own an activation boundary like every loop body
//(spec §4.1) but declare no head locals: only the owner is stamped.
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
	//D-1 (0.13.13a): a formal's default value evaluates at the CALL
	//site but scopes lexically around the formal. Walking the defaults
	//with the stacks cleared records that shape: a lambda at a
	//default's root arrives with an empty paragraph chain, marking the
	//call-site display (EnterLambda), and a host formal's declaring
	//paragraph is then off the walk path — RecordCapture snapshots it.
	for (auto &param : func.Params())
		if (auto *pValue = param.Value())
			Walk(*pValue);
	m_paraStack.clear();
	m_lambdaStack.clear();
	m_loopBodyStack.clear();
}

//The catch variable's declaring paragraph, mirroring the resolver's
//scoping decision: a braced handler owns its paragraph, a bare-
//statement handler keeps the enclosing paragraph hosting the try. The
//runtime lands the exception in the clause's frame slot; the handler-
//entry copy (EmitStmtTry) bridges it into the display field once a
//lambda captures it.
void CapturePass::HarvestCatchVar(SnCatchClause &clause)
{
	auto *pVar = clause.VarLocal();
	if (!pVar || m_paraStack.empty())
		return;  //unresolved catch type or outside any paragraph (defensive)
	SnParagraph *pDeclPara = (clause.Body()
		&& clause.Body()->Kind() == NK_Paragraph)
		? static_cast<SnParagraph *>(clause.Body())
		: m_paraStack.back();
	m_declParaOfLocal[pVar] = pDeclPara;
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
	//D-1 (0.13.13a): the empty paragraph chain says the lambda was
	//reached through a formal's default value (EnterFunction walks the
	//defaults after clearing the stacks) — its anchor is its own block
	//body and its free variables take snapshot storage (RecordCapture).
	site.bCallSiteDisplay = m_paraStack.empty();
	for (auto &up : lambda.ParamLocals())
		m_paramOwnerOf[up.get()] = &lambda;
	m_lambdaStack.push_back(site);
	//A lambda body is a fresh emission frame: paragraphs inside it take
	//no loop-activation owner from the enclosing frame's loops. Move
	//the stack aside rather than clearing it — WalkLoopChildren's
	//push/pop pairing must stay balanced across this descent.
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
	//D-1 (0.13.13a): a capture seen inside a call-site display (the
	//lambda at a formal's default-value root — see EnterLambda) has no
	//callee frame at closure creation: the storage must ride the
	//display the lambda itself carries, so the entry's key paragraph
	//becomes that lambda's block body (RecordSnapshotCapture). The
	//test is whether the declaring paragraph is on the walk path:
	//inside the call-site lambda (its formals, block locals) keeps
	//ordinary semantics; off-path declarations — the host's formals,
	//the only outer names a default can reference — take the snapshot.
	SnParagraph *pKeyPara = pDeclPara;
	bool bCallSiteStorage = false;
	for (auto it = m_lambdaStack.rbegin(); it != m_lambdaStack.rend(); ++it)
	{
		if (!it->bCallSiteDisplay)
			continue;   //an ordinary site above: keep descending
		if (std::find(m_paraStack.begin(), m_paraStack.end(), pDeclPara)
			== m_paraStack.end())
		{
			pKeyPara = it->pLambda->BlockBody();
			NLANG_ASSERT(pKeyPara);  //Phase 0 normalized every body
			bCallSiteStorage = true;
		}
		break;   //the call-site site is the stack bottom
	}
	if (bCallSiteStorage)
	{
		RecordSnapshotCapture(pDecl, pKeyPara, flavor);
		return;
	}
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

//D-1 (0.13.13a): one snapshot capture entry (the caller settled the
//key paragraph and the off-path test): the declaration keeps its own
//host-frame storage — the display field is a snapshot mirror
//(SynthCapturedField). Entries dedup per (declaration, display): a
//nested call-site lambda re-reads the same host formal and shares the
//field. They stay out of m_capturedOf — its first-seen single-storage
//semantics is for ordinary captures; a dual-captured host formal
//keeps both entries.
void CapturePass::RecordSnapshotCapture(SnField *pDecl,
	SnParagraph *pKeyPara, CaptureFlavor flavor)
{
	for (auto *pOther : m_capturedByPara[pKeyPara])
		if (pOther->pDecl == pDecl)
			return;
	auto up = std::make_unique<CapturedVar>();
	up->pDecl = pDecl;
	up->pDeclPara = pKeyPara;
	up->flavor = flavor;
	up->bCallSiteStorage = true;
	auto *pVar = up.get();
	m_callSiteVars.push_back(std::move(up));
	m_capturedByPara[pKeyPara].push_back(pVar);
	MarkDisplayPara(pKeyPara);
}

void CapturePass::AssignAnchors()
{
	//Walk order: a lambda anchors at the nearest display scope inside
	//its emission frame — the chain range bounded below by the nest
	//boundary; a lambda whose bounded range has no display scope
	//promotes its immediate paragraph.
	for (auto &site : m_sites)
	{
		//D-1 (0.13.13a): the lambda at a formal's default-value root
		//has no enclosing paragraph (EnterFunction walks the defaults
		//with the stacks cleared): its emission frame starts at the
		//lambda, so the anchor is the lambda's own block body — the
		//display constructed at the call site, delivered as local 0 of
		//the synthetic method frame (ResolveDisplayParents maps the
		//self-anchor to a null parent). A non-default lambda always has
		//a chain: field initializers fold to constants or anchor in
		//$staticInit's body paragraph.
		if (site.paraChain.empty())
		{
			site.pAnchor = site.pLambda->BlockBody();
			NLANG_ASSERT(site.pAnchor);  //Phase 0 normalized every body
			MarkDisplayPara(site.pAnchor);
			continue;
		}
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
