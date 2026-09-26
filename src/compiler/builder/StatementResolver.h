#pragma once
#include "BuildEnvironment.h"
#include "SyntaxNodeVisitor.h"
#include "SnArrayTypeToken.h"
#include "CastInfo.h"
#include "ExprResolver.h"
#include "SnStatements.h"
#include "SnData.h"

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
	//is defined in StatementResolverSwitchTry.cpp.
	enum class SwitchFamily { None, Int, Float, String };
	static SwitchFamily SwitchFamilyOfKind(NodeKind kind);

	//--- Phase 12 Step 1: switch family gating (D1/D2/D8) ---------------
	//D8 duplicate detection: only FOLDABLE labels (literals and enum
	//member references) are keyed — anything else (calls, variables,
	//computed expressions) is left to runtime first-match-wins.
	struct SwitchLabelKey
	{
		SwitchFamily	family = SwitchFamily::None;
		int32_t		intValue = 0;
		double		floatValue = 0.0;
		std::string	stringValue;
	};

	static bool ExtractSwitchLabelKey(SnExpression& label,
		SwitchLabelKey& key);
	void CheckDuplicateCaseLabels(SnSwitchStmt& sn);
	void Visitor(ISyntaxNodeVisitor *pVisitor);

private:
	void CheckLocalNameReserved(const std::string &name,
		const ISourceLocation *pLoc);
	void CheckIntCondition(SnExpression &cond, const char *what);

public:

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

	void Resolve(SnNamespace &root);
private:
	StatementResolveAccessor m_Accessor;
};

} //namespace nlang
