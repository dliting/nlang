#pragma once
#include "BuildEnvironment.h"
#include "SyntaxNodeVisitor.h"
#include "SnArrayTypeToken.h"
#include "CastInfo.h"
#include "ExprResolver.h"
#include "SnStatements.h"
#include "SnData.h"
#include <vector>

namespace nlang
{

class StatementResolveAccessor
{
public:
	explicit StatementResolveAccessor(BuildEnvironment &env);

	//--- Phase 12 Step 1: switch family model (D1/D2) ----------------------
	//Switch discriminants and labels come in three families: int (including
	//enum values), float, string. Enum member references masquerade as
	//NK_Int32 (SnEnumMember::EvalDataType), and enum-typed variables carry
	//NK_EnumDecl — both map to Int, making enum and int labels one family.
	//Nested here because SwitchLabelKey keys on it; the kind→family mapper
	//is a file-static helper in StatementResolverSwitchTry.cpp (sole consumer).
	enum class SwitchFamily { None, Int, Float, String };

	//--- Phase 12 Step 1: switch family gating (D1/D2/D8) ---------------
	//D8 duplicate detection: only FOLDABLE labels (literals and enum
	//member references) are keyed — anything else (calls, variables,
	//computed expressions) is left to runtime first-match-wins.
	struct SwitchLabelKey
	{
		SwitchFamily	family = SwitchFamily::None;
		//0.7.5: long/ulong labels carry their full 64-bit key (int
		//labels zero-extend into it — one comparison domain for the
		//whole Int family).
		int64_t		intValue = 0;
		double		floatValue = 0.0;
		std::string	stringValue;
	};

	static bool ExtractSwitchLabelKey(SnExpression& label,
		SwitchLabelKey& key);
	void CheckDuplicateCaseLabels(SnSwitchStmt& sn);
	void CheckSwitchLabelFitsDiscriminant(SnExpression& label,
		NodeKind condKind);
	void Visitor(ISyntaxNodeVisitor *pVisitor);

private:
	//Named decomposition phases shared across the per-concern TUs.
	SnParagraph *FindEnclosingParagraph(SyntaxNode *pNode);
	void CheckBoolCondition(SnExpression &cond, const char *what);
	void CheckFunctionNativeFormals(SnFunction &sn,
		std::vector<SnFormalParam*> &formals);
	void ResolveFunctionFormals(SnFunction &sn,
		std::vector<SnFormalParam*> &formals);
	void CheckDefaultForwardRefs(std::vector<SnFormalParam*> &formals,
		size_t i);
	void CheckDefaultTypeCompat(SnFormalParam *param);
	//0.7.5 constant-fit arm of the check above: an explicit-only
	//default must be an in-range constant (assignment-side rule).
	void CheckDefaultConstantFit(SnFormalParam *param,
		SnField *pFormalType);
	//Explicit-value arm of AssignEnumMemberValues: resolve + fold the
	//member's value expression and apply the non-negative invariant.
	bool TryAssignExplicitEnumValue(SnEnumMember &member, SnEnumDecl &sn,
		int32_t &nextValue);
	void RejectIllegalEnumMethod(SnFunction &method);
	void RejectConstWithoutInit(SnLocalDeclStmt &sn);
	SnAssignStmt *BuildLocalInitAssign(SnLocalDeclStmt &sn,
		SnLocalDeclStmt::LocalDecl &decl);
	void RegisterLocalDeclarators(SnLocalDeclStmt &sn,
		SnParagraph &paragraph, SnField *pTypeField, bool bIsArray);
	void ResolveClassBases(SnClassDecl &sn);
	void PropagateVirtualOverrides(SnClassDecl &sn);
	bool TryBindStatementFuncRef(SnExpression &expr,
		SnField *pExpectedType);
	bool TryGetAssignTargetType(SnExpression &left,
		SnField* &pTargetType);
	bool TryGetAssignSourceType(SnExpression &right,
		SnField *pTargetType, SnField* &pSourceType);
	void RejectConstStoreTarget(SnExpression &left,
		const ISourceLocation *pLoc);
	void RejectMethodCallStoreTarget(SnExpression &left,
		const ISourceLocation *pLoc);
	bool FinishInitListAssign(SnAssignStmt &sn);
	void FinishAssignTypeCheck(SnAssignStmt &sn);
	void PropagateInitListTarget(SnAssignStmt &sn);
	bool TryBindSubscriptStoreFuncRef(SnSubscriptAssignStmt &sn);
	bool RejectStringBase(SnSubscriptAssignStmt &sn);
	void ApplyArrayElementStoreCast(SnSubscriptAssignStmt &sn);
	void ApplyContainerStoreCasts(SnSubscriptAssignStmt &sn);
	void CheckForeachSource(SnForeachStmt &sn);
	void MatchForeachElemType(SnForeachStmt &sn, SnField *pVarField);
	void ResolveForInitDecl(SnForStmt &sn);
	SnAssignStmt *BuildForInitAssign(SnLocalDeclStmt &decl,
		SnLocalDeclStmt::LocalDecl &d, SnParagraph *pParagraph);
	static bool ExtractEnumMemberLabel(SnExpression& label,
		SwitchLabelKey& key);
	static bool ExtractLiteralLabel(SnLiteralExpr& lit, SwitchLabelKey& key);
	void ResolveSuperCallArgs(SnSuperCallStmt &sn);
	bool FindParentCtorShape(SnClassDecl *pParent, size_t &parentArity);

public:

