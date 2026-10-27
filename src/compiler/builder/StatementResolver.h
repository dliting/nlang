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
	//P3 review: a ctor default cannot reference `this` (explicitly or
	//through a bare field) — defaults evaluate at the call site, outside
	//the object under construction. No-op for non-ctor functions.
	void CheckCtorDefaultThisRefs(SnFunction &sn, SnFormalParam &param);
	//0.7.5 constant-fit arm of the check above: an explicit-only
	//default must be an in-range constant (assignment-side rule).
	void CheckDefaultConstantFit(SnFormalParam *param,
		SnField *pFormalType);
	//Explicit-value arm of AssignEnumMemberValues: resolve + fold the
	//member's value expression and apply the non-negative invariant.
	bool TryAssignExplicitEnumValue(SnEnumMember &member, SnEnumDecl &sn,
		int32_t &nextValue);
	void RejectIllegalEnumMethod(SnFunction &method);
	//0.9.0 semantic restriction: a variable declaration — local, formal
	//parameter, catch parameter, loop variable, class/struct field — may
	//not reuse the name of a type visible at that scope (see
	//ExprResolver::NamesVisibleType). Logs only; the caller proceeds.
	void RejectTypeNameVariable(const std::string &name, SyntaxNode &scope,
		const SnField &accessor, const ISourceLocation *pLoc);
	void RejectConstWithoutInit(SnLocalDeclStmt &sn);
	SnAssignStmt *BuildLocalInitAssign(SnLocalDeclStmt &sn,
		SnLocalDeclStmt::LocalDecl &decl);
	//0.9.0: build `name = new Type(args)` for a `Type name(args)`
	//declarator. Null = rejected (generic instantiation).
	SnAssignStmt *BuildCtorInitAssign(SnLocalDeclStmt &sn,
		SnLocalDeclStmt::LocalDecl &decl);
	//0.9.0: consume a `Type name(args)` declarator — build the
	//decomposition assign (BuildCtorInitAssign), insert it at iInsert
	//and resolve it; the emptied ctor-arg container is deleted either
	//way so the consumed slots never reach codegen.
	void SynthesizeCtorInitDeclarator(SnLocalDeclStmt &sn,
		SnLocalDeclStmt::LocalDecl &decl, SnParagraph &paragraph,
		NodeIterator iInsert);
	//0.9.0: consume a sized declarator `Type name[n]` (+ optional
	//`[...]` partial init) — build the creation assign plus one
	//subscript assign per entry, insert at iInsert and resolve. The
	//payload slots are dropped on both success and rejection.
	void SynthesizeSizedArrayDeclarator(SnLocalDeclStmt &sn,
		SnLocalDeclStmt::LocalDecl &decl, SnParagraph &paragraph,
		NodeIterator iInsert, SnField *pSizedElemField);
	//0.9.0: build the decomposition of a sized declarator
	//`Type name[n]` (+ optional `[...]` partial init): one creation
	//assign plus one subscript-store statement per initializer entry.
	//Empty = rejected (nothing detached); non-empty = the payload slots
	//of the declarator were consumed and must not be re-read.
	std::vector<SnStatement *> BuildSizedArrayDeclAssigns(
		SnLocalDeclStmt &sn, SnLocalDeclStmt::LocalDecl &decl,
		SnField *pSizedElemField);
	//0.9.0/0.10.7: sized-declarator typing for `T name[n]` — rejects
	//the jagged `int[] a[3]` spelling and returns the captured element
	//field for the creation synthesis (null = no sized declarator, or
	//the declaration was rejected). 0.10.7 D2: the statement type is
	//never re-bound; per-declarator array-ness rides
	//LocalDecl::bArrayType instead.
	SnField *BindSizedArrayDeclType(SnLocalDeclStmt &sn);
	void RegisterLocalDeclarators(SnLocalDeclStmt &sn,
		SnParagraph &paragraph, SnField *pTypeField, bool bIsArray,
		SnField *pSizedElemField);
	//0.10.7 D2: create and register one declarator's local — a size
	//payload types only its own declarator (the interned array token
	//is that declarator's field); a head-array statement types every
	//declarator. The array flag feeds bare [...] inference and member
	//receivers; LocalDecl::bArrayType carries the slot kind to codegen
	//past the consumed payload.
	SnLocalVar *CreateDeclaratorLocal(SnLocalDeclStmt::LocalDecl &decl,
		const ISourceLocation &loc, SnParagraph &paragraph,
		SnField *pTypeField, bool bIsArray, SnField *pSizedArrayField);
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
	//0.9.0: the subscript-store statement has no SnSubscriptExpr node to
	//carry the creation annotation, so its TYPE base (`int[3] = v`) is
	//recognized here. True = rejected (the caller stops).
	bool RejectCreationStoreTarget(SnExpression &base,
		const ISourceLocation *pLoc);
	//0.8.4/0.9.0: the compound-assign subscript LHS gates (string base,
	//creation twin via the NewArrayElem annotation). True = rejected.
	bool RejectCompoundSubscriptTarget(SnExpression &left,
		const ISourceLocation *pLoc);
	bool FinishInitListAssign(SnAssignStmt &sn);
	void FinishAssignTypeCheck(SnAssignStmt &sn);
	//0.9.10: the shift branch of Access(SnCompoundAssignStmt&) — the
	//integer-operand gate plus the shared count gate (AdmitShiftCount)
	//with the LHS's declared row as the carrier. Definition in
	//StatementResolverCompound.cpp.
	void FinishCompoundShift(SnCompoundAssignStmt &sn, SnField &targetType);
	//Shared tail of Access(SnReturnStmt&): the return-position func-ref
	//binding and the cast fixup against the declared return type. False
	//when the binding failed (diagnostic logged).
	bool FinishReturnExpr(SnFunction &outerFunc, SnReturnStmt &sn);
	//0.9.11 D1: constructor declaration-shape predicates. IsCtorNameShape
	//= a member function of a class or struct whose name equals the type
	//name, regardless of flags — the declaration contract must also
	//adjudicate the static/virtual/const spellings the no-return
	//productions admit. IsConstructorFunction = the same shape restricted
	//to instance members (ctor-slot semantics; a same-name static lives
	//in the static namespace, 0.9.10 D2.4). Definitions in
	//StatementResolverDeclCtor.cpp.
	static bool IsCtorNameShape(SnFunction &sn);
	static bool IsConstructorFunction(SnFunction &sn);
	//0.9.11 D1: the constructor declaration contract (no return type, no
	//static/virtual/const flags) plus the missing-return-type diagnostic
	//for plain class members. Logs all diagnostics; never mutates the AST.
	//Definition in StatementResolverDeclCtor.cpp.
	void CheckCtorDeclarationContract(SnFunction &sn);
	//0.9.11 D2: the struct member contract — a struct declares fields and
	//constructors only, and a struct constructor must not carry static/
	//virtual/const. Logs all diagnostics; never mutates the AST. Definition
	//in StatementResolverDeclCtor.cpp.
	void CheckStructCtorContract(SnStructDecl &owner, SnFunction &sn);
	//0.9.11 D9: inject one `this.<field> = <cloned literal>;` per field
	//initializer into one constructor, at the fixed anchor after a leading
	//super() call (or at body start). A member (not a free helper) because
	//Node::InsertChild is protected and reached through the SyntaxNode
	//friendship. Definition in ClassFieldInitializerInjection.cpp.
	void InjectIntoCtor(SnFunction &ctor,
		const std::vector<SnClassField *> &initFields);
	//0.9.11 D9: collect the class's user constructors (instance members
	//named after the class); false = a generated constructor marks an
	//already-injected class. Needs the private IsConstructorFunction.
	bool CollectUserCtors(SnClassDecl &sn,
		std::vector<SnFunction *> &ctors);
	//0.9.11 D9: phase-1 check for one constructor (native/bodyless
	//rejection, super-first rule with initializers present); emits the
	//diagnostic and returns false on rejection.
	bool ValidateCtorForFieldInit(SnFunction &ctor);
	void PropagateInitListTarget(SnAssignStmt &sn);
	//K3 task 4: publish the declared LHS type for the bare ctor-invoke
	//arm when the RHS is a bare invoke (TryBindChannelCtorInvoke).
	void PropagateCtorInvokeTarget(SnAssignStmt &sn);
	bool TryBindSubscriptStoreFuncRef(SnSubscriptAssignStmt &sn);
	bool RejectStringBase(SnExpression &base, const ISourceLocation *pLoc);
	void ApplyArrayElementStoreCast(SnSubscriptAssignStmt &sn);
	void ApplyContainerStoreCasts(SnSubscriptAssignStmt &sn);
	//Void-store diagnostic of Access(SnSubscriptAssignStmt&). Logs only —
	//the statement still finishes.
	void LogVoidSubscriptStoreValue(SnSubscriptAssignStmt &sn);
	void CheckForeachSource(SnForeachStmt &sn);
	void MatchForeachElemType(SnForeachStmt &sn, SnField *pVarField);
	void ResolveForInitDecl(SnForStmt &sn);
	SnAssignStmt *BuildForInitAssign(SnLocalDeclStmt &decl,
		SnLocalDeclStmt::LocalDecl &d, SnParagraph *pParagraph);
	static bool ExtractEnumMemberLabel(SnExpression& label,
		SwitchLabelKey& key);
	static bool ExtractLiteralLabel(SnLiteralExpr& lit, SwitchLabelKey& key);
	void ResolveSuperCallArgs(SnSuperCallStmt &sn);
	//0.9.11 T6 (D10): the parent ctor a super(...) call binds to — the
	//overload match over the parent's same-name non-static members, with
	//the builtin-parent intrinsic arity path kept. Null = builtin parent
	//or a logged rejection; a bare super() on a parent without ctors is
	//the legal no-op shape and also answers null.
	SnFunction *SelectSuperCtor(SnSuperCallStmt &sn, SnClassDecl *pParent);

