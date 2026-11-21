/*---
    LambdaCaptureAnalysis.h — capture promotion pre-pass (lambda/closure
    plan Task 6, spec §7.3). Runs AFTER resolution completes and BEFORE
    per-unit codegen: discovers every resolved lambda, classifies which
    locals/formals its body captures, synthesizes the per-paragraph
    display classes plus the $lambda_N synthetic methods, and moves each
    lambda body into its method. Declared here so ModuleBuilderUnits can
    hook it between SweepPendingFuncRefs and codegen. The analysis half
    (discovery / classification / anchor assignment) lives in
    LambdaCaptureAnalysis.cpp; the synthesis half (display classes,
    fields, synthetic methods) in LambdaDisplaySynthesis.cpp.
---*/
#pragma once

#include <cstddef>
#include <map>
#include <vector>

namespace nlang
{

class Node;
class ISourceLocation;
class SnNamespace;
class ModuleRegistry;
class SnParagraph;
class SnField;
class SnClassField;
class SnClassDecl;
class SnFunction;
class SnLambdaExpr;
class SnLocalVar;
class SnFormalParam;
class SnIdentifierExpr;
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
};

class CapturePass
{
public:
	void Run(SnNamespace &root, ModuleRegistry &registry);

private:
	//--- Analysis half (LambdaCaptureAnalysis.cpp) ---
	void Walk(Node &node);
	void WalkTypeDecl(Node &node);
	void EnterParagraph(SnParagraph &para);
	void EnterFunction(SnFunction &func);
	void EnterLambda(SnLambdaExpr &lambda);
	void ClassifyIdentifier(SnIdentifierExpr &id, LambdaSite &innermost);
	void ClassifyFieldRead(SnField *pField, LambdaSite &innermost);
	void ClassifyLocalRead(SnLocalVar &local, LambdaSite &innermost);
	void RecordCapture(SnField *pDecl, SnParagraph *pDeclPara,
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
	std::vector<SnField *> m_ownerStack;   //class/struct declarations
	std::vector<LambdaSite> m_lambdaStack; //currently-descending sites
	std::vector<LambdaSite> m_sites;       //all sites, walk order
	std::map<SnLocalVar *, SnLambdaExpr *> m_paramOwnerOf;
	std::map<SnLocalVar *, SnParagraph *> m_declParaOfLocal;
	std::map<SnFormalParam *, SnParagraph *> m_declParaOfFormal;
	std::map<SnParagraph *, SnField *> m_declOwnerOfPara;
	std::map<SnField *, CapturedVar> m_capturedOf;
	std::map<SnParagraph *, std::vector<CapturedVar *>> m_capturedByPara;
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
