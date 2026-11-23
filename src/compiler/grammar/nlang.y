/*bison version 3.8.2 */
%require "3.0"
%define api.pure
%parse-param { nlang::ScriptParser &parser }
%parse-param { yyscan_t yyscanner }
%lex-param { yyscan_t yyscanner }
%locations
%debug
/*The grammar carries accepted conflicts (2026-11-20 re-measure, bison
3.8.2: 50 shift/reduce + 12 reduce/reduce; the 2026-11-17 state was
35 s/r + 12 r/r). The lambda cover grammar's 15 s/r are the lambda-body
extension state (`ParenPrefix OT_Arrow Expression` shares the Expression
continuation items): every conflicting lookahead is an operator, `as`,
`.` or `[` — one that must extend the body (greedy-body binding, like
assignment in C) — and bison's shift default resolves all of them to
exactly that; the body terminators (`,` `)` `;` `]`) are not Expression
continuations and never conflict, and the resolution is pinned by
TestGreedyBody in test_compiler/test_lambda_parse.cpp. The group's
identifier state stays conflict-free on purpose: the typed-param arm
rides ParenHeadType (the group twin of HeadType), whose lookahead set
never overlaps the expression continuations on '.'/'&'/'<' — see the
ParenHeadType block.
Deterministic parsers cannot use %expect alongside any r/r
conflict, so there is no %expect here. The counts are pinned by the
accepted-conflict ledger below and re-measured by the direct bison
gate on every grammar edit; the build passes -Wno-conflicts-sr and
-Wno-conflicts-rr to stay silent.*/

%code requires {

#include "ScriptLocation.h"
#include "SyntaxTree.h"

typedef void *yyscan_t;

namespace nlang
{

class ScriptParser;

//The %union stores ImportSpec pointers; the full definition is in
//TranslationUnit.h, included by the parser prologue below.
struct ImportSpec;

} //namespace nlang

} /*%code requires */

%{

/*Text code parser for N-Language parser by yacc/bison */
#include "ScriptParser.h"
#include "TranslationUnit.h"  //complete ImportSpec for the import actions
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cctype>
#include <string>
#include <vector>
#include "SnExpressions.h"
#include "SyntaxTree.h"
#include <nlang/runtime/BuiltinGenericNames.h>
#include <nlang/runtime/RnData.h>
#include <nlang/runtime/Config.h>
#include <nlang/runtime/StackDump.h>
using namespace nlang;

//Rank cap at the single production point: compare the full comma
//count before the uint8 truncation, so a 256+ comma literal cannot
//wrap to a small rank. Both OT_BracketsND productions route through
//this one factory.
static SnArrayTypeExpr* MakeArrayTypeND(SnFieldExpr *pElem,
	ISourceLocation& loc, int commaCount, ScriptParser& parser)
{
	const int rank = commaCount + 1;
	if (rank > kMaxArrayRank)
		parser.Log(CLL_Error, loc,
			"array rank %d exceeds the maximum of %d.",
			rank, kMaxArrayRank);
	return new SnArrayTypeExpr(pElem, loc, static_cast<uint8_t>(
		rank > kMaxArrayRank ? kMaxArrayRank : rank));
}

//0.13.10 loop-body normalization: a single-statement loop body
//behaves exactly like a braced body holding that one statement
//(the mainstream reading — C#/Go/C++ all scope the embedded
//statement to the loop), so every loop production wraps its body
//into a paragraph at this single point. Downstream body-shape
//consumers — head scoping, capture displays, per-iteration
//activation — then see one shape instead of branching per site
//on dynamic_cast and silently skipping the non-block form (the
//single-statement capture crash). A null body (the warned empty
//statement ';') becomes an empty paragraph, which the loop
//constructors' non-null body assertions already require.
static SnStatement* WrapLoopBody(SnStatement *pBody,
	const ISourceLocation &loc)
{
	if (dynamic_cast<SnParagraph*>(pBody))
		return pBody;
	UniquePtrList<SnStatement> stmts;
	if (pBody)
		stmts.Add(pBody);
	return new SnParagraph(std::move(stmts), loc);
}

//0.13.10 lambda cover grammar (spec D9): the unified `( InnerList )`
//prefix feeds both the plain parenthesized group and the lambda
//parameter list. These helpers serve the two consuming actions.

//Frees one parameter item's nodes on a reject path (the grammar owns
//the nodes until SnLambdaExpr's constructor takes them).
static void DisposeLambdaParam(nlang::LambdaParam &param)
{
	delete param.pType;
	delete param.pValueExpr;
}

//Folds the arrow-confirmed group's bare identifier items into their name
//slots (the "bare identifier" parameter form); other raw expression
//items stay in pValueExpr for the resolver's parameter gate.
//The '&' declarator has no grammar arm in a group (the expression
//reading owns the '&' lookahead — see ParenHeadType), so `int& x`
//parses as the binary `int & x`. Once the arrow confirms the parameter
//reading, a bitwise binary over two bare identifiers reinterprets as a
//reference parameter: the left identifier becomes the type (kept as the
//same node, preserving its type kind — `int` carries NK_Int32), the
//right identifier's spelling becomes the name, NF_Ref rides the type
//wrapper. Deeper bit-and chains stay raw for the parameter gate.
static void FoldBareIdentifierParams(std::vector<nlang::LambdaParam> &params,
	ISourceLocation &loc)
{
	for (auto &param : params)
	{
		if (!param.pValueExpr)
			continue;
		if (param.pValueExpr->Kind() == NK_BinaryExpr)
		{
			auto *pBinary = static_cast<nlang::SnBinaryExpr *>(
				param.pValueExpr);
			if (pBinary->Op() == nlang::SnBinaryExpr::OP_BitAnd
				&& pBinary->Left()->Kind() == NK_IdentifierExpr
				&& pBinary->Right()->Kind() == NK_IdentifierExpr)
			{
				auto *pLeft = static_cast<nlang::SnIdentifierExpr *>(
					pBinary->Left());
				//Detach the reused left node before the binary's
				//destructor walks its remaining children.
				pBinary->DetachChild(pLeft);
				param.name = static_cast<nlang::SnIdentifierExpr *>(
					pBinary->Right())->Name();
				delete pBinary;
				param.pType = new nlang::SnNameExpr(pLeft, loc);
				param.pType->AddFlags(nlang::NF_Ref);
				param.pValueExpr = nullptr;
				continue;
			}
		}
		if (param.pValueExpr->Kind() == NK_IdentifierExpr)
		{
			param.name = static_cast<nlang::SnIdentifierExpr*>(
				param.pValueExpr)->Name();
			delete param.pValueExpr;
			param.pValueExpr = nullptr;
		}
	}
}

//The non-lambda paren-group post-check (review N3a/N3b/N3c): exactly one
//raw-expression item yields its value — today's `'( Expression ')'
//semantics; the shapes the old grammar rejected as syntax errors get
//semantic diagnostics here. The placeholder returned on the reject paths
//is never resolved (HasError aborts the build before the resolver runs).
static SnExpression* ParenGroupAsExpression(ScriptParser &parser,
	std::vector<nlang::LambdaParam> *pParams, ISourceLocation& loc)
{
	if (pParams->empty())
	{
		parser.Log(CLL_Error, loc,
			"empty parenthesized group is not an expression.");
		return new SnLiteralExpr(*RnInt32::Instance(), 0, loc);
	}
	if (pParams->size() == 1 && (*pParams)[0].pValueExpr)
	{
		//The parenthesized value: ownership passes to the caller.
		SnExpression* pInner = (*pParams)[0].pValueExpr;
		delete pParams;
		return pInner;
	}
	if (pParams->size() == 1)
		parser.Log(CLL_Error, loc,
			"a type annotation is not valid in a parenthesized expression.");
	else
		parser.Log(CLL_Error, loc,
			"a parenthesized group with multiple items is not an expression.");
	for (auto &param : *pParams)
		DisposeLambdaParam(param);
	delete pParams;
	return new SnLiteralExpr(*RnInt32::Instance(), 0, loc);
}

//Phase 9b: scan string literal content for ${identifier} interpolation.
//Returns SnLiteralExpr if no ${...} found; otherwise builds OP_Add tree.
//$$ escape: $$ -> $ (literal dollar).
static SnExpression* BuildStringExpr(
	std::string* pRawText, ISourceLocation& loc, ScriptParser& parser)
{
	//Fast path: no '$' at all -> plain literal. Transfer pRawText ownership.
	if (pRawText->find('$') == std::string::npos)
		return new SnLiteralExpr(*RnString::Instance(), pRawText, loc);

	//Interpolation path: copy content then free lexer's buffer.
	std::string s = *pRawText;
	delete pRawText;
	const size_t n = s.size();

	//NLang identifier rule: [A-Za-z_][A-Za-z0-9_]*  (matches lexer Name rule).
	auto isValidIdent = [](const std::string& nm) {
		if (nm.empty()) return false;
		char c0 = nm[0];
		if (!(std::isalpha(static_cast<unsigned char>(c0))
		      || c0 == '_')) return false;
		for (size_t k = 1; k < nm.size(); ++k) {
			char c = nm[k];
			if (!(std::isalnum(static_cast<unsigned char>(c))
			      || c == '_')) return false;
		}
		return true;
	};

	std::vector<SnExpression*> parts;
	std::string lit;
	size_t i = 0;
	while (i < n)
	{
		if (s[i] == '$' && i + 1 < n)
		{
			if (s[i + 1] == '$') { lit.push_back('$'); i += 2; continue; }
			if (s[i + 1] == '{')
			{
				//Flush accumulated literal (if non-empty).
				if (!lit.empty()) {
					parts.push_back(new SnLiteralExpr(
						*RnString::Instance(), new std::string(lit), loc));
					lit.clear();
				}
				//Find closing '}'
				size_t j = i + 2;
				while (j < n && s[j] != '}') ++j;
				if (j >= n) {
					parser.Log(CLL_Error, loc,
						"unterminated interpolation: missing '}' in \"${...}\".");
					//Free any parts already allocated before bailing.
					for (auto* p : parts) delete p;
					return new SnLiteralExpr(*RnString::Instance(),
						new std::string(""), loc);
				}
				//Extract identifier text between ${ and }
				std::string idName = s.substr(i + 2, j - i - 2);
				if (idName.empty()) {
					parser.Log(CLL_Error, loc,
						"empty interpolation ${}: identifier required.");
				} else if (!isValidIdent(idName)) {
					parser.Log(CLL_Error, loc,
						"invalid identifier \"%s\" in ${...}: "
						"only ${name} supported (no expressions).",
						idName.c_str());
				} else {
					//Undefined identifiers are not checked here; resolver
					//reports "undefined identifier" later, preserving the
					//expected error path.
					parts.push_back(new SnIdentifierExpr(
						new std::string(idName), loc));
				}
				i = j + 1;
				continue;
			}
		}
		lit.push_back(s[i]);
		++i;
	}
	//Flush trailing literal.
	if (!lit.empty()) {
		parts.push_back(new SnLiteralExpr(
			*RnString::Instance(), new std::string(lit), loc));
	}

	//No interpolation parts (e.g., only invalid ${} that logged errors).
	if (parts.empty())
		return new SnLiteralExpr(*RnString::Instance(),
			new std::string(""), loc);

	//If first part is an identifier (not string literal), prepend empty
	//string literal "" to force string context. Otherwise two int
	//identifiers would produce arithmetic OP_Add instead of concat.
	//   "${a}${b}" with a,b both int -> Add(Add("", a), b) -> string ctx.
	if (parts[0]->Kind() != NK_LiteralExpr) {
		auto* pEmpty = new SnLiteralExpr(*RnString::Instance(),
			new std::string(""), loc);
		parts.insert(parts.begin(), pEmpty);
	}

	//Left-associative fold: ((p0 + p1) + p2) + ...
	SnExpression* result = parts[0];
	for (size_t k = 1; k < parts.size(); ++k) {
		result = new SnBinaryExpr(SnBinaryExpr::OP_Add, result, parts[k], loc);
	}
	return result;
}

//0.8.3: the func keyword's generic spellings share one builder — the
//keyword is not an identifier token, so the base-name node is created
//from kBuiltinFuncTypeName; the resolver's generic dispatch matches on
//that base name (IsBuiltinGenericTypeName).
static SnGenericTypeExpr* MakeFuncType(
	std::vector<SnFieldExpr*> *pArgs, const ISourceLocation &loc)
{
	auto* pBase = new SnIdentifierExpr(new std::string(kBuiltinFuncTypeName), loc);
	return new SnGenericTypeExpr(new SnNameExpr(pBase, loc), pArgs, loc);
}

//Phase 4b: flatten a MemberExpr point-chain (a.b.c) into identifier
//segments for a qualified type reference. The chain is parsed with the
//normal member-access shifts, then converted only when it is reduced in
//a type position — this keeps a lone identifier a plain NameExpr and
//avoids a reduce/reduce over a single token (a dedicated dotted category
//at a statement head steals '.' from member access; a 2026-09-29 probe
//grammar with such a category measured +2 shift/reduce on '.').
//Returns false when the chain is not made of identifiers (a call, a
//subscript, a parenthesised outer) — the caller marks the node malformed
//and ExprResolver reports it, so no unchecked downcast happens here.
static bool CollectQualifiedSegments(
		SnExpression *pExpr, std::vector<std::string> &out)
{
	if (pExpr->Kind() == NK_IdentifierExpr)
	{
		out.push_back(static_cast<SnIdentifierExpr*>(pExpr)->Name());
		return true;
	}
	if (pExpr->Kind() != NK_MemberExpr)
		return false;
	auto& member = static_cast<SnMemberExpr&>(*pExpr);
	SnExpression* pInner = member.Inner();
	if (pInner == nullptr || pInner->Kind() != NK_IdentifierExpr)
		return false;   //a.b() — the inner side is an InvokeExpr
	if (!CollectQualifiedSegments(member.Outer(), out))
		return false;
	out.push_back(static_cast<SnIdentifierExpr*>(pInner)->Name());
	return true;
}

%}

/*data value union */
%union {
    nlang::nchar           					v_Char;
    nlang::int32        					v_Int;
    nlang::int64        					v_Long;
    nlang::uint64      					v_ULong;
    float               					v_Float;
    double             					v_Double;
    std::string *           				v_pStr;
    nlang::SnUsing *						v_pUsing;
    nlang::PtrList<nlang::SnUsing> *		v_pUsingList;
    std::vector<nlang::ImportSpec> *		v_pImportList;
    std::vector<std::string> *				v_pStrList;
    nlang::ImportSpec *						v_pImportSpec;
    nlang::SnFunction  *      				v_pFunction;
	nlang::PtrList<nlang::SnFormalParam> *	v_pFormalParamList;
	nlang::SnFormalParam *					v_pFormalParam;
    nlang::SnStatement *					v_pStatement;
    nlang::SnParagraph  *					v_pParagraph;
    nlang::PtrList<nlang::SnStatement> *	v_pStatementList;
	nlang::PtrList<nlang::SnExpression> *	v_pExpressionList;
    nlang::SnExpression *					v_pExpression;
	nlang::SnMemberExpr *					v_pMemberExpr;
	nlang::SnInvokeExpr *					v_pInvokeExpr;
	nlang::SnIdentifierExpr *				v_pIdentifierExpr;
	nlang::SnNameExpr *						v_pNameExpr;
	nlang::SnFieldExpr *					v_pFieldExpr;
	nlang::SnBinaryExpr *					v_pBinaryExpr;
	nlang::SnLocalDeclStmt *				v_pLocalDeclStmt;
	nlang::SnAssignStmt *					v_pAssignStmt;
	nlang::SnCompoundAssignStmt *			v_pCompoundAssignStmt;
	nlang::SnSubscriptAssignStmt *			v_pSubscriptAssignStmt;
	nlang::SnSubscriptExpr *				v_pSubscriptExpr;
	nlang::SnNewArrayExpr *					v_pNewArrayExpr;
	nlang::SnIfStmt *						v_pIfStmt;
	nlang::SnWhileStmt *					v_pWhileStmt;
	nlang::SnDoStmt *						v_pDoStmt;
	nlang::SnForStmt *						v_pForStmt;
	nlang::SnForeachStmt *					v_pForeachStmt;
	nlang::SnSwitchStmt *					v_pSwitchStmt;
	nlang::SnCaseClause *					v_pCaseClause;
	std::vector<nlang::SnCaseClause*> *		v_pCaseClauseList;
	nlang::SnTryStmt *						v_pTryStmt;
	nlang::SnCatchClause *					v_pCatchClause;
	std::vector<nlang::SnCatchClause*> *	v_pCatchClauseList;
	nlang::SnThrowStmt *					v_pThrowStmt;
	nlang::SnBreakStmt *					v_pBreakStmt;
	nlang::SnContinueStmt *				v_pContinueStmt;
	nlang::SnEnumDecl *					v_pEnumDecl;
	nlang::SnEnumMember *				v_pEnumMember;
	nlang::PtrList<nlang::SnEnumMember> *	v_pEnumMemberList;
	nlang::PtrList<nlang::SnFunction> *	v_pEnumMethodList;
	nlang::SnStructDecl *				v_pStructDecl;
	nlang::SnStructField *				v_pStructField;
	nlang::PtrList<nlang::SnField> *	v_pStructMemberList;
	nlang::SnClassDecl *				v_pClassDecl;
	nlang::PtrList<nlang::SnField> *	v_pClassMemberList;
	nlang::SnInterfaceDecl *			v_pInterfaceDecl;
	nlang::PtrList<nlang::SnFieldExpr> *	v_pNameExprList;
	std::vector<nlang::SnFieldExpr*>*	v_pFieldExprVec;
	std::vector<nlang::SnLocalDeclStmt::LocalDecl> * v_pLocalDeclList;
    nlang::PtrList<nlang::SnField> *		v_pMemberList;
	nlang::SnField *						v_pField;
	nlang::SyntaxNode *						v_pSyntaxNode;
	std::vector<nlang::SyntaxNode*>	*v_pSyntaxNodeVec;
	nlang::NodeBits							v_NodeFlags;
	nlang::FieldAccessType					v_AccessType;
	std::vector<nlang::InitEntry>*			v_pInitEntryList;
	nlang::InitEntry*						v_pInitEntry;
	std::vector<nlang::LambdaParam>*		v_pLambdaParamList;
	nlang::LambdaParam*						v_pLambdaParam;
}

