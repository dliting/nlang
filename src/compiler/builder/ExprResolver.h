#pragma once
#include "SnExpressions.h"
#include "SyntaxNodeVisitor.h"
#include "BuiltinNames.h"
#include <nlang/runtime/BuiltinGenericNames.h>
#include <nlang/runtime/Flagable.h>
#include <functional>
#include <vector>

namespace nlang
{

enum FindFuncResult
{
	//The function name is not found.
	FFR_FuncNameNotFound,
	//The function is found but the parameter types are not compatible.
	FFR_Incompatible,
	//The function is found and the parameter types are compatible but not 
	//exactly matched, automatic type cast is needed.
	FFR_ApproximateMatch,
	//The function is found and exactly matched, no type cast is needed.
	FFR_ExactMatch
};

enum ExprResolveFlag : uint8
{
	ERF_None = 0,
	//Don't search in using namespaces.
	ERF_IgnoreUsings = 1,
	//Don't search in remote ancestors.
	ERF_SearchInParentOnly = 2,
};

typedef uint8 ExprResolveFlagSet;

//0.9.10 D2.4: call-form namespaces. A member call through a type name
//only reaches static members; a member call through a value only reaches
//instance members; a bare call is namespace-level.
enum class InvokeReceiverForm
{
	Bare,
	TypeName,
	Value
};
InvokeReceiverForm ClassifyInvokeReceiver(const SnInvokeExpr &invoke);

//0.9.10 D2.4: a receiver expression that names a type (class / interface /
//enum) — that form selects the static namespace. Shared by the invoke and
//member-value channels (definition in ExprResolverInvokeMatch.cpp).
bool ReceiverNamesType(SnExpression *pOuter);

//0.9.10 D2.4: first same-name member of ONE namespace (static vs instance)
//along the receiver's chain — the rebind and cross-namespace diagnostic
//search for the receiver-namespace split (definition in
//ExprResolverInvokeMatch.cpp). No access check: the rebind caller gates
//the result with AllowAccess, the diagnostic callers only pick a message.
SnFunction *FindMethodInNamespace(SyntaxNode *pContext,
	const std::string &name, bool bStatic);

//Array redesign B: pure SHAPE test — identifier, or member access whose
//inner name is an identifier (the lvalue shapes whose Field() binding IS
//the variable/field itself). Array-ness is the IsArrayValued() property
//on SnExpression, not part of this check. Shared with StatementResolver.
bool IsPlainLvalueShape(const SnExpression& expr);

//Array redesign B: depth of the array-type chain (int[] = 1, int[][] = 2).
//Depth >= 2 is a jagged declaration form — the VM has no multi-dimensional
//layout and the EvalDataType masquerade degrades it twice silently — so
//declaration sites reject it (fields/params/return types in
//ResolveDataTypes; local declarations and the for-var/foreach-var
//handlers in StatementResolver — the latter resolve via the visitor where
//an SnArrayTypeExpr never resolves, so their gates are shape-based).
//Definition lives in ExprResolverTypes.cpp.
int ArrayTypeDepth(const SnFieldExpr* pType);

/*
Phase 13: bind a pending bare-function reference (see IsUnboundFuncRef)
to the expected func<...> type of the injection site. Validates the
exact signature, then rebinds the identifier: EvalDataType becomes the
func declaration — codegen detects the bound state structurally (a
value-position identifier whose Field() is an SnFunction) and emits
OP_MakeFunc. The 24 node-flag bits are fully allocated, so pending and
bound states are structural, not flag-based.
\return false after logging a named diagnostic (non-func expected type,
method without receiver, default parameters, imported stub, signature
mismatch) — the caller should stop resolving the statement.
Shared by ExprResolver (argument positions) and StatementResolver
(assignment / return / subscript-store positions).
*/
bool BindFuncRefToExpected(BuildEnvironment &env, SnIdentifierExpr &idExpr,
	SnField *pExpected);

/*
Phase 13 Step 2: bind a pending bound-method reference (receiver.name in
a value position, see IsUnboundMemberFuncRef) to the expected func<...>
type of the injection site. Selects the handle form exactly like the
direct-call codegen does (virtual / interface-declared methods dispatch
by name at runtime, everything else binds a static function index), with
named rejects for enum receivers, native methods (including native
overrides found by a subclass scan for by-name bindings) and out-carrying
signatures on by-name bindings. On success the member and its inner
identifier carry the func declaration — codegen detects the bound state
structurally, like the bare-name form.
\return false after logging a named diagnostic — the caller should stop
resolving the statement.
*/
bool BindMemberFuncRefToExpected(BuildEnvironment &env,
	SnMemberExpr &snMember, SnField *pExpected);

/*
Phase 13: loose pending predicate — true while a bare function
reference carries a non-func EvalDataType (its function's return type).
Consumed ONLY by the ModuleBuilder TU-end sweep, which reports any
reference that never met an expected type; the bind sites (argument /
assignment / return / subscript / init-list positions) use the strict
IsUnboundFuncRef below (review round-1 F1 split — see the .cpp comments
for why a func-typed EvalDataType alone does not prove a binding).
*/
bool IsPendingFuncRef(SyntaxNode &expr);
//Phase 13 (review round-1 F1): strict bind-site predicate — true unless
//the reference's own signature satisfies the func type it carries (a
//func-typed RETURN type leaking through ResolveFieldExprAs is NOT a
//binding). All bind sites use this form; IsPendingFuncRef above is the
//loose form kept for the end-of-build sweep only.
bool IsUnboundFuncRef(SyntaxNode &expr);
//Phase 13 Step 2: strict bind-site predicate for receiver-bound method
//references (receiver.name in a value position whose inner name resolved
//to a class/interface/enum method). Same F1 discipline as
//IsUnboundFuncRef; the bind sites pair it with
//BindMemberFuncRefToExpected above.
bool IsUnboundMemberFuncRef(SyntaxNode &expr);
//v2.2: a function owned by a namespace — or by nothing at all, for a
//detached imported stub that never joined the consumer root — is a
//module-qualified free function (lib.seven): referenced without a
//receiver, so wording and codegen split on this form everywhere the
//member func-reference channel branches (the VM side keeps its own
//static copy in EmitExprMember.cpp: subsystem boundary).
inline bool IsModuleOwnedFunction(const SnField &func)
{
	auto *pOwner = func.Parent();
	return pOwner == nullptr || pOwner->Kind() == NK_Namespace;
}
//Phase 13 (review round-1 F7): type arguments of a generic instantiation
//(List<T> → {T}, Dict<K,V> → {K,V}; empty otherwise). Shared with
//StatementResolver's Dict subscript-store bind site.
std::vector<SnField*> GetGenericTypeArgs(SnClassDecl* pClass);
//Phase 13: out-flag side table of a func instantiation (parallel to
//GetGenericTypeArgs; map access semantics, default-empty on miss).
//Definition in ExprResolverGenerics.cpp.
const std::vector<uint8>& GetGenericOutFlags(SnClassDecl* pClass);

//Reference-only use below; definition in ScriptLocation.h.
class ScriptLocation;

//Helpers shared across the ExprResolver*.cpp TUs (2026-09-25/26 splits).
//Definitions live across those TUs; builder-internal.
bool HasNamedArgument(SnInvokeExpr& invoke);
size_t ArgCountOf(SnInvokeExpr& invoke);
bool HasOutArgument(SnInvokeExpr& invoke);
std::string JoinDots(const std::vector<std::string>& segs);
std::vector<std::string> OuterIdentifierChain(
	const SnMemberExpr& snMember);
const char* StdLibKindName(uint8_t rtk);
bool IsGenericClassDecl(SnClassDecl* pClass);
SnClassDecl* GetGenericClassDecl(const std::string& baseName,
	const std::vector<SnField*>& typeArgs, const std::vector<uint8>& outFlags,
	const ISourceLocation* pLoc);
//Location pass-through for on-demand minted decls (built-in classes,
//generic instantiations): the caller's location when one exists, the
//fallback otherwise. Polymorphic locations must flow through
//ISourceLocation — punning to ScriptLocation reads past the end of the
//smaller imported-module location object. Definition in
//ExprResolverBuiltinClasses.cpp.
const ISourceLocation& DeclLocation(const ISourceLocation* pLoc,
	ScriptLocation& fallback);
bool IsFuncTypeDecl(SnField *pType);
//0.9.0: the bare `{...}` node of an expression whose target type is not
//set yet — the shared entry point of target-type inference (deferred
//call arguments, nested entries, statement-side propagation). Null when
//the expression is not a brace list awaiting a target. Definition in
//ExprResolverInitList.cpp.
SnInitListExpr *UntypedInitListOf(SnExpression &expr);
//0.9.0: the target shapes a collection initializer lowers to — array
//tokens, structs, List/Dict instantiations, non-builtin user classes.
//Scalars / string / func types / builtin classes / interfaces / enums
//are false (the emitter would leave the target slot unstaged). Single
//authority shared by the init-list target gate and the bare-brace
//argument admission (ExprResolverBinding.cpp). Definition in
//ExprResolverInitList.cpp.
bool IsInitListTargetShape(SnField *pType);
bool IsBarePoolScope(const SyntaxNode &scope);
//0.9.x shadow guard: the binding kinds a TYPE-position name (SnNameExpr)
//may take — type declarations (IsTypeField: the NF_Type|NF_Field family)
//or a namespace (using heads; namespaces carry no NF_Type). Everything
//else (locals, params, class fields, functions, enum members) is a value
//binding that must not hijack a type position. Definition in
//ExprResolverMemberFields.cpp.
bool IsTypeOrNamespaceBinding(const SnField *pField);
//2026-09-26 second split: builtin-class minting and the func signature
//match are consumed from the Values/New and Cast TUs respectively.
SnClassDecl* GetBuiltinClassDecl(const std::string& name,
	const ISourceLocation* pLoc);
bool FuncRefMatchesDecl(const SnFunction &func, SnClassDecl *pFuncDecl);

//String built-in method table entry (defined in vm/StdLib.h); the member
//phase declarations below hand around pointers into it.
struct StringMethodEntry;

//Library declaration symbol (nlang_langservice); the stdlib-call
//resolution reads signatures (param/return types, arity) from it.
namespace langservice { struct SymbolInfo; }

//Scope kinds of the invoke candidate scan (defined in SnMisc.h); forward
//declarations suffice for the reference parameters below.
class SnFunctionParentField;
class SnEnumDecl;


/*
The syntax node accessor for expression resolving.
This class should be used with \a ExprResolver.
*/
class ExprResolveAccessor : public ChildNodeListController, 
	public Flagable<uint8>
{
	friend class ExprResolver;
public:
	void Access(SnLiteralExpr &);

	void Access(SnNameExpr &);

	void Access(SnArrayTypeExpr &);

	//Phase 8e-3: resolve `List<T>` (and future built-in generics).
	//Mints or fetches a cached synthetic SnClassDecl per (base, typeArgs)
	//tuple to preserve pointer identity for CalcTypeDistance.
	void Access(SnGenericTypeExpr &);

	//Phase 4b: resolve a qualified type reference "ns.Type" in a type
	//position (library-defined class/struct/enum/interface).
	void Access(SnQualifiedTypeExpr &);

	void Access(SnIdentifierExpr &);

	void Access(SnInvokeExpr &);

	//Phase 9c: resolve a named argument `name = expr` at a call site.
	//Resolves the inner expression and propagates its EvalDataType so
	//the parent SnInvokeExpr sees the value's type for overload matching.
	//The name itself is consumed by TryBindArgs when matching formals.
	void Access(SnNamedArgExpr &);

	//Phase 9e: resolve an out argument `out ident` at a call site.
	//Validates the identifier binds to a caller-frame slot (local var or
	//formal param — both register as NK_FormalParam-kind fields), then
	//propagates the inner's EvalDataType. Consumed by TryBindArgs, which
	//requires the matched formal to carry NF_Out.
	void Access(SnOutArgExpr &);

	void Access(SnMemberExpr &);

	void Access(SnCastExpr &);

	//Phase 8e-1.5: resolve `expr as T` cast expression.
	void Access(SnAsExpr &);

	//Resolve binary/unary operator expression.
	void Access(SnBinaryExpr &);

	//Resolve new expression.
	void Access(SnNewExpr &);
	//Resolve new array expression.
	void Access(SnNewArrayExpr &);

	//Phase 8e-6: resolve collection initializer (bare [..] or new T{..}).
	//For bare form, uses InferredTarget set by AssignStmt resolver.
	void Access(SnInitListExpr &);

	//Resolve subscript expression.
	void Access(SnSubscriptExpr &);

	//Resolve this expression.
	void Access(SnThisExpr &);

	//Resolve class declaration.
	void Access(SnClassDecl &);

	//Resolve interface declaration (resolve method signatures only).
	void Access(SnInterfaceDecl &);

	//Resolve class field.
	void Access(SnClassField &);

	//Default action.
	void Access(SyntaxNode &)
	{
		assert(false && "invalid expression.");
	}
private:
	explicit ExprResolveAccessor(BuildEnvironment &env) : m_Env(env),
		m_pVisitor(nullptr), m_pContext(nullptr), m_pAccessor(nullptr)
	{
	}

	//Resolve every expression in the list, skipping already-resolved ones
	//and anything in pDeferredArgs — the bare brace arguments that wait
	//for the callee's formal type (0.9.0). Null skips nothing.
	bool ResolveExpressionList(SnExpressionList &exprs,
		const std::vector<SnExpression*> *pDeferredArgs = nullptr);

	//0.9.0: argument prologue of Access(SnInvokeExpr&) — collects the
	//deferred bare brace arguments into the out vector, resolves every
	//other argument, and validates the call syntax. False = the call is
	//consumed as failed (the sweep already reported the brace args).
	bool ResolveInvokeArguments(SnInvokeExpr &snInvoke,
		std::vector<SnExpression*> &deferredBraceArgs);

	//Chosen-callee admission gates of Access(SnInvokeExpr&) — the Phase
	//9e out-on-dispatched veto and the Phase 12 bare-method reject.
	//False = rejected (diagnostic logged, brace-arg sweep run).
	bool AdmitChosenCallee(SnInvokeExpr &snInvoke, SnFunction *pCallee,
		const std::vector<FormalBinding> &bindings,
		const std::vector<SnExpression*> &deferredBraceArgs);

	//0.7.3 B: token binder at the binding tails. The array-valued
	//property is derived from the interned SnArrayTypeToken carried in
	//EvalDataType; declaration-channel shapes already carry the token,
	//the arms mint it for fresh allocations and container element
	//shapes. An accessor member (not a free function) because writing
	//EvalDataType needs the SnExpression friendship.
	void BindArrayTypeToken(SnExpression &expr);

	//2026-09-26 decomposition of Access(SnAsExpr&): the array-operand
	//guard and the cast-kind adjudication (func→string arm + the two
	//reject families). Each returns true when the expression is
	//consumed (rejected with a named diagnostic, or resolved as the
	//func→string rendering); false falls through to the binding tail.
	bool RejectAsArrayOperand(SnAsExpr &sn);
	bool ResolveAsCastKind(SnAsExpr &sn, SnField *pSrcType,
		SnField *pTgtType, TypeCastKind kind);

	/*
	Module import visibility (spec §6.2 rule 5): resolve a dotted call
	chain (utils.helper.help(), lib.add(2,3)) against the module registry
	when the leftmost identifier resolves as nothing else (no local, field
	or type — m12 excludes function candidates so a cross-directory
	function name never shadows a module path's first segment). v1 shape:
	the chain is exactly module path + function name; qualified VALUE
	access is out of scope. Returns false when the chain does not match
	the module fallback's contract — the normal resolution path keeps the
	expression. Every matching branch consumes the member (resolved or
	diagnosed).
	*/
	bool TryResolveModuleQualified(SnMemberExpr &snMember);

	//Non-function field probe for the priority test above: true when the
	//name resolves as a local / field / type in the current context.
	bool ProbeNonFunctionField(const std::string &name);

	//spec §6.2 last line: after a failed class-path resolution, note the
	//module path sharing the name (silent when none does).
	void MaybeLogModuleHint(const std::string &name);

	//0.9.10: nearest enclosing function of the current resolution context —
	//the static-method gates (this / implicit this) inspect its NF_Static
	//flag. Null at namespace/module scope.
	SnFunction *EnclosingFunction() const;

	//0.9.10: a bare name that binds to a class field is the implicit
	//this.field form — rejected inside a static method. True = rejected
	//with its diagnostic.
	bool RejectImplicitThisInStatic(SnIdentifierExpr &idExpr, SnField *pField);

	//0.9.10 D2.4: the member VALUE channel (receiver.name with no invoke)
	//shares the invoke path's receiver-namespace split. The plain identifier
	//search is namespace-blind — it walks the receiver's whole chain, so an
	//own static shadows an inherited instance method and a static-only name
	//binds as a receiver-bearing handle (silent garbage at runtime). Called
	//from Access(SnIdentifierExpr) with the receiver context active; rebinds
	//rpField in place when the receiver form's own namespace holds the
	//same-name member. False = rejected with its diagnostic.
	bool ApplyReceiverNamespaceFilter(SnIdentifierExpr &idExpr,
		SnField *&rpField);

	//0.9.10 D2.4: type-name receiver + method value position — the
	//always-reject arm of ApplyReceiverNamespaceFilter. False = rejected.
	bool RejectTypeNameMethodValue(SnIdentifierExpr &idExpr);

	//0.9.10 D3.4: an interface constant is referenced only through the
	//qualified `I.NAME` member form. True = the bare binding was
	//rejected with the qualification hint.
	bool RejectBareInterfaceConstant(SnIdentifierExpr &idExpr,
		SnField &field);

	void ResolveFieldExprAs(SnFieldExpr &expr, SnField *pField);

	/*
	Member-expression resolution phases (the Access(SnMemberExpr&)
	decomposition): the accessor is a thin orchestrator running these in
	order; each Try* phase returns true when it consumed the expression
	(resolved or diagnosed — the builtin by-name families log their own
	errors), false to fall through to the next phase. pSavedContext
	threads the caller's scope: every consuming path restores m_pContext
	from it before returning.
	*/
	bool TryResolveMemberHead(SnMemberExpr &snMember,
		SnExpression *pOuterExpr);
	void SwitchContextToReceiver(SnMemberExpr &snMember);
	bool RejectVoidReceiverMemberAccess(SnMemberExpr &snMember,
		SyntaxNode *pSavedContext);
	bool RejectArrayReceiverMethodCall(SnMemberExpr &snMember,
		SnExpression *pOuterExpr, SyntaxNode *pSavedContext);
	bool TryResolveStringBuiltinMethod(SnMemberExpr &snMember,
		SnFieldExpr *pInnerExpr, SyntaxNode *pSavedContext);
	bool BindIntrinsicStringResult(SnMemberExpr &snMember,
		SnFieldExpr *pInnerExpr, NodeKind resultKind,
		SyntaxNode *pSavedContext);
	bool ResolveStringEqualsMethod(SnMemberExpr &snMember,
		SnFieldExpr *pInnerExpr, SnInvokeExpr &invoke,
		SyntaxNode *pSavedContext);
	bool TryResolveTableStringMethod(SnMemberExpr &snMember,
		SnFieldExpr *pInnerExpr, SnInvokeExpr &invoke,
		const std::string &name, SyntaxNode *pSavedContext);
	bool CheckTableStringMethodCall(SnInvokeExpr &invoke,
		const StringMethodEntry *pMethod);
	void CheckTableStringArgKinds(SnInvokeExpr &invoke,
		const StringMethodEntry *pMethod);
	void BindTableStringMethodResult(SnMemberExpr &snMember,
		SnFieldExpr *pInnerExpr, SnInvokeExpr &invoke,
		const StringMethodEntry *pMethod);
	bool TryResolveArrayLengthMethod(SnMemberExpr &snMember,
		SnExpression *pOuterExpr, SnFieldExpr *pInnerExpr,
		SyntaxNode *pSavedContext);
	SnField *ResolveStreamSpecialTypeArg(SnInvokeExpr &invoke,
		NodeKind wantKind, const char *pMethodDisp,
		SyntaxNode *pSavedContext);
	void ResolveStreamMethodTail(SnMemberExpr &snMember,
		SnFieldExpr *pInnerExpr, SnInvokeExpr &invoke,
		const std::string &name, NodeKind retKind,
		SyntaxNode *pSavedContext);
	void CheckStreamMethodSignature(SnInvokeExpr &invoke,
		const std::string &name);
	//0.7.5 Task 9: writeLong/writeDouble value-argument admission —
	//cast-matrix verdict with a widening wrap (see the TU for rationale).
	void AdmitWideStreamWriterArg(SnInvokeExpr &invoke,
		const std::string &name);
	//One scalar argument vs its declared runtime kind (0.7.5
	//scalar-matrix policy): TCK_Same admits as-is, a widening TCK_Auto
	//admits wrapped in a cast expr in place, narrowing stays
	//explicit-only. String keeps the exact-kind policy (the scalar
	//registry does not know NK_String).
	bool ScalarArgAdmitted(NodeIterator &it, SnField* pArgType,
		uint8_t want);
	bool TryResolveStreamBuiltinMethod(SnMemberExpr &snMember,
		SnFieldExpr *pInnerExpr, SyntaxNode *pSavedContext);
	bool TryResolveObjectProtocolMethod(SnMemberExpr &snMember,
		SnFieldExpr *pInnerExpr, SyntaxNode *pSavedContext);
	//0.9.10 D2.4: type-name receiver in the Object protocol arm with no
	//same-name static — the call is an instance call with no receiver.
	//Logs the diagnostic, restores the context, returns true.
	bool RejectTypeNameProtocolCall(SnInvokeExpr &invoke,
		const std::string &name, SyntaxNode *pSavedContext);
	bool TryResolveUserClassEqualsOrGetHashCode(SnMemberExpr &snMember,
		SnFieldExpr *pInnerExpr, SnInvokeExpr &invoke,
		const std::string &name, SyntaxNode *pSavedContext);
	bool IsNonClassToStringReceiver(SnMemberExpr &snMember);
	bool TryResolveNonClassToString(SnMemberExpr &snMember,
		SnFieldExpr *pInnerExpr, SyntaxNode *pSavedContext);
	bool TryResolveGenericContainerMethod(SnMemberExpr &snMember,
		SnFieldExpr *pInnerExpr, SyntaxNode *pSavedContext);
	bool CheckContainerMethodArity(SnInvokeExpr &invoke,
		const std::string &baseName, const std::string &name);
	bool BindContainerMethodArgs(SnInvokeExpr &invoke, SnClassDecl *pGenClass,
		const std::string &baseName, const std::string &name);
	bool BindContainerArgPositions(SnInvokeExpr &invoke,
		const std::vector<SnField*> &typeArgs,
		const std::string &baseName, const std::string &name,
		int elemSlot, size_t valArg,
		SnExpression *&rpWrapValue, SnExpression *&rpWrapKey);
	void WrapContainerElemArgs(SnInvokeExpr &invoke,
		const std::vector<SnField*> &typeArgs, int elemSlot,
		SnExpression *pWrapKey, SnExpression *pWrapValue);
	SnField *ComputeContainerMethodResult(SnMemberExpr &snMember,
		SnInvokeExpr &invoke, const std::vector<SnField*> &typeArgs,
		const std::string &baseName, const std::string &name);
	bool TryResolveExceptionField(SnMemberExpr &snMember,
		SnFieldExpr *pInnerExpr, SyntaxNode *pSavedContext);
	void ResolveInvokeArgsInCallerScope(SnFieldExpr *pInnerExpr,
		SyntaxNode *pSavedContext);
	void FinishResolvedMember(SnMemberExpr &snMember,
		SnFieldExpr *pInnerExpr);
	//Round-12/14 shared rejection for the builtin by-name method families:
	//named arguments cannot bind by-name dispatch, out arguments can never
	//write back through pResult-only intrinsics. Logs only — the calling
	//phase owns the m_pContext restore; returns true when rejected.
	bool RejectNamedOrOutArguments(SnInvokeExpr &invoke);

	/*
	Walk the scope chain from parent upward, returning the first field
	named sName that grants access. pFuncFilter (Module import visibility,
	D1/D7) narrows the bare pool: when set, an NK_Function candidate in a
	bare-pool scope (IsBarePoolScope) must also pass it, other scopes and
	other field kinds are untouched.
	*/
	SnField *FindFieldInAncestor(const std::string &sName, SyntaxNode &parent,
		const SnField &accessor, ExprResolveFlagSet flags,
		std::function<bool(SnField &)> pFuncFilter = nullptr);

	/*
	0.9.x: the scope-chain walk behind the SnNameExpr shadow guard. Value
	bindings are transparent (IsTypeOrNamespaceBinding decides); the first
	type/namespace declaration along the chain wins. Mirrors
	FindFieldInAncestor minus the bare-pool function filter (functions are
	values, never type bindings, so the filter cannot apply here).
	0.9.0 semantic restriction: bIncludeNamespaces=false narrows the walk
	to type declarations only (the variable-name gate must not reserve
	the stdlib package names).
	*/
	SnField *FindTypeNameInAncestor(const std::string &sName,
		SyntaxNode &parent, const SnField &accessor,
		bool bIncludeNamespaces = true);

	/*
	0.9.x: the shadow guard of Access(SnNameExpr&) — exchange a value
	binding found by the inner resolution for the type/namespace binding
	the type position requires. True = bound (the inner identifier is
	rebound too); false = the named miss is logged, the caller stops.
	Definition in ExprResolverValues.cpp.
	*/
	bool TryRebindShadowedTypeName(SnNameExpr &nameExpr,
		SnFieldExpr &fieldExpr);

	SnField *FindFieldInUsings(std::string &sName, const UsingList &usings,
		const SnField & accessor);

	/*
	Module import visibility (D1/D7): the bare-pool visibility test for a
	function candidate in a bare-pool scope (IsBarePoolScope is the scope-
	side authority; this is the owner-side test). curModule is the caller's
	precomputed OwnerOfContext (the "current TU"). Ownerless symbols (root
	built-ins, runtime tables) stay visible; an owned function must belong
	to the current TU or share its directory — external owners and other
	directories are out (M2: a namespace declared in another directory is
	just as foreign). Class/interface/enum scope candidates never reach
	this test: their scopes keep full visibility (types stay global, spec
	§5.5).
	*/
	bool IsBareVisible(SnFunction &func, uint32_t curModule);

	/*
	Module import visibility (F20): the guiding diagnostic for a name that
	only exists as a function OUTSIDE the current TU's bare pool. Silent
	under ERF_SearchInParentOnly (a member-scoped miss is never fixed by
	an import — obj.method() cannot resolve to a global function) and when
	no foreign function carries the name. curModule is the caller's
	precomputed OwnerOfContext, shared with the visibility test.
	\return true when the hint was logged — the caller must then suppress
	its generic failure text (M4).
	*/
	bool MaybeLogVisibilityHint(const std::string &name,
		const ISourceLocation *pLoc, uint32_t curModule);

	/*
	2026-09-27 decomposition of the invoke-resolution family
	(ExprResolverInvoke.cpp) — the candidate-collection primitives of
	FindFuncByInvoke, the two named-rejection gates of
	Access(SnInvokeExpr), the ambiguity diagnostic of MatchInvokeAgainst
	and the two named-reason sweeps of LogInvokeFailure. MaybeLog*
	return true when their diagnostic fired (the caller then skips the
	generic text); Reject* return true when they logged a rejection.
	*/
	void SearchFuncScope(SnFunctionParentField &parent, bool bBarePool,
		const std::string &sFuncName, uint32_t curModule,
		InvokeReceiverForm form, std::vector<SnFunction*> &candidates,
		bool &rbImportedMatch);
	void SearchSuperclassChain(SnClassDecl &classDecl,
		const std::string &sFuncName, uint32_t curModule,
		InvokeReceiverForm form, std::vector<SnFunction*> &candidates,
		bool &rbImportedMatch);
	void SearchEnumScope(SnEnumDecl &enumDecl,
		const std::string &sFuncName, InvokeReceiverForm form,
		std::vector<SnFunction*> &candidates, bool &rbImportedMatch);
	bool CollectInvokeCandidates(SnInvokeExpr &invoke, bool bSearchInAncestor,
		std::vector<SnFunction*> &candidates, bool &rbImportedMatch,
		bool &rbVisibilityHintLogged);
	bool TryBindDelegateCall(SnInvokeExpr &snInvoke);
	bool RejectBareMethodCall(SnInvokeExpr &snInvoke, SnFunction *pCallee);
	bool RejectTypeNameInstanceCall(SnInvokeExpr &snInvoke,
		SnFunction *pCallee);
	//0.9.10 D2.4: the namespace-filtered search found no callee, but the
	//opposite namespace holds a same-name member on the receiver's chain —
	//report the call-form mismatch instead of the generic not-found text.
	bool RejectCrossNamespaceCall(SnInvokeExpr &snInvoke);
	void LogAmbiguousCall(SnInvokeExpr &invoke);
	bool MaybeLogImportedFuncRef(SnInvokeExpr &invoke,
		bool bNameMatchedImported);
	bool MaybeLogPendingFuncRef(SnInvokeExpr &invoke);

	/*
	Find the best function declaration matched witch an invoke expression.
	Precondition: the parameters in the invoke expression are all resolved.
	On ExactMatch / ApproximateMatch, outBindings is filled with per-formal
	binding decisions (Phase 9c).
	Phase 13: rbNameMatchedImported is set when a no-viable-bind
	FFR_Incompatible had an NF_Imported candidate among the name-matched
	set (NOT on the ambiguity verdict, whose report is complete on its
	own) — it lets the caller report the imported-arg rejection reason
	instead of a generic message.
	Module import visibility (Task 6): the root and namespace scopes of
	the chain are filtered to the current TU's directory (the bare pool,
	D1/D7). rbVisibilityHintLogged mirrors rbNameMatchedImported: it is
	set when this Incompatible already logged the "not visible here"
	hint, so the caller does not append its generic failure text (M4).
	Delegates the matching itself to MatchInvokeAgainst after collecting
	the same-name candidates along the scope chain (enum branch included).
	*/
	FindFuncResult FindFuncByInvoke(SnFunction *&pFunc, SnInvokeExpr &invoke,
		std::vector<FormalBinding> &outBindings,
		bool &rbNameMatchedImported, bool &rbVisibilityHintLogged,
		std::vector<SnFunction*> *pOutCandidates = nullptr);

	/*
	Module import visibility (M3b): the TryBindArgs / type-distance core
	of FindFuncByInvoke — the single matching implementation shared by the
	bare path (candidates collected along the scope chain there) and the
	module-qualified call path (candidates from the module table). Silent
	on not-found / incompatible (the caller reports those with its own
	context); the ambiguity error is logged here (candidate-set
	independent) AND reported through rbAmbiguous so callers can tell it
	apart from a no-viable-bind FFR_Incompatible. pFunc is nulled on every
	failure path by contract.
	*/
	FindFuncResult MatchInvokeAgainst(SnInvokeExpr &invoke,
		const std::vector<SnFunction*> &candidates, SnFunction *&pFunc,
		std::vector<FormalBinding> &outBindings, bool &rbAmbiguous);

	/*
	0.9.11 T6 (D10): the constructor-call twin of MatchInvokeAgainst —
	overload matching over the candidate ctors of one class/struct with
	the same TryBindArgs / ComputeBindingDistance core (closest binding
	wins, a tie at the smallest viable distance is ambiguous). Fully
	silent: the caller owns the class/struct-flavored diagnostics, so no
	out-bindings are handed back (codegen rebuilds them from the selected
	ctor's Params()). Returns the selected ctor, or null on no viable
	bind; rbAmbiguous separates the tie from a plain no-match.
	*/
	SnFunction *MatchCtorAgainst(const std::vector<SnExpression*> &actuals,
		const std::vector<SnFunction*> &candidates, bool &rbAmbiguous);

	/*
	Module import visibility (M3b): the SUCCESS tail of Access(SnInvokeExpr),
	shared with the module-qualified call path so both bind a callee exactly
	alike — Phase 13 function-reference argument rebinding, implicit cast
	wrappers (approximate matches only), binding storage and the resolved
	type (ResolveFieldExprAs; a void return yields a null EvalDataType).
	\return false when a function-reference argument failed to bind (the
	binder logged); the invoke stays unresolved and the caller just stops.
	*/
	bool ResolveInvokeWithFunc(SnInvokeExpr &invoke, SnFunction &func,
		FindFuncResult match, std::vector<FormalBinding> &bindings);

	//Phase 9e: out arguments on virtual (or by-name dispatched) methods are
	//rejected — the writeback mask is baked into the call instruction
	//against the static callee's parameter layout; a runtime override
	//resolved by name-based dispatch could disagree with it. Shared by the
	//bare and the module-qualified call paths (a qualified call must not
	//bypass the restriction). Logs the error; returns true when rejected.
	bool OutArgOnDispatchedCalleeRejected(const SnInvokeExpr &invoke,
		const SnFunction &callee,
		const std::vector<FormalBinding> &bindings) const;

	/*
	The failure branch of Access(SnInvokeExpr) — not-found, imported-stub
	and generic incompatibility diagnostics. Shared with the module-qualified
	call path so both surfaces report identically (spec §7 / M4). Takes
	failure results only (FFR_Incompatible / FFR_FuncNameNotFound).
	*/
	void LogInvokeFailure(SnInvokeExpr &invoke, FindFuncResult res,
		SnFunction *pCallee, bool bNameMatchedImported,
		const std::vector<SnFunction*> &candidates);

	/*
	The failure tail of Access(SnInvokeExpr) — the 0.9.0 ctor fallback
	(TryResolveCtorInvoke) ahead of the shared diagnostics. Split out of
	Access for the 50-line function budget. deferredBraceArgs gates the
	ctor route (count-only binding has no target for a brace argument)
	and receives the missing-target sweep on the way out.
	*/
	void ResolveFailedInvoke(SnInvokeExpr &snInvoke, FindFuncResult res,
		SnFunction *pCallee, bool bNameMatchedImported,
		bool bVisibilityHintLogged,
		const std::vector<SnFunction*> &candidates,
		const std::vector<SnExpression*> &deferredBraceArgs);

	/*
	Per-argument detail for an incompatible call, shared by the bare and
	module-qualified paths: among the name-matched candidates, pick the
	first whose arguments route (correct arity / out markers) and log each
	bound argument that cannot implicitly bind its formal — a void result,
	a non-printable value for a string formal, or a scalar/array mismatch.
	Returns true when at least one detail line was logged; the caller still
	emits the summary so the call site stays named.
	*/
	SnFunction *SelectRoutableCandidate(SnInvokeExpr &invoke,
		const std::vector<SnFunction*> &candidates,
		std::vector<FormalBinding> &outBindings);
	bool MaybeLogArgumentMismatch(SnInvokeExpr &invoke,
		const std::vector<SnFunction*> &candidates);
	//True when an argument of argType implicitly binds formalType through
	//the same two gates the real invoke path uses: CalcTypeDistance
	//(candidate admission) followed by an implicit-only cast kind
	//(Same/Auto/Box). Mirrors ComputeBindingDistance +
	//FixupParamTypesWithBindings so the per-argument verdict matches the
	//final compile result (string->float passes the distance but has no
	//cast; class->string has a cast but fails the distance).
	bool CanImplicitlyBind(const SnField &argType,
		const SnField &formalType) const;
	void LogArgumentTypeMismatch(SnExpression &arg, const SnField &argType,
		const SnField &formalType, const std::string &callee, int paramIdx);
	bool LogSingleArgumentMismatch(SnExpression &arg,
		const SnFormalParam &formal, const std::string &callee, int paramIdx);

	/*
	Phase 13: locate the delegate target of a bare invoke — a non-function
	field (local / param / class field) whose value type is a func<...>
	instantiation. Returns nullptr when the callee name is not a func
	value (the normal free-function/method paths take over). Shadowing
	rule: name lookup finds the func value before any same-named function.
	*/
	SnField *FindDelegateTarget(SnInvokeExpr &invoke);

	/*
	Phase 13: bind an invoke to a func-typed field's signature (delegate
	call). Validates arity, out agreement and argument types, rebinds
	pending function-reference arguments against the func's parameter
	slots, then resolves the invoke with the func's return type as
	EvalDataType (nullptr for a void return — the established
	void-invoke convention). Every path consumes the invoke: errors are
	logged here, success resolves it.
	*/
	void BindDelegateInvoke(SnInvokeExpr &invoke, SnField *pDelegateField);

	/*
	2026-09-27 decomposition of the delegate-binding family
	(ExprResolverDelegate.cpp): the per-argument gate of BindDelegateInvoke
	(out-marker agreement, pending function-reference rebinding, and the
	conversion-free admission below) and that admission itself — a
	delegate call has no fixup pass, so only distance 0 and reference
	upcasts pass raw bits soundly.
	*/
	bool CheckDelegateArgument(SnExpression &arg, SnField *pFormal,
		bool bWantOut, size_t i);
	bool AdmitsDelegateArgType(SnField *pArgType, SnField *pFormal) const;

	/*
	Phase 9c: validate caller-side argument syntax — independent of any
	candidate. Reports specific errors for:
	  - positional argument following a named argument
	  - duplicate name in named arguments (e.g. foo(a=1, a=2))
	Returns true if syntax is well-formed; false after logging the error.
	Called before FindFuncByInvoke so candidate-specific failures don't
	mask these structural errors.
	*/
	bool ValidateInvokeSyntax(const SnInvokeExpr &invoke);

	/*
	Phase 9c: try to bind an actual-argument list to a candidate callee's
	formal parameters. Handles positional args, named args, and default
	param expressions. Returns true if every formal is bound (either by
	caller or by default); false if any required formal is left unbound or
	a caller-side error occurs (positional after named, etc.). Does NOT
	log — caller reports a generic "not compatible" error when no
	candidate matches. 0.9.11 T6: the list parameter (not an invoke node)
	lets constructor matching reuse this core from every call shape.
	*/
	bool TryBindArgs(const std::vector<SnExpression*> &actuals,
		const SnFunction &callee, std::vector<FormalBinding> &outBindings);

	/*
	Phase 9c: sum of CalcTypeDistance over the bound (positional / named)
	entries. B_Default contributes 0. Returns -1 if any bound entry has
	incompatible types.
	*/
	int ComputeBindingDistance(
		const std::vector<FormalBinding> &bindings) const;

	/*
	Phase 9c: apply implicit cast wrappers (SnCastExpr) to caller-side
	expressions in bindings where needed (TCK_Auto / TCK_Box). B_Default
	entries are skipped — their type was validated against the formal at
	declaration time (StatementResolver Step 2).
	*/
	void FixupParamTypesWithBindings(SnInvokeExpr &invoke,
		std::vector<FormalBinding> &bindings);

	/*
	0.9.0 (D11): resolve the deferred bare brace arguments once the
	overload is chosen — each formal's declared type becomes the init
	list's target. False when an argument failed (its diagnostic is
	logged); the invoke stays unresolved, mirroring the func-ref path.
	*/
	bool ResolveDeferredBraceArgs(std::vector<FormalBinding> &bindings);

	/*
	Calculate the type "distance" from source type to target type.
	\return
	1) If the source type can be implicitly covert to target type:
	1.1)If both are scalar primitives (0.7.5): the registry ladder — same
	row 0, in-category/cross-sign containment widening the rank delta,
	integer→float a fixed rung above every widening, any scalar→string
	one rung above that (see ExprResolverCast.cpp).
	1.2)If the source type is primitive type, the target type is Object, it
	returns PRIMITIVE_TYPE_COUNT.
	1.3)If the source type and the target type are classes or interfaces, it
	returns the depth of inheritance.
	1.4)If the source type is enumerator and the target type is not enumerator,
	the source type will be convert to it primitive type firstly, and use the
	above rules to calculate the distance.
	2) If the source type can not be implicitly covert to target type
	(including the 0.7.5 explicit-only narrowings, TCK_Explicit), it
	returns	-1.
	*/
	int CalcTypeDistance(const SnField &source,
		const SnField &target) const;

	/*
	Create a cast expression and replace the exist expression by cast
	information.
	If no cast is needed or the source cannot be automatically cast to the 
	target type, nothing will be done.
	\param iSrcExpr The position where the original expression locates.
	\param castInfo The cast information.
	\return Return false if the source expression can not be automatically cast
	to the target type, else return true.
	*/
	bool FixupExprType(NodeIterator &iSrcExpr, TypeCastInfo &castInfo);

	/*
	2026-09-27 decomposition of the cast/binding resolution family
	(ExprResolverCast.cpp) — the reject/skip gates of FixupExprType and
	the named-arg arm of FixupParamTypesWithBindings. ConstantFitGate
	(0.7.5) runs the Java/C# constant-fit rule inside
	RejectIncompatibleCast; its verdicts leave the cast info untouched
	(NotApplicable), promote an in-range constant narrowing to TCK_Auto
	(Promoted) or log the out-of-range error (Rejected). The fit/lossy
	predicates themselves live in ExprResolverCastFit.hpp (pure
	functions, no resolver state). 2026-10-12: ComputeBindingDistance and
	its binding-distance helpers (LogArrayBindingReject included) moved
	to ExprResolverBinding.cpp at the source-size guard.
	*/
	enum ConstantFitGateResult
	{
		CFG_NotApplicable,
		CFG_Promoted,
		CFG_Rejected
	};

	bool RejectIncompatibleCast(SnExpression &srcExpr, TypeCastInfo &castInfo);
	bool RejectArrayTokenCast(SnExpression &srcExpr, TypeCastInfo &castInfo);
	ConstantFitGateResult ConstantFitGate(SnExpression &srcExpr,
		TypeCastInfo &castInfo);
	bool SkipNullIdentityWrap(SnExpression &srcExpr, TypeCastInfo &castInfo);
	bool TryFixupNamedArgBinding(SnInvokeExpr &invoke, FormalBinding &b,
		TypeCastInfo &castInfo);
	void LogArrayBindingReject(const FormalBinding &b) const;

	/*
	2026-09-27 decomposition of Access(SnBinaryExpr&)
	(ExprResolverBinary.cpp) — the compare-gate family, the logical
	int-operand check and the arithmetic branch. The Reject-prefixed
	helpers return true when they logged a rejection; the Check- and
	Resolve-prefixed helpers return true when resolution may continue.
	*/
	bool CheckCompareOperands(SnBinaryExpr &sn, SnBinaryExpr::Operator op);
	bool RejectStringNonStringMix(SnBinaryExpr &sn, NodeKind lk, NodeKind rk,
		bool lNull, bool rNull);
	bool RejectFuncHandleMisuse(SnBinaryExpr &sn, SnBinaryExpr::Operator op,
		SnField *L, SnField *R, bool lNull, bool rNull);
	bool RejectArrayIdentityMisuse(SnBinaryExpr &sn, SnBinaryExpr::Operator op,
		NodeKind lk, NodeKind rk, bool lNull, bool rNull);
	bool RejectBoolMisuse(SnBinaryExpr &sn, SnBinaryExpr::Operator op,
		NodeKind lk, NodeKind rk);
	//0.7.5 char gates: arithmetic on char is rejected (string concat
	//exempt); char compares only with char (code-point order).
	bool RejectCharArithmetic(SnBinaryExpr &sn, NodeKind lk, NodeKind rk);
	bool RejectCharMisuse(SnBinaryExpr &sn, SnBinaryExpr::Operator op,
		NodeKind lk, NodeKind rk);
	//0.7.5: registry-derived numeric promotion (spec §2.2). Returns the
	//smallest type that can implicitly receive BOTH operands, or null
	//when no implicit common type exists (int/long + ulong). Shared by
	//arithmetic result selection and compare-operand promotion.
	SnField *CommonNumericType(NodeKind lk, NodeKind rk);
	bool PromoteCompareOperands(SnBinaryExpr &sn, NodeKind lk, NodeKind rk,
		bool lNull, bool rNull);
	bool CheckLogicalBoolOperands(SnBinaryExpr &sn);
	bool ResolveArithmeticBinary(SnBinaryExpr &sn, SnBinaryExpr::Operator op);
	//0.9.9: `<<`/`>>` — the left operand's row decides the result; the
	//count must convert to that row implicitly (see
	//ExprResolverBinary.cpp for the full rationale).
	bool ResolveShiftBinary(SnBinaryExpr &sn, SnBinaryExpr::Operator op);
	//0.9.10: the count-admission core of the shift family, shared by the
	//binary form (WrapShiftOperands) and the compound form
	//(StatementResolverAssign, through the ExprResolver forwarder below).
	//`iCount` positions the count expression in its parent's child list
	//(replaced in place by the carrier wrap, like FixupExprType). See
	//ExprResolverBinary.cpp for the full rationale.
	bool AdmitShiftCount(NodeIterator &iCount, SnField *pCarrier,
		const ISourceLocation *pLoc, const char *opName);
	bool WrapShiftOperands(SnBinaryExpr &sn, SnField* T_result,
		const char* opName);
	SnField *SelectArithmeticResultType(SnBinaryExpr &sn,
		SnBinaryExpr::Operator op);

	/*
	2026-09-27 decomposition of the allocation resolution family
	(ExprResolverNew.cpp) — the ctor-binding of Access(SnNewExpr&) and
	the allocation binding of Access(SnNewArrayExpr&).
	*/
	void TryResolveBuiltinClassName(SnFieldExpr &fieldExpr);
	//0.9.11 T6 (D10): the intrinsic arity table of the builtin/generic
	//classes (FileStream 2, the Exception family 1, List/Dict/generic 0,
	//Object 0) — their ctors live only in the CompiledClass, so a call
	//against them is arity-checked, never overload-matched. User classes
	//and structs answer false: their ctors are AST candidates.
	bool FindCtorParamRange(SnField *pTypeDecl,
		size_t &requiredArity, size_t &totalArity);
	//0.9.11 T6 (D10): selects the constructor a call binds to. Collects
	//the same-name candidate set (class Members() / struct Ctors()),
	//runs MatchCtorAgainst, and reports the catalog diagnostics on
	//ambiguity / no-match; the intrinsic-arity shapes keep the legacy
	//arity texts. Null = a diagnostic was logged (or the type has no
	//ctor and no arguments — the legal bare-allocation shape).
	SnFunction *SelectCtorForArgs(SnField *pTypeDecl,
		const std::vector<SnExpression*> &actuals,
		const ISourceLocation *pLoc);
	//Binds the resolved type declaration to the new-expression: classes keep
	//the Phase 13 func-type rejection, structs join directly (0.9.11 D2).
	//False = the diagnostic was logged and the caller must abort.
	bool BindNewExprType(SnNewExpr &sn, SnField &typeField);

	/*
	0.9.0 (Phase A): constructor calls without `new`. TryResolveCtorInvoke
	(bare `Foo(args)`) runs ONLY in the invoke-resolution failure branch —
	the function-overload lookup has already failed, so functions keep
	priority and existing calls cannot regress; a foreign same-name
	function keeps the visibility hint as the complete diagnosis.
	TryResolveQualifiedCtorInvoke (`shapes.Point(args)`) is the
	module-qualified twin, hooked into the module-qualified call path
	after its function lookup failed with no name-matched candidate.
	Both return true when the invoke is consumed (resolved as a ctor call,
	or rejected with a named diagnostic); false falls through to the
	caller's generic failure. ProbeCtorType resolves a bare name to a class
	or struct the way Access(SnNewExpr&) resolves its type name, but as a
	silent probe — a miss must not add a second diagnostic.
	*/
	bool TryResolveCtorInvoke(SnInvokeExpr &snInvoke);
	bool TryResolveQualifiedCtorInvoke(SnMemberExpr &snMember,
		SnInvokeExpr &invoke, const std::string &modulePath);
	SnField *ProbeCtorType(const std::string &name,
		const ISourceLocation *pLoc);

	/*
	2026-09-29 split of the collection-initializer resolution family
	(ExprResolverInitList.cpp, out of ExprResolverNew.cpp at the
	source-size guard) — the phases of Access(SnInitListExpr&): target
	determination, the class-form checks, the 0.7.5 Dict key-form gate
	and the element/field cast wraps.
	*/
	bool ResolveInitListTarget(SnInitListExpr &sn,
		SnField* &pTargetField, bool &bIsArray);
	//0.9.0: target-shape gate of Access(SnInitListExpr&) — only
	//collection targets lower (IsInitListTargetShape); array targets
	//skip the element-shape test. True = rejected (diagnostic logged,
	//node left unresolved).
	bool RejectNonCollectionInitTarget(SnInitListExpr &sn,
		SnField *pTargetField, bool bIsArray);
	//0.9.0: nested bare brace entries inherit the slot type they store
	//into — the element type for array/List/Dict targets, or the
	//addressed field's declared type for struct/class targets.
	void PropagateNestedInitListTargets(SnInitListExpr &sn,
		SnField *pTargetField, SnField *pElemType, bool bIsArray);
	void CheckClassInitListForm(SnInitListExpr &sn, SnClassDecl *pClassDecl);
	//0.9.11 T6 (D10): select the zero-formal ctor for an init list —
	//the ctor-selection step of CheckClassInitListForm, split out for
	//size.
	void CheckClassCtorForInitList(SnInitListExpr &sn,
		SnClassDecl *pClassDecl);
	SnField *ResolveInitListElemType(SnField *pTargetField, bool bIsArray);
	void BindInitListFuncRefs(SnInitListExpr &sn, SnField *pElemType);
	void ApplyInitListElemCasts(SnInitListExpr &sn, SnField *pElemType);
	void ApplyInitListFieldCasts(SnInitListExpr &sn, SnField *pTargetField);
	SnField *InitListEntryFieldType(SnField &targetDecl,
		const SnInitListExpr &sn, size_t entryIdx);
	bool RejectNonStringDictInitKeys(SnInitListExpr &sn,
		SnField *pTargetField);

	/*
	2026-09-27 decomposition of the value resolution family
	(ExprResolverValues.cpp) — the string subscript (0.7.5 byte read) /
	container sugar of Access(SnSubscriptExpr&).
	*/
	bool ResolveStringSubscript(SnSubscriptExpr &sn, SnField *pBaseType);
	bool TryResolveContainerSubscript(SnSubscriptExpr &sn,
		SnField *pBaseType);

	//0.9.0: consume the subscript as a sized array creation (`int[n]` /
	//`Foo[n]`) when the base identifier resolved as a TYPE; the index
	//child doubles as the size expression. True = consumed.
	bool TrySizedArrayCreation(SnSubscriptExpr &sn);

	//0.9.0: expression-position alias support for the creation twins —
	//the pre-pass expands type positions only, so `P[n]` / `P(args)`
	//reach the resolver as bare identifiers. The subscript-base resolve
	//with the alias splice, the chain-following TU alias lookup (null =
	//the name addresses no alias), the splice itself, and the silent
	//class-or-struct probe behind an alias for the ctor-invoke twin. All
	//defined in ExprResolverAlias.cpp.
	bool ResolveArrayBase(SnSubscriptExpr &sn);
	SnFieldExpr *FindAliasTarget(const ISourceLocation *pLoc,
		const std::string &name);
	void TrySpliceAliasType(SnIdentifierExpr &idExpr);
	SnField *ProbeAliasType(const std::string &name,
		const ISourceLocation *pLoc);

	/*
	2026-09-27 decomposition of the type-node resolution family
	(ExprResolverTypes.cpp) — the type-argument collection of
	Access(SnGenericTypeExpr&) and the builtin-name fallback of
	Access(SnIdentifierExpr&).
	*/
	bool TryResolveGenericTypeArgs(SnGenericTypeExpr &genType,
		const std::string &baseName, std::vector<SnField*> &typeArgs,
		std::vector<uint8> &outFlags);
	bool TryResolveBuiltinNameFallback(SnIdentifierExpr &idExpr,
		uint32_t curModule);
	//The value-position bare lookup of Access(SnIdentifierExpr&): the
	//ancestor walk with the bare-pool owner filter, then the optional
	//usings fallback. Null = not found (the caller reports it).
	SnField *FindBareValueField(SnIdentifierExpr &idExpr,
		uint32_t curModule);

	//Phase 4b: the namespace named by a qualified type is not imported
	//into the current TU — name the fix (mirrors
	//RejectUnimportedModuleCall).
	void RejectUnimportedQualifiedType(SnQualifiedTypeExpr &qtype,
		const std::string &nsPath);

	/*
	2026-09-27 decomposition of the module-qualified resolution
	(ExprResolverStdLib.cpp): the phase chain of TryResolveModuleQualified
	(decline tests, the unimported reject, argument handling, callee
	matching and the shared finish).
	*/
	bool TryResolveModuleCallTarget(SnMemberExpr &snMember,
		SnInvokeExpr *&rpInvoke, std::string &rModulePath);
	bool RejectUnimportedModuleCall(SnMemberExpr &snMember,
		const std::string &modulePath);
	bool TryResolveModuleQualifiedArgs(SnMemberExpr &snMember,
		SnInvokeExpr &invoke);
	bool ResolveModuleQualifiedCallee(SnMemberExpr &snMember,
		SnInvokeExpr &invoke, const std::string &modulePath);
	//Post-match close-out half of ResolveModuleQualifiedCallee
	//(out-argument veto, invoke binding, member callee/result wiring);
	//split out of it for the 50-line function budget.
	bool BindQualifiedInvoke(SnMemberExpr &snMember,
		SnInvokeExpr &invoke, SnFunction &callee, FindFuncResult res,
		std::vector<FormalBinding> &bindings);
	//Package-call admission policy: the module-qualified path is a
	//binding-policy domain of its own — library formals admit per the
	//conversion matrix only (the generic binder's scalar/array→string
	//coercion candidacy is a user-function rule), with the io coercing
	//trio (print/write/eprint) as the documented exception accepting
	//string, scalars, arrays and function values. AdmitCoercingTrioArgs
	//wraps coercing non-string args in the implicit to-string conversion
	//in place (pre-match) and rejects class/interface/enum/struct
	//values; RejectPackageStringCoercion vetoes →string bindings
	//post-match on every other package call.
	bool AdmitCoercingTrioArgs(SnInvokeExpr &invoke);
	bool RejectPackageStringCoercion(SnInvokeExpr &invoke,
		const std::vector<FormalBinding> &bindings,
		const std::string &modulePath);
	void FinishConsumedMember(SnMemberExpr &snMember);

	//Phase 5: package-qualified VALUE access (`alib.Color.Green`, the
	//enum-constant chain). The de-shelled tree has no namespace node for
	//the generic receiver route to anchor on, so the chain resolves
	//through the registry (FindModuleType). Calls keep
	//TryResolveModuleQualified; a chain whose tip is not a plain
	//identifier declines and keeps the generic path.
	bool TryResolveQualifiedTypeValue(SnMemberExpr &snMember);

	//v2.2: module-qualified free-function VALUE reference (`lib.seven`):
	//the tip names a function of an imported module and the chain
	//resolves as a pending function reference in the member channel
	//(bind sites shared with bound method references; signatures
	//serialize since the func TypeDesc grammar). Non-function tips
	//decline — the package-value path and the generic path keep them.
	bool TryResolveModuleFuncValue(SnMemberExpr &snMember);

	ISyntaxNodeVisitor *m_pVisitor;
	SyntaxNode *m_pContext;
	SnField *m_pAccessor;
	BuildEnvironment &m_Env;
};

//The helper class to resolve expressions.
class ExprResolver : public Flagable<uint8>
{
public:
	explicit ExprResolver(BuildEnvironment &env) : 
		m_Accessor(env), m_Visitor(m_Accessor, NVK_CustomTraverse)
	{
		m_Accessor.m_pVisitor = &m_Visitor;
	}

