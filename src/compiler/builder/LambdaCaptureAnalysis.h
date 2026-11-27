/*---
    LambdaCaptureAnalysis.h — capture promotion pre-pass (lambda/closure
    plan Task 6, spec §7.3). Runs AFTER resolution completes and BEFORE
    per-unit codegen: discovers every resolved lambda, classifies which
    locals/formals its body captures, synthesizes the per-paragraph
    display classes plus the $lambda_N synthetic methods, and moves each
    lambda body into its method. Declared here so ModuleBuilderUnits can
    hook it between SweepPendingFuncRefs and codegen. The analysis half
    (discovery / classification / anchor assignment) lives in
    LambdaCaptureAnalysis.cpp plus the read-classification family in
    LambdaCaptureClassify.cpp; the synthesis half (display classes,
    fields, synthetic methods) in LambdaDisplaySynthesis.cpp.
---*/
#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <vector>

namespace nlang
{

class Node;
class ISourceLocation;
class SnNamespace;
class ModuleRegistry;
class SnParagraph;
class SnForStmt;
class SnForeachStmt;
class SnWhileStmt;
class SnDoStmt;
class SnField;
class SnClassField;
class SnClassDecl;
class SnFunction;
class SnLambdaExpr;
class SnLocalVar;
class SnFormalParam;
class SnIdentifierExpr;
class SnCatchClause;
template <typename T> class UniquePtrList;

//Run the capture promotion pass over the merged syntax tree root.
//Attaches display classes to the root namespace's member list (end),
//owner-tagged for per-unit registration. Throws std::runtime_error on
//internal invariant breaks; user-visible errors are impossible here —
//the caller only invokes after a clean ResolveAll.
void RunLambdaCaptureAnalysis(SnNamespace &root, ModuleRegistry &registry);

//The storage flavor of one captured variable, read off its descriptor
//flags — the same predicates the backend's slot classification uses.
enum class CaptureFlavor
{
	Value,     //plain slot copy (scalar handle, record, class/array ref)
	Promoted,  //stable box: the display field holds the box handle
	Ref,       //reference declarator: the display field holds the pair
};

CaptureFlavor FlavorOf(const SnField &decl);

struct CapturedVar
{
	SnField *pDecl = nullptr;          //SnLocalVar or SnFormalParam
	SnParagraph *pDeclPara = nullptr;  //display scope hosting the storage
	SnClassField *pField = nullptr;    //synthesized display field
	CaptureFlavor flavor = CaptureFlavor::Value;
	//v0.13.13a D-1: the declaration keeps its own frame storage (the
	//host formal of a default-value lambda) — the display field is a
	//call-time snapshot mirror, not the storage redirect.
	bool bCallSiteStorage = false;
};

struct DisplayScope
{
	SnParagraph *pPara = nullptr;
	SnField *pOwnerType = nullptr;     //"$this" type row (class/struct)
	SnClassDecl *pClass = nullptr;     //synthesized
	SnParagraph *pParentPara = nullptr;  //"$outer" target (null = root)
	SnClassDecl *pParentClass = nullptr;
	bool bNeedsThis = false;
};

struct LambdaSite
{
	SnLambdaExpr *pLambda = nullptr;
	std::vector<SnParagraph *> paraChain;  //outermost..innermost at visit
	size_t visitDepth = 0;      //paraChain.size() at visit
	size_t boundaryDepth = 0;   //innermost enclosing lambda's visitDepth
	SnParagraph *pAnchor = nullptr;
	bool bReceiverCapture = false;
	//v0.13.13a D-1: the lambda sits at the root of a formal's default
	//value — walked with the function's paragraph stack cleared, so its
	//capture snapshot rides the display anchored at the lambda's own
	//block body (the call-site display, local 0 of the synthetic frame).
	bool bCallSiteDisplay = false;
};

class CapturePass
{
public:
	void Run(SnNamespace &root, ModuleRegistry &registry);

private:
	//--- Analysis half (LambdaCaptureAnalysis.cpp) ---
	void Walk(Node &node);
	void ClassifyExprRead(Node &node);
	void WalkFor(SnForStmt &stmt);
	void WalkForeach(SnForeachStmt &stmt);
	void WalkWhile(SnWhileStmt &stmt);
	void WalkDo(SnDoStmt &stmt);
	//Shared body-owning loop walk (owner stack discipline).
	void WalkLoopChildren(Node &stmt, SnParagraph *pBody);
	void WalkTypeDecl(Node &node);
	void EnterParagraph(SnParagraph &para);
	void EnterFunction(SnFunction &func);
	void EnterLambda(SnLambdaExpr &lambda);
	//I2: the catch variable's declaring paragraph (braced handler = its
	//own paragraph, bare handler = the paragraph hosting the try).
	void HarvestCatchVar(SnCatchClause &clause);
	void ClassifyIdentifier(SnIdentifierExpr &id, LambdaSite &innermost);
	void ClassifyFieldRead(SnField *pField, LambdaSite &innermost);
	void ClassifyLocalRead(SnLocalVar &local, LambdaSite &innermost);
	//Declaring-paragraph arms of ClassifyLocalRead (source-size guard
	//split, zero behavior change): the loop-head arm answers null when
	//the descriptor is not a loop head; the plain arm answers null only
	//for non-capturable storage (defensive miss).
	SnParagraph *DeclParaOfLoopHead(SnLocalVar &local);
	SnParagraph *DeclParaOfPlainLocal(SnLocalVar &local);
	void RecordCapture(SnField *pDecl, SnParagraph *pDeclPara,
		CaptureFlavor flavor);
	//v0.13.13a D-1: the call-site snapshot entry's registration (RecordCapture
	//settled the key paragraph and the off-path test).
	void RecordSnapshotCapture(SnField *pDecl, SnParagraph *pKeyPara,
		CaptureFlavor flavor);
	void AssignAnchors();
	void MarkDisplayPara(SnParagraph *pPara);
	SnParagraph *DisplayParaOf(SnParagraph *pPara) const;