%destructor { } <v_Char> <v_Int> <v_Long> <v_ULong> <v_Float> <v_Double>
%destructor { } <v_NodeFlags> <v_AccessType>
%destructor { delete $$; } <*>

%printer { fprintf (yyoutput, "%d", $$); } <v_Int>
%printer { fprintf (yyoutput, "%lld", (long long)$$); } <v_Long>
%printer { fprintf (yyoutput, "%llu", (unsigned long long)$$); } <v_ULong>
%printer { fprintf (yyoutput, "%g", $$); } <v_Float>
%printer { fprintf (yyoutput, "%g", $$); } <v_Double>
%printer { fprintf (yyoutput, "%u", $$); } <v_AccessType>
%printer { fprintf (yyoutput, "%llu", (unsigned long long)$$); } <v_NodeFlags>
%printer { fprintf (yyoutput, "\"%s\"", $$->c_str()); } <v_pStr>
%printer { fprintf (yyoutput, "&%p", (void*)$$); } <*>

/*declare nonterminals */
%type <v_AccessType>    		AccessType
%type <v_AccessType>			AccessTypeNonEmpty
%type <v_AccessType>			AccessTypeEmpty
%type <v_NodeFlags>    			NodeFlag
%type <v_NodeFlags>				NodeFlagsNonEmpty
%type <v_pNameExpr>				NameExpr
%type <v_pFieldExpr>				Type TypeArg QualifiedType HeadType ParenHeadType TypeName ImplementsName
%type <v_pFieldExpr>				AsGenericTarget
%type <v_pFieldExpr>				QualifiedGenericReceiver
%type <v_pFieldExpr>				TypeRef FuncTypeArg
%type <v_pFieldExprVec>			TypeList FuncTypeList
%type <v_pIdentifierExpr>		IdentifierExpr
%type <v_pInvokeExpr>			InvokeExpr
%type <v_pMemberExpr>			MemberExpr
%type <v_pUsing>				Using
%type <v_pUsingList>			UsingList
%type <v_pImportList>			ImportList
%type <v_pImportSpec>			ImportPath ImportTail
%type <v_pField>				NamespaceMember
%type <v_pSyntaxNode>			TopItem
%type <v_pSyntaxNodeVec>		TopMemberList
%type <v_pFormalParam>			FormalParam
%type <v_pFormalParamList>		FormalParamList
%type <v_pExpression>			Expression ParenthesesExpr LiteralExpr SubscriptExpr
%type <v_pExpressionList>		ConcreteParamList SubscriptItemList
%type <v_pExpression>			ConcreteParam BracedInitExpr SubscriptItem
%type <v_pExpression>			SliceItem SliceHead SliceStop SliceStepOpt SliceStep
%type <v_pInitEntryList>		InitEntries InitEntryList
%type <v_pInitEntry>			InitEntry
%type <v_pLambdaParamList>		InnerList ParenPrefix
%type <v_pLambdaParam>			InnerListElem
%type <v_pExpression>			LambdaExpr
%type <v_pFunction>				TopBareFunc TopModFunc TopBareHeader TopModHeader TopGenericBareHeader TopGenericModHeader
%type <v_pParagraph>			Paragraph DefaultCase FunctionBodyOrSemi
%type <v_pStatementList>		StatementList
%type <v_pStatement>			Statement ReturnStmt InvokeStmt LocalDeclStmt DeclLocalConst AssignStmt CompoundAssignStmt AssertStmt SubscriptAssignStmt IfStmt WhileStmt InitFor FiniFor TryStmt ThrowStmt SuperCallStmt FinallyClauseOpt
%type <v_pForStmt>		ForStmt
%type <v_pForeachStmt>	ForeachStmt
%type <v_pCatchClause>		CatchClause
%type <v_pCatchClauseList>	CatchClauseList
%type <v_pDoStmt>		DoStmt
%type <v_pSwitchStmt>	SwitchStmt
%type <v_pExpressionList>	CaseLabelList
%type <v_pCaseClause>	CaseClause
%type <v_pCaseClauseList>	CaseClauseList
%type <v_pBreakStmt>		BreakStmt
%type <v_pContinueStmt>	ContinueStmt
%type <v_pLocalDeclList>		LocalDeclList
%type <v_pEnumDecl>			EnumDecl
%type <v_pEnumDecl>			EnumDeclCore
%type <v_pEnumMember>		EnumMember
%type <v_pEnumMemberList>	EnumMemberList
%type <v_pEnumMethodList>	EnumMethodSection EnumMethodList
%type <v_pFunction>		EnumMethod
%type <v_pStructDecl>		StructDecl
%type <v_pStructDecl>		StructDeclCore
%type <v_pStructField>		StructField
%type <v_pField>			StructMember
%type <v_pFunction>		StructCtor
%type <v_pStructMemberList>	StructMemberList
%type <v_pClassDecl>		ClassDecl
%type <v_pClassDecl>		ClassDeclCore
%type <v_pStrList>			TypeParamsOpt TypeParams TypeParamRests
%type <v_pInterfaceDecl>	InterfaceDecl
%type <v_pInterfaceDecl>	InterfaceDeclCore
%type <v_pNameExprList>		ImplementsOpt NameList
%type <v_pClassMemberList>	InterfaceMemberList
%type <v_pField>			InterfaceMember
%type <v_pField>			ClassMember
%type <v_pClassMemberList>	ClassMemberList
%type <v_pFieldExpr>			ClassInheritOpt

%start CompileUnit

/*declare tokens */
%token <v_Char>		TT_Char
%token <v_pStr>		TT_Identifier
%token <v_pStr>		TT_String /*Note: TT_String is not KT_String */
%token <v_Int>		TT_Int
%token <v_Long>		TT_Long
%token <v_ULong>	TT_ULong
%token <v_Float>	TT_Float
%token <v_Double>	TT_Double
%token TT_Comment	TT_Error

/*keyword type */
/*NOTE: keyword tokens KT_Bool..KT_While must stay one contiguous
  block -- nide's SyntaxHighlighter colors the whole [KT_Bool, KT_While]
  number range as keywords. */
%token KT_Bool
%token KT_As
%token KT_Assert
%token KT_Break
%token KT_Byte
%token KT_Case
%token KT_Catch
%token KT_Char
%token KT_Class
%token KT_Const
%token KT_Continue
%token KT_Do
%token KT_Default
%token KT_Double
%token KT_Else
%token KT_Enum
%token KT_False
%token KT_Finally
%token KT_Float
%token KT_For
%token KT_Foreach
%token KT_Func
%token KT_If
%token KT_Implements
%token KT_Import
%token KT_In
%token KT_Int
%token KT_Interface
%token KT_Long
%token KT_Native
%token KT_Null
%token KT_Out
%token KT_Private
%token KT_Protected
%token KT_Public
%token KT_Return
%token KT_Short
%token KT_Static
%token KT_String
%token KT_Struct
%token KT_Super
%token KT_Switch
%token KT_This
%token KT_Throw
%token KT_True
%token KT_Try
%token KT_TypeName
%token KT_Ubyte
%token KT_Uint
%token KT_ULong
%token KT_Ushort
%token KT_Using
%token KT_Virtual
%token KT_Void
%token KT_While

/*double-character operator type */
%token OT_LSH
%token OT_RSH
%token OT_LSHS
%token OT_RSHS
%token OT_LE
%token OT_GE
%token OT_NE
%token OT_EQ
%token OT_Arrow
%token OT_AND
%token OT_OR
%token OT_MODS
%token OT_BANDS
%token OT_MULS
%token OT_INCS
%token OT_DECS
%token OT_DIVS
%token OT_BORS
%token OT_BXORS
%token OT_Brackets
%token <v_Int>	OT_BracketsND
%token OT_TypeLt

%nonassoc P_Then
%nonassoc KT_Else

%right OT_BXORS OT_BORS OT_DIVS OT_DECS OT_INCS OT_MULS OT_BANDS OT_MODS
%left OT_OR
%left OT_AND
%left '|'
%left '^'
%left '&'
%left OT_EQ OT_NE
%left OT_GE '>' OT_LE '<'
%left OT_RSH OT_LSH
%left '-' '+'
%left '/' '*' '%'
%right '~' P_Minus '!'
%left KT_As

%nonassoc P_NonMember
%left '.' '['
%nonassoc P_Field
%nonassoc '(' ')'
%nonassoc P_ClassMethod
%nonassoc P_ClassField

%code provides {

#include "TranslationUnit.h"
#include "BuildEnvironment.h"
#include "Utils.h"

#ifndef YY_USER_DEFS_
#define YY_USER_DEFS_

namespace nlang
{

typedef yyscan_t NScanInfo;
typedef YYSTYPE NToken;

} //namespace nlang

void yyerror(YYLTYPE *loc, nlang::ScriptParser&, yyscan_t, char *s, ...);

extern int yylex \
    (YYSTYPE *yylval_param, YYLTYPE *yylloc_param, yyscan_t yyscanner);


#endif //YY_USER_DEFS_

}

%%

CompileUnit:	ImportList UsingList TopMemberList {
						//v0.9.1 script mode: split top-level items into unit members (functions
						//and type declarations) and script statements. The statement paragraph is
						//later wrapped into a synthesized int main() by ModuleBuilder (D2).
						TranslationUnit *pTransUnit = parser.TransUnit();
						PtrList<SnField>* pMembers = new PtrList<SnField>();
						PtrList<SnStatement>* pStmts = new PtrList<SnStatement>();
						for (nlang::SyntaxNode* pItem : *$3)
						{
							if (nlang::IsUnitMemberKind(pItem->Kind()))
								pMembers->push_back(static_cast<SnField*>(pItem));
							else
								pStmts->push_back(static_cast<SnStatement*>(pItem));
						}
						pTransUnit->Init($2, pMembers, @3);
						pTransUnit->SetImports($1);
						if (!pStmts->empty())
							pTransUnit->SetScriptParagraph(new SnParagraph(pStmts, @3));
						else
							delete pStmts;
						delete $3;
					} ;

ImportList:	ImportList KT_Import ImportTail ';' {
						$1->push_back(*($3));
						delete $3;
						$$ = $1;
					} |
					ImportList KT_Import TT_String ';' {
						//The string form is removed. Keep the production so the
						//diagnostic names the mistake instead of a generic
						//syntax error.
						parser.Log(CLL_Error, @3,
							"String import is removed. Use "
							"'import <module>;' with an identifier path.");
						delete $3;
						$$ = $1;
					} |
					{
						/*on empty */
						$$ = new std::vector<ImportSpec>();
					} ;

ImportTail:	ImportPath {
						$$ = $1;
					} |
					ImportPath '.' '*' {
						$1->wildcard = true;
						$$ = $1;
					} ;

ImportPath:	TT_Identifier {
						$$ = new ImportSpec();
						$$->segments.push_back(*($1));
						delete $1;
					} |
					ImportPath '.' TT_Identifier {
						$1->segments.push_back(*($3));
						delete $3;
						$$ = $1;
					} ;

UsingList:	UsingList Using {
					$1->push_back($2);
					$$ = $1;
				} |
				{
					/*on empty */
					$$ = new PtrList<SnUsing>();
				} ;

Using:	KT_Using NameExpr ';' {
			   $$ = new SnUsing($2, @2);
			} |
			//Phase 13: type alias form. The NameExpr is the alias name; the
			//Type is the aliased target, expanded at use sites by the TU-level
			//alias pre-pass (runs before translation units are merged).
			KT_Using NameExpr '=' Type ';' {
			   $$ = new SnUsing($2, $4, @2);
			} ;

//v0.9.1 script mode: top-level list mixing declarations and script statements.
TopMemberList:	TopMemberList TopItem {
						if ($2 != nullptr)
							$1->push_back($2);
						$$ = $1;
					} |
					{
						/*on empty */
						$$ = new std::vector<nlang::SyntaxNode*>();
					} ;

TopItem:	NamespaceMember {
						$$ = $1;
					} |
					Statement {
						$$ = $1;
					} |
					TopBareFunc {
						$$ = $1;
					} |
					TopModFunc {
						$$ = $1;
					} |
					//v0.13.0 D5: bare const-led function shapes parse here so
					//the rejection can name the rule — otherwise the shape
					//rides the statement path and dies at the body or param
					//tokens with a bare syntax error. Top-level only: the
					//in-body `const T p();` zero-argument construction keeps
					//its LocalDeclList reading. The public spellings create an
					//SnFunction and are named by the resolver contract check.
					//Adds one shared-paren conflict state (1 s/r + 2 r/r) of
					//the accepted epsilon family — see the FormalParamList
					//comment and the ledger header counts.
					KT_Const HeadType TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
						parser.Log(CLL_Error, @1,
							"const is not allowed on functions; declare it without const.");
						$$ = nullptr;
					} |
					KT_Const KT_Void TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
						parser.Log(CLL_Error, @1,
							"const is not allowed on functions; declare it without const.");
						$$ = nullptr;
					} ;

//v3H (updated 0.9.15): the bare form without a body is a declaration and
//moves behind the explicit KT_Func marker (func Foo f();). The bare
//"T name();" is no longer ambiguous: the explicit empty-parens arms make
//')' a shift, and the token after ')' decides — '{' reads a zero-parameter
//definition, ';' or ',' reads a zero-argument construction (the same
//reading the in-body form has always had).
TopBareFunc:	TopBareHeader Paragraph {
						$1->Body($2);
						$$ = $1;
					} |
					KT_Func TopBareHeader ';' {
						$2->AddFlags(NF_Abstract);
						$$ = $2;
					} |
					//0.13.1a: the generic spellings ride the same Paragraph /
					//abstract ';' assemblies as the plain header.
					TopGenericBareHeader Paragraph {
						$1->Body($2);
						$$ = $1;
					} |
					KT_Func TopGenericBareHeader ';' {
						$2->AddFlags(NF_Abstract);
						$$ = $2;
					} ;

//0.13.1a: generic free function declarations. A REQUIRED TypeParams
//bracket on arms of its own — NOT the optional TypeParamsOpt the class
//head uses. After `Type name` the '(' lookahead must keep the baseline
//deferred decision between the function-formals reading and the
//constructor-initializer local reading (LocalDeclList shares this state);
//an epsilon bracket there would lose to shift-preference and hijack every
//ordinary function definition into LocalDeclList. The '<'-family
//lookaheads (plain '<' for unknown names, OT_TypeLt for probe-matched
//spellings like `f<T>(`) have no competing derivation in this state.
TopGenericBareHeader:	HeadType TT_Identifier TypeParams '(' FormalParamList ')' {
							$$ = new SnFunction(FA_Default, NF_NONE, $1, $2, $5, @1);
							$$->SetTypeParamNames(std::move(*$3));
							delete $3;
						} |
						HeadType TT_Identifier TypeParams '(' ')' {
							$$ = new SnFunction(FA_Default, NF_NONE, $1, $2, new PtrList<SnFormalParam>(), @1);
							$$->SetTypeParamNames(std::move(*$3));
							delete $3;
						} |
						//v0.13.9 phase 3: `T& pick<T>(...)` — the '&' mark rides
						//the return-type node (same convention as the out mark).
						HeadType '&' TT_Identifier TypeParams '(' FormalParamList ')' {
							$1->AddFlags(NF_Ref);
							$$ = new SnFunction(FA_Default, NF_NONE, $1, $3, $6, @1);
							$$->SetTypeParamNames(std::move(*$4));
							delete $4;
						} |
						HeadType '&' TT_Identifier TypeParams '(' ')' {
							$1->AddFlags(NF_Ref);
							$$ = new SnFunction(FA_Default, NF_NONE, $1, $3, new PtrList<SnFormalParam>(), @1);
							$$->SetTypeParamNames(std::move(*$4));
							delete $4;
						} |
						KT_Void TT_Identifier TypeParams '(' FormalParamList ')' {
							$$ = new SnFunction(FA_Default, NF_NONE | NF_ExplicitVoid, nullptr, $2, $5, @1);
							$$->SetTypeParamNames(std::move(*$3));
							delete $3;
						} ;

TopBareHeader:	HeadType TT_Identifier '(' FormalParamList ')' {
						$$ = new SnFunction(FA_Default, NF_NONE, $1, $2, $4, @1);
					} |
					HeadType TT_Identifier '(' ')' {
						$$ = new SnFunction(FA_Default, NF_NONE, $1, $2, new PtrList<SnFormalParam>(), @1);
					} |
					//v0.13.9 phase 3: `int& f()` — inline '&' arm; the mark
					//rides the return-type node.
					HeadType '&' TT_Identifier '(' FormalParamList ')' {
						$1->AddFlags(NF_Ref);
						$$ = new SnFunction(FA_Default, NF_NONE, $1, $3, $5, @1);
					} |
					HeadType '&' TT_Identifier '(' ')' {
						$1->AddFlags(NF_Ref);
						$$ = new SnFunction(FA_Default, NF_NONE, $1, $3, new PtrList<SnFormalParam>(), @1);
					} |
					KT_Void TT_Identifier '(' FormalParamList ')' {
						$$ = new SnFunction(FA_Default, NF_NONE | NF_ExplicitVoid, nullptr, $2, $4, @1);
					} ;

//Modifier-led top-level function. The modifier slot never derives empty:
//(A,empty) enters on a literal access keyword, (empty,F) on a flag keyword
//(the AccessType epsilon reduces only on flag lookaheads, none of which
//starts a statement), (A,F) on both.
TopModFunc:	TopModHeader FunctionBodyOrSemi {
						if ($2 == nullptr)
							$1->AddFlags(NF_Abstract);
						else
							$1->Body($2);
						$$ = $1;
					} |
					TopGenericModHeader FunctionBodyOrSemi {
						if ($2 == nullptr)
							$1->AddFlags(NF_Abstract);
						else
							$1->Body($2);
						$$ = $1;
					} ;