	bool Resolve(SnExpression &sn, SyntaxNode &context, SnField &accessor,
		ExprResolveFlagSet flags = ERF_None)
	{
		m_Accessor.m_pContext = &context;
		m_Accessor.m_pAccessor = &accessor;
		m_Accessor.Flags(flags);
		sn.Accept(m_Visitor);
		return sn.IsResolved();
	}

	//Resolve the child data type nodes in a specified field.
	//\note The data types in expressions will not be resolved.
	bool ResolveDataTypes(SnField &, SnField &outerType);

	/*
	Create a cast expression and replace the exist expression by cast
	information.
	If no cast is needed or the source cannot be automatically cast to the target
	type, nothing will be done.
	\param iSrcExpr The iterator reference where the original expression 
	locates.
		This reference will be replace with the inserted iterator to the newly
	created	cast expression.
	\param castInfo The cast information.
	\return Return false if the source expression can not be automatically cast
	to the target type, else return true.
	\note The replacement erases the source's list node, invalidating exactly
	that iterator. A caller looping over the same child list must NOT advance
	its own stale iterator afterwards — re-find the next target fresh (the
	TryFixupNamedArgBinding discipline) or resume from the replaced iterator
	(the binary-promotion callers). Advancing a stale std::list iterator walks
	into nodes other lists reuse and silently scrambles the tree.
	*/
	bool FixupExprType(NodeIterator &iSrcExpr, TypeCastInfo &castInfo)
	{
		return m_Accessor.FixupExprType(iSrcExpr, castInfo);
	}

