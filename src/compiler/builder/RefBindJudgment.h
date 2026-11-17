#pragma once
#include "BuildEnvironment.h"
#include <vector>

namespace nlang
{

class SnExpression;
class SnField;
class ISourceLocation;
struct FormalBinding;

//v0.13.9 phase 3 Task 5: the &-binding judgment as free functions. The
//same contract serves BOTH consumption sites (spec §3.9, one MakeRef
//judgment): a &-binding initializer (StatementResolveAccessor) and a
//&-formal argument at a call site (ExprResolveAccessor). The two
//accessor classes are unrelated hierarchies, so the shared judgment
//lives here and the member versions are thin delegates.
//
//True = rejected (diagnostic logged).

//The full binding judgment for one resolved initializer/argument:
//the position family plus the cast/exact-type tail; rvalues are the
//accepted temporary-record contract. constBinding marks a read-only
//(const T&) binding: the const-storage arm of the position family
//relaxes (Task 9) while every other gate keeps firing.
bool RejectRefBindTarget(BuildEnvironment &env, SnExpression &expr,
	SnField *pTypeField, const ISourceLocation *pLoc,
	bool constBinding = false);

//The call-site consumption (ExprResolveAccessor): every argument bound
//to a & formal runs RejectRefBindTarget + MarkPromotedBindTarget on its
//RAW argument types. False = a rejection logged; the invoke stays
//unresolved.
bool RejectRefBindCallArgs(BuildEnvironment &env,
	std::vector<FormalBinding> &bindings);

//Promotion marking for an ACCEPTED &-binding: scalar/enum/string local
//and formal targets get NF_Promoted so codegen compiles their storage
//as one stable boxed record (design §3.2).
void MarkPromotedBindTarget(SnExpression &expr);

//The position family: identifier/member/subscript; member and
//identifier targets run the storage gates, subscript binds elements
//and rejects slices.
bool RejectRefBindPosition(BuildEnvironment &env, SnExpression &expr,
	const ISourceLocation *pLoc, bool constBinding = false);

//The storage gates shared by the identifier/member arms and the
//-return judgment: enum members, const and static slots. A const
//binding (const T&) skips the const arm — read-only aliasing of const
//storage is the form's contract (Task 9); the &-return judgment keeps
//the default (a handed-out alias must stay writable-sourced).
bool RejectRefTargetStorage(BuildEnvironment &env, SnField *pField,
	const ISourceLocation *pLoc, bool constBinding = false);

//The binding-only gate: a binding aliases storage, not a binding slot.
bool RejectRefTargetIsReference(BuildEnvironment &env, SnField *pField,
	const ISourceLocation *pLoc);

//Task 9: the root storage a member-chain receiver writes into, or
//null when the chain leaves a local's own slots (an identity-typed
//field hop, or a this/call/subscript receiver). Shared by the
//&-binding position family and the const member-store gate.
SnField *FindMemberChainRootStorage(SnExpression *pReceiver);

//Task 9: true when a member-chain root is const slot-copy storage —
//a const struct local/formal (class/array roots are identity handles;
//const is shallow for those). Const fields and enum members keep
//their own gate arms.
bool IsConstStructStorage(SnField *pRoot);

} //namespace nlang