//Generic variants of the modifier-led header — same matrix, required
//TypeParams bracket, same reasoning as TopGenericBareHeader above.
//v0.13.9 phase 3: each type-slot arm gains an inline-'&' twin (`static
//int& f()`, `static T& pick<T>(...)`); the mark rides the type node.
//KT_Void keeps no twin: `void&` is structurally meaningless.
TopGenericModHeader:	AccessTypeNonEmpty HeadType TT_Identifier TypeParams '(' FormalParamList ')' {
							$$ = new SnFunction($1, NF_NONE, $2, $3, $6, @2);
							$$->SetTypeParamNames(std::move(*$4));
							delete $4;
						} |
						AccessTypeNonEmpty HeadType '&' TT_Identifier TypeParams '(' FormalParamList ')' {
							$2->AddFlags(NF_Ref);
							$$ = new SnFunction($1, NF_NONE, $2, $4, $7, @2);
							$$->SetTypeParamNames(std::move(*$5));
							delete $5;
						} |
						AccessTypeNonEmpty KT_Void TT_Identifier TypeParams '(' FormalParamList ')' {
							$$ = new SnFunction($1, NF_NONE | NF_ExplicitVoid, nullptr, $3, $6, @2);
							$$->SetTypeParamNames(std::move(*$4));
							delete $4;
						} |
						AccessTypeEmpty NodeFlagsNonEmpty HeadType TT_Identifier TypeParams '(' FormalParamList ')' {
							$$ = new SnFunction($1, $2, $3, $4, $7, @3);
							$$->SetTypeParamNames(std::move(*$5));
							delete $5;
						} |
						AccessTypeEmpty NodeFlagsNonEmpty HeadType '&' TT_Identifier TypeParams '(' FormalParamList ')' {
							$3->AddFlags(NF_Ref);
							$$ = new SnFunction($1, $2, $3, $5, $8, @3);
							$$->SetTypeParamNames(std::move(*$6));
							delete $6;
						} |
						AccessTypeEmpty NodeFlagsNonEmpty KT_Void TT_Identifier TypeParams '(' FormalParamList ')' {
							$$ = new SnFunction($1, $2 | NF_ExplicitVoid, nullptr, $4, $7, @3);
							$$->SetTypeParamNames(std::move(*$5));
							delete $5;
						} |
						AccessTypeNonEmpty NodeFlagsNonEmpty HeadType TT_Identifier TypeParams '(' FormalParamList ')' {
							$$ = new SnFunction($1, $2, $3, $4, $7, @3);
							$$->SetTypeParamNames(std::move(*$5));
							delete $5;
						} |
						AccessTypeNonEmpty NodeFlagsNonEmpty HeadType '&' TT_Identifier TypeParams '(' FormalParamList ')' {
							$3->AddFlags(NF_Ref);
							$$ = new SnFunction($1, $2, $3, $5, $8, @3);
							$$->SetTypeParamNames(std::move(*$6));
							delete $6;
						} |
						AccessTypeNonEmpty NodeFlagsNonEmpty KT_Void TT_Identifier TypeParams '(' FormalParamList ')' {
							$$ = new SnFunction($1, $2 | NF_ExplicitVoid, nullptr, $4, $7, @3);
							$$->SetTypeParamNames(std::move(*$5));
							delete $5;
						} ;

TopModHeader:	AccessTypeNonEmpty HeadType TT_Identifier '(' FormalParamList ')' {
						$$ = new SnFunction($1, NF_NONE, $2, $3, $5, @2);
					} |
					AccessTypeNonEmpty HeadType '&' TT_Identifier '(' FormalParamList ')' {
						$2->AddFlags(NF_Ref);
						$$ = new SnFunction($1, NF_NONE, $2, $4, $6, @2);
					} |
					AccessTypeNonEmpty KT_Void TT_Identifier '(' FormalParamList ')' {
						$$ = new SnFunction($1, NF_NONE | NF_ExplicitVoid, nullptr, $3, $5, @2);
					} |
					AccessTypeEmpty NodeFlagsNonEmpty HeadType TT_Identifier '(' FormalParamList ')' {
						$$ = new SnFunction($1, $2, $3, $4, $6, @3);
					} |
					AccessTypeEmpty NodeFlagsNonEmpty HeadType '&' TT_Identifier '(' FormalParamList ')' {
						$3->AddFlags(NF_Ref);
						$$ = new SnFunction($1, $2, $3, $5, $7, @3);
					} |
					AccessTypeEmpty NodeFlagsNonEmpty KT_Void TT_Identifier '(' FormalParamList ')' {
						$$ = new SnFunction($1, $2 | NF_ExplicitVoid, nullptr, $4, $6, @3);
					} |
					AccessTypeNonEmpty NodeFlagsNonEmpty HeadType TT_Identifier '(' FormalParamList ')' {
						$$ = new SnFunction($1, $2, $3, $4, $6, @3);
					} |
					AccessTypeNonEmpty NodeFlagsNonEmpty HeadType '&' TT_Identifier '(' FormalParamList ')' {
						$3->AddFlags(NF_Ref);
						$$ = new SnFunction($1, $2, $3, $5, $7, @3);
					} |
					AccessTypeNonEmpty NodeFlagsNonEmpty KT_Void TT_Identifier '(' FormalParamList ')' {
						$$ = new SnFunction($1, $2 | NF_ExplicitVoid, nullptr, $4, $6, @3);
					} ;

//v0.9.1: epsilon-only access slot for the (empty,F) header arm. Keeps flag
//keywords out of the classic AccessType follow set (they would collide
//with AccessTypeNonEmpty as a same-string double derivation).
AccessTypeEmpty:	{ $$ = FA_Default; } ;

AccessTypeNonEmpty:	KT_Private { $$ = FA_Private; } |
					KT_Protected { $$ = FA_Protected; } |
					KT_Public { $$ = FA_Public; } ;

NodeFlagsNonEmpty:	NodeFlagsNonEmpty NodeFlag {
						const NodeBits toAdd = $2;
						if (($1 & toAdd) != 0)
						{
							parser.Log(CLL_Error, @2, "Duplicate modifier \"%s\".",
								NodeFlagInfo::NameOf(toAdd));
							$$ = $1;
						}
						else
							$$ = $1 | toAdd;
					} |
					NodeFlag {
						$$ = $1;
					} ;

//File-top member table; the container is the file's own package (the
//path-derived unit — Phase 5 removed the `namespace` shell syntax).
NamespaceMember:	EnumDecl {
							$$ = $1;
						} |
						StructDecl {
							$$ = $1;
						} |
						ClassDecl {
							$$ = $1;
						} |
						InterfaceDecl {
							$$ = $1;
						} ;

FormalParamList:	FormalParamList ',' FormalParam {
							if ($1->empty())
							{
								parser.Log(CLL_Error, @1, "Unexpected ',' in a param list.");
								delete $3;
							}
							else
								$1->push_back($3);
							$$ = $1;
						} |
						FormalParam {
							$$ = new PtrList<SnFormalParam>();
							$$->push_back($1);
						} |
						{
							/*on empty */
							$$ = new PtrList<SnFormalParam>();
							//v0.9.1: this epsilon and ConcreteParamList's epsilon meet in
							//the shared paren state (2 r/r; a third such state exists
						//since v0.13.0 behind the top-level const-function
						//diagnostic arms on TopItem). Rule order picks the winner:
							//FormalParamList stays earlier in this file, so the function
							//header wins and zero-parameter definitions parse (D6).
						} ;

FormalParam:	NodeFlagsNonEmpty TypeRef TT_Identifier '=' Expression {
						$$ = new SnFormalParam($1, $2, $3, $5, @1);
					} |
					NodeFlagsNonEmpty TypeRef TT_Identifier {
						$$ = new SnFormalParam($1, $2, $3, nullptr, @1);
					} |
					/*v0.9.1: bare arms. Type must open the production so a
					 * dot-before-Type item lives in the shared paren state;
					 * without it the epsilon suppression kills "int f(int x)".
					 * v0.13.9 phase 3: TypeRef — the arms also accept the
					 * reference '&' mark (`int& v`); the out arms below keep
					 * plain Type, so `out int& v` stays a syntax error.*/
					TypeRef TT_Identifier '=' Expression {
						$$ = new SnFormalParam(NF_NONE, $1, $2, $4, @1);
					} |
					TypeRef TT_Identifier {
						$$ = new SnFormalParam(NF_NONE, $1, $2, nullptr, @1);
					} |
					/* Phase 9e: out parameter. v0.13.9 phase 4: diagnostic
					 * anchor only — ResolveFunctionFormals reports the
					 * retirement for every NF_Out formal. No default-value
					 * form exists, so `out T x = d` stays a syntax error. */
					NodeFlagsNonEmpty KT_Out Type TT_Identifier {
						$$ = new SnFormalParam($1, $3, $4, nullptr, @2);
						$$->AddFlags(NF_Out);
					} |
					KT_Out Type TT_Identifier {
						$$ = new SnFormalParam(NF_NONE, $2, $3, nullptr, @1);
						$$->AddFlags(NF_Out);
					} ;

Paragraph:	'{' StatementList '}' {
					$$ = new SnParagraph($2, @1);
				} ;

StatementList:	StatementList Statement {
						if ($2 != nullptr)
							$1->push_back($2);
						$$ = $1;
					} |
					{
						//on empty
						$$ = new PtrList<SnStatement>();
					} ;

Statement:	';' {
					parser.Log(CLL_Warn, @1, "Empty statement will be ignored.");
					$$ = nullptr;
				} |
				ReturnStmt {
					$$ = $1;
				} |
				InvokeStmt {
					$$ = $1;
				} |
				LocalDeclStmt {
					$$ = $1;
				} |
				DeclLocalConst {
					$$ = $1;
				} |
				AssignStmt {
					$$ = $1;
				} |
				CompoundAssignStmt {
					$$ = $1;
				} |
				AssertStmt {
					$$ = $1;
				} |
				SubscriptAssignStmt {
					$$ = $1;
				} |
				IfStmt {
					$$ = $1;
				} |
				WhileStmt {
					$$ = $1;
				} |
				DoStmt {
					$$ = $1;
				} |
				ForStmt {
					$$ = $1;
				} |
				ForeachStmt {
					$$ = $1;
				} |
				SwitchStmt {
					$$ = $1;
				} |
				BreakStmt {
					$$ = $1;
				} |
				ContinueStmt {
					$$ = $1;
				} |
				TryStmt {
					$$ = $1;
				} |
				ThrowStmt {
					$$ = $1;
				} |
				SuperCallStmt {
					$$ = $1;
				} |
				Paragraph {
					$$ = $1;
				} |
				error ';'
				{
					parser.Log(CLL_Error, @1, "Invalid statement.");
					parser.Scanner().ClearGenericDepth();
					$$ = nullptr;
				} ;

ReturnStmt: KT_Return Expression ';' {
					$$ = new SnReturnStmt($2, @1);
				} |
				//0.9.0: bare brace return — the enclosing function's return
				//type is the init list's target (resolver-side inference).
				KT_Return BracedInitExpr ';' {
					$$ = new SnReturnStmt($2, @1);
				} |
				//Void support: bare `return;` for early exit from void functions.
				KT_Return ';' {
					$$ = new SnReturnStmt(nullptr, @1);
				} ;

InvokeStmt: InvokeExpr ';' { $$ = new SnInvokeStmt($1, @1); } |
				MemberExpr ';' { $$ = new SnInvokeStmt($1, @1); } ;

/*
Local variable declaration statement.
Phase 9a: `const Type decls;` form marks all locals as const (NF_Const).
*/
LocalDeclStmt: HeadType LocalDeclList ';' {
						$$ = new SnLocalDeclStmt($1, $2, @1);
					} |
					//v0.13.9 phase 3: reference local declarations. The
					//'&' is an INLINE arm after the head — no wrapper
					//nonterminal — so the post-type state holds only
					//shift actions and never conflicts with the plain
					//declarator or the expression readings. The mark
					//rides the type node; identifier-led heads reach the
					//same shift through the HeadType completion (the
					//accepted reduce/reduce at the statement-head
					//identifier state resolves to it — see the conflict
					//ledger in this file's header).
					HeadType '&' LocalDeclList ';' {
						$1->AddFlags(NF_Ref);
						$$ = new SnLocalDeclStmt($1, $3, @1);
					} |
					KT_Const HeadType LocalDeclList ';' {
						$$ = new SnLocalDeclStmt($2, $3, true, @1);
					} |
					//v0.12.6: static locals and local static constants.
					//Fixed order `static const`; `const static` stays a
					//syntax error. The resolver (StatementResolverDecls)
					//validates placement; slot-backed storage arrives with
					//the static-local tasks.
					KT_Static HeadType LocalDeclList ';' {
						$$ = new SnLocalDeclStmt($2, $3, NF_Static, @1);
					} |
					KT_Static KT_Const HeadType LocalDeclList ';' {
						$$ = new SnLocalDeclStmt($3, $4, NF_Static | NF_Const, @1);
					} ;

//v0.13.9 phase 3 Task 9: the const read-only reference form. The '&'
//rides the type node exactly like the plain reference arm above, and
//the const bit rides the statement — the resolver's write gates key on
//the descriptor's NF_Const, while the binding judgment relaxes its
//const-storage arm for a const binding (RefBindJudgment).
DeclLocalConst:	KT_Const HeadType '&' LocalDeclList ';' {
				$2->AddFlags(NF_Ref);
				$$ = new SnLocalDeclStmt($2, $4, true, @1);
			} ;

LocalDeclList: LocalDeclList ',' TT_Identifier {
						$1->push_back({*$3, nullptr});
						$$ = $1;
					} |
					LocalDeclList ',' TT_Identifier '=' Expression {
						$1->push_back({*$3, $5});
						$$ = $1;
					} |
					//0.10.7 D1: brace-init continuation `T a, b = {…};` —
					//the declared type becomes the init list's target
					//(resolver-side inference, same as the first arm).
					LocalDeclList ',' TT_Identifier '=' BracedInitExpr {
						$1->push_back({*$3, $5});
						$$ = $1;
					} |
					//0.10.7 D1: direct-init continuation `T a, b(args);` —
					//the ctor-arg payload rides the same slot as the first
					//arm; the resolver dispatches per declarator.
					LocalDeclList ',' TT_Identifier '(' ConcreteParamList ')' {
						$1->push_back({*$3, nullptr, $5, nullptr});
						$$ = $1;
					} |
					//0.10.7 D1: zero-argument direct-init continuation
					//`T a, b();` — explicit empty parens (0.9.15 reading).
					LocalDeclList ',' TT_Identifier '(' ')' {
						$1->push_back({*$3, nullptr, new PtrList<SnExpression>(), nullptr});
						$$ = $1;
					} |
					//0.10.7 D1: sized continuation `T a, b[n];` (and the
					//partial-init form). 0.10.7 D2: the size types only
					//its own declarator — siblings keep the head type.
					LocalDeclList ',' TT_Identifier '[' Expression ']' {
						$1->push_back({*$3, nullptr, nullptr, $5});
						$$ = $1;
					} |
					LocalDeclList ',' TT_Identifier '[' Expression ']' '=' Expression {
						$1->push_back({*$3, $8, nullptr, $5});
						$$ = $1;
					} |
					//0.10.7 D3: brace form on a sized continuation
					//declarator — parses so the resolver can name the
					//form violation.
					LocalDeclList ',' TT_Identifier '[' Expression ']' '=' BracedInitExpr {
						$1->push_back({*$3, $8, nullptr, $5});
						$$ = $1;
					} |
					//0.11.3: comma sizes on a continuation declarator
					//get the same named reject as the first one.
					LocalDeclList ',' TT_Identifier '[' Expression
					',' ConcreteParamList ']' {
						parser.Log(CLL_Error, @6,
							"array declarations take one size per "
							"bracket; multi-dimensional arrays use the "
							"creation expression int[2, 3]");
						for (auto* pExpr : *$7)
							delete pExpr;
						delete $7;
						$1->push_back({*$3, nullptr, nullptr, $5});
						$$ = $1;
					} |
					LocalDeclList ',' TT_Identifier '[' Expression
					',' ConcreteParamList ']' '=' Expression {
						parser.Log(CLL_Error, @6,
							"array declarations take one size per "
							"bracket; multi-dimensional arrays use the "
							"creation expression int[2, 3]");
						for (auto* pExpr : *$7)
							delete pExpr;
						delete $7;
						$1->push_back({*$3, $10, nullptr, $5});
						$$ = $1;
					} |
					LocalDeclList ',' TT_Identifier '[' Expression
					',' ConcreteParamList ']' '=' BracedInitExpr {
						parser.Log(CLL_Error, @6,
							"array declarations take one size per "
							"bracket; multi-dimensional arrays use the "
							"creation expression int[2, 3]");
						for (auto* pExpr : *$7)
							delete pExpr;
						delete $7;
						$1->push_back({*$3, $10, nullptr, $5});
						$$ = $1;
					} |
					TT_Identifier '=' Expression {
						$$ = new std::vector<SnLocalDeclStmt::LocalDecl>();
						$$->push_back({*$1, $3});
					} |
					//0.9.0: bare brace declaration initializer — decomposed
					//into the same AssignStmt as the Expression arm, and the
					//declared type becomes the init list's target.
					TT_Identifier '=' BracedInitExpr {
						$$ = new std::vector<SnLocalDeclStmt::LocalDecl>();
						$$->push_back({*$1, $3});
					} |
					//0.9.0: direct-init declaration `T name(args);` — the
					//resolver decomposes it into the same tree as
					//`T name = new T(args);`. Single declarator only: the
					//ctor-arg payload attaches to this arm alone.
					TT_Identifier '(' ConcreteParamList ')' {
						$$ = new std::vector<SnLocalDeclStmt::LocalDecl>();
						$$->push_back({*$1, nullptr, $3, nullptr});
					} |
					//0.9.15: zero-argument direct-init `T name();` — the
					//explicit empty-parens arms make ')' a shift instead of
					//the double-epsilon reduce that forced the function
					//header reading; the token after ')' decides.
					TT_Identifier '(' ')' {
						$$ = new std::vector<SnLocalDeclStmt::LocalDecl>();
						$$->push_back({*$1, nullptr, new PtrList<SnExpression>(), nullptr});
					} |
					//0.9.0: C-style sized array declaration `T name[n];`
					//(and the partial-init form `T name[n] = [e1, e2];`).
					//The size rides the pArraySize payload slot; the resolver
					//synthesizes `name = new T[n]` plus one subscript assign
					//per initializer entry. 0.10.7 D2: a size payload types
					//only its own declarator; siblings keep the head type.
					TT_Identifier '[' Expression ']' {
						$$ = new std::vector<SnLocalDeclStmt::LocalDecl>();
						$$->push_back({*$1, nullptr, nullptr, $3});
					} |
					TT_Identifier '[' Expression ']' '=' Expression {
						$$ = new std::vector<SnLocalDeclStmt::LocalDecl>();
						$$->push_back({*$1, $6, nullptr, $3});
					} |
					//0.10.7 D3: the brace form parses so the resolver can
					//name the form violation (array initializers keep the
					//bracket form — 0.9.11 D6 single-shape design).
					TT_Identifier '[' Expression ']' '=' BracedInitExpr {
						$$ = new std::vector<SnLocalDeclStmt::LocalDecl>();
						$$->push_back({*$1, $6, nullptr, $3});
					} |
					//0.11.3: comma sizes parse so the parser can name the
					//C-style form and point at the creation twin. The first
					//size rides the array-size payload (0.10.7 D3 precedent)
					//so the subtree joins the owning AST; the ConcreteParamList
					//is released here — deleting each element frees its whole
					//subtree (ImmutableNodeList ownership), so the reject
					//leaks nothing even in a long-lived host (nide).
					TT_Identifier '[' Expression ',' ConcreteParamList ']' {
						parser.Log(CLL_Error, @4,
							"array declarations take one size per "
							"bracket; multi-dimensional arrays use the "
							"creation expression int[2, 3]");
						for (auto* pExpr : *$5)
							delete pExpr;
						delete $5;
						$$ = new std::vector<SnLocalDeclStmt::LocalDecl>();
						$$->push_back({*$1, nullptr, nullptr, $3});
					} |
					TT_Identifier '[' Expression ',' ConcreteParamList ']'
					'=' Expression {
						parser.Log(CLL_Error, @4,
							"array declarations take one size per "
							"bracket; multi-dimensional arrays use the "
							"creation expression int[2, 3]");
						for (auto* pExpr : *$5)
							delete pExpr;
						delete $5;
						$$ = new std::vector<SnLocalDeclStmt::LocalDecl>();
						$$->push_back({*$1, $8, nullptr, $3});
					} |
					TT_Identifier '[' Expression ',' ConcreteParamList ']'
					'=' BracedInitExpr {
						parser.Log(CLL_Error, @4,
							"array declarations take one size per "
							"bracket; multi-dimensional arrays use the "
							"creation expression int[2, 3]");
						for (auto* pExpr : *$5)
							delete pExpr;
						delete $5;
						$$ = new std::vector<SnLocalDeclStmt::LocalDecl>();
						$$->push_back({*$1, $8, nullptr, $3});
					} |
					TT_Identifier {
						$$ = new std::vector<SnLocalDeclStmt::LocalDecl>();
						$$->push_back({*$1, nullptr});
					} ;