	//0.9.10: statement-side entry of the shared shift count gate (the
	//compound `<<=`/`>>=` branch in StatementResolverAssign). Same
	//contract as the accessor member of the same name.
	bool AdmitShiftCount(NodeIterator &iCount, SnField *pCarrier,
		const ISourceLocation *pLoc, const char *opName)
	{
		return m_Accessor.AdmitShiftCount(iCount, pCarrier, pLoc, opName);
	}

	//0.9.11 T6 (D10): statement-side entry of the ctor overload match —
	//the super(...) selection in StatementResolverSuper. Same contract as
	//the accessor member of the same name; the match reads only the
	//resolved actuals and the candidate signatures, so no Resolve()
	//context is required.
	SnFunction *MatchCtorAgainst(const std::vector<SnExpression*> &actuals,
		const std::vector<SnFunction*> &candidates, bool &rbAmbiguous)
	{
		return m_Accessor.MatchCtorAgainst(actuals, candidates, rbAmbiguous);
	}

	void ResolveFieldExprAs(SnFieldExpr &expr, SnField *pField)
	{
		m_Accessor.ResolveFieldExprAs(expr, pField);
	}

	/*
	0.9.0 semantic restriction: does `name` resolve to a TYPE at `scope`?
	Types only — namespaces are deliberately excluded (the stdlib package
	names io/math/fs are common variable names; a local shadowing a
	namespace keeps working through the type-position guard). Mirrors the
	type-position predicate (FindTypeNameInAncestor + builtin class names
	+ builtin generic class names) so "spellable as a type here" and
	"usable as a variable name here" are exact complements. Shared with
	StatementResolver's variable-declaration gates.
	*/
	bool NamesVisibleType(const std::string &name, SyntaxNode &scope,
		const SnField &accessor)
	{
		return IsBuiltinClassName(name)
			|| name == kBuiltinListTypeName
			|| name == kBuiltinDictTypeName
			|| m_Accessor.FindTypeNameInAncestor(name, scope, accessor,
				/*bIncludeNamespaces=*/false) != nullptr;
	}
private:

	bool ResolveDataType(SnFieldExpr & typeExpr, SnField & outerType);

	bool ResolveChildFields(SnField & sn);
	ExprResolveAccessor m_Accessor;
	SyntaxNodeVisitor<ExprResolveAccessor> m_Visitor;
};

} //namespace nlang