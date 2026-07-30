/*-----------------------------------------------------------------------------
ncomp/intf/SyntaxNodeConsts.h
This file define the common constants for syntax nodes.
-----------------------------------------------------------------------------*/

#pragma once
#include "TypeDef.h"
#include <nlang/runtime/NodeConsts.h>

namespace nlang
{

//Bits combination of syntax node flags.
typedef NodeBits SyntaxNodeBits;

#define COMPILE_ONLY_NODE_DECL(MACRO_IMPL)							\
	MACRO_IMPL(Using)					/* using directive */				\
	MACRO_IMPL(LiteralExpr)				/* literal constant expression */	\
	MACRO_IMPL(IdentifierExpr)			/* identifier expression  */		\
	MACRO_IMPL(InvokeExpr)				/* invoke expression  */			\
	MACRO_IMPL(NameExpr)				/* name expression  */				\
	MACRO_IMPL(MemberExpr)				/* member access expression  */		\
	MACRO_IMPL(CastExpr)				/* type cast expression  */			\
	MACRO_IMPL(BinaryExpr)				/* binary/unary operator expression */\
	MACRO_IMPL(ReturnStmt)				/* return statement */				\
	MACRO_IMPL(InvokeStmt)				/* invoke statement */				\
	MACRO_IMPL(LocalDeclStmt)			/* local variable declaration */		\
	MACRO_IMPL(AssignStmt)				/* assignment statement */			\
	MACRO_IMPL(IfStmt)					/* if/else statement */				\
	MACRO_IMPL(WhileStmt)				/* while loop statement */			\
	MACRO_IMPL(DoStmt)					/* do-while loop statement */		\
	MACRO_IMPL(ForStmt)				/* for loop statement */				\
	MACRO_IMPL(SwitchStmt)			/* switch statement */					\
	MACRO_IMPL(CaseClause)			/* case clause */						\
	MACRO_IMPL(BreakStmt)				/* break statement */					\
	MACRO_IMPL(ContinueStmt)			/* continue statement */				\
	MACRO_IMPL(Paragraph)				/* paragraph */						

//#define COMPILE_ONLY_NODE_TYPE_DECL(MACRO_IMPL)							\
//	MACRO_IMPL(LiteralExpr)				/* literal constant expression */	\
//	MACRO_IMPL(LocalExpr)				/* local declaration expression */	\
//	MACRO_IMPL(InvokeExpr)				/* invoke expression  */			\
//	MACRO_IMPL(CastExpr)				/* type cast */						\
//	MACRO_IMPL(ThisExpr)				/* "this" expression */				\
//	MACRO_IMPL(CondClause)				/* condition clause */				\
//	MACRO_IMPL(LocalStamt)				/* local declaration statement */	\
//	MACRO_IMPL(ReturnStmt)				/* return statement */				\
//	MACRO_IMPL(InvokeStmt)				/* invoke statement */				\
//	MACRO_IMPL(AssignStmt)				/* assign statement */				\
//	MACRO_IMPL(IfStmt)					/* if statement */					\
//	MACRO_IMPL(WhileStmt)				/* while statement */				\
//	MACRO_IMPL(DoStmt)					/* do statement */					\
//	MACRO_IMPL(ForStmt)					/* for statement */					\
//	MACRO_IMPL(SwitchStmt)				/* switch statement */				\
//	MACRO_IMPL(BreakStmt)				/* break statement */				\
//	MACRO_IMPL(ContinueStmt) 			/* continue statement */			\
//	MACRO_IMPL(Using)					/* using directive */				\
//	MACRO_IMPL(Identifier)				/* identifier */			

#define SYNTAX_NODE_DECL(MACRO_IMPL) \
	RUNTIME_NODE_DECL(MACRO_IMPL) \
	COMPILE_ONLY_NODE_DECL(MACRO_IMPL)

//Enumerations of extra syntax node types.
enum CompNodeKind: NodeKind
{
	NK_BEFOR = NK_RT_END,
#define MACRO_IMPL(T) NK_##T,
	COMPILE_ONLY_NODE_DECL(MACRO_IMPL)	
	NK_CP_END
#undef MACRO_IMPL
};

static_assert(NK_CP_END < NODE_KIND_LIMIT,
	"Syntax node kind definition error.");

//Enumerations of extra syntax node flags.
#define COMPILE_ONLY_NODE_FLAG_DECL(MACRO_IMPL)								\
	MACRO_IMPL(Expression,	0, "expression")								\
	MACRO_IMPL(Statement,	1, "statement")									\
	MACRO_IMPL(Imported,	2, "imported")									\
	MACRO_IMPL(Resolving,	3, "resolving")									\
	MACRO_IMPL(Resolved,	4, "resolved")									\
	MACRO_IMPL(Generated,	5, "code generated")							\
	MACRO_IMPL(Override,	6, "override function")							\
	MACRO_IMPL(Invalid,		7, "invalid node")

enum CompNodeFlag: NodeBits
{
#define MACRO_IMPL(name, offset, desc) NF_##name = (NF_RT_END - 1) << (offset + 1),
	COMPILE_ONLY_NODE_FLAG_DECL(MACRO_IMPL)
#undef MACRO_IMPL
	NF_CP_END
};

//Imported syntax node which needn't to be parsed.
const NodeBits NF_IMPORTED_SYMBOL = NF_Imported | NF_Resolved;

static_assert(NF_CP_END < NODE_FLAG_LIMIT, "Syntax node flag definition error.");

//RnField searching flags.
enum FieldSearchFlag 
{
	FS_SearchInParent		= 0x01,
	FS_PrivateAccess		= 0x02,
	FS_ProtectedAccess		= 0x04,
	FS_DontSearchInSuper	= 0x08,
	//Search in using namespaces
	FS_SearchInUsing		= 0x10	
};

//Bits combination of field searching flags.
typedef uint8 FieldSearchFlagSet;

static const FieldSearchFlagSet FSS_PublicMember = 0;
static const FieldSearchFlagSet FSS_Everywhere = 
	FS_PrivateAccess | FS_SearchInParent | FS_SearchInUsing;

}