/*
Assignment statement.
*/
AssignStmt: IdentifierExpr '=' Expression ';' {
					$$ = new SnAssignStmt($1, $3, @1);
				} |
				//0.9.0: bare brace assignment RHS — the LHS's type is the
				//init list's target (resolver-side inference).
				IdentifierExpr '=' BracedInitExpr ';' {
					$$ = new SnAssignStmt($1, $3, @1);
				} |
				MemberExpr '=' Expression ';' {
					$$ = new SnAssignStmt($1, $3, @1);
				} |
				MemberExpr '=' BracedInitExpr ';' {
					$$ = new SnAssignStmt($1, $3, @1);
				} ;

/*
Compound assignment statement (e.g. x += 1).
Phase 9a: left-value is evaluated only once.
0.8.4: bare-subscript lvalues (arr[i] += v) — SubscriptExpr left,
base and index each evaluated exactly once (read-modify-write).
*/
CompoundAssignStmt: IdentifierExpr OT_INCS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Add, $1, $3, @1);
				} |
				IdentifierExpr OT_DECS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Sub, $1, $3, @1);
				} |
				IdentifierExpr OT_MULS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Mul, $1, $3, @1);
				} |
				IdentifierExpr OT_DIVS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Div, $1, $3, @1);
				} |
				IdentifierExpr OT_MODS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Mod, $1, $3, @1);
				} |
				//0.9.10: shift compound assignment (<<=, >>=). The operator
				//rides the same SnCompoundAssignStmt node; resolver gates and
				//emission join the arithmetic family in later stages.
				IdentifierExpr OT_LSHS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Shl, $1, $3, @1);
				} |
				IdentifierExpr OT_RSHS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Shr, $1, $3, @1);
				} |
				//v0.13.7: bitwise compound forms (&=, |=, ^=). The operator
				//rides the same SnCompoundAssignStmt node; resolver gates and
				//emission join the compound family in the later stages.
				IdentifierExpr OT_BANDS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_BitAnd, $1, $3, @1);
				} |
				IdentifierExpr OT_BORS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_BitOr, $1, $3, @1);
				} |
				IdentifierExpr OT_BXORS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_BitXor, $1, $3, @1);
				} |
				MemberExpr OT_INCS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Add, $1, $3, @1);
				} |
				MemberExpr OT_DECS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Sub, $1, $3, @1);
				} |
				MemberExpr OT_MULS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Mul, $1, $3, @1);
				} |
				MemberExpr OT_DIVS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Div, $1, $3, @1);
				} |
				MemberExpr OT_MODS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Mod, $1, $3, @1);
				} |
				MemberExpr OT_LSHS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Shl, $1, $3, @1);
				} |
				MemberExpr OT_RSHS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Shr, $1, $3, @1);
				} |
				MemberExpr OT_BANDS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_BitAnd, $1, $3, @1);
				} |
				MemberExpr OT_BORS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_BitOr, $1, $3, @1);
				} |
				MemberExpr OT_BXORS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_BitXor, $1, $3, @1);
				} |
				SubscriptExpr OT_INCS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Add, $1, $3, @1);
				} |
				SubscriptExpr OT_DECS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Sub, $1, $3, @1);
				} |
				SubscriptExpr OT_MULS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Mul, $1, $3, @1);
				} |
				SubscriptExpr OT_DIVS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Div, $1, $3, @1);
				} |
				SubscriptExpr OT_MODS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Mod, $1, $3, @1);
				} |
				SubscriptExpr OT_LSHS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Shl, $1, $3, @1);
				} |
				SubscriptExpr OT_RSHS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Shr, $1, $3, @1);
				} |
				SubscriptExpr OT_BANDS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_BitAnd, $1, $3, @1);
				} |
				SubscriptExpr OT_BORS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_BitOr, $1, $3, @1);
				} |
				SubscriptExpr OT_BXORS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_BitXor, $1, $3, @1);
				} ;

/*
Assert statement: assert(cond); - exit(1) on failure.
Phase 9a: simple runtime check.
*/
AssertStmt: KT_Assert '(' Expression ')' ';' {
					$$ = new SnAssertStmt($3, @1);
				} ;

/*
Phase 9d: try/catch statement. Phase 9d-2 adds the optional finally
clause (full Java semantics: finally runs on normal, catch, break/
continue/return, and exception paths).
The try body is a single Statement (typically a Paragraph block).
CatchClauseList allows zero or more catch clauses (zero is meaningful
only together with finally — `try {} finally {}`; a resolver-stage check
rejects try with neither catches nor finally).
*/
TryStmt: KT_Try Statement CatchClauseList FinallyClauseOpt {
					$$ = new SnTryStmt($2, $3, $4, @1);
				} ;

CatchClauseList: /* empty */ {
					$$ = new std::vector<SnCatchClause*>();
				} | CatchClauseList CatchClause {
					$$ = $1;
					if ($2) $$->push_back($2);
				} ;

CatchClause: KT_Catch '(' Type TT_Identifier ')' Statement {
					$$ = new SnCatchClause($3, *$4, $6, @1);
				} ;

FinallyClauseOpt: /* empty */ {
					$$ = nullptr;
				} | KT_Finally Statement {
					$$ = $2;
				} ;

/*
Phase 9d: throw statement. `throw <expr>;` raises the given Exception
instance; `throw;` (no operand) re-raises the in-flight exception of the
enclosing catch handler.
*/
ThrowStmt: KT_Throw Expression ';' {
					$$ = new SnThrowStmt($2, @1);
				} | KT_Throw ';' {
					$$ = new SnThrowStmt(nullptr, @1);
				} ;

/*
Phase 9d-2: super(...) call statement — forwards constructor arguments
to the direct parent class constructor. Legal anywhere inside a user
constructor (not required to be the first statement). Reuses
ConcreteParamList (positional args; named args are rejected by the
resolver).
*/
SuperCallStmt: KT_Super '(' ConcreteParamList ')' ';' {
					$$ = new SnSuperCallStmt(*$3, @1);
				} ;

/*
Array subscript assignment statement (e.g. arr[i] = value).
v0.12.7 Task 11: rides the unified SubscriptItemList, so slice lvalues
(m[1:3, :] = v) parse here and are rejected by the resolver with the
slice-assignment diagnostic (stores through a slice are not supported
yet — a writable view needs materialization semantics first).
*/
SubscriptAssignStmt: Expression '[' SubscriptItemList ']' '=' Expression ';' {
					$$ = new SnSubscriptAssignStmt($1, $3, $6, @1);
				} |
				//0.9.0: bare brace stored value — the base's element type is
				//the init list's target (resolver-side inference).
				Expression '[' SubscriptItemList ']' '=' BracedInitExpr ';' {
					$$ = new SnSubscriptAssignStmt($1, $3, $6, @1);
				} ;

/*
If/else statement.
*/
IfStmt:	KT_If '(' Expression ')' Statement %prec P_Then {
				$$ = new SnIfStmt($3, $5, nullptr, @1);
			} |
			KT_If '(' Expression ')' Statement KT_Else Statement {
				$$ = new SnIfStmt($3, $5, $7, @1);
			} ;


/*
While loop statement.
*/
WhileStmt:	KT_While '(' Expression ')' Statement {
					$$ = new SnWhileStmt($3, WrapLoopBody($5, @5), @1);
				} ;

/*
Do-while loop statement.
*/
DoStmt:	KT_Do Statement KT_While '(' Expression ')' ';' {
				$$ = new SnDoStmt($5, WrapLoopBody($2, @2), @1);
			} ;

/*
For loop statement.
*/
ForStmt:	KT_For '(' InitFor ';' Expression ';' FiniFor ')' Statement {
					$$ = new SnForStmt($3, $5, $7, WrapLoopBody($9, @9), @1);
				} ;

InitFor:	{
					/* on empty */
					$$ = nullptr;
				} |
				Type LocalDeclList {
					$$ = new SnLocalDeclStmt($1, $2, @1);
				} |
				InvokeExpr {
					$$ = new SnInvokeStmt($1, @1);
				} |
				IdentifierExpr '=' Expression {
					$$ = new SnAssignStmt($1, $3, @1);
				} ;

FiniFor:	{
					/* on empty */
					$$ = nullptr;
				} |
				InvokeExpr {
					$$ = new SnInvokeStmt($1, @1);
				} |
				IdentifierExpr '=' Expression {
					$$ = new SnAssignStmt($1, $3, @1);
				} |
				IdentifierExpr OT_INCS Expression {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Add, $1, $3, @1);
				} |
				IdentifierExpr OT_DECS Expression {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Sub, $1, $3, @1);
				} |
				IdentifierExpr OT_MULS Expression {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Mul, $1, $3, @1);
				} |
				IdentifierExpr OT_DIVS Expression {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Div, $1, $3, @1);
				} |
				IdentifierExpr OT_MODS Expression {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Mod, $1, $3, @1);
				} |
				//0.9.10: shift compound forms in the for-update slot.
				IdentifierExpr OT_LSHS Expression {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Shl, $1, $3, @1);
				} |
				IdentifierExpr OT_RSHS Expression {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Shr, $1, $3, @1);
				} |
				IdentifierExpr OT_BANDS Expression {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_BitAnd, $1, $3, @1);
				} |
				IdentifierExpr OT_BORS Expression {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_BitOr, $1, $3, @1);
				} |
				IdentifierExpr OT_BXORS Expression {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_BitXor, $1, $3, @1);
				} ;

/*
Foreach loop statement (Phase 8e-5).
Iterates Array / List<T> / Dict<K,V> (keys).
*/
ForeachStmt:	KT_Foreach '(' TypeRef TT_Identifier KT_In Expression ')' Statement {
					$$ = new SnForeachStmt($3, *$4, $6, WrapLoopBody($8, @8), @1);
				} |
				//0.9.13 B1: bare brace source for foreach.
				//v0.13.9 phase 3: TypeRef — `foreach (int& v in xs)` marks
				//the element binding as a reference.
				KT_Foreach '(' TypeRef TT_Identifier KT_In BracedInitExpr ')' Statement {
					$$ = new SnForeachStmt($3, *$4, $6, WrapLoopBody($8, @8), @1);
				} ;

/*
Break statement.
*/
BreakStmt:	KT_Break ';' {
					$$ = new SnBreakStmt(@1);
				} ;

/*
Continue statement.
*/
ContinueStmt:	KT_Continue ';' {
					$$ = new SnContinueStmt(@1);
				} ;

/*
Switch statement.
*/
SwitchStmt:	KT_Switch '(' Expression ')' '{' CaseClauseList DefaultCase '}' {
				$$ = new SnSwitchStmt($3, $6, $7, @1);
			} ;

CaseClauseList:	CaseClauseList CaseClause {
					$1->push_back($2);
					$$ = $1;
				} |
				{
					/*on empty */
					$$ = new std::vector<SnCaseClause*>();
				} ;

//Phase 12: comma-separated case labels — `case 1, 2:` enters the clause
//body when ANY label matches. PtrList<SnExpression> follows ConcreteParamList.
CaseLabelList:	CaseLabelList ',' Expression {
					$1->push_back($3);
					$$ = $1;
			} |
				Expression {
					$$ = new PtrList<SnExpression>();
					$$->push_back($1);
			} ;

CaseClause:	KT_Case CaseLabelList ':' StatementList {
				$$ = new SnCaseClause($2, $4, @1);
			} ;

DefaultCase:	{
					/*on empty */
					$$ = nullptr;
				} |
				KT_Default ':' StatementList {
					$$ = new SnParagraph($3, @1);
				} ;
/*
Enum type declaration. The access-prefix layer (0.13.8 item 5, D5.4)
wraps every top-level type-declaration family: only AccessTypeNonEmpty
leads — it never derives empty, so the bare core stays the no-prefix
reading and no epsilon reduction enters the type-decl follow sets. The
setter runs in the wrapper action because the decl node does not exist
until its core reduces. The const-aggregate spelling stays inside the
struct/class cores, so the single accepted sr this layer adds is the
KT_Const shift in the access-keyword state (the ledger re-measure has
the row).
*/
EnumDecl:	AccessTypeNonEmpty EnumDeclCore {
					$2->AccessType($1);
					$$ = $2;
				} |
			EnumDeclCore ;

EnumDeclCore:	KT_Enum TT_Identifier '{' EnumMemberList EnumMethodSection '}' {
					$$ = new SnEnumDecl($2, $4, $5, @1);
				} ;

EnumMemberList:	EnumMemberList ',' EnumMember {
					$1->push_back($3);
					$$ = $1;
				} |
				EnumMember {
					$$ = new PtrList<SnEnumMember>();
					$$->push_back($1);
				} ;

EnumMember:	TT_Identifier {
				$$ = new SnEnumMember($1, nullptr, @1);
				} |
				TT_Identifier '=' Expression {
					$$ = new SnEnumMember($1, $3, @1);
				} ;

/*
Optional method table, separated from the member list by a ';' tail.
The lone ';' production accepts the Java-style "enum E { A; }" closing
separator with no methods following.
*/
EnumMethodSection:	';' EnumMethodList {
				$$ = $2;
			} |
				';' {
				$$ = new PtrList<SnFunction>();
			} |
				/* empty */ {
				$$ = new PtrList<SnFunction>();
			} ;

EnumMethodList:	EnumMethodList EnumMethod {
				$1->push_back($2);
				$$ = $1;
			} |
				EnumMethod {
				$$ = new PtrList<SnFunction>();
				$$->push_back($1);
			} ;

/*
Enum methods take no NodeFlag modifiers (no static/override/native),
unlike ClassMember. Bodyless declarations still parse (FunctionBodyOrSemi)
so the resolver can reject them with a proper diagnostic.
*/
EnumMethod:	AccessType TypeRef TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
				auto* func = new SnFunction($1, NF_NONE, $2, $3, $5, @2);
				if ($7 == nullptr)
					func->AddFlags(NF_Abstract);
				else
					func->Body($7);
				$$ = func;
			} |
				AccessType KT_Void TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
				auto* func = new SnFunction($1, NF_NONE | NF_ExplicitVoid, nullptr, $3, $5, @2);
				if ($7 == nullptr)
					func->AddFlags(NF_Abstract);
				else
					func->Body($7);
				$$ = func;
			} ;

/*
Struct type declaration. Access-prefix wrapper per the EnumDecl comment.
*/
StructDecl:	AccessTypeNonEmpty StructDeclCore {
					$2->AccessType($1);
					$$ = $2;
				} |
			StructDeclCore ;