	//--- Synthesis half (LambdaDisplaySynthesis.cpp) ---
	void BuildDisplaysAndMethods(SnNamespace &root, ModuleRegistry &registry);
	void CreateDisplayClasses(SnNamespace &root, ModuleRegistry &registry);
	void ResolveDisplayParents();
	void MarkReceiverChains();
	void SynthDisplayFields();
	void SynthCapturedField(SnClassDecl &cls, CapturedVar &var);
	//v0.13.13a D-1: the snapshot shape's declaration-side publish (the
	//CaptureSource link, the first-set-wins DisplayField).
	void PublishSnapshotCapture(CapturedVar &var);
	void SynthNamedField(SnClassDecl &cls, SnField &typeRow,
		const ISourceLocation &loc, const char *name);
	void SynthLambdaMethods();
	void SynthLambdaMethod(LambdaSite &site, DisplayScope &scope,
		int &lambdaCounter);
	void RebuildLambdaFormals(SnLambdaExpr &lambda,
		const ISourceLocation &loc,
		UniquePtrList<SnFormalParam> &upParams);

	//Discovery state.
	std::vector<SnParagraph *> m_paraStack;
	std::vector<SnParagraph *> m_loopBodyStack;
	std::vector<SnField *> m_ownerStack;   //class/struct declarations
	std::vector<LambdaSite> m_lambdaStack; //currently-descending sites
	std::vector<LambdaSite> m_sites;       //all sites, walk order
	std::map<SnLocalVar *, SnLambdaExpr *> m_paramOwnerOf;
	std::map<SnLocalVar *, SnParagraph *> m_declParaOfLocal;
	std::map<SnFormalParam *, SnParagraph *> m_declParaOfFormal;
	std::map<SnField *, SnParagraph *> m_loopHeadParaOf;
	std::map<SnParagraph *, SnField *> m_declOwnerOfPara;
	std::map<SnField *, CapturedVar> m_capturedOf;
	std::map<SnParagraph *, std::vector<CapturedVar *>> m_capturedByPara;
	//v0.13.13a D-1: call-site capture entries (the lambda inside a
	//formal's default value) live here, not in m_capturedOf: the
	//first-seen single-storage semantics of that map is for ordinary
	//captures, and a dual-captured declaration (host formal snapshotted
	//by the default lambda AND display-hosted by a later ordinary one)
	//needs both entries. The pointers ride m_capturedByPara like the
	//map's own values.
	std::vector<std::unique_ptr<CapturedVar>> m_callSiteVars;
	//Per-paragraph frame context, recorded at push: the enclosing chain
	//(outermost..innermost, excluding the paragraph itself), the chain
	//position where the innermost enclosing lambda's body starts (the
	//nest boundary — emission-frame reachability cut), that lambda, and
	//the class/struct whose method hosts the paragraph.
	std::map<SnParagraph *, std::vector<SnParagraph *>> m_paraChainOf;
	std::map<SnParagraph *, size_t> m_paraBoundaryOf;
	std::map<SnParagraph *, SnLambdaExpr *> m_paraEnclosingLambdaOf;
	//Display scopes in first-marking order (= walk order, which is
	//deterministic for a given input — the synthesis order must be, or
	//the $disp_N/$lambda_N names would jitter in the module tables).
	std::vector<SnParagraph *> m_displayParas;
	//Synthesis state, built phase by phase in walk order.
	std::map<SnParagraph *, DisplayScope> m_displays;
	std::map<SnParagraph *, SnClassDecl *> m_classOfPara;
};

} //namespace nlang