public:

	//Pre-pass entry (forward-reference fix): resolve one class's
	//extends/implements before any function body is visited, so an upcast in
	//an earlier-merged consumer TU still sees the inheritance chain.
	void ResolveClassBaseNow(SnClassDecl &sn) { ResolveClassBases(sn); }

	//0.9.11 D9 pre-pass entry: apply one class's field initializers —
	//validate every constructor (native/bodyless rejection, super-position
	//rule), then inject fresh `this.<field> = <clone>;` statements or
	//synthesize the public zero-arg constructor. Runs before the document-
	//order traversal so a constructor call site written before the class
	//declaration still sees the synthesized member. Definition in
	//ClassFieldInitializerInjection.cpp.
	void InjectClassFieldInitializers(SnClassDecl &sn);

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
	//v0.9.1: nesting depth of breakable constructs (loops + switch,
	//validates break) and of loops only (validates continue). Zero at
	//function-body level — a top-level break/continue reaches codegen's
	//empty loop stack, whose guard makes it a silent no-op in Release
	//(assertion in Debug); reject here instead.
	int m_breakableDepth = 0;
	int m_loopDepth = 0;
	//v0.9.1 scope guard for the two depth counters above (top-level
	//break/continue rejection). A loop bumps both depths (break +
	//continue are valid); a switch bumps only the breakable depth
	//(continue passes through to the enclosing loop). Nested here so
	//every per-concern TU of this class shares one guard; holds across
	//every exit path of the statement walk.
	struct FlowDepthGuard
	{
		FlowDepthGuard(int& breakable, int& loop)
			: m_breakable(breakable), m_pLoop(&loop)
		{ m_breakable += 1; *m_pLoop += 1; }
		explicit FlowDepthGuard(int& breakable)
			: m_breakable(breakable), m_pLoop(nullptr)
		{ m_breakable += 1; }
		~FlowDepthGuard()
		{
			m_breakable -= 1;
			if (m_pLoop)
				*m_pLoop -= 1;
		}
		FlowDepthGuard(const FlowDepthGuard&) = delete;
		FlowDepthGuard& operator=(const FlowDepthGuard&) = delete;
	private:
		int& m_breakable;
		int* m_pLoop;
	};
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

	//Pre-pass (0.9.11 D9): inject class field-initializer assignments and
	//synthesize zero-arg constructors before the document-order traversal,
	//so a constructor call site written before the class declaration still
	//sees the synthesized member (same forward-reference discipline as
	//PreResolveClassBases; idempotent on already-injected classes).
	static void PreInjectClassFieldInitializers(Node& node,
		StatementResolveAccessor& accessor);

	void Resolve(SnNamespace &root);
private:
	StatementResolveAccessor m_Accessor;
};

} //namespace nlang