StructDeclCore:	KT_Struct TT_Identifier '{' StructMemberList '}' {
					$$ = new SnStructDecl($2, $4, @1);
				} |
			//v0.12.4 D1: const-aggregate prefix (const struct) — a
			//declaration-level marker bit, no member-level semantics.
			KT_Const KT_Struct TT_Identifier '{' StructMemberList '}' {
					auto* pStruct = new SnStructDecl($3, $5, @2);
					pStruct->SetConstAggregate();
					$$ = pStruct;
				} ;

StructMemberList:	StructMemberList StructMember {
					$1->push_back($2);
					$$ = $1;
				} |
					StructMember {
					$$ = new PtrList<SnField>();
					$$->push_back($1);
				} ;

StructMember:	StructField {
					$$ = $1;
				} |
					StructCtor {
					$$ = $1;
				} ;

StructField:	Type TT_Identifier ';' {
					$$ = new SnStructField($1, $2, FA_Default, @1);
				} |

				//0.9.11 D4: a struct field initializer is a compile-time
				//default applied at every allocation. The grammar admits the
				//shape; the constant-surface check (FieldInitializerChecks.hpp)
				//runs in the Access(SnStructDecl&) field loop. LALR: the state
				//reached after `Type TT_Identifier` inside a struct body has a
				//core of StructField items only, distinct from the local-
				//declaration state (whose Declarator items attach '=' to the
				//declarator), so the states never merge and the '=' shift is
				//unconflicted.
				Type TT_Identifier '=' Expression ';' {
					$$ = new SnStructField($1, $2, FA_Default, @1, $4);
				} |

				//0.9.12 D7 (D-new-2): struct members are public-only, but the
				//modifier spellings must reach the declaration contract (the
				//precise "must be public" error) instead of a bare syntax
				//error. The AccessTypeNonEmpty split mirrors StructCtor: the
				//epsilon AccessType layer cannot prefix these arms because its
				//empty reduction would conflict with the Type->TT_Identifier
				//shift at the member position. After the keyword the state
				//keeps the field arms and the StructCtor arms open until the
				//name/'(' lookahead decides, so no new conflict is introduced.
				AccessTypeNonEmpty Type TT_Identifier ';' {
					$$ = new SnStructField($2, $3, $1, @1);
				} |
				AccessTypeNonEmpty Type TT_Identifier '=' Expression ';' {
					$$ = new SnStructField($2, $3, $1, @1, $5);
				} ;

//0.9.11 D2: struct constructor spelling -- no return type, the same
//member shape as the class constructor arms; whether the member name
//matches the struct (constructor) or not is decided by the declaration
//contract. LALR: at the member position '(' after the leading identifier
//separates the constructor from the field's `Type name ';'` (after a
//reduced Type the parser needs a name token, not '('). The classic
//AccessType layer cannot prefix these arms the way it prefixes
//ClassMember (whose every arm shares the AccessType prefix): its epsilon
//would have to reduce on the TT_Identifier lookahead that the field's
//Type->TT_Identifier shift claims, and bison's shift default would hide
//default-access constructors. The access slot is therefore split the way
//TopModHeader splits it: AccessTypeNonEmpty never derives empty, the bare
//arms carry FA_Default in the action, and NodeFlagsNonEmpty (never
//empty) covers one-or-more modifiers.
StructCtor:	TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction(FA_Default, NF_NONE, nullptr, $1, $3, @1);
					if ($5 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($5);
					$$ = func;
				} |
					NodeFlagsNonEmpty TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction(FA_Default, $1, nullptr, $2, $4, @2);
					if ($6 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($6);
					$$ = func;
				} |
					AccessTypeNonEmpty TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, NF_NONE, nullptr, $2, $4, @2);
					if ($6 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($6);
					$$ = func;
				} |
					AccessTypeNonEmpty NodeFlagsNonEmpty TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, $2, nullptr, $3, $5, @3);
					if ($7 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($7);
					$$ = func;
				} ;


/*
Class type declaration. Access-prefix wrapper per the EnumDecl comment.
*/
ClassDecl:	AccessTypeNonEmpty ClassDeclCore {
					$2->AccessType($1);
					$$ = $2;
				} |
			ClassDeclCore ;

ClassDeclCore:	KT_Class TT_Identifier TypeParamsOpt ClassInheritOpt ImplementsOpt '{' ClassMemberList '}' {
					auto* pClass = new SnClassDecl($2, $4, $7, @1);
					if ($5)
						for (auto *pNode : *$5)
							pClass->AddImplementsName(pNode);
					if ($3)
						pClass->SetTypeParamNames(std::move(*$3));
					delete $3;
					$$ = pClass;
				} |
			//Empty body (marker classes): the separate alternative is the
			//InterfaceDecl shape — an empty ClassMemberList arm would
			//reintroduce the epsilon-list conflict family the accepted-
			//conflict ledger records as removed.
KT_Class	TT_Identifier TypeParamsOpt ClassInheritOpt ImplementsOpt '{' '}' {
					auto* pClass = new SnClassDecl($2, $4, new PtrList<SnField>(), @1);
					if ($5)
						for (auto *pNode : *$5)
							pClass->AddImplementsName(pNode);
					if ($3)
						pClass->SetTypeParamNames(std::move(*$3));
					delete $3;
					$$ = pClass;
				} |
			//v0.12.4 D1: const-aggregate prefix (const class) — same two
			//body shapes with a leading KT_Const; the marker bit lands on
			//the declaration node (inheritance propagates it at query time).
			KT_Const KT_Class TT_Identifier TypeParamsOpt ClassInheritOpt ImplementsOpt '{' ClassMemberList '}' {
					auto* pClass = new SnClassDecl($3, $5, $8, @2);
					if ($6)
						for (auto *pNode : *$6)
							pClass->AddImplementsName(pNode);
					if ($4)
						pClass->SetTypeParamNames(std::move(*$4));
					delete $4;
					pClass->SetConstAggregate();
					$$ = pClass;
				} |
			KT_Const KT_Class TT_Identifier TypeParamsOpt ClassInheritOpt ImplementsOpt '{' '}' {
					auto* pClass = new SnClassDecl($3, $5, new PtrList<SnField>(), @2);
					if ($6)
						for (auto *pNode : *$6)
							pClass->AddImplementsName(pNode);
					if ($4)
						pClass->SetTypeParamNames(std::move(*$4));
					delete $4;
					pClass->SetConstAggregate();
					$$ = pClass;
				} ;

//Type parameter names of a generic class declaration: `class Name<T, U>`
//— names only, no bounds. Declaration context only (right after
//`class <name>`), so the leading '<' has no competing production;
//measured conflict delta goes in the ledger.
TypeParamsOpt:	TypeParams { $$ = $1; } |
				{ $$ = nullptr; } ;

//Generic-declaration openers are always plain '<' (2026-10-28 merge):
//declared generics (Promise, user `class C<T>`) are not in the
//scanner's flag set — no scan-time set can know a module's class
//declarations — and the flagged built-ins are never class-declared, so
//no other spelling is reachable here. Use sites split the same way:
//flagged names open on OT_TypeLt, declared names on plain '<' (the
//Type/HeadType twin arms).
//
//The parameter list is the standard list idiom — a head parameter plus
//zero or more (',' parameter) rests, closed by one '>'. The previous
//shape (a self-recursive arm that re-opened with '<') made every
//declaration with two or more parameters a syntax error: after
//`class C<K` the parser needed '>' to reduce the base arm, so the ','
//had no action (0.11.0 shipped single-parameter-only; the spec's
//TypeParams EBNF always described the list).
TypeParams:	'<' TT_Identifier TypeParamRests '>' {
					$$ = $3;
					$$->insert($$->begin(), *$2);
					delete $2;
				} |
				OT_TypeLt TT_Identifier TypeParamRests '>' {
					/*0.13.1a: the probe-matched spelling. A generic
					 *FUNCTION declaration `f<T>(...)` scans as OT_TypeLt —
					 *the forward probe sees identifier '<' ids '>' '(' and
					 *cannot see the declaration/expression distinction —
					 *while a generic CLASS declaration `c<T>` arrives
					 *plain. The declaration-side list accepts both
					 *openers; both incremented the scan-time depth, so
					 *">>" splits identically inside. */
					$$ = $3;
					$$->insert($$->begin(), *$2);
					delete $2;
				} ;

TypeParamRests:	{
					$$ = new std::vector<std::string>;
				} |
				TypeParamRests ',' TT_Identifier {
					$$ = $1;
					$$->push_back(*$3);
					delete $3;
				} ;


ClassInheritOpt:	':' TypeName { $$ = $2; } |
					{ $$ = nullptr; } ;

//A type spelled at a position that has a unique leading token (':' after a
//class name, `as`, `implements`): the dotted chain may be reduced freely,
//so NameExpr and QualifiedType both derive here without fighting over '.'.
//Measured: +2 shift/reduce, both on '.', both benign — no production
//expects a '.' AFTER a type name, so bison's shift default is the only
//correct reading. A statement head cannot use this shape (see the
//HeadType comment above).
TypeName:	NameExpr { $$ = $1; } |
			QualifiedType { $$ = $1; } ;

/*
Optional "implements I1, I2" clause on a class. Empty when omitted.
The list carries SnFieldExpr* type names (dotted chains allowed for
imported interfaces) resolved to interface decls during semantic analysis.
*/
ImplementsOpt:	KT_Implements NameList {
					$$ = $2;
				} |
				{
					$$ = nullptr;
				} ;

/*
One implements-clause entry: a plain type name, or the generic
spelling `I<Args>` (0.13.1c). The generic arms live HERE, not on
TypeName: TypeName is shared with the `as`-expression position, where
`x as y < z` is a legal comparison today — the merged LALR state would
resolve the '<' shift-default for BOTH and silently break that
spelling. The implements position is unambiguous (a '<' after an
entry's head can only open type arguments), so the dual angle spellings
follow the Type rule exactly: OT_TypeLt for scan-flagged names, plain
'<' (with the scan-time depth increment) for user-declared generics.
Both create the same SnGenericTypeExpr the type path builds, so the
clause resolves through the one generic-use path and the class binds
the INSTANTIATED interface.
*/
ImplementsName:	TypeName { $$ = $1; } |
				IdentifierExpr OT_TypeLt TypeList '>' {
					$$ = new SnGenericTypeExpr(new SnNameExpr($1, @1),
						$3, @1);
				} |
				IdentifierExpr '<' {
						parser.Scanner().IncGenericDepth();
					} TypeList '>' {
						$$ = new SnGenericTypeExpr(new SnNameExpr($1, @1),
							$4, @1);
					} ;

/*
Comma-separated list of ImplementsName values (for the implements
clause). Returns a PtrList<SnFieldExpr> owning the type-name
expressions.
*/
NameList:	ImplementsName {
					$$ = new PtrList<SnFieldExpr>();
					$$->push_back($1);
				} |
				NameList ',' ImplementsName {
					$1->push_back($3);
					$$ = $1;
				} ;

/*
Interface type declaration. Members are method signatures; only a
static member may carry a body (resolver-enforced). An interface
establishes a contract that implementing classes must satisfy. The
optional type-parameter list (0.13.1c) mirrors the ClassDecl shape:
declaration context only, so the leading '<' has no competing
production. The access-prefix wrapper follows the EnumDecl comment.
*/
InterfaceDecl:	AccessTypeNonEmpty InterfaceDeclCore {
						$2->AccessType($1);
						$$ = $2;
					} |
				InterfaceDeclCore ;

InterfaceDeclCore:	KT_Interface TT_Identifier TypeParamsOpt '{' InterfaceMemberList '}' {
					auto* pIface = new SnInterfaceDecl($2, $5, @1);
					if ($3)
						pIface->SetTypeParamNames(std::move(*$3));
					delete $3;
					$$ = pIface;
				} |
				KT_Interface TT_Identifier TypeParamsOpt '{' '}' {
					auto* pIface = new SnInterfaceDecl($2,
						new PtrList<SnField>(), @1);
					if ($3)
						pIface->SetTypeParamNames(std::move(*$3));
					delete $3;
					$$ = pIface;
				} ;

InterfaceMemberList:	InterfaceMemberList InterfaceMember {
					$1->push_back($2);
					$$ = $1;
				} |
				InterfaceMember {
					$$ = new PtrList<SnField>();
					$$->push_back($1);
				} ;

//Interface members share ClassMember's FunctionBodyOrSemi shape: the
//bodyless form still sets NF_Abstract, and a parsed body is kept for
//the resolver to admit (static) or reject (instance) per the 0.9.10
//declaration contract.
InterfaceMember:	AccessType NodeFlagsNonEmpty Type TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, $2, $3, $4, $6, @2);
					if ($8 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($8);
					$$ = func;
				} |
				//v0.13.9 phase 3 (v3 grammar): inline '&' twins — same
				//shape-for-shape mirror as ClassMember's.
				AccessType NodeFlagsNonEmpty Type '&' TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					$3->AddFlags(NF_Ref);
					auto* func = new SnFunction($1, $2, $3, $5, $7, @2);
					if ($9 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($9);
					$$ = func;
				} |
				AccessType Type TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, NF_NONE, $2, $3, $5, @2);
					if ($7 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($7);
					$$ = func;
				} |
				AccessType Type '&' TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					$2->AddFlags(NF_Ref);
					auto* func = new SnFunction($1, NF_NONE, $2, $4, $6, @2);
					if ($8 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($8);
					$$ = func;
				}
				| AccessType NodeFlagsNonEmpty KT_Void TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, $2 | NF_ExplicitVoid, nullptr, $4, $6, @2);
					if ($8 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($8);
					$$ = func;
				}
				| AccessType KT_Void TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, NF_NONE | NF_ExplicitVoid, nullptr, $3, $5, @2);
					if ($7 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($7);
					$$ = func;
				} |
				//0.9.10 D3: interface member variables exist only as
				//`static const` constants folded at compile time. The
				//declaration contract (StatementResolverTypes.cpp)
				//enforces the flag set, foldability, type compatibility
				//and the method-name clash.
				AccessType NodeFlagsNonEmpty Type TT_Identifier '=' Expression ';' {
					auto* field = new SnClassField($3, $4, $1, @1, $6);
					field->AddFlags($2);
					$$ = field;
				} |
				//0.10.7 D4: the field shapes with missing parts parse
				//and hit the declaration contract (StatementResolver-
				//Types.cpp): zero-flag forms report "must be static
				//const", the no-initializer form reports the missing
				//constant expression. Isomorphic to ClassMember's
				//field arms — the ';'/`=`/`(` lookahead split is
				//proven conflict-free in the class body.
				AccessType Type TT_Identifier '=' Expression ';' {
					$$ = new SnClassField($2, $3, $1, @1, $5);
				} |
				AccessType Type TT_Identifier ';' {
					$$ = new SnClassField($2, $3, $1, @1);
				} |
				AccessType NodeFlagsNonEmpty Type TT_Identifier ';' {
					auto* field = new SnClassField($3, $4, $1, @1);
					field->AddFlags($2);
					$$ = field;
				} ;

ClassMemberList:	ClassMemberList ClassMember {
					$1->push_back($2);
					$$ = $1;
				} |
					ClassMember {
					$$ = new PtrList<SnField>();
					$$->push_back($1);
				} ;