	//Pre-pass entry (forward-reference fix): resolve one class's
	//extends/implements before any function body is visited, so an upcast in
	//an earlier-merged consumer TU still sees the inheritance chain.
	void ResolveClassBaseNow(SnClassDecl &sn) { ResolveClassBases(sn); }

	void Access(SnNamespace &sn);
	void Access(SnFunction &sn);
	void Access(SnField &sn);
	void Access(SnReturnStmt &sn);
	void Access(SnExpression &sn);
	void Access(SnLocalDeclStmt &sn);
	void Access(SnAssignStmt &sn);
	void Access(SnIfStmt &sn);
	void Access(SnWhileStmt &sn);
	void Access(SnDoStmt &sn);
	void Access(SnForStmt &sn);
	void Access(SnForeachStmt &sn);
	void Access(SnBreakStmt &sn);
	void Access(SnContinueStmt &sn);
	void Access(SnSwitchStmt &sn);
	void Access(SnCaseClause &sn);
	void Access(SnParagraph &sn);
	void Access(SnEnumDecl &sn);
	void AssignEnumMemberValues(SnEnumDecl &sn);
	void Access(SnEnumMember &sn);
	void Access(SnStructDecl &sn);
	void Access(SnStructField &sn);
	void Access(SnClassDecl &sn);
	void Access(SnClassField &sn);
	void Access(SnNewExpr &sn);
	void Access(SnInvokeStmt &sn);
	void Access(SnCompoundAssignStmt &sn);
	void Access(SnAssertStmt &sn);
	void Access(SnTryStmt &sn);
	void Access(SnCatchClause &sn);
	void Access(SnThrowStmt &sn);
	void Access(SnSuperCallStmt &sn);
	bool IsExceptionSubclass(SnField *t);
	void Access(SnSubscriptAssignStmt &sn);
	void Access(SnThisExpr &sn);
	void Access(SnArrayTypeExpr &);

	//Note: SnAsExpr intentionally has NO Access() override here — it falls
	//through to Access(SnExpression&) which delegates to ExprResolver,
	//which calls ExprResolveAccessor.Access(SnAsExpr&). Providing an empty
	//stub would shadow the catch-all and skip resolution entirely.

	void Access(SnInterfaceDecl &sn);
	void Access(SyntaxNode &sn);

private:
	BuildEnvironment &m_Env;
	ISyntaxNodeVisitor *m_pVisitor;
	SnField *m_pCurrType;
	SnClassDecl *m_pCurrClass = nullptr;
	bool m_inFinallyBody = false;
	ExprResolver m_ExprResolver;
};

class StatementResolver
{
public:
	explicit StatementResolver(BuildEnvironment &env);

	static void PreAssignEnumMemberValues(Node& node,
		StatementResolveAccessor& accessor);

	//Pre-pass: resolve every class's extends/implements before the
	//document-order traversal visits any function body (forward-reference
	//fix for upcasts across the user/library TU merge order).
	static void PreResolveClassBases(Node& node,
		StatementResolveAccessor& accessor);

	void Resolve(SnNamespace &root);
private:
	StatementResolveAccessor m_Accessor;
};

} //namespace nlang