//0.13.1c: a bodyless non-native method IS an abstract method — there
//is no separate keyword. Abstract implies virtual, so the pair
//NF_Abstract | NF_Virtual is created together here (grammar-time, like
//the explicit `virtual` keyword: PropagateVirtualOverrides reads the
//ancestor's flag at derived-resolution time, and the class bases are
//only pointer-bound by then). Native declarations are record-only
//(isNative) and never abstract.
ClassMember:	AccessType NodeFlag Type TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, $2, $3, $4, $6, @2);
					if ($8 == nullptr && ($2 & NF_Native) == 0)
						func->AddFlags(NF_Abstract | NF_Virtual);
					else
						func->Body($8);
					$$ = func;
				} |
				//v0.13.9 phase 3 (v3 grammar): inline '&' twins of the
				//plain-type method arms. A TypeRef wrapper here would put
				//a `TypeRef: Type` reduction into the shared post-type
				//state and lose it to the field-arm identifier shifts,
				//dead-ending every plain-type method; the twins instead
				//shift '&' directly in that state.
				AccessType NodeFlag Type '&' TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					$3->AddFlags(NF_Ref);
					auto* func = new SnFunction($1, $2, $3, $5, $7, @2);
					if ($9 == nullptr && ($2 & NF_Native) == 0)
						func->AddFlags(NF_Abstract | NF_Virtual);
					else
						func->Body($9);
					$$ = func;
				} |
				AccessType NodeFlagsNonEmpty Type TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, $2, $3, $4, $6, @3);
					if ($8 == nullptr && ($2 & NF_Native) == 0)
						func->AddFlags(NF_Abstract | NF_Virtual);
					else
						func->Body($8);
					$$ = func;
				} |
				AccessType NodeFlagsNonEmpty Type '&' TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					$3->AddFlags(NF_Ref);
					auto* func = new SnFunction($1, $2, $3, $5, $7, @3);
					if ($9 == nullptr && ($2 & NF_Native) == 0)
						func->AddFlags(NF_Abstract | NF_Virtual);
					else
						func->Body($9);
					$$ = func;
				} |
				AccessType Type TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, NF_NONE, $2, $3, $5, @2);
					if ($7 == nullptr)
						func->AddFlags(NF_Abstract | NF_Virtual);
					else
						func->Body($7);
					$$ = func;
				} |
				AccessType Type '&' TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					$2->AddFlags(NF_Ref);
					auto* func = new SnFunction($1, NF_NONE, $2, $4, $6, @2);
					if ($8 == nullptr)
						func->AddFlags(NF_Abstract | NF_Virtual);
					else
						func->Body($8);
					$$ = func;
				} |

				AccessType NodeFlag KT_Void TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, $2 | NF_ExplicitVoid, nullptr, $4, $6, @2);
					if ($8 == nullptr && ($2 & NF_Native) == 0)
						func->AddFlags(NF_Abstract | NF_Virtual);
					else
						func->Body($8);
					$$ = func;
				}
				| AccessType NodeFlagsNonEmpty KT_Void TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, $2 | NF_ExplicitVoid, nullptr, $4, $6, @3);
					if ($8 == nullptr && ($2 & NF_Native) == 0)
						func->AddFlags(NF_Abstract | NF_Virtual);
					else
						func->Body($8);
					$$ = func;
				}
				| AccessType KT_Void TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, NF_NONE | NF_ExplicitVoid, nullptr, $3, $5, @2);
					if ($7 == nullptr)
						func->AddFlags(NF_Abstract | NF_Virtual);
					else
						func->Body($7);
					$$ = func;
				} |

				//0.9.11 D1: constructor spelling -- no return type. Whether a
				//no-return member is a constructor is decided semantically
				//(member name == class name); a non-matching name is rejected
				//by the declaration contract. LALR: '(' after the identifier
				//separates this shape from the typed productions (after a
				//reduced Type the parser needs a name token, not '(').
				//The three ctor-spelling arms (no return type): whether the
				//member is a constructor is semantic (name == class name),
				//so the abstract pair stops at NF_Abstract here — NF_Virtual
				//is dispatch bookkeeping a constructor must never carry
				//(the ctor flag contract rejects it), and the resolver's
				//abstract contract pass names the ctor conflict itself.
				AccessType NodeFlag TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, $2, nullptr, $3, $5, @2);
					if ($7 == nullptr && ($2 & NF_Native) == 0)
						func->AddFlags(NF_Abstract);
					else
						func->Body($7);
					$$ = func;
				} |
				AccessType NodeFlagsNonEmpty TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, $2, nullptr, $3, $5, @3);
					if ($7 == nullptr && ($2 & NF_Native) == 0)
						func->AddFlags(NF_Abstract);
					else
						func->Body($7);
					$$ = func;
				} |
				AccessType TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, NF_NONE, nullptr, $2, $4, @2);
					if ($6 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($6);
					$$ = func;
				} |

				AccessType Type TT_Identifier ';' {
					$$ = new SnClassField($2, $3, $1, @1);
				} |

				//0.9.11 D4: a class field initializer is a compile-time
				//default. The grammar admits the shape; the constant-surface
				//check (FieldInitializerChecks.hpp) runs at Access(SnClassField&)
				//and the injection that applies the default runs later.
				AccessType Type TT_Identifier '=' Expression ';' {
					$$ = new SnClassField($2, $3, $1, @1, $5);
				} |

				//v0.12.6 D1 ruling: the singular-flag twins of the field
				//arms below. ClassMember — unlike InterfaceMember — also
				//carries singular NodeFlag FUNCTION arms, and the flag
				//overlap resolves by shifting on type openers (the 16-
				//conflict family at the `AccessType NodeFlag .` state):
				//without these arms `static int x;` dies inside the
				//function arm waiting for '('. The ';'/`=`/`(` split after
				//`AccessType NodeFlag Type TT_Identifier` is the same clean
				//token split as the plural arms.
				AccessType NodeFlag Type TT_Identifier ';' {
					auto* field = new SnClassField($3, $4, $1, @1);
					field->AddFlags($2);
					$$ = field;
				} |
				AccessType NodeFlag Type TT_Identifier '=' Expression ';' {
					auto* field = new SnClassField($3, $4, $1, @1, $6);
					field->AddFlags($2);
					$$ = field;
				} |

				//v0.12.6 D1: static fields on classes. The flagged shapes
				//mirror InterfaceMember's field arms — the ';'/`=`/`(`
				//lookahead split after `AccessType NodeFlagsNonEmpty Type
				//TT_Identifier` is the same one proven in the interface
				//body. The member contract (StatementResolverTypes.cpp)
				//admits only NF_Static and NF_Static|NF_Const here.
				AccessType NodeFlagsNonEmpty Type TT_Identifier ';' {
					auto* field = new SnClassField($3, $4, $1, @1);
					field->AddFlags($2);
					$$ = field;
				} |
				AccessType NodeFlagsNonEmpty Type TT_Identifier '=' Expression ';' {
					auto* field = new SnClassField($3, $4, $1, @1, $6);
					field->AddFlags($2);
					$$ = field;
				} ;

FunctionBodyOrSemi:	Paragraph { $$ = $1; } |
				';' { $$ = nullptr; } ;


NodeFlag:	KT_Const 		{ $$ = NF_Const;    	} |
				KT_Static       { $$ = NF_Static;       } |
				KT_Virtual      { $$ = NF_Virtual;      } |
				KT_Native       { $$ = NF_Native;       } ;

AccessType:	KT_Private  	{ $$ = FA_Private;      } |
				KT_Protected    { $$ = FA_Protected;    } |
				KT_Public       { $$ = FA_Public;       } |
								{ $$ = FA_Default;	/*on empty */	} ;

//NameExpr is identifier-only by design. It formerly also derived
//MemberExpr (for `A.B` qualified types) — zero usage in the language,
//and the dual parentage (NameExpr|Expression both deriving MemberExpr)
//was the dominant source of reduce/reduce conflicts. Removing it (plus
//the InterfaceDecl empty-body production) took the grammar from 75 rr
//conflicts down to none, as the re-measure below shows. Removed in the
//Phase 10 audit; do not re-add without a real use.
//Accepted-conflict ledger (re-measured 2026-10-28, bison 3.8.2, on the
//0.11 merge grammar): 30 shift/reduce, 3 reduce/reduce. Re-measured
//unchanged 2026-10-29 (v0.12.1, bison 3.8.2): the Type arm's mid-rule
//depth marker adds one state and renumbers only the ClassMember family
//(677→678); every other state number holds. Re-measured 2026-11-02
//(v0.12.6, bison 3.8.2): the two static local statement arms add ONE
//shift/reduce — the second conflict of the top-level boundary state —
//moving the totals to 31/3; every conflicted state renumbered, and
//the whole list below re-derived from the current .output. Re-measured
//2026-11-05 (v0.12.7 Task 11, bison 3.8.2): unifying the two subscript
//arms into SubscriptItemList REMOVES the two `ConcreteParam: Expression
//.' shift/reduce conflicts — ConcreteParam no longer lives in the
//bracket context — moving the totals to 29/3; every other conflict is
//unchanged in kind (rule renumbering only, 364→375 family). Re-measured
//2026-11-13 (v0.13.7 dead-token teardown, bison 3.8.2): removing the
//KT_Elseif/KT_State/OT_INC/OT_DEC/OT_AEQ declarations and their
//precedence entries — no production ever consumed them — holds the
//totals at 29/3 unchanged. History: dev's
//line measured 27/3 with every generic opener on the 0.10.8 D1
//OT_TypeLt token; master's declared-generics line re-adopted the
//plain-'<' family because a user-declared generic class name cannot
//live in any scan-time set. The merge keeps BOTH openers with disjoint
//duties: OT_TypeLt survives where the name is scan-time-known and the
//opener must never compete with the comparison '<' (the anchored
//literal `List<int>{...}`, an Expression production — a plain-'<' opener
//there flips every `a < b` state to the generic reading), and Type/
//HeadType carry twin arms on both spellings so user-declared generics
//parse in type positions. The total moves 27/3 → 30/3. The same
//re-measure also replaced TypeParams' base arm with the standard list
//idiom (TypeParamRests) — the old self-recursive shape rejected every
//declaration with two or more parameters without touching these
//counts. Every one
//resolved by bison's default to the intended reading. Deterministic
//parsers cannot use %expect alongside r/r conflicts, so no %expect pins
//these numbers: the build passes -Wno-conflicts-sr/-Wno-conflicts-rr to
//stay silent, and the counts are guarded by re-measuring (the
//conflicts: lines of a `bison -v` .output) on every grammar edit —
//compare against this ledger and never bump it silently. State numbers
//are those of the current .output; they shift with every grammar
//change, so the whole list is re-derived at each re-measure.
//Re-measured 2026-11-15 (cross-module completion Task 2, bison 3.8.2).
//Two corrections first, both found while verifying this re-measure:
//(1) the true merge (14e824dd, 2026-11-14) moved the totals 29/3 to
//32/5 (+3 sr, +2 rr) without a ledger entry — verified by regenerating
//the .output at 073a3538 (29/3 exact, so the 2026-11-13 entry held)
//and at 14e824dd (32/5). The three new families are master-side
//v0.13.7-era arms the merge brought over: the const-flagged top-level
//header (`const T name(` — +1 sr +2 rr), the generic-header
//empty-parens shape (`f<T>(` — +1 sr), and the plain-'<' arm in `as`
//position splitting out of the direct-init family (+1 sr); the
//v0.12.7 comma-index rows stay absent. (2) the itemized list had also
//gone stale on its own: the totals moved twice since the 2026-11-02
//re-derivation (v0.12.7's -2 among them) without the list being
//re-derived, so its rows no longer summed to the header. The list is
//re-derived below in full from the current .output. Task 2's own
//change — the access-prefix wrapper layer on the four top-level
//type-decl families — adds ZERO conflicts (32/5 before, 32/5 after,
//identical conflict-shape multiset): AccessTypeNonEmpty's reduce sits
//in an isolated single-item state (state 55), so it reduces by
//default before any continuation token is ever seen; every
//continuation lives in the GOTO states on AccessTypeNonEmpty (87
//top-level, 467 struct-body) where only shifts compete; and since an
//elaborated type spelling after an access prefix (`private Box<int>`)
//is not in the language, no '<' FOLLOW ever reaches those states.
//- state 12 (2): the top-level boundary vs AccessTypeEmpty's epsilon
//  reduce. v0.9.1's `const` member: `const int x = 5;` lives (shift
//  opens the statement) and `const int f(){}` is rejected. v0.12.6's
//  `static` twin is the same trade one keyword later: `static int x =
//  1;` parses as a top-level statement (the resolver's static-local
//  placement contract then rejects it), while a `static`-flagged
//  top-level function header is unparseable — the pre-declared cost
//  of putting KT_Static in FIRST(Statement), zero corpus hits.
//- states 124/144 (1 each): the '<' shapes in head position — an
//  explicit generic type head (`Box<int> b;`) and the void-return func
//  spellings, on the plain-'<' arms. Shift keeps the generic reading;
//  the comparison reading loses only in these head states — never in
//  pure expression states (the anchored literal holds OT_TypeLt).
//- state 325 (2): catch/finally after a nested `try` statement — the
//  dangling-clause shape; shift binds the clause to the innermost try.
//- states 344/441 (1 sr + 2 rr each): after `T name(` — and, one
//  keyword earlier, after `const T name(` — ')' shifts to the explicit
//  empty-parens arm, and FormalParamList's vs ConcreteParamList's empty
//  productions both reduce on ')' and ',' — the function header wins
//  (earlier-declared rule), so a bodyless bare `T name();` is always a
//  construction, never a declaration; forward declarations carry the
//  explicit `func` marker for that reason. The 441 twin arrived with
//  the merge (master's const-flagged top-level header arms).
//- states 350/351 (1 each, added with TypeName): ':' / `as` followed by
//  a dotted type name — '.' shifts to continue the chain, the default
//  reduces TypeName. Nothing may follow a type name with '.', so the
//  default is the intended reading.
//- state 353 (1): '<' in `as` position — NameExpr vs the AsGenericTarget
//  plain-'<' arm; shift keeps the generic-cast reading. Arrived with
//  the merge, when the as-generic family split out of the state 504
//  parent.
//- states 445/458 (1 each, added with 0.9.15): the empty-parens shape
//  on the body-context first declarator (`T name();` inside a body)
//  and the top-level header alone — ')' shifts to the explicit empty
//  arm; same reading as the state 344 family.
//- state 504 (1 sr + 1 rr): NameExpr vs Expression on '.'/'<' after a
//  top-level direct-init first argument — NameExpr wins, so
//  `T name(a.b);` fails cleanly on the type path; the workaround is
//  `T name = T(a.b);`. Function bodies are unaffected.
//- states 508/648 (1 each): the same empty-parens shape on a generic
//  header (`f<T>(` — arrived with the merge's declared-generics line)
//  and on a continuation declarator (added with 0.10.7 D1) — after
//  `T a, b(`, ')' shifts to the explicit empty-parens continuation
//  arm; `T a, b();` is a zero-argument construction, same reading as
//  the first arm.
//- state 826 (16, renumbered 677→678→698 by v0.12.1/v0.12.6 and again
//  here): ClassMember's
//  NodeFlag-singular vs
//  NodeFlagsNonEmpty-plural productions overlap on the flag/type first
//  tokens — both derivations parse the same member; shift keeps reading
//  flags. The v0.9.1 epsilon-layer removal deleted the NodeFlags empty
//  arm, taking this family from 20 down to 16.
//Re-measured 2026-11-17 (v0.13.9 phase 3 Task 1, bison 3.8.2): the
//reference-'&' declarator grammar (the v3 shape — inline
//`HeadType '&' ...` arms on every declarator consumer, no wrapper
//nonterminals) moves the totals 32/5 → 35/12. Every conflicted state
//renumbered; the old list above keeps its 2026-11-15 numbers, whose
//new homes are: 12→12, 124→125, 144→145, 325→331, 344→351, 350→359,
//351→360, 353→362, 441→453, 445→473, 458→458, 504→523, 508→527,
//648→681, 826→878. Each new conflict was read from the state tables
//and resolves as intended:
//- states 122/143 (1 rr each, new): `HeadType: MemberExpr .` vs
//  `Expression: MemberExpr .` over '&' — HeadType wins (earlier rule),
//  so `ns.Point& r = ...;` routes into the declarator arms; the
//  ';'/assign/compound-assign shifts in the statement-head twin (122)
//  are untouched.
//- states 125/145 (1 sr + 1 rr each, renumbered from 124/144): the same
//  '&' reduce/reduce on the identifier spellings — HeadType wins, so
//  `Point& r = p;` and `Box<int>& r = q;` reach the declarator route
//  while every expression continuation keeps its shift. The sr row is
//  the pre-existing generic-head '<' family.
//- state 523 (1 sr + 2 rr, renumbered from 504): '&' joins the
//  NameExpr-vs-Expression family in the top-level direct-init argument
//  position — NameExpr wins, so `T name(a & b);` fails cleanly on the
//  type path exactly like `T name(a.b);` already did; the workaround
//  is the init form `T name = T(a & b);`, whose context holds no
//  HeadType/NameExpr-type items and keeps the bitwise reading.
//- states 528/620/679 (new; 528 adds 1 sr + 2 rr, 620/679 add 1 sr
//  each): the '&'-header empty-parens family — after `int& f(` and its
//  generic twin `int& f<T>(`, ')' shifts to the explicit empty arm;
//  528's two rr rows are FormalParamList's vs ConcreteParamList's
//  epsilon reduces, the exact accepted 344-family shape on the new
//  arm.
//Route note: '&' never shifts in any expression-completion state. The
//statement-head/init-context bitwise readings reduce through
//Expression-only states (no HeadType items), and the three bitwise
//guards in tests/test_compiler/test_ref_declarator.cpp (init,
//subscript, call) pin that. Deleted with this change: the Route C
//wrapper family DeclLocal/HeadTypeScan/HeadTypeScanRef/HeadTypeMarked/
//ReturnSlot/ReturnSlotFlagged (interim measurement 44 sr + 8 rr,
//rejected). Also removed with it: the `%prec P_Then` on
//`Expression: IdentifierExpr` (Route C baggage; the identifier
//state's only conflict is the '&' reduce/reduce, resolved by rule
//order) and the ScriptScanner probe extension that armed
//`Box<int>&`-shaped openers (unnecessary — the plain-'<' HeadType arms
//complete HeadType directly and the '&' declarator shift follows).
NameExpr:	IdentifierExpr	{ $$ = new SnNameExpr($1, @1); } ;

//Type non-terminal used in type contexts (declarations, params, fields).
//Uses OT_Brackets ('[]' as single token) to disambiguate array type
//suffix from subscript expression (arr[i]).
//The array suffix produces a dedicated SnArrayTypeExpr node, keeping
//SnNameExpr focused on plain name expressions.
//Phase 8e-3: the generic spellings produce SnGenericTypeExpr for
//generic types like List<T>. Both angle spellings exist (2026-10-28
//merge): OT_TypeLt for scan-flagged names (built-ins + Promise) and
//plain '<' for user-declared generics, whose names cannot live in a
//scan-time set. The openers are distinct tokens, so the sister
//productions never compete over one lookahead.
//v0.12.1: the plain-'<' arm carries a mid-rule depth increment — the
//scanner's glued-closer split (">>" as two '>') reads the nesting
//depth, and only the parser can know a declared generic's '<' opened
//a type-argument list.
Type:	NameExpr		{ $$ = $1; } |
				QualifiedType	{ $$ = $1; } |
				IdentifierExpr OT_TypeLt TypeList '>'	{ $$ = new SnGenericTypeExpr(new SnNameExpr($1, @1), $3, @1); } |
				IdentifierExpr '<' { parser.Scanner().IncGenericDepth(); } TypeList '>'	{ $$ = new SnGenericTypeExpr(new SnNameExpr($1, @1), $4, @1); } |
				//Phase 13: void in the first type-arg slot (func's return
				//slot). KT_Void never derives Type, so the void spellings
				//need their own sister productions; the void node must be a
				//real type argument so `func<void,int>` and `func<int>`
				//produce different GenericInstKeys.
				IdentifierExpr OT_TypeLt KT_Void '>'	{
						auto* pVoid = new SnIdentifierExpr(NK_Void, @3);
						$$ = new SnGenericTypeExpr(new SnNameExpr($1, @1),
							new std::vector<nlang::SnFieldExpr*>{ pVoid }, @1);
					} |
				IdentifierExpr '<' KT_Void '>'	{
						auto* pVoid = new SnIdentifierExpr(NK_Void, @3);
						$$ = new SnGenericTypeExpr(new SnNameExpr($1, @1),
							new std::vector<nlang::SnFieldExpr*>{ pVoid }, @1);
					} |
				IdentifierExpr OT_TypeLt KT_Void ',' TypeList '>'	{
						auto* pVoid = new SnIdentifierExpr(NK_Void, @3);
						$5->insert($5->begin(), pVoid);
						$$ = new SnGenericTypeExpr(new SnNameExpr($1, @1), $5, @1);
					} |
				IdentifierExpr '<' KT_Void ',' TypeList '>'	{
						auto* pVoid = new SnIdentifierExpr(NK_Void, @3);
						$5->insert($5->begin(), pVoid);
						$$ = new SnGenericTypeExpr(new SnNameExpr($1, @1), $5, @1);
					} |
				//0.8.3: func is a keyword; its generic spellings mirror
				//the NameExpr ones above but start from KT_Func and create
				//the base-name node from kBuiltinFuncTypeName. The func
				//keyword is always scan-flagged, so only the OT_TypeLt
				//spelling is reachable here. v0.13.9 phase 3: the argument
				//slots ride FuncTypeList so `func<int&>` parses.
				KT_Func OT_TypeLt FuncTypeList '>'	{
						$$ = MakeFuncType($3, @1);
					} |
				KT_Func OT_TypeLt KT_Void '>'	{
						auto* pVoid = new SnIdentifierExpr(NK_Void, @3);
						$$ = MakeFuncType(
							new std::vector<nlang::SnFieldExpr*>{ pVoid }, @1);
					} |
				KT_Func OT_TypeLt KT_Void ',' FuncTypeList '>'	{
						auto* pVoid = new SnIdentifierExpr(NK_Void, @3);
						$5->insert($5->begin(), pVoid);
						$$ = MakeFuncType($5, @1);
					} |
				Type OT_Brackets	{ $$ = new SnArrayTypeExpr($1, @2); } |
				Type OT_BracketsND	{ $$ = MakeArrayTypeND($1, @2, $2, parser); } ;

//v0.13.9 phase 3: the '&' reference mark rides a Type wherever the
//grammar holds only type items (formal params, foreach heads, member
//methods, enum methods). Expression-context heads never read '&' here —
//they route through the statement-head/DeclHeader arms instead, which
//keeps '&' out of Type's lookahead in the shared statement-head states.
TypeRef:	Type	{ $$ = $1; } |
			Type '&'	{ $1->AddFlags(NF_Ref); $$ = $1; } ;

//Qualified type reference for UNAMBIGUOUS type positions (params, fields,
//catch, foreach, return types, type arguments, new): "ns.Type" /
//"a.b.Type". It starts from a NameExpr and requires at least one '.', so
//a lone identifier stays a plain NameExpr and never collides over a
//single token. These positions do not pass through the statement-head
//state where a declaration and an expression statement are ambiguous,
//so the NameExpr-led chain does not create a reduce/reduce there. The
//segments drive ExprResolver's module lookup. Generic/array suffixes
//stay on the outer Type rule.
QualifiedType:	NameExpr '.' TT_Identifier {
					auto* pFirst = static_cast<SnIdentifierExpr*>($1->Expr());
					$$ = new SnQualifiedTypeExpr(pFirst->Name(), *$3, @1);
					delete $1;
					delete $3;
				} |
				QualifiedType '.' TT_Identifier {
					auto* pQ = static_cast<SnQualifiedTypeExpr*>($1);
					pQ->AppendSegment(*$3);
					delete $3;
					$$ = $1;
				} ;

//HeadType: a type at a STATEMENT HEAD (a local declaration), where a
//declaration and an expression statement are otherwise
//indistinguishable. The dotted chain is parsed with the normal member-
//access shifts (bison keeps shifting '.'), and is reduced to a type
//only once the token after the chain proves it is a declaration — that
//token is the variable-name identifier (`a.b c;`); '=' / ';' / '.' / an
//operator instead mean an expression statement (`a.b;`, `a.b = ...;`).
//LALR keeps only the viable reduction per lookahead. Plain/generic/
//array spellings are included so all local declarations route here.
HeadType:	IdentifierExpr {
				$$ = new SnNameExpr($1, @1);
			} |
			//Dual angle spellings, same rule as Type above: OT_TypeLt
			//for scan-flagged names, plain '<' for user-declared
			//generics. Distinct openers, no shared lookahead.
			//v0.13.9 phase 3 (v3 grammar): every '&'-declarator consumer
			//holds an INLINE `HeadType '&' ...` arm, so post-type states
			//keep shift-only '&' actions. The accepted reduce/reduce at
			//the statement-head identifier state resolves HeadType-first
			//(earlier rule) and routes into those declarator arms;
			//init contexts hold no HeadType items, so bitwise '&' stays
			//an expression operator there. See the conflict ledger in
			//this file's header.
			IdentifierExpr OT_TypeLt TypeList '>' {
				$$ = new SnGenericTypeExpr(new SnNameExpr($1, @1), $3, @1);
			} |
			IdentifierExpr '<' TypeList '>' {
				$$ = new SnGenericTypeExpr(new SnNameExpr($1, @1), $3, @1);
			} |
			IdentifierExpr OT_TypeLt KT_Void '>' {
				auto* pVoid = new SnIdentifierExpr(NK_Void, @3);
				$$ = new SnGenericTypeExpr(new SnNameExpr($1, @1),
					new std::vector<nlang::SnFieldExpr*>{ pVoid }, @1);
			} |
			IdentifierExpr '<' KT_Void '>' {
				auto* pVoid = new SnIdentifierExpr(NK_Void, @3);
				$$ = new SnGenericTypeExpr(new SnNameExpr($1, @1),
					new std::vector<nlang::SnFieldExpr*>{ pVoid }, @1);
			} |
			IdentifierExpr OT_TypeLt KT_Void ',' TypeList '>' {
				auto* pVoid = new SnIdentifierExpr(NK_Void, @3);
				$5->insert($5->begin(), pVoid);
				$$ = new SnGenericTypeExpr(new SnNameExpr($1, @1), $5, @1);
			} |
			IdentifierExpr '<' KT_Void ',' TypeList '>' {
				auto* pVoid = new SnIdentifierExpr(NK_Void, @3);
				$5->insert($5->begin(), pVoid);
				$$ = new SnGenericTypeExpr(new SnNameExpr($1, @1), $5, @1);
			} |
			MemberExpr {
				std::vector<std::string> segs;
				SnQualifiedTypeExpr* pQ = nullptr;
				if (CollectQualifiedSegments($1, segs) && segs.size() >= 2)
				{
					pQ = new SnQualifiedTypeExpr(segs[0], segs[1], @1);
					for (size_t k = 2; k < segs.size(); ++k)
						pQ->AppendSegment(segs[k]);
				}
				else
				{
					//Not an identifier chain (a call, a subscript, a
					//parenthesised outer). Keep a typed, unresolved node on
					//the tree so the resolver owns the diagnostic.
					pQ = new SnQualifiedTypeExpr(
						std::string(), std::string(), @1);
					pQ->MarkMalformed();
				}
				$$ = pQ;
				delete $1;
			} |
			//0.8.3: func is a keyword; its generic spellings mirror
			//the IdentifierExpr ones above but start from KT_Func and
			//create the base-name node from kBuiltinFuncTypeName. The
			//keyword is always scan-flagged, so only the OT_TypeLt
			//spelling is reachable. v0.13.9 phase 3: the argument
			//slots ride FuncTypeList so `func<int&>` parses.
			KT_Func OT_TypeLt FuncTypeList '>' {
				$$ = MakeFuncType($3, @1);
			} |
			KT_Func OT_TypeLt KT_Void '>' {
				auto* pVoid = new SnIdentifierExpr(NK_Void, @3);
				$$ = MakeFuncType(
					new std::vector<nlang::SnFieldExpr*>{ pVoid }, @1);
			} |
			KT_Func OT_TypeLt KT_Void ',' FuncTypeList '>' {
				auto* pVoid = new SnIdentifierExpr(NK_Void, @3);
				$5->insert($5->begin(), pVoid);
				$$ = MakeFuncType($5, @1);
			} |
			HeadType OT_Brackets {
				$$ = new SnArrayTypeExpr($1, @2);
			} |
			HeadType OT_BracketsND {
				$$ = MakeArrayTypeND($1, @2, $2, parser);
			} ;

//ParenHeadType: the group-position twin of HeadType (lambda/closure
//plan Task 2). Same deferred-reduction shape — dotted chains ride
//MemberExpr and only the parameter-name identifier proves the type
//reading — but with a different ambiguity policy, because a
//parenthesized group is an EXPRESSION position while a statement head
//is a declaration position. HeadType's identifier state resolves the
//accepted reduce/reduce HeadType-first and its consumers hold inline
//'&' declarator arms, so statement heads read ident-first as
//declarations. The twin deliberately omits the plain-'<' generic arms
//and any '&' declarator: '.', '&' and '<' then have no type-side
//viability in the twin's identifier state and route to member access,
//bitwise-and and comparison — the pre-existing parenthesized-expression
//behavior every existing source relies on (stdlib/regex.n's
//`(mFlags & kFlagDot)` idiom, `(mPos < mCpCount)` guards). What remains
//type-only (ident-followed-by-ident, OT_TypeLt generics, OT_Brackets
//arrays) keeps the twin's identifier state conflict-free. A lambda
//parameter typed `int&` therefore arrives as the binary `int & x`;
//the LambdaExpr action reinterprets it once the arrow confirms the
//parameter reading (FoldBareIdentifierParams). User-declared generics
//spelled with plain '<' cannot head a group parameter — OT_TypeLt
//(scan-flagged builtin names) still can.
ParenHeadType:	IdentifierExpr {
					$$ = new SnNameExpr($1, @1);
				} |
				IdentifierExpr OT_TypeLt TypeList '>' {
					$$ = new SnGenericTypeExpr(new SnNameExpr($1, @1), $3, @1);
				} |
				IdentifierExpr OT_TypeLt KT_Void '>' {
					auto* pVoid = new SnIdentifierExpr(NK_Void, @3);
					$$ = new SnGenericTypeExpr(new SnNameExpr($1, @1),
						new std::vector<nlang::SnFieldExpr*>{ pVoid }, @1);
				} |
				IdentifierExpr OT_TypeLt KT_Void ',' TypeList '>' {
					auto* pVoid = new SnIdentifierExpr(NK_Void, @3);
					$5->insert($5->begin(), pVoid);
					$$ = new SnGenericTypeExpr(new SnNameExpr($1, @1), $5, @1);
				} |
				MemberExpr {
					std::vector<std::string> segs;
					SnQualifiedTypeExpr* pQ = nullptr;
					if (CollectQualifiedSegments($1, segs) && segs.size() >= 2)
					{
						pQ = new SnQualifiedTypeExpr(segs[0], segs[1], @1);
						for (size_t k = 2; k < segs.size(); ++k)
							pQ->AppendSegment(segs[k]);
					}
					else
					{
						//Not an identifier chain (a call, a subscript, a
						//parenthesised outer). Keep a typed, unresolved node on
						//the tree so the resolver owns the diagnostic.
						pQ = new SnQualifiedTypeExpr(
							std::string(), std::string(), @1);
						pQ->MarkMalformed();
					}
					$$ = pQ;
					delete $1;
				} |
				KT_Func OT_TypeLt FuncTypeList '>' {
					$$ = MakeFuncType($3, @1);
				} |
				KT_Func OT_TypeLt KT_Void '>' {
					auto* pVoid = new SnIdentifierExpr(NK_Void, @3);
					$$ = MakeFuncType(
						new std::vector<nlang::SnFieldExpr*>{ pVoid }, @1);
				} |
				KT_Func OT_TypeLt KT_Void ',' FuncTypeList '>' {
					auto* pVoid = new SnIdentifierExpr(NK_Void, @3);
					$5->insert($5->begin(), pVoid);
					$$ = MakeFuncType($5, @1);
				} |
				ParenHeadType OT_Brackets {
					$$ = new SnArrayTypeExpr($1, @2);
				} |
				ParenHeadType OT_BracketsND {
					$$ = MakeArrayTypeND($1, @2, $2, parser);
				} ;

//A single type argument inside `<...>`: a plain type, or an
//out-marked type. v0.13.9 phase 4: the out arms are diagnostic
//anchors only — RejectInvalidTypeArg reports the retirement on every
//NF_Out node; nothing keys or compares the flag anymore.
TypeArg:	Type {
						$$ = $1;
					} |
					KT_Out Type {
						$2->AddFlags(NF_Out);
						$$ = $2;
					} ;

//Comma-separated list of type arguments inside `<...>`. Used by
//the generic Type rule above and the anchored `func<...>(...)` arm.
TypeList:	TypeArg {
						$$ = new std::vector<nlang::SnFieldExpr*>{ $1 };
					} |
					TypeList ',' TypeArg {
						$1->push_back($3);
						$$ = $1;
					} ;

//v0.13.9 phase 3: argument list of a func TYPE spelling. Sister of
//TypeArg with the '&' mark added; `out int&` stays a syntax error and
//plain generic arms keep plain TypeList, so `List<int&>` is rejected.
FuncTypeArg:	Type {
						$$ = $1;
					} |
					KT_Out Type {
						$2->AddFlags(NF_Out);
						$$ = $2;
					} |
					Type '&' {
						$1->AddFlags(NF_Ref);
						$$ = $1;
					} ;

FuncTypeList:	FuncTypeArg {
						$$ = new std::vector<nlang::SnFieldExpr*>{ $1 };
					} |
					FuncTypeList ',' FuncTypeArg {
						$1->push_back($3);
						$$ = $1;
					} ;

Expression:	ParenthesesExpr	{ $$ = $1; } |
				MemberExpr		{ $$ = $1; } |
				LiteralExpr		{ $$ = $1; } |
				InvokeExpr		{ $$ = $1; } |
				IdentifierExpr	{ $$ = $1; } |
				SubscriptExpr		{ $$ = $1; } |
				KT_This		{ $$ = new SnThisExpr(@1); } |
				KT_Null		{ auto* _nl = new SnLiteralExpr(*RnInt32::Instance(), 0, @1); _nl->AddFlags(NF_NullLiteral); $$ = _nl; } |
				//v0.13.1 D4: typeName(Type) — the canonical name of a
				//type as a string. KT_TypeName leads no other rule, so the
				//parenthesized Type (the same nonterminal the unambiguous
				//type positions use) needs no dedicated state.
				KT_TypeName '(' Type ')'	{ $$ = new SnTypeNameExpr($3, @1); } |
				//Phase 8e-6: bare `[...]` array/list literal. Only the bracket
				//form is allowed bare; dict/struct `{...}` requires an anchored
				//type (`Vec{...}`) because bare `{...}` would LALR-conflict
				//with Paragraph (block statement).
				'[' InitEntries ']'	{ $$ = new SnInitListExpr(nullptr, *$2, true, @1); delete $2; } |
				//0.9.13 B5: the lexer collapses `[]` into one OT_Brackets token.
				OT_Brackets	{ auto* pEmpty = new std::vector<nlang::InitEntry>(); $$ = new SnInitListExpr(nullptr, *pEmpty, true, @1); delete pEmpty; } |
				//0.9.0: anchored brace init `Vec{x: 1, y: 2}`. The identifier
				//reads as a type name at resolve time.
				//No LALR conflict: a legal
				//program never places '{' after a completed expression.
				TT_Identifier '{' InitEntries '}' {
					auto* pId = new SnIdentifierExpr($1, @1);
					auto* pName = new SnNameExpr(pId, @1);
					$$ = new SnInitListExpr(pName, *$3, false, @1);
					delete $3;
				} |
				//0.10.8 D1: anchored generic instantiation literal
				//`List<int>{60}`. The opener is the dedicated OT_TypeLt
				//(scan-flagged generic names only): as an Expression
				//production it must never compete with the comparison
				//'<' — a plain-'<' opener here would flip every
				//`a < b` expression state to the generic reading
				//(0.11 merge re-measure). Since 0.13.1a Task 3 declared
				//generics gained expression-position use too, through
				//QualifiedGenericReceiver on the probe-created OT_TypeLt
				//(below); their type contexts still parse on the
				//plain-'<' arms. In this state '{' shifts
				//(the literal) and a name reduces the type.
				IdentifierExpr OT_TypeLt TypeList '>' '{' InitEntries '}' {
					$$ = new SnInitListExpr(
						new SnGenericTypeExpr(new SnNameExpr($1, @1), $3, @1),
						*$6, false, @1);
					delete $6;
				} |
				//0.13.1a Task 3: constructor call on an explicit
				//instantiation — `Hold<int>(5)`. The receiver is the
				//probe-created QualifiedGenericReceiver; SnNewExpr takes
				//the class name as a SnFieldExpr and the argument list
				//exactly like the SnInvokeExpr arm does.
				QualifiedGenericReceiver '(' ConcreteParamList ')' {
					//0.13.1c identity rescue: keep the verbatim bytes —
					//when this spelling turns out to be a misread
					//comparison shape, the rescue re-parses exactly what
					//was written, not an AST re-render.
					auto *pNew = new SnNewExpr($1, $3, @1);
					pNew->SpanText(parser.CaptureSpanText(@1, @4));
					$$ = pNew;
				} |
				Expression '+' Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Add, $1, $3, @1); } |
				Expression '-' Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Sub, $1, $3, @1); } |
				Expression '*' Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Mul, $1, $3, @1); } |
				Expression '/' Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Div, $1, $3, @1); } |
				Expression '%' Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Mod, $1, $3, @1); } |
				Expression '<' Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Less, $1, $3, @1); } |
				Expression OT_LE Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_LessEqual, $1, $3, @1); } |
				Expression '>' Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Greater, $1, $3, @1); } |
				Expression OT_GE Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_GreaterEqual, $1, $3, @1); } |
				//0.9.9: shift operators; tokens and precedence existed since
				//0.7.5 (between relational and additive), productions did not.
				Expression OT_LSH Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Shl, $1, $3, @1); } |
				Expression OT_RSH Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Shr, $1, $3, @1); } |
				//v0.13.7: bitwise family; tokens and precedence existed
				//since 0.7.5, productions did not (same history as shift).
				Expression '&' Expression		{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_BitAnd, $1, $3, @1); } |
				Expression '^' Expression		{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_BitXor, $1, $3, @1); } |
				Expression '|' Expression		{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_BitOr, $1, $3, @1); } |
				Expression OT_EQ Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Equal, $1, $3, @1); } |
				Expression OT_NE Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_NotEqual, $1, $3, @1); } |
				Expression OT_AND Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_LogicalAnd, $1, $3, @1); } |
				Expression OT_OR Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_LogicalOr, $1, $3, @1); } |
				'-' Expression %prec P_Minus	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Neg, $2, @1); } |
				'+' Expression %prec P_Minus	{ $$ = $2; } |
				'!' Expression					{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_LogicalNot, $2, @1); } |
				'~' Expression					{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_BitNot, $2, @1); } |
				Expression KT_As TypeName		{ $$ = new SnAsExpr($1, $3, @2); } |
				//v0.13.1 D5: the as-target gains the generic spelling —
				//`o as Box<int>` distinguishes instantiations of one erased
				//template row (the descriptor check joins OP_CheckCast).
				//Both angle spellings mirror Type's generic arms: OT_TypeLt
				//for scan-flagged names (built-ins), plain '<' with the
				//mid-rule depth increment for user-declared templates.
				Expression KT_As IdentifierExpr OT_TypeLt TypeList '>'	{
						$$ = new SnAsExpr($1,
							new SnGenericTypeExpr(new SnNameExpr($3, @3), $5, @3), @2);
					} |
				//v0.13.1 D5: the plain-'<' generic target routes through
				//AsGenericTarget (one mid-rule depth increment for the
				//whole family — see its rule).
				Expression KT_As AsGenericTarget	{
						$$ = new SnAsExpr($1, $3, @2);
					} |
				//v0.13.1 D5: the annotated array of instantiations —
				//`o as Box<string>[]`. The element check joins the rank
				//check (OP_CheckArrayType + the element descriptor row).
				Expression KT_As IdentifierExpr OT_TypeLt TypeList '>' OT_Brackets	{
						$$ = new SnAsExpr($1,
							new SnArrayTypeExpr(
								new SnGenericTypeExpr(new SnNameExpr($3, @3), $5, @3), @7),
							@2);
					} |
				Expression KT_As AsGenericTarget OT_Brackets
					{ $$ = new SnAsExpr($1, new SnArrayTypeExpr($3, @4), @2); } |
				Expression KT_As IdentifierExpr OT_TypeLt TypeList '>' OT_BracketsND	{
						$$ = new SnAsExpr($1,
							MakeArrayTypeND(
								new SnGenericTypeExpr(new SnNameExpr($3, @3), $5, @3),
								@7, $7, parser), @2);
					} |
				Expression KT_As AsGenericTarget OT_BracketsND
					{ $$ = new SnAsExpr($1, MakeArrayTypeND($3, @4, $4, parser), @2); } |
				//v0.12.7: the as-target gains the bracket spellings — the
				//boundary rank check (OP_CheckArrayRank) is the whole point
				//of `dyn as int[]`. The generic spelling adds nothing as a
				//target (it is unannotated — no check, no conversion), so
				//only the annotation forms exist here.
				Expression KT_As TypeName OT_Brackets
					{ $$ = new SnAsExpr($1, new SnArrayTypeExpr($3, @4), @2); } |
				Expression KT_As TypeName OT_BracketsND
					{ $$ = new SnAsExpr($1, MakeArrayTypeND($3, @4, $4, parser), @2); } |
				//0.9.0: `func<...>(...)` at value positions keeps the anchored
				//resolver reject (func types have no by-name constructor; bind
				//a reference) - the diagnostic experience inherited from the
				//removed `new func<...>()` spelling. KT_Func is a keyword, so
				//this arm cannot collide with the generic-name parses; the
				//keyword is always scan-flagged, so only the OT_TypeLt
				//spelling is reachable.
				KT_Func OT_TypeLt FuncTypeList '>' '(' ConcreteParamList ')' {
					$$ = new SnNewExpr(MakeFuncType($3, @1), $6, @1);
				} ;

//0.13.10 D9: the unified prefix for parenthesized groups and lambda
//parameter lists. `( InnerList )` always reduces to ParenPrefix; on the
//OT_Arrow lookahead the parser then shifts into LambdaExpr, on anything
//else it reduces ParenthesesExpr and the post-check decides the group
//shape. `->` sits in no existing expression FOLLOW set, so the split
//stays conflict-free.
ParenPrefix:	'(' InnerList ')'	{ $$ = $2; } ;

InnerList:	/* empty */
					{ $$ = new std::vector<nlang::LambdaParam>(); } |
				InnerListElem {
						auto* pVec = new std::vector<nlang::LambdaParam>();
						pVec->push_back(std::move(*$1));
						delete $1;
						$$ = pVec;
					} |
				InnerList ',' InnerListElem {
						$1->push_back(std::move(*$3));
						delete $3;
						$$ = $1;
					} ;

InnerListElem:	Expression {
						auto* pElem = new nlang::LambdaParam();
						pElem->pValueExpr = $1;
						$$ = pElem;
					} |
				//The typed-param arm rides ParenHeadType (the group-position
				//twin of HeadType, see there), not TypeRef: QualifiedType
				//would put '.' in the type lookahead of the group's
				//identifier state, and the reduce/reduce there resolves
				//ident '.' to the earlier type rule — every parenthesized
				//member access `(t.length)` would die as a qualified-type
				//chain. Keyword types are IdentifierExprs, so
				//`int`/`func<...>` ride the same arm. The reference
				//declarator `int& x` has no grammar arm here (the expression
				//reading owns '&'); it parses as the binary `int & x` and
				//the LambdaExpr action reinterprets it after the arrow.
				ParenHeadType TT_Identifier {
						auto* pElem = new nlang::LambdaParam();
						pElem->pType = $1;
						pElem->name = *$2;
						delete $2;
						$$ = pElem;
					} ;

ParenthesesExpr:	ParenPrefix {
						$$ = ParenGroupAsExpression(parser, $1, @1);
					} |
					LambdaExpr	{ $$ = $1; } ;

LambdaExpr:	ParenPrefix OT_Arrow Expression {
						FoldBareIdentifierParams(*$1, @1);
						$$ = new SnLambdaExpr(std::move(*$1), $3, nullptr, @1);
						delete $1;
					} |
				ParenPrefix OT_Arrow Paragraph {
						FoldBareIdentifierParams(*$1, @1);
						$$ = new SnLambdaExpr(std::move(*$1), nullptr, $3, @1);
						delete $1;
					} ;

//v0.13.1 D5: the plain-'<' generic as-target core for the array-suffix
//arms (`o as Box<string>[]`). Its ONE mid-rule depth increment is its
//own parse position — an inline twin inside the Expression arms would
//put three empty actions on one position, and the reduce/reduce
//resolution keeps only the first (the suffix arms would go dead).
AsGenericTarget:	IdentifierExpr '<' {
						parser.Scanner().IncGenericDepth();
					} TypeList '>'	{
						$$ = new SnGenericTypeExpr(
							new SnNameExpr($1, @1), $4, @1);
					} ;

//0.13.1a Task 3: expression-position generic spelling on a declared
//generic name — `Pair<int, string>.sig()` (static member call) and
//`Hold<int>(5)` (constructor call). The opener is the dedicated
//OT_TypeLt created by the scanner's spelling probe (an identifier
//followed by a balanced <...> whose terminator is exactly '.' or '(' —
//the C# "grammar ambiguities" resolution), so this nonterminal never
//competes with the plain-'<' comparison arms (same Expression-position
//discipline as the 0.10.8 anchored literal arm). Base + args create like
//the as-target arms; continuations live in MemberExpr ('.') and the
//Expression constructor arm ('(').
QualifiedGenericReceiver:
				IdentifierExpr OT_TypeLt TypeList '>'	{
					$$ = new SnGenericTypeExpr(
						new SnNameExpr($1, @1), $3, @1);
				} ;

MemberExpr:	Expression '.' InvokeExpr		{
					$$ = new SnMemberExpr($1, $3, @1);
				} |
				Expression '.' IdentifierExpr	{
					$$ = new SnMemberExpr($1, $3, @1);
				} |
				//0.13.1a Task 3: explicit type-argument spelling on a
				//static member call — `Pair<int, string>.sig()`. The
				//receiver creates through the same SnGenericTypeExpr the
				//as-target arms use, so the resolver's existing
				//Access(SnGenericTypeExpr&) arm resolves it.
				QualifiedGenericReceiver '.' InvokeExpr	{
					$$ = new SnMemberExpr($1, $3, @1);
				} |
				QualifiedGenericReceiver '.' IdentifierExpr	{
					$$ = new SnMemberExpr($1, $3, @1);
				} ;

LiteralExpr:	TT_Int		{ $$ = new SnLiteralExpr(*RnInt32::Instance(),	$1,	@1);	} |
					TT_Long		{ $$ = new SnLiteralExpr(*RnLong::Instance(),	$1,	@1);	} |
					TT_ULong	{ $$ = new SnLiteralExpr(*RnULong::Instance(),	$1,	@1);	} |
					TT_Float	{ $$ = new SnLiteralExpr(*RnFloat::Instance(),	$1,	@1);	} |
					TT_Double	{ $$ = new SnLiteralExpr(*RnDouble::Instance(),	$1,	@1);	} |
					TT_Char		{ $$ = new SnLiteralExpr(*RnChar::Instance(),	$1,	@1);	} |
					KT_True		{ $$ = new SnLiteralExpr(*RnBool::Instance(),	1,	@1);	} |
					KT_False	{ $$ = new SnLiteralExpr(*RnBool::Instance(),	0,	@1);	} |
					TT_String 	{ $$ = BuildStringExpr($1, @1, parser);	} ;

InvokeExpr:	TT_Identifier '(' ConcreteParamList ')' {
					$$ = new SnInvokeExpr($1, $3, @1);
				} ;

IdentifierExpr:	TT_Identifier	{ $$ = new SnIdentifierExpr($1, @1);			} |
					KT_Int   		{ $$ = new SnIdentifierExpr(NK_Int32, @1);	} |
					KT_Byte		{ $$ = new SnIdentifierExpr(NK_Byte, @1);	} | 
					KT_Ubyte	{ $$ = new SnIdentifierExpr(NK_UByte, @1);	} | 
					KT_Short	{ $$ = new SnIdentifierExpr(NK_Short, @1);	} | 
					KT_Ushort	{ $$ = new SnIdentifierExpr(NK_UShort, @1);	} | 
					KT_Uint		{ $$ = new SnIdentifierExpr(NK_UInt32, @1);	} | 
					KT_Long		{ $$ = new SnIdentifierExpr(NK_Long, @1);	} | 
					KT_ULong	{ $$ = new SnIdentifierExpr(NK_ULong, @1);	} | 
					KT_Float		{ $$ = new SnIdentifierExpr(NK_Float, @1);	} |
					KT_Double	{ $$ = new SnIdentifierExpr(NK_Double, @1);	} |
					KT_String		{ $$ = new SnIdentifierExpr(NK_String, @1);	} |
					KT_Bool		{ $$ = new SnIdentifierExpr(NK_Bool, @1);	} |
					KT_Char		{ $$ = new SnIdentifierExpr(NK_Char, @1);	} ;


//v0.12.7 Task 11: single unified arm. Scalar subscripts ride
//SubscriptItem: Expression, comma lists ride SubscriptItemList, slices
//ride SliceItem (resolver-side: any slice item -> view result, all
//scalar -> element). Behavior change vs. the former two arms: named/out
//subscript forms (which rode ConcreteParamList and were rejected by the
//resolver) are now parse errors — the diagnosis moved from resolve to
//parse time (ledger note in the accepted-conflict ledger above).
SubscriptExpr:	Expression '[' SubscriptItemList ']' {
					$$ = new SnSubscriptExpr($1, $3, @1);
				} ;

/*
Subscript item list (v0.12.7 Task 11): one or more comma-separated
items, each a scalar expression or a slice item. Deliberately no empty
production — unlike ConcreteParamList's empty arm for zero-argument
calls, `[]` is not a subscript, and keeping the epsilon out keeps the
shared bracket states unambiguous.
*/
SubscriptItemList:	SubscriptItem {
						$$ = new PtrList<SnExpression>();
						$$->push_back($1);
					} |
					SubscriptItemList ',' SubscriptItem {
						$1->push_back($3);
						$$ = $1;
					} ;

SubscriptItem:	Expression {
					$$ = $1;
				} |
				SliceItem {
					$$ = $1;
				} ;

/*
Slice item h:s:st with every part optional (`:`, `:s`, `h:`, `h:s`,
`::st`, ...). Absent parts stay null — presence itself carries the
slice's meaning; the emitter folds part presence into the mask word.
*/
SliceItem:	SliceHead ':' SliceStop SliceStepOpt {
				$$ = new SnSliceItemExpr($1, $3, $4, @$);
			} ;

SliceHead:	{
				$$ = nullptr;
			} |
			Expression {
				$$ = $1;
			} ;

SliceStop:	{
				$$ = nullptr;
			} |
			Expression {
				$$ = $1;
			} ;

SliceStepOpt:	{
					$$ = nullptr;
				} |
				':' SliceStep {
					$$ = $2;
				} ;

SliceStep:	{
				$$ = nullptr;
			} |
			Expression {
				$$ = $1;
			} ;

//0.9.0: bare `{...}` initializer for the value positions wired below
//(declaration/assignment RHS, subscript store, call argument, return,
//nested list element). Deliberately NOT an Expression arm: a statement-
//head `{` is the Paragraph block, and hanging the brace off Expression
//reintroduces the LALR conflict that restriction bought (see the
//Expression `[...]` arm). The node carries no type — the resolver's
//target-type inference (InferredTarget) binds it from context.
BracedInitExpr:	'{' InitEntries '}' {
					$$ = new SnInitListExpr(nullptr, *$2, false, @1);
					delete $2;
				} ;

//0.10.10: one comma-separated entry list shared by both forms (`[...]`
//and `{...}`). Each entry carries an optional key expression and a value
//expression; the target shape (array/List/Dict/struct/class) decides how
//the key is read — absent, a value slot, or a field name. The list may be
//empty (matches `[]` and `{}`).
InitEntries:	{
						$$ = new std::vector<nlang::InitEntry>();
					} |
					InitEntryList {
						$$ = $1;
					} ;

InitEntryList:	InitEntry {
						auto* pVec = new std::vector<nlang::InitEntry>();
						pVec->push_back(std::move(*$1));
						delete $1;
						$$ = pVec;
					} |
					InitEntryList ',' InitEntry {
						$1->push_back(std::move(*$3));
						delete $3;
						$$ = $1;
					} ;

InitEntry:	//Keyed entry. The key is any expression; the target shape
					//decides how it is read (Dict value slot / struct-class
					//field name). No ternary operator exists, so `Expression
					//':'` is unambiguous.
					Expression ':' Expression {
						auto* pE = new nlang::InitEntry();
						pE->pKey = $1;
						pE->pValue = $3;
						$$ = pE;
					} |
					//0.10.8 D4: keyed entry with a bare brace value. The
					//slot type (Dict value / struct-class field) supplies
					//the nested target (PropagateNestedInitListTargets);
					//array-typed fields keep the bracket form via the
					//existing array-form gate.
					Expression ':' BracedInitExpr {
						auto* pE = new nlang::InitEntry();
						pE->pKey = $1;
						pE->pValue = $3;
						$$ = pE;
					} |
					//Value-only entry (List/array elements, positional
					//struct fields). The resolver rejects a value-only
					//entry on Dict and class targets.
					Expression {
						auto* pE = new nlang::InitEntry();
						pE->pValue = $1;
						$$ = pE;
					} |
					//0.9.13 B6: nested bare brace entry.
					BracedInitExpr {
						auto* pE = new nlang::InitEntry();
						pE->pValue = $1;
						$$ = pE;
					} ;

ConcreteParamList:	ConcreteParamList ',' ConcreteParam {
							if ($1->empty())
							{
								parser.Log(CLL_Error, @2, "Invalid concrete param list, "
									"expected a param before ','.");
								delete $3;
							}
							else
								$1->push_back($3);
							$$ = $1;
						} |
						ConcreteParam {
							$$ = new PtrList<SnExpression>();
							$$->push_back($1);
						} |
						{
							//on empty
							$$ = new PtrList<SnExpression>();
								//v0.9.1: must stay BELOW FormalParamList in this file —
								//the two epsilons meet in the shared paren state; see
								//the note there (D6).
						} ;

//Phase 9c: a concrete parameter is either a positional argument (any
//expression) or a named argument `name = expr`. The `=` here is at the
//call-site parameter level — NLang has no assignment-as-expression, so
//inside `foo(...)` the form `Identifier '=' Expression` is unambiguous.
ConcreteParam:	TT_Identifier '=' BracedInitExpr {
						$$ = new SnNamedArgExpr($1, $3, @1);
					} |
					TT_Identifier '=' Expression {
						$$ = new SnNamedArgExpr($1, $3, @1);
					} |
					/* Phase 9e: out argument. v0.13.9 phase 4: the marker is
					 * retired; the arm still parses `out ident` so the resolver
					 * rejects it with the named retirement diagnostic instead
					 * of a bare syntax error. */
					KT_Out TT_Identifier {
						auto* pId = new SnIdentifierExpr($2, @2);
						$$ = new SnOutArgExpr(pId, @1);
					} |
					//0.9.0: bare brace argument — resolution defers until an
					//overload is chosen; the formal's type is the target.
					BracedInitExpr {
						$$ = $1;
					} |
					Expression {
						$$ = $1;
					} ;

%%

void yyerror(YYLTYPE *pLoc, nlang::ScriptParser &parser, yyscan_t, char *szFmt, ...)
{
    va_list ap;
    va_start(ap, szFmt);
	NLANG_ASSERT(pLoc);
    parser.Env().VLog(CLL_Error, *pLoc, szFmt, ap);
    va_end(ap);
}
